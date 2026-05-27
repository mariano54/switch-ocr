from __future__ import annotations

import os
import pty
import signal
import shutil
import subprocess
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import fcntl


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CLIENT_PATH = ROOT_DIR / "tmp" / "sysdvr-client" / "mac-arm64" / "SysDVR-Client"
DEFAULT_LOCK_PATH = ROOT_DIR / "tmp" / "sysdvr-capture.lock"
DEFAULT_RECORD_SECONDS = 0.85
DEFAULT_CAPTURE_RETRIES = 3
DEFAULT_CONNECT_TIMEOUT_SECONDS = 5.5
DEFAULT_STOP_TIMEOUT_SECONDS = 8.0
CAPTURE_LOCK = threading.Lock()


class FrameCaptureError(RuntimeError):
    pass


@dataclass(frozen=True)
class BridgeCaptureConfig:
    switch_ip: str
    client_path: Path = DEFAULT_CLIENT_PATH
    record_seconds: float = DEFAULT_RECORD_SECONDS
    capture_retries: int = DEFAULT_CAPTURE_RETRIES
    connect_timeout_seconds: float = DEFAULT_CONNECT_TIMEOUT_SECONDS
    stop_timeout_seconds: float = DEFAULT_STOP_TIMEOUT_SECONDS
    use_pty: bool = False


def config_from_env() -> BridgeCaptureConfig:
    return BridgeCaptureConfig(
        switch_ip=os.environ.get("SWITCH_IP", "192.168.0.136"),
        client_path=Path(os.environ.get("SYSDVR_CLIENT", str(DEFAULT_CLIENT_PATH))),
        record_seconds=float(os.environ.get("SYSDVR_RECORD_SECONDS", str(DEFAULT_RECORD_SECONDS))),
        capture_retries=int(os.environ.get("SYSDVR_CAPTURE_RETRIES", str(DEFAULT_CAPTURE_RETRIES))),
        connect_timeout_seconds=float(os.environ.get("SYSDVR_CONNECT_TIMEOUT_SECONDS", str(DEFAULT_CONNECT_TIMEOUT_SECONDS))),
        stop_timeout_seconds=float(os.environ.get("SYSDVR_STOP_TIMEOUT_SECONDS", str(DEFAULT_STOP_TIMEOUT_SECONDS))),
        use_pty=os.environ.get("SYSDVR_USE_PTY", "0") == "1",
    )


def cleanup_capture_artifacts(directory: Path) -> None:
    for pattern in ("*.mp4", "*.tmp.jpg", "*.sysdvr.tmp.mp4"):
        for path in directory.glob(pattern):
            path.unlink(missing_ok=True)


class RollingBridgeCapture:
    def __init__(self, output_path: Path, config: BridgeCaptureConfig, interval_seconds: float) -> None:
        self.output_path = output_path
        self.config = config
        self.interval_seconds = interval_seconds
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self.last_error = ""

    def start(self) -> None:
        if self._thread is not None:
            return

        self._thread = threading.Thread(target=self._run, name="sysdvr-frame-capture", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=3)

    def _run(self) -> None:
        while not self._stop_event.is_set():
            started = time.monotonic()
            try:
                with self._lock:
                    capture_bridge_frame(self.output_path, self.config)
                self.last_error = ""
            except FrameCaptureError as error:
                self.last_error = str(error)
                print(f"[FrameCapture] {self.last_error}", flush=True)
            except Exception as error:
                self.last_error = f"Unexpected capture error: {error}"
                print(f"[FrameCapture] {self.last_error}", flush=True)

            elapsed = time.monotonic() - started
            wait_seconds = max(10.0 if self.last_error else 0.0, self.interval_seconds - elapsed)
            self._stop_event.wait(wait_seconds)


def capture_bridge_frame(output_path: Path, config: BridgeCaptureConfig) -> None:
    with CAPTURE_LOCK:
        with _bridge_capture_file_lock():
            _capture_bridge_frame_locked(output_path, config)


@contextmanager
def _bridge_capture_file_lock() -> Iterator[None]:
    DEFAULT_LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    with DEFAULT_LOCK_PATH.open("w") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _capture_bridge_frame_locked(output_path: Path, config: BridgeCaptureConfig) -> None:
    if not config.client_path.exists():
        raise FrameCaptureError(f"SysDVR client not found at {config.client_path}")

    attempts = max(1, config.capture_retries + 1)
    last_error = ""
    for attempt in range(1, attempts + 1):
        try:
            _capture_bridge_frame_once(output_path, config)
            return
        except FrameCaptureError as error:
            last_error = str(error)
            _cleanup_bridge_attempt(output_path, config.client_path)
            if attempt == attempts:
                break
            time.sleep(min(0.5, 0.2 * attempt))

    raise FrameCaptureError(f"SysDVR frame capture failed after {attempts} attempts. Last error: {last_error}")


