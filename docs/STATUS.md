# Development status

Last updated: 2026-07-29

Branch: `codex/zrinput-cleanroom`

## Current checkpoint

Completed: tolerant pinyin analysis checkpoint.

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

## Verification

Verified on Windows 11 x64 with MSVC 19.40.33811 and SDK 10.0.22621.0:

```text
cmake --preset windows-x64                  PASS
cmake --build --preset x64-release          PASS (strict warnings as errors)
ctest --preset x64-release                  PASS (1 executable, 14 cases)
```

The cases include a 1024 UTF-16-unit internal buffer, non-mutating soft-limit
notification, hard-limit recovery through Backspace, arbitrary selection
replacement, word deletion, and surrogate-pair-safe movement/deletion.
Parser cases cover ambiguous segmentation, mixed case/umlaut normalization,
repeated separators, invalid tails, abbreviations, corrections, stable splits,
and 256 separator characters.

`cmake --preset windows-arm64` was attempted and failed during compiler
detection because this machine does not have the Visual Studio ARM64 C++ tools
or libraries installed. The preset remains ready for a machine with
`Microsoft.VisualStudio.Component.VC.Tools.ARM64`.

## Next resume point

Implement and test dictionary/decoder contracts, the explicit ranking formula,
and the version-bound candidate request model.

## Known risks

- The Visual Studio ARM64 component is absent, so ARM64 cannot be built on this
  machine until that optional workload is installed.
- End-to-end TSF input requires registration and interactive host-app tests;
  portable tests alone are insufficient.
- One-hour and eight-hour stress tiers must run after the deterministic fuzzer
  is stable; until then no endurance result will be reported.
