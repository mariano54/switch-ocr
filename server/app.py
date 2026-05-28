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
from mining import MiningService, create_mining_service_from_env
from ocr import perform_ocr
from ocr.formatting import switch_display_text

SERVER_DIR = Path(__file__).resolve().parent
LAST_OCR_UPLOAD = SERVER_DIR / "last_ocr_upload.jpg"
SWITCH_SCREENSHOT_PROBE = SERVER_DIR / "switch_screenshot_probe.jpg"
SWITCH_SCREENSHOT_PROBE_META = SERVER_DIR / "switch_screenshot_probe.json"
LATEST_FRAME = SERVER_DIR / "latest_frame.jpg"
UDP_RESPONSE_LIMIT = 12_000
MINING_SERVICE: MiningService | None = None


def log(message: str) -> None:
    try:
        print(message, flush=True)
    except OSError:
        pass


def recent_frame_fallback_seconds() -> float:
    return float(os.environ.get("SYSDVR_RECENT_FRAME_FALLBACK_SECONDS", "2.0"))


def ready_frame_max_age_seconds() -> float:
    return float(os.environ.get("SYSDVR_READY_FRAME_MAX_AGE_SECONDS", "1.5"))


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


def latest_frame_age_seconds() -> float | None:
    if not LATEST_FRAME.exists():
        return None
    return time.time() - LATEST_FRAME.stat().st_mtime


def latest_frame_status_payload() -> dict[str, Any]:
    frame_age = latest_frame_age_seconds()
    max_ready_age = ready_frame_max_age_seconds()
    ready = frame_age is not None and max_ready_age > 0 and frame_age <= max_ready_age
    return {
        "ok": ready,
        "ready": ready,
        "max_ready_age_seconds": max_ready_age,
        **latest_frame_metadata(),
    }


def concise_error(message: str) -> str:
    return message.splitlines()[0].strip() if message.strip() else "Unknown error"


def ocr_latest_payload() -> dict[str, Any]:
    started = time.time()
    capture_error = ""
    capture_performed = False
    if os.environ.get("SYSDVR_CAPTURE_ON_REQUEST", "1") != "0":
        frame_age = latest_frame_age_seconds()
        max_ready_age = ready_frame_max_age_seconds()
        if frame_age is None or max_ready_age <= 0 or frame_age > max_ready_age:
            try:
                capture_bridge_frame(LATEST_FRAME, config_from_env())
                capture_performed = True
            except Exception as exc:
                capture_error = str(exc)
                log(f"[FrameCapture] on-request capture failed: {exc}")
                fallback_seconds = recent_frame_fallback_seconds()
                frame_age = latest_frame_age_seconds()
                if frame_age is None or frame_age > fallback_seconds:
                    return {
                        "ok": False,
                        "success": False,
                        "error": f"Frame capture failed: {concise_error(capture_error)}",
                        "capture_error": capture_error,
                        "display_text": "Mac frame capture failed.",
                        **latest_frame_metadata(),
                    }
                log(f"[FrameCapture] using recent frame after capture failure, age={frame_age:.2f}s")

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
    payload["capture_performed"] = capture_performed
    payload["total_elapsed_seconds"] = round(time.time() - started, 3)
    if capture_error:
        payload["capture_error"] = capture_error
        payload["frame_fallback"] = "recent"
    log(f"[SwitchOCR] /ocr-latest total {time.time() - started:.2f}s, bytes={len(body)}")
    return payload


def ocr_bytes_payload(body: bytes, content_type: str, source_path: Path) -> dict[str, Any]:
    started = time.time()
    result = perform_ocr(body, mime_type=content_type)
    result["ok"] = bool(result.get("success"))
    result["bytes"] = len(body)
    result["source"] = str(source_path)
    result["ocr_elapsed_seconds"] = round(time.time() - started, 3)
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
    for key in (
        "capture_error",
        "capture_performed",
        "frame_fallback",
        "source",
        "frame_age_seconds",
        "frame_mtime",
        "bytes",
        "ocr_elapsed_seconds",
        "total_elapsed_seconds",
    ):
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


def mining_service() -> MiningService:
    global MINING_SERVICE
    if MINING_SERVICE is None:
        MINING_SERVICE = create_mining_service_from_env()
    return MINING_SERVICE


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
                    log("[UDP] socket error while receiving")
                break

            command = data.decode("utf-8", errors="replace").strip()
            log(f"{client[0]} - UDP {command or '<empty>'}")
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
                log(f"[UDP] failed to reply to {client}: {exc}")


