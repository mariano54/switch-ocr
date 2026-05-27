#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "server" / "latest_frame.jpg"


def copy_test_frame(source: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output)
    print(f"Wrote test frame: {output}", flush=True)


def capture_rtsp(rtsp_url: str, output: Path, interval_seconds: float) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp_output = output.with_suffix(".tmp.jpg")
    print(f"Capturing latest frame from {rtsp_url} -> {output}", flush=True)

    while True:
        command = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-rtsp_transport",
            "tcp",
            "-timeout",
            "5000000",
            "-i",
            rtsp_url,
            "-frames:v",
            "1",
            "-q:v",
            "2",
            str(tmp_output),
        ]
        result = subprocess.run(command, check=False)
        if result.returncode == 0 and tmp_output.exists() and tmp_output.stat().st_size > 0:
            tmp_output.replace(output)
            print(f"Updated {output} ({output.stat().st_size} bytes)", flush=True)
        else:
            print(f"Frame capture failed with exit code {result.returncode}", flush=True)

        time.sleep(interval_seconds)


def main() -> None:
    parser = argparse.ArgumentParser(description="Maintain server/latest_frame.jpg for Switch OCR.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    test_parser = subparsers.add_parser("test", help="Copy an existing JPEG/PNG as latest frame.")
    test_parser.add_argument("source", type=Path)

    rtsp_parser = subparsers.add_parser("rtsp", help="Poll a SysDVR RTSP stream for frames.")
    rtsp_parser.add_argument("--url", required=True, help="Example: rtsp://192.168.0.136:6666/")
    rtsp_parser.add_argument("--interval", type=float, default=1.0)

    args = parser.parse_args()
    if args.mode == "test":
        copy_test_frame(args.source, args.output)
    elif args.mode == "rtsp":
        capture_rtsp(args.url, args.output, args.interval)


if __name__ == "__main__":
    main()
