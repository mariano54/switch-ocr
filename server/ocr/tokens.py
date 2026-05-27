from __future__ import annotations

PARTICLE_SURFACES = {
    "は",
    "が",
    "を",
    "に",
    "へ",
    "と",
    "で",
    "も",
    "の",
    "や",
    "か",
    "ね",
    "よ",
    "な",
    "ぞ",
    "ぜ",
    "わ",
    "さ",
    "から",
    "まで",
    "より",
    "だけ",
    "しか",
    "こそ",
    "でも",
    "ほど",
    "くらい",
    "ぐらい",
    "など",
    "なんて",
    "って",
}

PUNCTUATION = set(".,!?;:()[]{}<>\"'。、！？；：（）「」『』【】〈〉《》…‥・ー〜～")


def suppress_definition(surface: str, base: str = "") -> bool:
    token = (surface or base).strip()
    return bool(token) and (token in PARTICLE_SURFACES or all(char in PUNCTUATION for char in token))
