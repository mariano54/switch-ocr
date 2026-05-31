from __future__ import annotations

import base64
import json
import re
import socket
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

from .config import gemini_model, google_api_key
from .enrichment import enrich_words
from .formatting import definition_lines, display_text, normalize_words, switch_display_text
from .prompts import OCR_PROMPT, OCR_SYSTEM_INSTRUCTION
from .types import OcrResult


JSON_BLOCK_RE = re.compile(r"```(?:json)?\s*(\{.*?\})\s*```", re.DOTALL)

# We stream the response (SSE) so a stall is detected by the gap between chunks
# rather than waiting for the whole reply. The connection timeout must be
# generous enough to cover connecting plus uploading the (~180 KB) image plus
# the first response, otherwise a slow upload trips the timeout and the request
# is retried (re-uploading), which spirals under load. Once the stream is
# flowing, the per-chunk read timeout is tightened so a silent connection is
# cancelled quickly.
GEMINI_CONNECT_TIMEOUT_SECONDS = 10.0     # connect + image upload + first response
GEMINI_STREAM_READ_TIMEOUT_SECONDS = 5.0  # max gap between streamed chunks
GEMINI_TOTAL_DEADLINE_SECONDS = 16.0      # hard cap across all attempts
GEMINI_MAX_ATTEMPTS = 3
GEMINI_RETRY_STATUS = {429, 500, 502, 503, 504}


def _log(message: str) -> None:
    try:
        print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)
    except OSError:
        pass

OCR_RESPONSE_SCHEMA: dict[str, Any] = {
    "type": "OBJECT",
    "properties": {
        "words": {
            "type": "ARRAY",
            "items": {
                "type": "OBJECT",
                "properties": {
                    "w": {"type": "STRING"},
                    "b": {"type": "STRING"},
                    "t": {"type": "STRING"},
                },
                "required": ["w", "b", "t"],
            },
        },
    },
    "required": ["words"],
}


