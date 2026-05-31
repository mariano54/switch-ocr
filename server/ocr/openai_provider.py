from __future__ import annotations

import base64
import json
import re
import socket
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from .config import openai_api_key, openai_model
from .enrichment import enrich_words
from .formatting import definition_lines, display_text, normalize_words, switch_display_text
from .prompts import OCR_PROMPT, OCR_SYSTEM_INSTRUCTION
from .types import OcrResult

JSON_BLOCK_RE = re.compile(r"```(?:json)?\s*(\{.*?\})\s*```", re.DOTALL)

# Mirror the Gemini provider: stream the response so a stall is caught by the
# gap between chunks, with a hard total deadline and retries on transient errors.
OPENAI_STREAM_READ_TIMEOUT_SECONDS = 6.0
OPENAI_TOTAL_DEADLINE_SECONDS = 20.0
OPENAI_MAX_ATTEMPTS = 3
OPENAI_RETRY_STATUS = {429, 500, 502, 503, 504}
OPENAI_MAX_OUTPUT_TOKENS = 4096

# Standard (strict) JSON schema for the OpenAI Responses API structured output.
OCR_RESPONSE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["words"],
    "properties": {
        "words": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["w", "b", "t"],
                "properties": {
                    "w": {"type": "string"},
                    "b": {"type": "string"},
                    "t": {"type": "string"},
                },
            },
        },
    },
}


def _log(message: str) -> None:
    try:
        print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)
    except OSError:
        pass


class OpenAiOcrProvider:
    name = "openai"

    def __init__(self) -> None:
        self.model = openai_model()

    def perform_ocr(self, image_bytes: bytes, mime_type: str = "image/jpeg") -> OcrResult:
        api_key = openai_api_key()
        if not api_key:
            return self._error("Missing OAI_KEY or OPENAI_API_KEY")

        data_url = f"data:{mime_type};base64,{base64.b64encode(image_bytes).decode('ascii')}"
        payload: dict[str, Any] = {
            "model": self.model,
            "input": [
                {"role": "system", "content": [{"type": "input_text", "text": OCR_SYSTEM_INSTRUCTION}]},
                {
                    "role": "user",
                    "content": [
                        {"type": "input_text", "text": OCR_PROMPT},
                        {"type": "input_image", "image_url": data_url},
                    ],
                },
            ],
            "stream": True,
            "max_output_tokens": OPENAI_MAX_OUTPUT_TOKENS,
            "text": {
                "format": {
                    "type": "json_schema",
                    "name": "ocr_words",
                    "strict": True,
                    "schema": OCR_RESPONSE_SCHEMA,
                }
            },
        }
        # Chat models do not reason; reasoning models run at minimal effort.
        if "chat" not in self.model:
            payload["reasoning"] = {"effort": "minimal"}

        request = Request(
            "https://api.openai.com/v1/responses",
            data=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"},
            method="POST",
        )

        started = time.time()
        response_text, error = self._stream_text(request)
        if error is not None:
            return self._error(error)
        if not response_text:
            return self._error("OpenAI returned no text content")

        try:
            parsed = json.loads(response_text)
        except json.JSONDecodeError:
            parsed = self._parse_json_fallback(response_text)
            if parsed is None:
                return self._error("OpenAI returned invalid JSON", response_text)

        words = enrich_words(normalize_words(parsed.get("words") if isinstance(parsed, dict) else []))
        elapsed = time.time() - started
        _log(f"[OpenAiOCR] model={self.model} bytes={len(image_bytes)} total={elapsed:.2f}s words={len(words)}")
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
        """Streams the Responses API output, returning (text, None) or (None, error).

        The socket read timeout bounds the wait for each SSE chunk so a stalled
        connection is cancelled fast; the whole loop is bounded by the total
        deadline, and transient failures are retried.
        """
        deadline = time.time() + OPENAI_TOTAL_DEADLINE_SECONDS
        last_error = "OpenAI request failed"
        for attempt in range(1, OPENAI_MAX_ATTEMPTS + 1):
            if time.time() >= deadline:
                break
            started = time.time()
            chunks: list[str] = []
            try:
                with urlopen(request, timeout=OPENAI_STREAM_READ_TIMEOUT_SECONDS) as response:
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
                    _log(f"[OpenAiOCR] stream ok in {time.time() - started:.2f}s (attempt {attempt})")
                    return text, None
                last_error = "OpenAI returned no streamed text"
            except HTTPError as error:
                detail = error.read().decode("utf-8", errors="replace")
                last_error = f"OpenAI HTTP {error.code}: {detail}"
                if error.code not in OPENAI_RETRY_STATUS:
                    return None, last_error
            except (socket.timeout, TimeoutError, URLError) as error:
                last_error = f"OpenAI stream stalled: {error}"
            _log(f"[OpenAiOCR] attempt {attempt}/{OPENAI_MAX_ATTEMPTS} failed in {time.time() - started:.2f}s: {last_error}")

        return None, last_error

    def _chunk_text(self, data: str) -> str:
        try:
            parsed = json.loads(data)
        except json.JSONDecodeError:
            return ""
        if not isinstance(parsed, dict):
            return ""
        # Responses API streams text as events of type "response.output_text.delta".
        event_type = str(parsed.get("type", ""))
        if "output_text.delta" in event_type:
            delta = parsed.get("delta")
            return delta if isinstance(delta, str) else ""
        return ""

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
        _log(f"[OpenAiOCR] ERROR: {message}")
        return result
