from __future__ import annotations

import hashlib
import json
from http.cookiejar import CookieJar
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import HTTPCookieProcessor, Request, build_opener

from .provider import MiningProviderError, MiningWord, SavedWord


class IssenMiningProvider:
    name = "issen"

    def __init__(self, email: str, password: str, domain: str = "https://app.issen.com", timeout_seconds: float = 10.0) -> None:
        self.email = email
        self.password = password
        self.domain = domain.rstrip("/")
        self.timeout_seconds = timeout_seconds
        self._opener = build_opener(HTTPCookieProcessor(CookieJar()))

    def login(self) -> None:
        prehashed = hashlib.sha256(self.password.encode("utf-8")).hexdigest()
        response = self._post("/login", {"username": self.email, "prehashed": prehashed})
        if not response.get("success"):
            raise MiningProviderError(str(response.get("message") or "ISSEN login failed"))

    def load_saved_words(self, language: str) -> list[SavedWord]:
        response = self._post("/get_words", {"getDefinitions": False, "language": language})
        words = response.get("words")
        if not isinstance(words, list):
            return []
        saved: list[SavedWord] = []
        for item in words:
            if isinstance(item, dict):
                word = str(item.get("word", "")).strip()
                if word:
                    saved.append(SavedWord(word=word))
        return saved

    def add_word(self, word: MiningWord) -> None:
        term = word.term
        if not term:
            raise MiningProviderError("No word selected")
        self._post("/add_words", {"language": word.language, "words": [{"word": term, "sentence": word.sentence}]})

    def _post(self, path: str, payload: dict[str, Any]) -> dict[str, Any]:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        request = Request(
            self.domain + path,
            data=body,
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "Client-Platform": "web",
                "Client-Version": "1.13.6",
            },
        )
        try:
            with self._opener.open(request, timeout=self.timeout_seconds) as response:
                raw = response.read().decode("utf-8")
        except HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise MiningProviderError(f"ISSEN HTTP {exc.code}: {detail[:200]}") from exc
        except URLError as exc:
            raise MiningProviderError(f"ISSEN request failed: {exc.reason}") from exc

        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError as exc:
            raise MiningProviderError("ISSEN returned invalid JSON") from exc
        if not isinstance(parsed, dict):
            raise MiningProviderError("ISSEN returned an unexpected response")
        return parsed
