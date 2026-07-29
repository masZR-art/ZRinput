#!/usr/bin/env python3
"""Build ZRinput's runtime lexicon from pinned, permissively licensed data."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import math
from pathlib import Path
import re
import sys
from typing import Iterable


EXPECTED_PACKAGES = {
    "jieba": "0.42.1",
    "pypinyin": "0.55.0",
}

EXPECTED_SOURCE_HASHES = {
    "jieba/dict.txt": (
        "7197c3211ddd98962b036cdf40324d1ea2bfaa12bd028e68faa70111a88e12a8"
    ),
    "pypinyin/pinyin_dict.json": (
        "5f294c01e6c6c0a1c8e329c79335a3f8e0b27d06bf1de7a99244b765892d1e5b"
    ),
    "pypinyin/phrases_dict.json": (
        "a45ff140a6b631ca9c82127b280a2f414e0aba6bb2824a0e9d1e77fff359c665"
    ),
}

ASCII_SYLLABLE = re.compile(r"[a-z]+\Z")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_sources() -> tuple[Path, Path, Path]:
    try:
        import jieba
        import pypinyin
    except ImportError as error:
        raise RuntimeError(
            "Install pinned build dependencies with "
            "'python -m pip install -r scripts/requirements-lexicon.txt'."
        ) from error

    for package, expected in EXPECTED_PACKAGES.items():
        actual = importlib.metadata.version(package)
        if actual != expected:
            raise RuntimeError(
                f"{package} {actual} is installed; exactly {expected} is required"
            )

    jieba_dictionary = Path(jieba.__file__).with_name("dict.txt")
    pypinyin_root = Path(pypinyin.__file__).parent
    sources = {
        "jieba/dict.txt": jieba_dictionary,
        "pypinyin/pinyin_dict.json": pypinyin_root / "pinyin_dict.json",
        "pypinyin/phrases_dict.json": pypinyin_root / "phrases_dict.json",
    }
    for label, path in sources.items():
        actual = sha256(path)
        expected = EXPECTED_SOURCE_HASHES[label]
        if actual != expected:
            raise RuntimeError(
                f"source hash mismatch for {label}: expected {expected}, got {actual}"
            )
    return (
        jieba_dictionary,
        sources["pypinyin/pinyin_dict.json"],
        sources["pypinyin/phrases_dict.json"],
    )


def is_cjk_word(word: str) -> bool:
    return bool(word) and all("\u3400" <= character <= "\u9fff" for character in word)


def normalize_syllable(value: str) -> str | None:
    normalized = value.lower().replace("u:", "v").replace("ue:", "ve")
    normalized = normalized.replace("ü", "v")
    return normalized if ASCII_SYLLABLE.fullmatch(normalized) else None


def read_baseline(path: Path) -> set[str]:
    syllables: set[str] = set()
    for number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if not ASCII_SYLLABLE.fullmatch(line):
            raise RuntimeError(f"invalid baseline syllable at {path}:{number}: {line}")
        syllables.add(line)
    if not syllables:
        raise RuntimeError(f"empty syllable baseline: {path}")
    return syllables


def jieba_rows(path: Path) -> Iterable[tuple[str, int]]:
    with path.open(encoding="utf-8") as source:
        for number, raw_line in enumerate(source, 1):
            fields = raw_line.rstrip("\n").rsplit(" ", 2)
            if len(fields) != 3:
                raise RuntimeError(f"malformed Jieba row {number}")
            word, frequency_text, _part_of_speech = fields
            try:
                frequency = int(frequency_text)
            except ValueError as error:
                raise RuntimeError(f"invalid Jieba frequency at row {number}") from error
            yield word, frequency


def apply_overrides(
    path: Path,
    entries: dict[tuple[str, str], float],
) -> int:
    loaded = 0
    with path.open(encoding="utf-8") as source:
        for number, raw_line in enumerate(source, 1):
            line = raw_line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                raise RuntimeError(f"malformed override at {path}:{number}")
            pinyin_key, text, frequency_text = fields
            try:
                frequency = float(frequency_text)
            except ValueError as error:
                raise RuntimeError(
                    f"invalid override frequency at {path}:{number}"
                ) from error
            if not math.isfinite(frequency) or frequency <= 0 or not text:
                raise RuntimeError(f"invalid override at {path}:{number}")
            if not add_entry(entries, pinyin_key.split(), text, frequency):
                raise RuntimeError(f"invalid override pinyin at {path}:{number}")
            loaded += 1
    return loaded


def add_entry(
    entries: dict[tuple[str, str], float],
    pinyin_tokens: Iterable[str],
    text: str,
    frequency: float,
) -> bool:
    tokens = list(pinyin_tokens)
    if not tokens or any(not ASCII_SYLLABLE.fullmatch(token) for token in tokens):
        return False
    key = " ".join(tokens)
    item = (key, text)
    entries[item] = max(entries.get(item, 0.0), max(frequency, 1.0))
    return True


def build_entries(
    jieba_dictionary: Path,
    overrides: Path,
    minimum_frequency: int,
    maximum_word_length: int,
    baseline: set[str],
) -> tuple[dict[tuple[str, str], float], dict[str, int]]:
    from pypinyin import Style, lazy_pinyin, pinyin
    from pypinyin.constants import PINYIN_DICT

    entries: dict[tuple[str, str], float] = {}
    statistics = {
        "eligible_words": 0,
        "phrases": 0,
        "characters": 0,
        "rejected_pronunciations": 0,
        "supplemental_characters": 0,
        "curated_overrides": 0,
    }

    for word, frequency in jieba_rows(jieba_dictionary):
        if not is_cjk_word(word) or len(word) > maximum_word_length:
            continue
        if len(word) > 1 and frequency < minimum_frequency:
            continue
        statistics["eligible_words"] += 1

        if len(word) == 1:
            groups = pinyin(
                word,
                style=Style.NORMAL,
                heteronym=True,
                strict=True,
                errors="ignore",
            )
            readings: list[str] = []
            for group in groups:
                for reading in group:
                    normalized = normalize_syllable(reading)
                    if normalized and normalized not in readings:
                        readings.append(normalized)
            for index, reading in enumerate(readings):
                # Keep alternate readings useful without letting a rare reading
                # displace the primary pronunciation of an equally frequent char.
                adjusted_frequency = frequency * math.pow(0.02, index)
                add_entry(entries, [reading], word, adjusted_frequency)
            if readings:
                statistics["characters"] += 1
            else:
                statistics["rejected_pronunciations"] += 1
            continue

        readings = lazy_pinyin(
            word,
            style=Style.NORMAL,
            strict=True,
            errors="ignore",
        )
        normalized_readings = [normalize_syllable(reading) for reading in readings]
        if len(normalized_readings) != len(word) or any(
            reading is None for reading in normalized_readings
        ):
            statistics["rejected_pronunciations"] += 1
            continue
        if add_entry(
            entries,
            (reading for reading in normalized_readings if reading is not None),
            word,
            frequency,
        ):
            statistics["phrases"] += 1

    statistics["curated_overrides"] = apply_overrides(overrides, entries)

    covered = {
        pinyin_key
        for (pinyin_key, text), _score in entries.items()
        if len(text) == 1 and " " not in pinyin_key
    }
    missing = baseline - covered
    if missing:
        representatives: dict[str, list[str]] = {syllable: [] for syllable in missing}
        for codepoint in sorted(PINYIN_DICT):
            character = chr(codepoint)
            groups = pinyin(
                character,
                style=Style.NORMAL,
                heteronym=True,
                strict=True,
                errors="ignore",
            )
            for group in groups:
                for reading in group:
                    normalized = normalize_syllable(reading)
                    if normalized in representatives:
                        representatives[normalized].append(character)
        for syllable in sorted(missing):
            candidates = representatives[syllable]
            if not candidates:
                raise RuntimeError(f"no representative character for syllable '{syllable}'")
            # Prefer a BMP character because every Windows font stack can display it.
            character = next(
                (candidate for candidate in candidates if ord(candidate) <= 0xFFFF),
                candidates[0],
            )
            add_entry(entries, [syllable], character, 1.0)
            statistics["supplemental_characters"] += 1

    return entries, statistics


def write_lexicon(
    output: Path,
    entries: dict[tuple[str, str], float],
    minimum_frequency: int,
    maximum_word_length: int,
) -> None:
    ordered = sorted(
        ((pinyin_key, text, score) for (pinyin_key, text), score in entries.items()),
        key=lambda item: (item[0], -item[2], item[1]),
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as destination:
        destination.write("# ZRinput generated lexicon; do not edit by hand.\n")
        destination.write("# Sources: Jieba 0.42.1 and pypinyin 0.55.0 (MIT).\n")
        destination.write("# See THIRD_PARTY_NOTICES.md and scripts/generate_lexicon.py.\n")
        destination.write(
            f"# Parameters: minimum_frequency={minimum_frequency}; "
            f"maximum_word_length={maximum_word_length}.\n"
        )
        destination.write("# pinyin<TAB>text<TAB>frequency\n")
        for pinyin_key, text, score in ordered:
            destination.write(f"{pinyin_key}\t{text}\t{score:.6f}\n")
    temporary.replace(output)


def parse_arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "data" / "default_lexicon.tsv",
    )
    parser.add_argument(
        "--syllables",
        type=Path,
        default=repository / "data" / "standard_pinyin_syllables.txt",
    )
    parser.add_argument(
        "--overrides",
        type=Path,
        default=repository / "data" / "lexicon_overrides.tsv",
    )
    parser.add_argument("--minimum-frequency", type=int, default=50)
    parser.add_argument("--maximum-word-length", type=int, default=8)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.minimum_frequency < 1:
        raise RuntimeError("minimum frequency must be positive")
    if arguments.maximum_word_length < 1:
        raise RuntimeError("maximum word length must be positive")

    jieba_dictionary, _pinyin_data, _phrase_data = verify_sources()
    baseline = read_baseline(arguments.syllables)
    entries, statistics = build_entries(
        jieba_dictionary,
        arguments.overrides,
        arguments.minimum_frequency,
        arguments.maximum_word_length,
        baseline,
    )
    write_lexicon(
        arguments.output,
        entries,
        arguments.minimum_frequency,
        arguments.maximum_word_length,
    )
    print(f"Wrote {len(entries):,} entries to {arguments.output}")
    print(f"Covered {len(baseline)} required syllables")
    for label, value in statistics.items():
        print(f"{label}: {value:,}")
    print(f"sha256: {sha256(arguments.output)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"lexicon generation failed: {error}", file=sys.stderr)
        sys.exit(1)
