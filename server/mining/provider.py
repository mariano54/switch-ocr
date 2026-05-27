from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol


@dataclass(frozen=True)
class MiningWord:
    surface: str
    reading: str
    base: str
    definition: str
    sentence: str
    language: str = "japanese"

    @property
    def term(self) -> str:
        return (self.base or self.surface).strip()


@dataclass(frozen=True)
class SavedWord:
    word: str


class MiningProviderError(RuntimeError):
    pass


class MiningProvider(Protocol):
    name: str

    def login(self) -> None:
        ...

    def load_saved_words(self, language: str) -> list[SavedWord]:
        ...

    def add_word(self, word: MiningWord) -> None:
        ...


def mining_key(text: str) -> str:
    return text.strip().casefold()
