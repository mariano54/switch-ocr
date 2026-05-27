#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import BinaryIO, Iterator


ROOT = Path(__file__).resolve().parents[1]
SERVER_DIR = ROOT / "server"
DEFAULT_OUTPUT = ROOT / "server" / "latest_frame.jpg"
JPEG_START = b"\xff\xd8"
JPEG_END = b"\xff\xd9"

sys.path.insert(0, str(SERVER_DIR))
from frame_capture import FrameCaptureError, capture_bridge_frame, config_from_env  # noqa: E402


def copy_test_frame(source: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output)
    print(f"Wrote test frame: {output}", flush=True)


def write_frame(output: Path, image_bytes: bytes) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp_output = output.with_suffix(".tmp.jpg")
    tmp_output.write_bytes(image_bytes)
    tmp_output.replace(output)
    print(f"Updated {output} ({output.stat().st_size} bytes)", flush=True)


def iter_mjpeg_frames(stream: BinaryIO) -> Iterator[bytes]:
    buffer = bytearray()
    while True:
        chunk = stream.read(65536)
        if not chunk:
            return
        buffer.extend(chunk)

        while True:
            start = buffer.find(JPEG_START)
            if start < 0:
                if len(buffer) > 2:
                    del buffer[:-2]
                break

            if start > 0:
                del buffer[:start]

            end = buffer.find(JPEG_END, len(JPEG_START))
            if end < 0:
                if len(buffer) > 8_000_000:
                    del buffer[:-2]
                break

            frame = bytes(buffer[: end + len(JPEG_END)])
            del buffer[: end + len(JPEG_END)]
            yield frame


def capture_rtsp(rtsp_url: str, output: Path, interval_seconds: float, restart_delay_seconds: float) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    fps = max(0.1, 1.0 / interval_seconds)
    print(f"Streaming latest frame from {rtsp_url} -> {output} at {fps:.2f} fps", flush=True)

    while True:
        command = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-rtsp_transport",
            "tcp",
            "-timeout",
            "5000000",
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
            "-probesize",
            "32",
            "-analyzeduration",
            "0",
            "-i",
            rtsp_url,
            "-an",
            "-vf",
            f"fps={fps:.3f}",
            "-q:v",
            "2",
            "-f",
            "image2pipe",
            "-vcodec",
            "mjpeg",
            "pipe:1",
        ]

        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            if process.stdout is None:
                raise RuntimeError("ffmpeg stdout pipe was not created")
            for frame in iter_mjpeg_frames(process.stdout):
                write_frame(output, frame)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)

        print(f"RTSP ffmpeg exited with code {process.returncode}; restarting", flush=True)
        time.sleep(restart_delay_seconds)


def capture_bridge(output: Path, interval_seconds: float) -> None:
    print(f"Rolling SysDVR TCP bridge frames -> {output}", flush=True)
    while True:
        started = time.monotonic()
        failed = False
        try:
            capture_bridge_frame(output, config_from_env())
            print(f"Updated {output} via bridge in {time.monotonic() - started:.2f}s", flush=True)
        except FrameCaptureError as error:
            failed = True
            print(f"Bridge frame capture failed: {error}", flush=True)
        except Exception as error:
            failed = True
            print(f"Unexpected bridge frame capture error: {error}", flush=True)

        elapsed = time.monotonic() - started
        wait_seconds = max(10.0 if failed else 0.0, interval_seconds - elapsed)
        time.sleep(wait_seconds)


def main() -> None:
    parser = argparse.ArgumentParser(description="Maintain server/latest_frame.jpg for Switch OCR.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    test_parser = subparsers.add_parser("test", help="Copy an existing JPEG/PNG as latest frame.")
    test_parser.add_argument("source", type=Path)

    rtsp_parser = subparsers.add_parser("rtsp", help="Stream a SysDVR RTSP source into latest_frame.jpg.")
    rtsp_parser.add_argument("--url", required=True, help="Example: rtsp://192.168.0.136:6666/")
    rtsp_parser.add_argument("--interval", type=float, default=0.5)
    rtsp_parser.add_argument("--restart-delay", type=float, default=2.0)

    bridge_parser = subparsers.add_parser("bridge", help="Roll TCP bridge clips into latest_frame.jpg.")
    bridge_parser.add_argument("--interval", type=float, default=2.0)

    args = parser.parse_args()
    if args.mode == "test":
        copy_test_frame(args.source, args.output)
    elif args.mode == "rtsp":
        capture_rtsp(args.url, args.output, args.interval, args.restart_delay)
    elif args.mode == "bridge":
        capture_bridge(args.output, args.interval)


if __name__ == "__main__":
    main()
