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
| 32 | 0 / 0.0001 / 0.0001 | 0.638 / 0.837 / 1.040 | 2.231 / 3.431 / 3.538 |
| 64 | 0 / 0.0001 / 0.0001 | 1.475 / 3.443 / 4.534 | 2.812 / 3.319 / 3.345 |
| 128 | 0 / 0.0001 / 0.0001 | 6.312 / 7.542 / 8.022 | 2.263 / 3.108 / 3.336 |
| 256 | 0 / 0.0001 / 0.0001 | 10.685 / 16.120 / 17.396 | 3.015 / 3.358 / 3.472 |
| 1024 | 0 / 0.0001 / 0.0001 | 76.639 / 96.669 / 97.017 | 5.377 / 5.667 / 5.705 |

These values are from the final x64 Release verification run on 2026-08-07.
The measured 256-unit parse plus edit P95 is below the 20 ms requirement. The
1024-unit result establishes safe internal operation but shows why incremental
parse reuse remains a performance milestone for extreme edits.

## Endurance status

The one-hour and eight-hour host-integrated tiers have not run yet. No duration,
memory plateau, crash, deadlock, lost-key, or ordering claim will be made until
the TSF host harness, event recorder, and replay minimizer are connected.
