from __future__ import annotations

from pathlib import Path

from .config import load_env, ocr_provider
from .gemini import GeminiOcrProvider
from .openai_provider import OpenAiOcrProvider
from .types import OcrResult

PROJECT_ROOT = Path(__file__).resolve().parents[2]
load_env(PROJECT_ROOT / ".env")


def perform_ocr(image_bytes: bytes, mime_type: str = "image/jpeg", provider: str | None = None) -> OcrResult:
    selected = provider or ocr_provider()
    if selected == "openai":
        return OpenAiOcrProvider().perform_ocr(image_bytes, mime_type=mime_type)
    if selected == "gemini":
        return GeminiOcrProvider().perform_ocr(image_bytes, mime_type=mime_type)
    return {
        "success": False,
        "provider": selected,
        "words": [],
        "error": f"Unsupported OCR provider: {selected}",
    }
