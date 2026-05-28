from __future__ import annotations

import html

from .formatting import strip_html
from .frequency_dictionary import lookup_frequency_terms
from .jmdict_dictionary import lookup_terms
from .kanji_dictionary import lookup_kanji_text
from .tokens import suppress_definition
from .types import OcrWord


def _plain(text: str) -> str:
    return html.unescape(strip_html(text)).strip()


def _word_terms(word: OcrWord) -> list[str]:
    if suppress_definition(_plain(word.get("w", "")), _plain(word.get("b", ""))):
        return []

    terms = [_plain(word.get("b", "")), _plain(word.get("w", ""))]
    for term in list(terms):
        if term.endswith("する") and len(term) > 2:
            terms.append(term[:-2])
    return list(dict.fromkeys(term for term in terms if term))


def _short_meaning(meaning: str) -> str:
    return " ".join(meaning.split()[:2])


def _kanji_summary(text: str) -> str:
    parts: list[str] = []
    for entry in lookup_kanji_text(text):
        meaning = ", ".join(part for part in (_short_meaning(raw) for raw in entry["meanings"][:2]) if part)
        if meaning:
            parts.append(f'{entry["kanji"]} {meaning}')
        if len(parts) >= 2:
            break
    return "   ".join(parts)


def enrich_words(words: list[OcrWord]) -> list[OcrWord]:
    term_lists = [_word_terms(word) for word in words]
    terms = list(dict.fromkeys(term for term_list in term_lists for term in term_list))
    jmdict_matches = lookup_terms(terms)
    frequency_matches = lookup_frequency_terms(terms)

    enriched: list[OcrWord] = []
    for word, word_terms in zip(words, term_lists):
        output: OcrWord = dict(word)
        if not word_terms and suppress_definition(_plain(word.get("w", "")), _plain(word.get("b", ""))):
            output["b"] = ""
            output["t"] = ""
            enriched.append(output)
            continue

        for term in word_terms:
            entry = jmdict_matches.get(term)
            if entry is not None:
                output["b"] = output.get("b") or entry.term
                output["t"] = "; ".join(entry.definitions)[:220]
                break

        for term in word_terms:
            frequency = frequency_matches.get(term)
            if frequency is not None:
                output["f"] = str(frequency["value"])
                output["fv"] = str(frequency["value"])
                break

        kanji_text = "".join(word_terms)
        kanji = _kanji_summary(kanji_text)
        if kanji:
            output["k"] = kanji[:96]
        enriched.append(output)

    return enriched
