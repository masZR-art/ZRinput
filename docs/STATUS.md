# Development status

Last updated: 2026-08-07

Branch: `codex/zrinput-cleanroom`

## Current checkpoint

Completed: dictionary, decoder, and ranking checkpoint.

## Completed

- Created an empty orphan worktree connected to the requested GitHub remote.
- Recorded architecture decisions, clean-room boundary, limits, and MVP scope.
- Added x64 and ARM64 CMake presets with strict C++20 warnings.
- Implemented a dynamic UTF-16 composition buffer with selection replacement,
  code-point-safe cursor movement, Home/End, forward/backward deletion,
  Ctrl+Backspace semantics, centralized limits, and monotonic versions.
- Added a dependency-free test harness and six deterministic composition tests.
- Added a bounded, multi-path pinyin parser that keeps raw UTF-16 input intact,
  normalizes only its query view, accepts repeated separators, represents
  abbreviations/incomplete syllables explicitly, applies bounded duplicate and
  transposition corrections, and retains an unparsed tail.
- Stable-prefix selection considers only complete syllable boundaries and
  refuses unsafe separator-only splits.
- Added CRC32-protected, versioned binary dictionary packages with byte/entry/
  field budgets, v1 migration, atomic replacement, and corruption rejection.
- Added system/domain/user/session dictionary layers that publish immutable
  exact, compact-prefix, and initial indexes by generation.
- Added version-preserving decode requests and candidates that retain raw
  unparsed tails. Exact full pinyin, simplified initials, and incomplete
  prefixes share one explicit scoring path.
- Documented and implemented named ranking weights with per-candidate raw and
  weighted score breakdowns, logarithmic static frequency, time decay,
  application/context hooks, correction/completion costs, and negative feedback.
- Added a reproducible maintenance pipeline using freshly downloaded,
  SHA-256-pinned MIT-licensed sources. The generated system package has 348,918
  entries and 411 syllables; an independent second generation was byte-identical.
- Added strict TSV-to-ZRDICT compiler and query tools. Replaced the memory-heavy
  per-prefix hash expansion with a sorted compact-reading range index.
- Added bounded beam sentence decoding, so continuous input can be composed
  from independently indexed words without an exact long-phrase row.
- Added one permanent replace-latest candidate worker with cooperative
  cancellation, internal submission IDs, composition-version gates, lock-free
  publication callbacks, bounded pending state, callback re-entry, and
  idempotent shutdown.

## Verification

Verified on Windows 11 x64 with MSVC 19.40.33811 and SDK 10.0.22621.0:

```text
cmake --preset windows-x64                  PASS
cmake --build --preset x64-release          PASS (strict warnings as errors)
ctest --preset x64-release                  PASS (2 executables, 32 core cases)
```

The cases include a 1024 UTF-16-unit internal buffer, non-mutating soft-limit
notification, hard-limit recovery through Backspace, arbitrary selection
replacement, word deletion, and surrogate-pair-safe movement/deletion.
Parser cases cover ambiguous segmentation, mixed case/umlaut normalization,
repeated separators, invalid tails, abbreviations, corrections, stable splits,
and 256 separator characters.
Dictionary/decoder cases cover package round-trip, migration, CRC corruption,
immutable snapshot replacement, every ranking term, half-life decay, exact/
initial/incomplete lookup, raw-tail retention, and personalization ordering.

Real-package checks on 2026-08-07:

```text
system.zrdict                         11,034,733 bytes
entries / syllables                   348,918 / 411
fresh-generation package hash match  PASS
xianzai / zhongguo / shurufa / nihao PASS (real Chinese candidates)
package read / index construction     45.56 / 261.00 ms
1,000-query P50 / P95 / P99           0.039 / 0.340 / 0.596 ms
integration peak working set          93.23 MiB
```

`zrinput_lexicon_integration_tests` enforces 30 ms query P95, 2.5 s cold
initialization, and 140 MiB peak working-set gates while checking five known
Chinese results against the actual 348,918-entry package.

The complete core executable passed 100 consecutive runs under MSVC after
candidate-pipeline integration.

`cmake --preset windows-arm64` was attempted and failed during compiler
detection because this machine does not have the Visual Studio ARM64 C++ tools
or libraries installed. The preset remains ready for a machine with
`Microsoft.VisualStudio.Component.VC.Tools.ARM64`.

## Next resume point

Integrate the asynchronous personal-memory writer and versioned theme engine
being developed in parallel, then connect the TSF adapter and decoder service.

## Known risks

- The Visual Studio ARM64 component is absent, so ARM64 cannot be built on this
  machine until that optional workload is installed.
- End-to-end TSF input requires registration and interactive host-app tests;
  portable tests alone are insufficient.
- One-hour and eight-hour stress tiers must run after the deterministic fuzzer
  is stable; until then no endurance result will be reported.