class GeminiOcrProvider:
    name = "gemini"

    def __init__(self) -> None:
        self.model = gemini_model()

    def perform_ocr(self, image_bytes: bytes, mime_type: str = "image/jpeg") -> OcrResult:
        api_key = google_api_key()
        if not api_key:
            return {
                "success": False,
                "provider": self.name,
                "model": self.model,
                "words": [],
                "error": "Missing GOOGLE_AI_STUDIO_API_KEY or GOOGLE_API_KEY",
            }

        started = time.time()
        payload = {
            "systemInstruction": {"parts": [{"text": OCR_SYSTEM_INSTRUCTION}]},
            "contents": [
                {
                    "role": "user",
                    "parts": [
                        {"text": OCR_PROMPT},
                        {
                            "inlineData": {
                                "mimeType": mime_type,
                                "data": base64.b64encode(image_bytes).decode("ascii"),
                            }
                        },
                    ],
                }
            ],
            "generationConfig": {
            "temperature": 0.1,
            "maxOutputTokens": 4096,
                "responseMimeType": "application/json",
                "responseSchema": OCR_RESPONSE_SCHEMA,
                "candidateCount": 1,
                "thinkingConfig": {
                    "thinkingBudget": 0,
                    "includeThoughts": False,
                },
            },
            "safetySettings": [
                {"category": "HARM_CATEGORY_HARASSMENT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_HATE_SPEECH", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_SEXUALLY_EXPLICIT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_DANGEROUS_CONTENT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_CIVIC_INTEGRITY", "threshold": "BLOCK_NONE"},
            ],
        }

        url = (
            "https://generativelanguage.googleapis.com/v1beta/models/"
            f"{quote(self.model, safe='')}:streamGenerateContent?alt=sse&key={quote(api_key, safe='')}"
        )
        request = Request(
            url,
            data=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        response_text, error = self._stream_text(request)
        if error is not None:
            return self._error(error)
        if not response_text:
            return self._error("Gemini returned no text content")

        try:
            parsed = json.loads(response_text)
        except json.JSONDecodeError:
            parsed = self._parse_json_fallback(response_text)
            if parsed is None:
                return self._error("Gemini returned invalid JSON", response_text)

        words = enrich_words(normalize_words(parsed.get("words") if isinstance(parsed, dict) else []))
        elapsed = time.time() - started
        _log(f"[GeminiOCR] model={self.model} bytes={len(image_bytes)} total={elapsed:.2f}s words={len(words)}")
        return {
            "success": True,
            "provider": self.name,
            "model": self.model,
            "words": words,
            "text": display_text(words),
            "display_text": self._switch_display(words),
            "definitions": definition_lines(words),
        }

    def _stream_text(self, request: Request) -> tuple[str | None, str | None]:
        """Streams the model output, returning (text, None) on success or
        (None, error) on failure.

        The socket read timeout (GEMINI_STREAM_READ_TIMEOUT_SECONDS) bounds the
        wait for each streamed chunk, so a connection that goes silent is
        cancelled within seconds rather than hanging. Transient failures are
        retried up to GEMINI_MAX_ATTEMPTS within GEMINI_TOTAL_DEADLINE_SECONDS.
        """
        deadline = time.time() + GEMINI_TOTAL_DEADLINE_SECONDS
        last_error = "Gemini request failed"
        for attempt in range(1, GEMINI_MAX_ATTEMPTS + 1):
            if time.time() >= deadline:
                break
            started = time.time()
            chunks: list[str] = []
            try:
                with urlopen(request, timeout=GEMINI_CONNECT_TIMEOUT_SECONDS) as response:
                    # Connect/upload used the generous timeout above; now that the
                    # response is streaming, tighten the per-chunk read timeout so a
                    # stalled stream is cancelled quickly.
                    raw_sock = getattr(getattr(getattr(response, "fp", None), "raw", None), "_sock", None)
                    if raw_sock is not None:
                        try:
                            raw_sock.settimeout(GEMINI_STREAM_READ_TIMEOUT_SECONDS)
                        except OSError:
                            pass
                    for raw_line in response:
                        if time.time() >= deadline:
                            raise TimeoutError("exceeded total deadline")
                        line = raw_line.decode("utf-8", errors="replace").strip()
                        if not line or not line.startswith("data:"):
                            continue
                        data = line[len("data:"):].strip()
                        if data == "[DONE]":
                            break
                        fragment = self._chunk_text(data)
                        if fragment:
                            chunks.append(fragment)
                text = "".join(chunks)
                if text:
                    _log(f"[GeminiOCR] stream ok in {time.time() - started:.2f}s (attempt {attempt})")
                    return text, None
                last_error = "Gemini returned no streamed text"
            except HTTPError as error:
                detail = error.read().decode("utf-8", errors="replace")
                last_error = f"Gemini HTTP {error.code}: {detail}"
                if error.code not in GEMINI_RETRY_STATUS:
                    return None, last_error
            except (socket.timeout, TimeoutError, URLError) as error:
                last_error = f"Gemini stream stalled: {error}"
            _log(f"[GeminiOCR] attempt {attempt}/{GEMINI_MAX_ATTEMPTS} failed in {time.time() - started:.2f}s: {last_error}")

        return None, last_error

    def _chunk_text(self, data: str) -> str:
        try:
            parsed = json.loads(data)
        except json.JSONDecodeError:
            return ""
        if not isinstance(parsed, dict):
            return ""
        return self._extract_text(parsed)

    def _error(self, message: str, raw: Any | None = None) -> OcrResult:
        result: OcrResult = {
            "success": False,
            "provider": self.name,
            "model": self.model,
            "words": [],
            "error": message,
        }
        if raw is not None:
            result["raw"] = raw
        _log(f"[GeminiOCR] ERROR: {message}")
        return result

    def _extract_text(self, response: dict[str, Any]) -> str:
        candidates = response.get("candidates")
        if not isinstance(candidates, list) or not candidates:
            return ""

        parts = candidates[0].get("content", {}).get("parts", [])
        if not isinstance(parts, list):
            return ""

        return "".join(part.get("text", "") for part in parts if isinstance(part, dict))

    def _parse_json_fallback(self, text: str) -> dict[str, Any] | None:
        match = JSON_BLOCK_RE.search(text)
        if match:
            try:
                return json.loads(match.group(1))
            except json.JSONDecodeError:
                return None

        start = text.find("{")
        end = text.rfind("}")
        if start >= 0 and end > start:
            try:
                return json.loads(text[start : end + 1])
            except json.JSONDecodeError:
                return None
        return None

    def _switch_display(self, words: list[dict[str, str]]) -> str:
        text = switch_display_text(words)
        definitions = definition_lines(words)
        if not definitions:
            return text or "No active dialogue detected."

        return "\n".join([text, "", *definitions[:12]])
