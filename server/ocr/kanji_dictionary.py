from __future__ import annotations

import json
import os
import zipfile
from pathlib import Path
from typing import Any, TypedDict


class KanjiEntry(TypedDict):
    kanji: str
    meanings: list[str]


_kanji_cache: dict[str, KanjiEntry] = {}
_loaded_paths: set[str] = set()


def _default_dictionary_paths() -> list[Path]:
    env_paths = os.getenv("YOMITAN_KANJI_PATHS")
    if env_paths:
        return [Path(path).expanduser() for path in env_paths.split(os.pathsep) if path.strip()]

    desktop = Path.home() / "Desktop"
    return sorted(path for path in desktop.glob("*.zip") if "KANJIDIC" in path.name.upper())


def _short_meaning(meaning: str) -> str:
    return " ".join(meaning.split()[:4])


def _entry_from_yomitan_row(row: list[Any]) -> KanjiEntry | None:
    if len(row) < 5:
        return None

    kanji = row[0]
    raw_meanings = row[4]
    if not isinstance(kanji, str) or len(kanji) != 1 or not isinstance(raw_meanings, list):
        return None

    meanings = [
        _short_meaning(meaning.strip())
        for meaning in raw_meanings
        if isinstance(meaning, str) and meaning.strip()
    ][:3]
    return {"kanji": kanji, "meanings": meanings} if meanings else None


def _load_kanji_zip(path: Path) -> None:
    normalized_path = str(path.resolve())
    if normalized_path in _loaded_paths:
        return

    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if not name.startswith("kanji_bank_") or not name.endswith(".json"):
                continue
            rows = json.loads(archive.read(name))
            if not isinstance(rows, list):
                continue
            for row in rows:
                if not isinstance(row, list):
                    continue
                entry = _entry_from_yomitan_row(row)
                if entry is not None:
                    _kanji_cache[entry["kanji"]] = entry

    _loaded_paths.add(normalized_path)


def ensure_kanji_dictionaries_loaded() -> None:
    for path in _default_dictionary_paths():
        if path.exists() and path.suffix.lower() == ".zip":
            _load_kanji_zip(path)


def lookup_kanji_text(text: str) -> list[KanjiEntry]:
    ensure_kanji_dictionaries_loaded()
    entries: list[KanjiEntry] = []
    seen: set[str] = set()
    for char in text:
        if char in seen:
            continue
        seen.add(char)
        entry = _kanji_cache.get(char)
        if entry is not None:
            entries.append(entry)
    return entries
