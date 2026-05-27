#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import socket
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from frame_capture import RollingBridgeCapture, capture_bridge_frame, cleanup_capture_artifacts, config_from_env
from ocr import perform_ocr
from ocr.formatting import switch_display_text

SERVER_DIR = Path(__file__).resolve().parent
LAST_OCR_UPLOAD = SERVER_DIR / "last_ocr_upload.jpg"
LATEST_FRAME = SERVER_DIR / "latest_frame.jpg"
UDP_RESPONSE_LIMIT = 12_000


def latest_frame_metadata() -> dict[str, Any]:
    if not LATEST_FRAME.exists():
        return {}

    stat = LATEST_FRAME.stat()
    return {
        "source": str(LATEST_FRAME),
        "frame_age_seconds": round(time.time() - stat.st_mtime, 1),
        "frame_mtime": stat.st_mtime,
        "bytes": stat.st_size,
    }


def ocr_latest_payload() -> dict[str, Any]:
    started = time.time()
    capture_error = ""
    if os.environ.get("SYSDVR_CAPTURE_ON_REQUEST", "1") != "0":
        try:
            capture_bridge_frame(LATEST_FRAME, config_from_env())
        except Exception as exc:
            capture_error = str(exc)
            print(f"[FrameCapture] on-request capture failed: {exc}", flush=True)
            if os.environ.get("SYSDVR_ALLOW_STALE_FRAME_ON_CAPTURE_FAIL", "0") != "1":
                return {
                    "ok": False,
                    "success": False,
                    "error": f"Frame capture failed: {capture_error}",
                    "capture_error": capture_error,
                    "display_text": "Mac frame capture failed.",
                    **latest_frame_metadata(),
                }

    if not LATEST_FRAME.exists():
        return {
            "ok": False,
            "success": False,
            "error": f"No latest frame found at {LATEST_FRAME}",
            "display_text": "No Mac-side frame source is running yet.",
        }

    body = LATEST_FRAME.read_bytes()
    payload = ocr_bytes_payload(body, "image/jpeg", LATEST_FRAME)
    payload.update(latest_frame_metadata())
    if capture_error:
        payload["capture_error"] = capture_error
    print(f"[SwitchOCR] /ocr-latest total {time.time() - started:.2f}s, bytes={len(body)}", flush=True)
    return payload


def ocr_bytes_payload(body: bytes, content_type: str, source_path: Path) -> dict[str, Any]:
    result = perform_ocr(body, mime_type=content_type)
    result["ok"] = bool(result.get("success"))
    result["bytes"] = len(body)
    result["source"] = str(source_path)
    return result


