#include "core/ranking.h"

#include <algorithm>
#include <cmath>

namespace zrinput::core {
namespace {

bool FiniteInRange(double value, double minimum, double maximum) {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

}  // namespace

bool RankingWeights::IsValid() const noexcept {
  return FiniteInRange(pinyin_match, 0.0, 100.0) &&
         FiniteInRange(static_frequency, 0.0, 100.0) &&
         FiniteInRange(user_frequency, 0.0, 100.0) &&
         FiniteInRange(recency, 0.0, 100.0) &&
         FiniteInRange(context, 0.0, 100.0) &&
         FiniteInRange(application, 0.0, 100.0) &&
         FiniteInRange(sentence_coverage, 0.0, 100.0) &&
         FiniteInRange(correction_penalty, 0.0, 100.0) &&
         FiniteInRange(completion_penalty, 0.0, 100.0) &&
         FiniteInRange(negative_feedback, 0.0, 100.0) &&
         FiniteInRange(recency_half_life_days, 0.01, 36500.0) &&
         FiniteInRange(static_frequency_scale, 1.0, 1.0e15);
}

ScoreBreakdown ScoreCandidate(const RankingFeatures& features,
                              const RankingWeights& weights) {
  ScoreBreakdown score;
  score.weighted.pinyin_match = features.pinyin_match * weights.pinyin_match;
  score.weighted.static_frequency =
      features.static_frequency * weights.static_frequency;
  score.weighted.user_frequency =
      features.user_frequency * weights.user_frequency;
  score.weighted.recency = features.recency * weights.recency;
  score.weighted.context = features.context * weights.context;
  score.weighted.application = features.application * weights.application;
  score.weighted.sentence_coverage =
      features.sentence_coverage * weights.sentence_coverage;
  score.weighted.correction_cost =
      -features.correction_cost * weights.correction_penalty;
  score.weighted.completion =
      -features.completion * weights.completion_penalty;
  score.weighted.negative_feedback =
      -features.negative_feedback * weights.negative_feedback;
  const auto& weighted = score.weighted;
  score.total = weighted.pinyin_match + weighted.static_frequency +
                weighted.user_frequency + weighted.recency + weighted.context +
                weighted.application + weighted.sentence_coverage +
                weighted.correction_cost + weighted.completion +
                weighted.negative_feedback;
  return score;
}

double NormalizeStaticFrequency(double frequency,
                                const RankingWeights& weights) noexcept {
  if (!std::isfinite(frequency) || frequency <= 0.0 ||
      !weights.IsValid()) {
    return 0.0;
  }
  return std::clamp(std::log1p(frequency) /
                        std::log1p(weights.static_frequency_scale),
                    0.0, 1.5);
}

double TimeDecay(std::int64_t last_used_seconds,
                 std::int64_t now_seconds,
                 const RankingWeights& weights) noexcept {
  if (last_used_seconds <= 0 || now_seconds < last_used_seconds ||
      !weights.IsValid()) {
    return 0.0;
  }
  constexpr double kSecondsPerDay = 86400.0;
  const double age_days =
      static_cast<double>(now_seconds - last_used_seconds) / kSecondsPerDay;
  return std::exp2(-age_days / weights.recency_half_life_days);
}

}  // namespace zrinput::core

