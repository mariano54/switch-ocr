from __future__ import annotations

import hashlib
import hmac
import json
import os
import threading
import time
from collections import deque
from dataclasses import dataclass
from http import HTTPStatus
from pathlib import Path
from typing import Mapping
from urllib.parse import urlsplit


AUTH_SCHEME = "SWITCHOCR-HMAC-SHA256"
DEFAULT_MAX_UPLOAD_BYTES = 5 * 1024 * 1024
DEFAULT_TIMESTAMP_SKEW_SECONDS = 100 * 365 * 24 * 60 * 60
DEFAULT_NONCE_TTL_SECONDS = 24 * 60 * 60
DEFAULT_RATE_LIMIT_PER_MINUTE = 60


@dataclass(frozen=True)
class AuthResult:
    ok: bool
    status: HTTPStatus = HTTPStatus.OK
    error: str = ""
    key_id: str = ""


_nonce_lock = threading.Lock()
_seen_nonces: dict[tuple[str, str], float] = {}
_rate_events: dict[str, deque[float]] = {}


def api_keys_path() -> Path:
    return Path(os.environ.get("SWITCHOCR_API_KEYS_PATH", "~/.switchocr/api_keys.json")).expanduser()


def max_upload_bytes() -> int:
    return int(os.environ.get("SWITCHOCR_MAX_UPLOAD_BYTES", str(DEFAULT_MAX_UPLOAD_BYTES)))


def _timestamp_skew_seconds() -> int:
    return int(os.environ.get("SWITCHOCR_TIMESTAMP_SKEW_SECONDS", str(DEFAULT_TIMESTAMP_SKEW_SECONDS)))


def _nonce_ttl_seconds() -> int:
    return int(os.environ.get("SWITCHOCR_NONCE_TTL_SECONDS", str(DEFAULT_NONCE_TTL_SECONDS)))


def _rate_limit_per_minute() -> int:
    return int(os.environ.get("SWITCHOCR_RATE_LIMIT_PER_MINUTE", str(DEFAULT_RATE_LIMIT_PER_MINUTE)))


def _load_keys() -> dict[str, str]:
    path = api_keys_path()
    if not path.exists():
        return {}

    data = json.loads(path.read_text(encoding="utf-8"))
    keys = data.get("keys", [])
    if not isinstance(keys, list):
        return {}

    result: dict[str, str] = {}
    for item in keys:
        if not isinstance(item, dict):
            continue
        key_id = str(item.get("id", "")).strip()
        secret = str(item.get("secret", "")).strip()
        if key_id and secret:
            result[key_id] = secret
    return result


def _canonical_request(method: str, path: str, timestamp: str, nonce: str, body_sha256: str) -> bytes:
    return "\n".join((AUTH_SCHEME, method.upper(), path, timestamp, nonce, body_sha256)).encode("utf-8")


def _header(headers: Mapping[str, str], key: str) -> str:
    value = headers.get(key, "")
    return value.strip() if isinstance(value, str) else ""


def _check_replay_and_rate(key_id: str, nonce: str, now: float) -> AuthResult:
    ttl = _nonce_ttl_seconds()
    limit = _rate_limit_per_minute()
    with _nonce_lock:
        expired = [key for key, expires_at in _seen_nonces.items() if expires_at <= now]
        for key in expired:
            _seen_nonces.pop(key, None)

        nonce_key = (key_id, nonce)
        if nonce_key in _seen_nonces:
            return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Replay nonce rejected.", key_id)
        _seen_nonces[nonce_key] = now + ttl

        if limit > 0:
            events = _rate_events.setdefault(key_id, deque())
            while events and events[0] <= now - 60:
                events.popleft()
            if len(events) >= limit:
                return AuthResult(False, HTTPStatus.TOO_MANY_REQUESTS, "Rate limit exceeded.", key_id)
            events.append(now)

    return AuthResult(True, key_id=key_id)


def verify_request(method: str, raw_path: str, headers: Mapping[str, str], body: bytes) -> AuthResult:
    keys = _load_keys()
    if not keys:
        return AuthResult(False, HTTPStatus.SERVICE_UNAVAILABLE, "No API keys configured.")

    key_id = _header(headers, "X-SwitchOCR-Key-Id")
    timestamp = _header(headers, "X-SwitchOCR-Timestamp")
    nonce = _header(headers, "X-SwitchOCR-Nonce")
    body_sha256 = _header(headers, "X-SwitchOCR-Content-SHA256")
    signature = _header(headers, "X-SwitchOCR-Signature")
    if not all((key_id, timestamp, nonce, body_sha256, signature)):
        return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Missing authentication headers.")

    secret = keys.get(key_id)
    if secret is None:
        return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Unknown API key.")

    expected_body_sha256 = hashlib.sha256(body).hexdigest()
    if not hmac.compare_digest(body_sha256, expected_body_sha256):
        return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Body hash mismatch.", key_id)

    try:
        request_time = int(timestamp)
    except ValueError:
        return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Invalid timestamp.", key_id)

    now = time.time()
    if abs(now - request_time) > _timestamp_skew_seconds():
        return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Request timestamp outside allowed window.", key_id)

    path = urlsplit(raw_path).path or "/"
    canonical = _canonical_request(method, path, timestamp, nonce, body_sha256)
    expected_signature = hmac.new(secret.encode("utf-8"), canonical, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(signature, expected_signature):
        return AuthResult(False, HTTPStatus.UNAUTHORIZED, "Invalid signature.", key_id)

    return _check_replay_and_rate(key_id, nonce, now)
