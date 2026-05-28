#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ipaddress
import json
import os
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from auth import max_upload_bytes, verify_request
from mining import MiningService, create_mining_service_from_env
from ocr import perform_ocr
from ocr.formatting import switch_display_text

SERVER_DIR = Path(__file__).resolve().parent
LAST_OCR_UPLOAD = SERVER_DIR / "last_ocr_upload.jpg"
SWITCH_SCREENSHOT_PROBE = SERVER_DIR / "switch_screenshot_probe.jpg"
SWITCH_SCREENSHOT_PROBE_META = SERVER_DIR / "switch_screenshot_probe.json"
MINING_SERVICE: MiningService | None = None


def log(message: str) -> None:
    try:
        print(message, flush=True)
    except OSError:
        pass


def allow_lan_unauth() -> bool:
    return os.environ.get("SWITCHOCR_ALLOW_LAN_UNAUTH", "1") != "0"


def is_private_lan_client(address: str) -> bool:
    try:
        ip = ipaddress.ip_address(address)
    except ValueError:
        return False
    return ip.is_private and not ip.is_loopback


def concise_error(message: str) -> str:
    return message.splitlines()[0].strip() if message.strip() else "Unknown error"


def ocr_bytes_payload(body: bytes, content_type: str, source_path: Path) -> dict[str, Any]:
    started = time.time()
    result = perform_ocr(body, mime_type=content_type)
    result["ok"] = bool(result.get("success"))
    result["bytes"] = len(body)
    result["source"] = str(source_path)
    result["ocr_elapsed_seconds"] = round(time.time() - started, 3)
    return result


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
    for key in ("source", "bytes", "ocr_elapsed_seconds"):
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


class OcrDevHandler(BaseHTTPRequestHandler):
    server_version = "SwitchOCRDev/0.1"
    protected_get_paths = {"/mining/status"}
    protected_post_paths = {"/ocr-upload", "/mine-word", "/ocr", "/screenshot-probe", "/log"}

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send_json({"ok": True})
            return

        if self._is_protected_get() and not self._authorize(b""):
            return

        if self.path == "/mining/status":
            self._send_json(mining_service().status_payload(include_saved_words=True))
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
            if not self._authorize(body):
                return
            LAST_OCR_UPLOAD.write_bytes(body)
            content_type = self.headers.get("Content-Type", "image/jpeg").split(";", 1)[0]
            self._send_json(ocr_bytes_payload(body, content_type, LAST_OCR_UPLOAD))
            return

        if self.path == "/ocr-upload":
            body = self._read_body()
            if not self._authorize(body):
                return
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
            if not self._authorize(body):
                return
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

        if self.path == "/log":
            body = self._read_body()
            if not self._authorize(body):
                return
            payload = self._parse_json_body(body)
            message = str(payload.get("message", payload.get("raw", "")))[:1000]
            log(f"[SwitchLog] {self.client_address[0]} - {message}")
            self._send_json({"ok": True})
            return

        if self.path == "/mine-word":
            body = self._read_body()
            if not self._authorize(body):
                return
            payload = mining_service().mine_word(self._parse_json_body(body))
            status = HTTPStatus.OK if payload.get("ok") else HTTPStatus.SERVICE_UNAVAILABLE
            self._send_json(payload, status)
            return

        self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def log_message(self, fmt: str, *args: Any) -> None:
        try:
            log(f"{self.client_address[0]} - {fmt % args}")
        except BrokenPipeError:
            pass

    def _is_protected_get(self) -> bool:
        return self.path.split("?", 1)[0] in self.protected_get_paths

    def _is_protected_post(self) -> bool:
        return self.path.split("?", 1)[0] in self.protected_post_paths

    def _authorize(self, body: bytes) -> bool:
        if getattr(self, "_body_error_sent", False):
            return False
        if not self._is_protected_get() and not self._is_protected_post():
            return True
        if allow_lan_unauth() and is_private_lan_client(self.client_address[0]):
            return True
        result = verify_request(self.command, self.path, self.headers, body)
        if result.ok:
            return True
        self._send_json({"ok": False, "success": False, "error": result.error}, result.status)
        return False

    def _read_body(self) -> bytes:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._body_error_sent = True
            self._send_json({"ok": False, "success": False, "error": "Invalid Content-Length."}, HTTPStatus.BAD_REQUEST)
            return b""
        if length > max_upload_bytes():
            self._body_error_sent = True
            self._send_json({"ok": False, "success": False, "error": "Request body too large."}, HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
            return b""
        return self.rfile.read(length) if length else b""

    def _read_json(self) -> dict[str, Any]:
        return self._parse_json_body(self._read_body())

    def _parse_json_body(self, body: bytes) -> dict[str, Any]:
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
    args = parser.parse_args()

    global MINING_SERVICE
    MINING_SERVICE = create_mining_service_from_env()
    MINING_SERVICE.startup()
    mining_status = MINING_SERVICE.status_payload()
    if mining_status.get("ok"):
        log(f"[Mining] {mining_status['provider']} ready with {mining_status['saved_count']} saved words")
    else:
        log(f"[Mining] unavailable: {mining_status.get('error', 'not configured')}")

    httpd = ThreadingHTTPServer((args.host, args.port), OcrDevHandler)
    log(f"Switch OCR dev server listening on http://{args.host}:{args.port}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
