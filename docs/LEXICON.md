# Lexicon production and runtime budget

The shipped system dictionary is generated independently from pinned upstream
archives. No discarded-repository dictionary or Microsoft dictionary is used.

## Rebuild

```powershell
python -m pip download --no-deps --dest .cache\lexicon-downloads `
  jieba==0.42.1 pypinyin==0.55.0
python -m pip install --no-deps --require-hashes `
  --target .cache\lexicon-deps -r scripts\requirements-lexicon.lock
python scripts\generate_lexicon.py `
  --dependency-root .cache\lexicon-deps `
  --archive-root .cache\lexicon-downloads `
  --output .cache\system-lexicon.tsv
cmake --build --preset x64-release --target zrinput_dictionary_compiler
.\build\x64\tools\Release\zrinput_dictionary_compiler.exe `
  .cache\system-lexicon.tsv data\system.zrdict
python scripts\finalize_lexicon_manifest.py `
  --manifest data\lexicon_manifest.json --package data\system.zrdict
```

The generator verifies the downloaded archive SHA-256 values before importing
either maintenance dependency. It filters non-Han rows, bounds word length,
normalizes readings to canonical lowercase syllables, merges reviewed local
overrides, sorts deterministically, and records all counts and hashes.

## Current artifact

| Metric | Value |
| --- | ---: |
| Candidate entries | 348,918 |
| Unique syllables | 411 |
| Package bytes | 11,034,733 |
| Package SHA-256 | `aa74ee75ebc17e4793444fb19f2b9132fa1922c2572d53650d40f446d25a4682` |
| Package read/validation | 45.56 ms |
| Immutable index construction | 261.00 ms |
| Query P50 / P95 / P99 (1,000 runs) | 0.039 / 0.340 / 0.596 ms |
| Peak integration-test working set | 93.23 MiB |

The measurements were taken by `zrinput_lexicon_integration_tests` on the
development Windows 11 x64 machine on 2026-08-07. Its enforced gates are
2,500 ms total cold initialization, 30 ms query P95, and 140 MiB peak working
set. A second clean generation produced byte-identical TSV, syllable, and
ZRDICT outputs.

The working set is acceptable for the future shared decoder process but not
for replication into every host process. Therefore the production TSF adapter
will query an out-of-process local decoder service; its in-process fallback is
limited to a small emergency dictionary. This architectural constraint is a
release gate, not an optional optimization.
