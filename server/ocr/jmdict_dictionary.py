from __future__ import annotations

import json
import os
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class TermEntry:
    term: str
    reading: str
    score: int
    definitions: tuple[str, ...]


_term_index: dict[str, list[TermEntry]] = {}
_reading_index: dict[str, list[TermEntry]] = {}
_loaded_paths: set[str] = set()


def _default_dictionary_paths() -> list[Path]:
    env_paths = os.getenv("YOMITAN_TERM_DICTIONARY_PATHS")
    if env_paths:
        return [Path(path).expanduser() for path in env_paths.split(os.pathsep) if path.strip()]

    desktop = Path.home() / "Desktop"
    preferred = [
        desktop / "[Bilingual] JMdict (Recommended).zip",
        desktop / "[Bilingual] JMdict (Recommended) (1).zip",
    ]
    found = [path for path in preferred if path.exists()]
    found.extend(path for path in sorted(desktop.glob("*JMdict*Recommended*.zip")) if path not in found)
    return found


def _text_from_structured_content(value: Any) -> list[str]:
    if isinstance(value, str):
        stripped = value.strip()
        return [stripped] if stripped else []
    if isinstance(value, list):
        parts: list[str] = []
        for item in value:
            parts.extend(_text_from_structured_content(item))
        return parts
    if isinstance(value, dict):
        data = value.get("data")
        if isinstance(data, dict) and data.get("content") not in (None, "glossary"):
            return []
        if "content" in value:
            return _text_from_structured_content(value["content"])
        if "text" in value:
            return _text_from_structured_content(value["text"])
    return []


def _definitions_from_row(raw_definitions: Any) -> tuple[str, ...]:
    if not isinstance(raw_definitions, list):
        return ()

    definitions: list[str] = []
    seen: set[str] = set()
    for item in raw_definitions:
        text = " ".join(_text_from_structured_content(item)).strip()
        if text and text not in seen:
            definitions.append(text)
            seen.add(text)
        if len(definitions) >= 2:
            break
    return tuple(definitions)


def _entry_from_yomitan_row(row: list[Any]) -> TermEntry | None:
    if len(row) < 6:
        return None

    term = row[0]
    reading = row[1]
    score = row[4]
    if not isinstance(term, str) or not term:
        return None
    if not isinstance(reading, str):
        reading = term
    if not isinstance(score, int):
        score = 0

    definitions = _definitions_from_row(row[5])
    if not definitions:
        return None

    return TermEntry(term=term, reading=reading or term, score=score, definitions=definitions)


def _add_index_entry(index: dict[str, list[TermEntry]], key: str, entry: TermEntry) -> None:
    if key:
        index.setdefault(key, []).append(entry)


def _load_dictionary_zip(path: Path) -> None:
    normalized_path = str(path.resolve())
    if normalized_path in _loaded_paths:
        return

    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if not name.startswith("term_bank_") or not name.endswith(".json"):
                continue
            rows = json.loads(archive.read(name))
            if not isinstance(rows, list):
                continue
            for row in rows:
                if not isinstance(row, list):
                    continue
                entry = _entry_from_yomitan_row(row)
                if entry is None:
                    continue
                _add_index_entry(_term_index, entry.term, entry)
                if entry.reading != entry.term:
                    _add_index_entry(_reading_index, entry.reading, entry)

    for index in (_term_index, _reading_index):
        for entries in index.values():
            entries.sort(key=lambda item: item.score, reverse=True)
    _loaded_paths.add(normalized_path)


def ensure_dictionary_loaded() -> None:
    for path in _default_dictionary_paths():
        if path.exists() and path.suffix.lower() == ".zip":
            _load_dictionary_zip(path)


def lookup_terms(terms: list[str]) -> dict[str, TermEntry]:
    ensure_dictionary_loaded()
    matches: dict[str, TermEntry] = {}
    for term in terms:
        normalized = term.strip()
        if not normalized:
            continue
        entries = _term_index.get(normalized) or _reading_index.get(normalized)
        if entries:
            matches[normalized] = entries[0]
    return matches
