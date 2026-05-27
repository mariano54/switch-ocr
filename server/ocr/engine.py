from __future__ import annotations

from pathlib import Path

from .config import load_env
from .gemini import GeminiOcrProvider
from .types import OcrResult

PROJECT_ROOT = Path(__file__).resolve().parents[2]
load_env(PROJECT_ROOT / ".env")


def perform_ocr(image_bytes: bytes, mime_type: str = "image/jpeg", provider: str = "gemini") -> OcrResult:
    if provider != "gemini":
        return {
            "success": False,
            "provider": provider,
            "words": [],
            "error": f"Unsupported OCR provider: {provider}",
        }

    return GeminiOcrProvider().perform_ocr(image_bytes, mime_type=mime_type)
