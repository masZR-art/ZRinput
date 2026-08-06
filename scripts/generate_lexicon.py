#!/usr/bin/env python3
"""Generate a reproducible maintenance TSV from pinned permissive sources."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


GENERATOR_VERSION = 1
READING_RE = re.compile(r"^[a-z]+(?: [a-z]+)*$")
SOURCE_ARCHIVES = {
    "jieba-0.42.1.tar.gz":
        "055ca12f62674fafed09427f176506079bc135638a14e23e25be909131928db2",
    "pypinyin-0.55.0-py2.py3-none-any.whl":
        "d53b1e8ad2cdb815fb2cb604ed3123372f5a28c6f447571244aca36fc62a286f",
}


def is_han_word(word: str) -> bool:
    return bool(word) and all(
        "\u3400" <= character <= "\u9fff" or
        "\uf900" <= character <= "\ufaff"
        for character in word
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_archives(directory: Path) -> None:
    for name, expected in SOURCE_ARCHIVES.items():
        path = directory / name
        if not path.is_file():
            raise RuntimeError(f"missing pinned source archive: {path}")
        actual = sha256(path)
        if actual != expected:
            raise RuntimeError(
                f"source archive checksum mismatch for {name}: {actual}"
            )


def normalize_syllable(value: str) -> str | None:
    result = value.lower().replace("u:", "v").replace("ü", "v")
    result = "".join(character for character in result if not character.isdigit())
    return result if result.isascii() and result.isalpha() else None


def load_overrides(path: Path) -> dict[tuple[str, str], float]:
    entries: dict[tuple[str, str], float] = {}
    if not path.exists():
        return entries
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not READING_RE.fullmatch(fields[0]):
            raise RuntimeError(f"invalid override at line {line_number}")
        entries[(fields[0], fields[1])] = float(fields[2])
    return entries


def generate(dependency_root: Path,
             archive_root: Path,
             overrides_path: Path,
             output_path: Path,
             syllables_path: Path,
             manifest_path: Path) -> None:
    verify_archives(archive_root)
    sys.path.insert(0, str(dependency_root.resolve()))
    import jieba  # type: ignore[import-not-found]  # pylint: disable=import-outside-toplevel
    from pypinyin import Style, lazy_pinyin  # type: ignore[import-not-found]  # pylint: disable=import-outside-toplevel

    entries = load_overrides(overrides_path)
    skipped = 0
    with Path(jieba.get_dict_file().name).open("r", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split()
            if len(fields) < 2:
                skipped += 1
                continue
            word = fields[0]
            if not is_han_word(word) or len(word) > 12:
                skipped += 1
                continue
            try:
                frequency = float(fields[1])
            except ValueError:
                skipped += 1
                continue
            raw_reading = lazy_pinyin(
                word,
                style=Style.NORMAL,
                neutral_tone_with_five=False,
                errors=lambda characters: list(characters),
            )
            syllables = [normalize_syllable(value) for value in raw_reading]
            if any(value is None for value in syllables):
                skipped += 1
                continue
            reading = " ".join(value for value in syllables if value)
            if not READING_RE.fullmatch(reading):
                skipped += 1
                continue
            key = (reading, word)
            entries[key] = max(entries.get(key, 0.0), frequency)

    ordered = sorted(entries.items(), key=lambda item: (item[0][0], -item[1], item[0][1]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as output:
        for (reading, word), frequency in ordered:
            output.write(f"{reading}\t{word}\t{frequency:.6g}\n")

    syllables = sorted({part for (reading, _word), _frequency in ordered
                        for part in reading.split(" ")})
    syllables_path.write_text("\n".join(syllables) + "\n", encoding="ascii", newline="\n")
    manifest = {
        "format": "zrinput.lexicon-manifest",
        "version": 1,
        "generator_version": GENERATOR_VERSION,
        "sources": SOURCE_ARCHIVES,
        "entry_count": len(ordered),
        "syllable_count": len(syllables),
        "skipped_source_rows": skipped,
        "maintenance_tsv_sha256": sha256(output_path),
        "overrides_sha256": sha256(overrides_path),
    }
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=True, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
        newline="\n",
    )
    print(json.dumps(manifest, indent=2, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dependency-root", type=Path, required=True)
    parser.add_argument("--archive-root", type=Path, required=True)
    parser.add_argument("--overrides", type=Path,
                        default=Path("data/lexicon_overrides.tsv"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--syllables", type=Path,
                        default=Path("data/pinyin_syllables.txt"))
    parser.add_argument("--manifest", type=Path,
                        default=Path("data/lexicon_manifest.json"))
    arguments = parser.parse_args()
    generate(arguments.dependency_root, arguments.archive_root,
             arguments.overrides, arguments.output,
             arguments.syllables, arguments.manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