def _capture_bridge_frame_once(output_path: Path, config: BridgeCaptureConfig) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    video_path = output_path.with_suffix(".sysdvr.tmp.mp4")
    frame_path = output_path.with_suffix(".tmp.jpg")
    video_path.unlink(missing_ok=True)
    frame_path.unlink(missing_ok=True)

    try:
        _record_bridge_clip(video_path, config)
        _extract_latest_frame(video_path, frame_path)
        frame_path.replace(output_path)
    finally:
        video_path.unlink(missing_ok=True)
        frame_path.unlink(missing_ok=True)


def _cleanup_bridge_attempt(output_path: Path, client_path: Path) -> None:
    output_path.with_suffix(".sysdvr.tmp.mp4").unlink(missing_ok=True)
    output_path.with_suffix(".tmp.jpg").unlink(missing_ok=True)
    _kill_stale_sysdvr_client(client_path)


def _record_bridge_clip(video_path: Path, config: BridgeCaptureConfig) -> None:
    _kill_stale_sysdvr_client(config.client_path)
    command = [
        str(config.client_path),
        "bridge",
        config.switch_ip,
        "--no-audio",
        "--file",
        str(video_path),
        "--debug",
        "log",
    ]

    lines: list[str] = []
    master_fd: int | None = None
    if config.use_pty:
        master_fd, slave_fd = pty.openpty()
        process = subprocess.Popen(
            command,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            text=True,
            start_new_session=True,
        )
        os.close(slave_fd)
    else:
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )

    def read_output() -> None:
        if master_fd is not None:
            pending = b""
            try:
                while True:
                    chunk = os.read(master_fd, 4096)
                    if not chunk:
                        break
                    pending += chunk
                    while b"\n" in pending:
                        raw_line, pending = pending.split(b"\n", 1)
                        lines.append(raw_line.decode("utf-8", errors="replace").rstrip("\r"))
            except OSError:
                pass
            finally:
                if pending:
                    lines.append(pending.decode("utf-8", errors="replace").rstrip("\r"))
                os.close(master_fd)
            return

        stdout = process.stdout
        if stdout is None:
            return
        for line in stdout:
            lines.append(line.rstrip())

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()

    started = False
    deadline = time.monotonic() + config.connect_timeout_seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        if any("Recording started" in line or "Decoder.CodecCtx uses pixel format" in line for line in lines):
            started = True
            break
        time.sleep(0.1)

    if not started:
        _stop_process(process, signal.SIGTERM, config.stop_timeout_seconds)
        raise FrameCaptureError(_summarize_failure("SysDVR bridge did not start recording", lines))

    time.sleep(config.record_seconds)
    _stop_recording_process(process, master_fd, config.stop_timeout_seconds)
    reader.join(timeout=1)

    if not video_path.exists() or video_path.stat().st_size == 0:
        raise FrameCaptureError(_summarize_failure("SysDVR bridge produced no video", lines))


def _extract_latest_frame(video_path: Path, frame_path: Path) -> None:
    ffmpeg = _find_ffmpeg()
    result = subprocess.run(
        [
            str(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-sseof",
            "-1",
            "-i",
            str(video_path),
            "-frames:v",
            "1",
            "-strict",
            "-2",
            "-q:v",
            "2",
            str(frame_path),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not frame_path.exists() or frame_path.stat().st_size == 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise FrameCaptureError(f"Could not extract latest SysDVR frame: {detail}")


def _find_ffmpeg() -> Path:
    found = shutil.which("ffmpeg")
    if found:
        return Path(found)

    for candidate in (Path("/opt/homebrew/bin/ffmpeg"), Path("/usr/local/bin/ffmpeg")):
        if candidate.exists():
            return candidate

    raise FrameCaptureError("ffmpeg is not installed or not on PATH")


def _stop_recording_process(process: subprocess.Popen[str], master_fd: int | None, timeout_seconds: float) -> None:
    if process.poll() is not None:
        return

    try:
        if master_fd is not None:
            os.write(master_fd, b"\n")
        elif process.stdin is not None:
            process.stdin.write("\n")
            process.stdin.flush()
    except (BrokenPipeError, OSError):
        pass

    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        _stop_process(process, signal.SIGINT, timeout_seconds)


def _stop_process(process: subprocess.Popen[str], sig: signal.Signals, timeout_seconds: float) -> None:
    if process.poll() is not None:
        return

    try:
        os.killpg(process.pid, sig)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        process.wait(timeout=timeout_seconds)


def _kill_stale_sysdvr_client(client_path: Path) -> None:
    subprocess.run(["pkill", "-f", str(client_path)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _summarize_failure(message: str, lines: list[str]) -> str:
    tail = "\n".join(lines[-8:])
    if not tail:
        return message
    return f"{message}:\n{tail}"
