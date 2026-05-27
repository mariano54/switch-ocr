from __future__ import annotations

from typing import Any, TypedDict


class OcrWord(TypedDict, total=False):
    w: str
    b: str
    t: str
    f: str
    fv: str
    k: str


class OcrResult(TypedDict, total=False):
    success: bool
    provider: str
    model: str
    words: list[OcrWord]
    text: str
    display_text: str
    definitions: list[str]
    error: str
    raw: Any
