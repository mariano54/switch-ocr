from __future__ import annotations

import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CLIENT_PATH = ROOT_DIR / "tmp" / "sysdvr-client" / "mac-arm64" / "SysDVR-Client"


class FrameCaptureError(RuntimeError):
    pass


@dataclass(frozen=True)
class BridgeCaptureConfig:
    switch_ip: str
    client_path: Path = DEFAULT_CLIENT_PATH
    record_seconds: float = 0.7
    connect_timeout_seconds: float = 15.0
    stop_timeout_seconds: float = 8.0


def config_from_env() -> BridgeCaptureConfig:
    return BridgeCaptureConfig(
        switch_ip=os.environ.get("SWITCH_IP", "192.168.0.136"),
        client_path=Path(os.environ.get("SYSDVR_CLIENT", str(DEFAULT_CLIENT_PATH))),
        record_seconds=float(os.environ.get("SYSDVR_RECORD_SECONDS", "0.7")),
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
            wait_seconds = max(0.1, self.interval_seconds - elapsed)
            self._stop_event.wait(wait_seconds)


def capture_bridge_frame(output_path: Path, config: BridgeCaptureConfig) -> None:
    if not config.client_path.exists():
        raise FrameCaptureError(f"SysDVR client not found at {config.client_path}")

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


def _record_bridge_clip(video_path: Path, config: BridgeCaptureConfig) -> None:
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

    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    lines: list[str] = []

    def read_output() -> None:
        if process.stdout is None:
            return
        for line in process.stdout:
            lines.append(line.rstrip())

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()

    started = False
    deadline = time.monotonic() + config.connect_timeout_seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        if any("Recording started" in line for line in lines):
            started = True
            break
        time.sleep(0.1)

    if not started:
        _stop_process(process, signal.SIGTERM, config.stop_timeout_seconds)
        raise FrameCaptureError(_summarize_failure("SysDVR bridge did not start recording", lines))

    time.sleep(config.record_seconds)
    _stop_process(process, signal.SIGINT, config.stop_timeout_seconds)
    reader.join(timeout=1)

    if not video_path.exists() or video_path.stat().st_size == 0:
        raise FrameCaptureError(_summarize_failure("SysDVR bridge produced no video", lines))


def _extract_latest_frame(video_path: Path, frame_path: Path) -> None:
    result = subprocess.run(
        [
            "ffmpeg",
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


def _stop_process(process: subprocess.Popen[str], sig: signal.Signals, timeout_seconds: float) -> None:
    if process.poll() is not None:
        return

    process.send_signal(sig)
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=timeout_seconds)


def _summarize_failure(message: str, lines: list[str]) -> str:
    tail = "\n".join(lines[-8:])
    if not tail:
        return message
    return f"{message}:\n{tail}"
