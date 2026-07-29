#!/usr/bin/env python3
"""Offline integrity, coverage and ranking checks for the shipped lexicon."""

from __future__ import annotations

import math
from pathlib import Path
import re
import sys


REPOSITORY = Path(__file__).resolve().parent.parent
LEXICON = REPOSITORY / "data" / "default_lexicon.tsv"
SYLLABLES = REPOSITORY / "data" / "standard_pinyin_syllables.txt"
ASCII_KEY = re.compile(r"[a-z]+(?: [a-z]+)*\Z")


def fail(message: str) -> None:
    raise AssertionError(message)


def main() -> int:
    required = {
        line.strip()
        for line in SYLLABLES.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    }
    entries: dict[str, list[tuple[str, float]]] = {}
    seen: set[tuple[str, str]] = set()
    previous_key = ""
    previous_score = math.inf

    with LEXICON.open(encoding="utf-8") as source:
        for number, raw_line in enumerate(source, 1):
            line = raw_line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                fail(f"malformed row {number}")
            pinyin_key, text, score_text = fields
            if not ASCII_KEY.fullmatch(pinyin_key) or not text:
                fail(f"invalid pinyin or text at row {number}")
            try:
                score = float(score_text)
            except ValueError:
                fail(f"invalid score at row {number}")
            if not math.isfinite(score) or score <= 0:
                fail(f"non-positive score at row {number}")
            if (pinyin_key, text) in seen:
                fail(f"duplicate candidate at row {number}: {pinyin_key}/{text}")
            seen.add((pinyin_key, text))
            if pinyin_key < previous_key:
                fail(f"pinyin order regressed at row {number}")
            if pinyin_key == previous_key and score > previous_score:
                fail(f"score order regressed at row {number}")
            previous_key = pinyin_key
            previous_score = score
            entries.setdefault(pinyin_key, []).append((text, score))

    if len(seen) < 60_000:
        fail(f"lexicon unexpectedly small: {len(seen)} entries")
    unique_words = {text for _pinyin, text in seen}
    if len(unique_words) < 55_000:
        fail(f"vocabulary unexpectedly small: {len(unique_words)} words")
    if LEXICON.stat().st_size > 3 * 1024 * 1024:
        fail(f"lexicon exceeds the 3 MiB package budget: {LEXICON.stat().st_size}")

    single_character_coverage = {
        pinyin_key
        for pinyin_key, candidates in entries.items()
        if " " not in pinyin_key and any(len(text) == 1 for text, _score in candidates)
    }
    missing = sorted(required - single_character_coverage)
    if missing:
        fail(f"missing required syllable candidates: {', '.join(missing)}")

    expected_first = {
        "a": "啊",
        "de": "的",
        "hang": "行",
        "hao": "好",
        "ni": "你",
        "shi": "是",
        "wo": "我",
        "wei": "为",
        "xian": "先",
        "xian zai": "现在",
        "yin wei": "因为",
        "zhong guo": "中国",
    }
    for pinyin_key, expected in expected_first.items():
        candidates = entries.get(pinyin_key, [])
        if not candidates or candidates[0][0] != expected:
            actual = candidates[0][0] if candidates else "<missing>"
            fail(f"{pinyin_key} ranked {actual!r} first instead of {expected!r}")

    expected_available = {
        "shu ru fa": "输入法",
        "ji suan ji": "计算机",
        "mei guan xi": "没关系",
        "wan shang hao": "晚上好",
        "xie xie": "谢谢",
    }
    for pinyin_key, expected in expected_available.items():
        if expected not in {text for text, _score in entries.get(pinyin_key, [])}:
            fail(f"missing curated candidate {pinyin_key}/{expected}")

    print(
        f"Lexicon audit passed: {len(seen):,} entries, "
        f"{len(unique_words):,} unique words, {len(required)} syllables, "
        f"{LEXICON.stat().st_size:,} bytes."
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAILED: {error}", file=sys.stderr)
        sys.exit(1)
