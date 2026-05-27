from __future__ import annotations

import json
import os
import zipfile
from pathlib import Path
from typing import Any, TypedDict


class FrequencyEntry(TypedDict):
    term: str
    value: int
    displayValue: str
    dictionary: str
    reading: str


_frequency_cache: dict[str, FrequencyEntry] = {}
_loaded_paths: set[str] = set()


def _default_dictionary_paths() -> list[Path]:
    env_paths = os.getenv("YOMITAN_FREQUENCY_PATHS")
    if env_paths:
        return [Path(path).expanduser() for path in env_paths.split(os.pathsep) if path.strip()]

    desktop = Path.home() / "Desktop"
    return sorted(path for path in desktop.glob("*.zip") if path.name.startswith("[Freq]"))


def _entry_from_yomitan_row(row: list[Any], dictionary: str) -> FrequencyEntry | None:
    if len(row) < 3:
        return None

    term = row[0]
    metadata = row[-1]
    if not isinstance(term, str) or not term or not isinstance(metadata, dict):
        return None

    frequency_data = metadata.get("frequency") if isinstance(metadata.get("frequency"), dict) else metadata
    raw_value = frequency_data.get("value")
    if not isinstance(raw_value, int):
        return None

    display_value = frequency_data.get("displayValue")
    reading = metadata.get("reading")
    return {
        "term": term,
        "value": raw_value,
        "displayValue": display_value if isinstance(display_value, str) else str(raw_value),
        "dictionary": dictionary,
        "reading": reading if isinstance(reading, str) else "",
    }


def _load_frequency_zip(path: Path) -> None:
    normalized_path = str(path.resolve())
    if normalized_path in _loaded_paths:
        return

    with zipfile.ZipFile(path) as archive:
        try:
            index = json.loads(archive.read("index.json"))
        except KeyError:
            index = {}
        dictionary_name = index.get("title", path.stem) if isinstance(index, dict) else path.stem

        for name in archive.namelist():
            if not name.startswith("term_meta_bank_") or not name.endswith(".json"):
                continue
            rows = json.loads(archive.read(name))
            if not isinstance(rows, list):
                continue
            for row in rows:
                if not isinstance(row, list):
                    continue
                entry = _entry_from_yomitan_row(row, str(dictionary_name))
                if entry is None:
                    continue
                existing = _frequency_cache.get(entry["term"])
                if existing is None or entry["value"] < existing["value"]:
                    _frequency_cache[entry["term"]] = entry

    _loaded_paths.add(normalized_path)


def ensure_frequency_dictionaries_loaded() -> None:
    for path in _default_dictionary_paths():
        if path.exists() and path.suffix.lower() == ".zip":
            _load_frequency_zip(path)


def lookup_frequency_terms(terms: list[str]) -> dict[str, FrequencyEntry]:
    ensure_frequency_dictionaries_loaded()
    result: dict[str, FrequencyEntry] = {}
    for term in terms:
        normalized = term.strip()
        if not normalized:
            continue
        entry = _frequency_cache.get(normalized)
        if entry is not None:
            result[normalized] = entry
    return result