def compact_udp_payload(payload: dict[str, Any]) -> bytes:
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(body) <= UDP_RESPONSE_LIMIT:
        return body

    compact = {
        "ok": bool(payload.get("ok")),
        "success": bool(payload.get("success")),
        "display_text": str(payload.get("display_text", ""))[:3000],
        "words": payload.get("words", [])[:32] if isinstance(payload.get("words"), list) else [],
        "truncated": True,
    }
    return json.dumps(compact, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def compact_switch_payload(payload: dict[str, Any]) -> dict[str, Any]:
    words = payload.get("words")
    display_text = str(payload.get("display_text", ""))
    if isinstance(words, list) and words:
        display_text = switch_display_text(words)

    compact: dict[str, Any] = {
        "ok": bool(payload.get("ok")),
        "success": bool(payload.get("success")),
        "display_text": display_text[:1200],
    }
    if "error" in payload:
        compact["error"] = str(payload.get("error", ""))
    for key in ("capture_error", "source", "frame_age_seconds", "frame_mtime", "bytes"):
        if key in payload:
            compact[key] = payload[key]

    if isinstance(words, list):
        compact_words: list[dict[str, str]] = []
        for word in words[:40]:
            if not isinstance(word, dict):
                continue
            compact_word = {
                "w": str(word.get("w", ""))[:160],
                "b": str(word.get("b", ""))[:160],
                "t": str(word.get("t", ""))[:240],
            }
            for key, limit in (("r", 80), ("f", 24), ("fv", 12), ("k", 96)):
                value = str(word.get(key, ""))[:limit]
                if value:
                    compact_word[key] = value
            compact_words.append(compact_word)
        compact["words"] = compact_words

    return compact


class UdpOcrServer:
    def __init__(self, host: str, port: int) -> None:
        self._address = (host, port)
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind(self._address)
        self._socket.settimeout(0.5)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, name="switch-ocr-udp", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._socket.close()

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                data, client = self._socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                if not self._stop.is_set():
                    print("[UDP] socket error while receiving", flush=True)
                break

            command = data.decode("utf-8", errors="replace").strip()
            print(f"{client[0]} - UDP {command or '<empty>'}", flush=True)
            if command != "ocr-latest":
                response = {"ok": False, "success": False, "error": f"Unknown UDP command: {command}"}
            else:
                try:
                    response = ocr_latest_payload()
                except Exception as exc:
                    response = {
                        "ok": False,
                        "success": False,
                        "error": f"OCR request failed: {exc}",
                        "display_text": "Mac OCR request failed.",
                    }

            try:
                self._socket.sendto(compact_udp_payload(response), client)
            except OSError as exc:
                print(f"[UDP] failed to reply to {client}: {exc}", flush=True)


class OcrDevHandler(BaseHTTPRequestHandler):
    server_version = "SwitchOCRDev/0.1"

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send_json({"ok": True})
            return

        self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:
        if self.path == "/hello":
            payload = self._read_json()
            self._send_json(
                {
                    "ok": True,
                    "message": "Hello from the Mac OCR server",
                    "received": payload,
                }
            )
            return

        if self.path == "/ocr":
            body = self._read_body()
            LAST_OCR_UPLOAD.write_bytes(body)
            content_type = self.headers.get("Content-Type", "image/jpeg").split(";", 1)[0]
            self._send_json(ocr_bytes_payload(body, content_type, LAST_OCR_UPLOAD))
            return

        if self.path == "/ocr-latest":
            payload = compact_switch_payload(ocr_latest_payload())
            status = HTTPStatus.OK if payload.get("ok") else HTTPStatus.SERVICE_UNAVAILABLE
            self._send_json(payload, status)
            return

        if self.path == "/log":
            payload = self._read_json()
            message = str(payload.get("message", payload.get("raw", "")))[:1000]
            print(f"[SwitchLog] {self.client_address[0]} - {message}", flush=True)
            self._send_json({"ok": True})
            return

        self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"{self.client_address[0]} - {fmt % args}", flush=True)

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length) if length else b""

    def _read_json(self) -> dict[str, Any]:
        body = self._read_body()
        if not body:
            return {}
        try:
            parsed = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return {"raw": body.decode("utf-8", errors="replace")}
        return parsed if isinstance(parsed, dict) else {"value": parsed}

    def _send_json(self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

def main() -> None:
    parser = argparse.ArgumentParser(description="Local dev server for Switch OCR prototypes.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--no-frame-capture", action="store_true")
    args = parser.parse_args()

    cleanup_capture_artifacts(SERVER_DIR)
    capture_worker: RollingBridgeCapture | None = None
    if not args.no_frame_capture and os.environ.get("SYSDVR_BACKGROUND_CAPTURE", "0") != "0":
        capture_worker = RollingBridgeCapture(
            LATEST_FRAME,
            config_from_env(),
            interval_seconds=float(os.environ.get("SYSDVR_CAPTURE_INTERVAL", "1.0")),
        )
        capture_worker.start()
        print(f"SysDVR bridge frame capture writing {LATEST_FRAME}", flush=True)

    httpd = ThreadingHTTPServer((args.host, args.port), OcrDevHandler)
    udp_server = UdpOcrServer(args.host, args.port)
    udp_server.start()
    print(f"Switch OCR dev server listening on http://{args.host}:{args.port}", flush=True)
    print(f"Switch OCR UDP server listening on udp://{args.host}:{args.port}", flush=True)
    try:
        httpd.serve_forever()
    finally:
        udp_server.stop()
        if capture_worker is not None:
            capture_worker.stop()


if __name__ == "__main__":
    main()
