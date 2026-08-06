# Long composition and stability evidence

## Limits and recovery

Defaults are centralized in `CompositionLimits`:

| Limit | UTF-16 code units |
| --- | ---: |
| Visible composition window | 96 |
| Active soft limit | 256 |
| Parser/internal supported length | 1024 |
| Hard safety limit | 4096 |

The buffer is a dynamic `std::u16string`; none of these values sizes a fixed
array. On insertion beyond the active limit, the parser selects the closest
complete, non-abbreviated syllable boundary near the configured midpoint. The
caller commits that prefix and retains the exact tail, translating anchor and
caret positions. A split inside a surrogate pair is impossible. If no safe
boundary exists, growth is rejected without mutating text or version; deletion,
movement, cancellation, raw commit, and candidate selection remain available.

## Deterministic and property tests

`zrinput_composition_fuzz_tests` currently verifies:

- 256 deterministic random letters;
- 256 consecutive apostrophes and Backspace to empty;
- a 256-unit valid pinyin stream followed by a required stable split;
- arbitrary insertion, Backspace, Delete, Left/Right, Home/End, Ctrl+Backspace,
  selection replacement, commit, cancellation, and non-mutating command events;
- selection bounds, UTF-16 surrogate integrity, hard limit, and monotonic
  versions after every event.

On 2026-08-07, 12 recorded seeds ran 10,000 events each (120,000 total):

```text
applied insertions  83,626
limit rejections       347
stable splits            3
invariant failures       0
```

The fuzzer prints the failing seed and event index. A standalone replay/minimize
tool and host-application event capture are still release work.

## Measured latency

Release x64, Windows 11, MSVC 19.40. The benchmark loads the real 348,918-entry
dictionary once. Values are milliseconds.

| Units | Replace P50/P95/P99 | Parse P50/P95/P99 | Decode P50/P95/P99 |
| ---: | --- | --- | --- |
| 32 | 0 / 0.0001 / 0.0001 | 0.638 / 0.682 / 0.892 | 2.259 / 2.447 / 2.798 |
| 64 | 0 / 0.0001 / 0.0001 | 1.516 / 3.082 / 3.209 | 2.977 / 4.844 / 4.929 |
| 128 | 0 / 0.0001 / 0.0001 | 6.575 / 8.803 / 9.650 | 2.338 / 3.461 / 3.703 |
| 256 | 0 / 0.0001 / 0.0001 | 11.348 / 16.046 / 18.252 | 3.002 / 5.444 / 5.485 |
| 1024 | 0 / 0.0001 / 0.0001 | 79.691 / 97.121 / 98.016 | 5.570 / 10.600 / 11.595 |

The measured 256-unit parse plus edit P95 is below the 20 ms requirement. The
1024-unit result establishes safe internal operation but shows why incremental
parse reuse remains a performance milestone for extreme edits.

## Endurance status

The one-hour and eight-hour host-integrated tiers have not run yet. No duration,
memory plateau, crash, deadlock, lost-key, or ordering claim will be made until
the TSF host harness, event recorder, and replay minimizer are connected.

