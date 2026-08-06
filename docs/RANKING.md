# Candidate ranking

ZRinput scores candidates with named, validated weights. No ranking constant is
embedded in a decoder branch. Configuration loaders must reject non-finite,
negative, or out-of-range weights before constructing a decoder.

For candidate `c`, input `x`, context `h`, application `a`, and time `t`:

```text
score(c) =
    Wmatch      * pinyin_match(c, x)
  + Wstatic     * log1p(dictionary_frequency(c)) / log1p(static_scale)
  + Wuser       * user_frequency(c, x)
  + Wrecency    * 2 ^ (-age_days(c) / recency_half_life_days)
  + Wcontext    * context_affinity(c, h)
  + Wapplication* application_affinity(c, a)
  + Wcoverage   * parsed_source_units(c) / raw_source_units(x)
  - Wcorrection * correction_cost(c, x)
  - Wcompletion * is_completion(c)
  - Wnegative   * negative_feedback(c, x)
```

Default weights are declared together in `RankingWeights`:

| Feature | Default |
| --- | ---: |
| Pinyin match | 4.0 |
| Static frequency | 1.0 |
| User frequency | 1.6 |
| Recency | 0.9 |
| Context | 1.4 |
| Application | 0.7 |
| Sentence coverage | 1.1 |
| Correction penalty | 1.3 |
| Completion penalty | 0.35 |
| Negative feedback | 2.0 |
| Recency half-life | 30 days |
| Static frequency scale | 100,000 |

Every returned candidate includes both raw features and weighted terms in a
`ScoreBreakdown`, so diagnostics can explain ordering without recording the
user's text in telemetry.

