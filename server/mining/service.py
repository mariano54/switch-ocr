from __future__ import annotations

import os
import threading
from pathlib import Path
from typing import Any

from ocr.config import load_env

from .issen import IssenMiningProvider
from .provider import MiningProvider, MiningProviderError, MiningWord, mining_key


PROJECT_ROOT = Path(__file__).resolve().parents[2]
load_env(PROJECT_ROOT / ".env")


class MiningService:
    def __init__(self, provider: MiningProvider | None, language: str = "japanese") -> None:
        self.provider = provider
        self.language = language
        self._lock = threading.Lock()
        self._logged_in = False
        self._last_error = ""
        self._saved_count = 0
        self._saved_words: set[str] = set()
        self._lookup_count = 0

    @property
    def provider_name(self) -> str:
        return self.provider.name if self.provider is not None else "none"

    def startup(self) -> None:
        if self.provider is None:
            with self._lock:
                self._last_error = "ISSEN credentials missing"
            return
        try:
            self.provider.login()
            with self._lock:
                self._logged_in = True
                self._last_error = ""
            self.refresh_saved_words()
        except MiningProviderError as exc:
            with self._lock:
                self._logged_in = False
                self._last_error = str(exc)

    def refresh_saved_words(self) -> None:
        if self.provider is None:
            return
        words = self.provider.load_saved_words(self.language)
        saved_keys = {mining_key(word.word) for word in words if mining_key(word.word)}
        with self._lock:
            self._saved_words = saved_keys
            self._saved_count = len(words)
            self._last_error = ""

    def status_payload(self, include_saved_words: bool = False) -> dict[str, Any]:
        with self._lock:
            payload: dict[str, Any] = {
                "ok": self.provider is not None and self._logged_in,
                "provider": self.provider_name,
                "logged_in": self._logged_in,
                "saved_count": self._saved_count,
                "lookup_count": self._lookup_count,
                "error": self._last_error,
            }
            if include_saved_words:
                payload["saved_words"] = sorted(self._saved_words)
            return payload

    def mine_word(self, payload: dict[str, Any]) -> dict[str, Any]:
        word = MiningWord(
            surface=str(payload.get("surface", "")),
            reading=str(payload.get("reading", "")),
            base=str(payload.get("base", "")),
            definition=str(payload.get("definition", "")),
            sentence=str(payload.get("sentence", "")),
            language=str(payload.get("language") or self.language),
        )
        term = word.term
        key = mining_key(term)
        if not key:
            return {**self.status_payload(), "ok": False, "error": "No word selected"}

        with self._lock:
            already_saved = key in self._saved_words
        if already_saved:
            return {**self.status_payload(), "ok": True, "saved": True, "already_saved": True}

        if self.provider is None:
            return {**self.status_payload(), "ok": False, "error": "No mining provider configured"}
        with self._lock:
            logged_in = self._logged_in
        if not logged_in:
            return {**self.status_payload(), "ok": False, "error": "Mining provider is not logged in"}

        try:
            self.provider.add_word(word)
        except MiningProviderError as exc:
            with self._lock:
                self._last_error = str(exc)
            return {**self.status_payload(), "ok": False, "error": str(exc)}

        with self._lock:
            self._saved_words.add(key)
            self._saved_count = max(self._saved_count + 1, len(self._saved_words))
        return {**self.status_payload(include_saved_words=True), "ok": True, "saved": True, "already_saved": False}


def create_mining_service_from_env() -> MiningService:
    email = os.getenv("ISSEN_EMAIL", "").strip()
    password = os.getenv("ISSEN_PASSWORD", "")
    language = os.getenv("ISSEN_LANGUAGE", "japanese").strip() or "japanese"
    if not email or not password:
        return MiningService(None, language=language)
    provider = IssenMiningProvider(
        email=email,
        password=password,
        domain=os.getenv("ISSEN_API_URL", "https://app.issen.com"),
        timeout_seconds=float(os.getenv("ISSEN_TIMEOUT_SECONDS", "10")),
    )
    return MiningService(provider, language=language)
