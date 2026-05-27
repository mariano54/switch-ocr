from __future__ import annotations

import html
import re

from .types import OcrWord

TAG_RE = re.compile(r"<[^>]+>")
RUBY_RE = re.compile(r"<ruby(?:\s[^>]*)?>(.*?)</ruby>", re.DOTALL | re.IGNORECASE)
RT_RE = re.compile(r"<rt(?:\s[^>]*)?>(.*?)</rt>", re.DOTALL | re.IGNORECASE)


def strip_html(text: str) -> str:
    return TAG_RE.sub("", RT_RE.sub("", text))


def switch_plain_text(text: str) -> str:
    def replace_ruby(match: re.Match[str]) -> str:
        content = match.group(1)
        base = html.unescape(strip_html(RT_RE.sub("", content))).strip()
        reading = "".join(html.unescape(strip_html(rt)).strip() for rt in RT_RE.findall(content))
        if reading and reading != base:
            return f"{base}({reading})"
        return base

    return html.unescape(strip_html(RUBY_RE.sub(replace_ruby, text)))


def normalize_words(raw_words: object) -> list[OcrWord]:
    if not isinstance(raw_words, list):
        return []

    words: list[OcrWord] = []
    for item in raw_words:
        if not isinstance(item, dict):
            continue

        word = {
            "w": str(item.get("w", "")),
            "b": str(item.get("b", "")),
            "t": str(item.get("t", "")),
        }
        for key in ("f", "fv", "k"):
            if item.get(key) not in (None, ""):
                word[key] = str(item.get(key, ""))
        if word["w"] or word["b"] or word["t"]:
            words.append(word)

    return words


def display_text(words: list[OcrWord]) -> str:
    return "".join(strip_html(word.get("w", "")) for word in words)


def switch_display_text(words: list[OcrWord]) -> str:
    return "".join(switch_plain_text(word.get("w", "")) for word in words)


def definition_lines(words: list[OcrWord]) -> list[str]:
    lines: list[str] = []
    for word in words:
        surface = strip_html(word.get("w", "")).strip()
        definition = word.get("t", "").strip()
        if surface and definition:
            lines.append(f"{surface}: {definition}")
    return lines