class OcrDevHandler(BaseHTTPRequestHandler):
    server_version = "SwitchOCRDev/0.1"

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send_json({"ok": True})
            return

        if self.path == "/mining/status":
            self._send_json(mining_service().status_payload(include_saved_words=True))
            return

        if self.path == "/frame-status":
            self._send_json(latest_frame_status_payload())
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

        if self.path == "/ocr-upload":
            body = self._read_body()
            if not body:
                self._send_json(
                    {
                        "ok": False,
                        "success": False,
                        "error": "Empty OCR upload body.",
                        "display_text": "Switch screenshot upload was empty.",
                        "bytes": 0,
                        "source": str(LAST_OCR_UPLOAD),
                    },
                    HTTPStatus.BAD_REQUEST,
                )
                return
            LAST_OCR_UPLOAD.write_bytes(body)
            content_type = self.headers.get("Content-Type", "image/jpeg").split(";", 1)[0]
            try:
                payload = compact_switch_payload(ocr_bytes_payload(body, content_type, LAST_OCR_UPLOAD))
            except Exception as exc:
                log(f"[SwitchOCR] /ocr-upload failed: {exc}")
                payload = {
                    "ok": False,
                    "success": False,
                    "error": f"OCR request failed: {concise_error(str(exc))}",
                    "display_text": "Mac OCR request failed.",
                    "bytes": len(body),
                    "source": str(LAST_OCR_UPLOAD),
                }
            status = HTTPStatus.OK if payload.get("ok") else HTTPStatus.SERVICE_UNAVAILABLE
            self._send_json(payload, status)
            return

        if self.path == "/screenshot-probe":
            body = self._read_body()
            SWITCH_SCREENSHOT_PROBE.write_bytes(body)
            payload = {
                "ok": True,
                "bytes": len(body),
                "content_type": self.headers.get("Content-Type", "application/octet-stream").split(";", 1)[0],
                "saved_to": str(SWITCH_SCREENSHOT_PROBE),
                "received_at": time.time(),
            }
            SWITCH_SCREENSHOT_PROBE_META.write_text(json.dumps(payload, indent=2), encoding="utf-8")
            log(f"[SwitchScreenshotProbe] saved {len(body)} bytes to {SWITCH_SCREENSHOT_PROBE}")
            self._send_json(payload)
            return

        if self.path == "/ocr-latest":
            try:
                payload = compact_switch_payload(ocr_latest_payload())
            except Exception as exc:
                log(f"[SwitchOCR] /ocr-latest failed: {exc}")
                payload = {
                    "ok": False,
                    "success": False,
                    "error": f"OCR request failed: {concise_error(str(exc))}",
                    "display_text": "Mac OCR request failed.",
                }
            status = HTTPStatus.OK if payload.get("ok") else HTTPStatus.SERVICE_UNAVAILABLE
            self._send_json(payload, status)
            return

        if self.path == "/log":
            payload = self._read_json()
            message = str(payload.get("message", payload.get("raw", "")))[:1000]
            log(f"[SwitchLog] {self.client_address[0]} - {message}")
            self._send_json({"ok": True})
            return

        if self.path == "/mine-word":
            payload = mining_service().mine_word(self._read_json())
            status = HTTPStatus.OK if payload.get("ok") else HTTPStatus.SERVICE_UNAVAILABLE
            self._send_json(payload, status)
            return

        self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def log_message(self, fmt: str, *args: Any) -> None:
        try:
            log(f"{self.client_address[0]} - {fmt % args}")
        except BrokenPipeError:
            pass

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
    parser.add_argument("--port", type=int, default=8742)
    parser.add_argument("--no-frame-capture", action="store_true")
    args = parser.parse_args()

    cleanup_capture_artifacts(SERVER_DIR)
    global MINING_SERVICE
    MINING_SERVICE = create_mining_service_from_env()
    MINING_SERVICE.startup()
    mining_status = MINING_SERVICE.status_payload()
    if mining_status.get("ok"):
        log(f"[Mining] {mining_status['provider']} ready with {mining_status['saved_count']} saved words")
    else:
        log(f"[Mining] unavailable: {mining_status.get('error', 'not configured')}")

    capture_worker: RollingBridgeCapture | None = None
    if not args.no_frame_capture and os.environ.get("SYSDVR_BACKGROUND_CAPTURE", "0") != "0":
        capture_worker = RollingBridgeCapture(
            LATEST_FRAME,
            config_from_env(),
            interval_seconds=float(os.environ.get("SYSDVR_CAPTURE_INTERVAL", "1.0")),
        )
        capture_worker.start()
        log(f"SysDVR bridge frame capture writing {LATEST_FRAME}")

    httpd = ThreadingHTTPServer((args.host, args.port), OcrDevHandler)
    udp_server = UdpOcrServer(args.host, args.port)
    udp_server.start()
    log(f"Switch OCR dev server listening on http://{args.host}:{args.port}")
    log(f"Switch OCR UDP server listening on udp://{args.host}:{args.port}")
    try:
        httpd.serve_forever()
    finally:
        udp_server.stop()
        if capture_worker is not None:
            capture_worker.stop()


if __name__ == "__main__":
    main()
