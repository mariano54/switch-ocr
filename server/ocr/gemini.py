from __future__ import annotations

import base64
import json
import re
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
                "maxOutputTokens": 8192,
                "responseMimeType": "application/json",
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
            f"{quote(self.model, safe='')}:generateContent?key={quote(api_key, safe='')}"
        )
        request = Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        try:
            with urlopen(request, timeout=120) as response:
                raw_response = json.loads(response.read().decode("utf-8"))
        except HTTPError as error:
            return self._error(f"Gemini HTTP {error.code}: {error.read().decode('utf-8', errors='replace')}")
        except (URLError, TimeoutError, json.JSONDecodeError) as error:
            return self._error(f"Gemini request failed: {error}")

        response_text = self._extract_text(raw_response)
        if not response_text:
            return self._error("Gemini returned no text content", raw_response)

        try:
            parsed = json.loads(response_text)
        except json.JSONDecodeError:
            parsed = self._parse_json_fallback(response_text)
            if parsed is None:
                return self._error("Gemini returned invalid JSON", response_text)

        words = enrich_words(normalize_words(parsed.get("words") if isinstance(parsed, dict) else []))
        elapsed = time.time() - started
        print(f"[GeminiOCR] {elapsed:.2f}s, words={len(words)}", flush=True)
        return {
            "success": True,
            "provider": self.name,
            "model": self.model,
            "words": words,
            "text": display_text(words),
            "display_text": self._switch_display(words),
            "definitions": definition_lines(words),
        }

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
        print(f"[GeminiOCR] ERROR: {message}", flush=True)
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
