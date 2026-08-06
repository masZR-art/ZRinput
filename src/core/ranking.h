#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace zrinput::core {

struct RankingWeights {
  double pinyin_match = 4.0;
  double static_frequency = 1.0;
  double user_frequency = 1.6;
  double recency = 0.9;
  double context = 1.4;
  double application = 0.7;
  double sentence_coverage = 1.1;
  double correction_penalty = 1.3;
  double completion_penalty = 0.35;
  double negative_feedback = 2.0;
  double recency_half_life_days = 30.0;
  double static_frequency_scale = 100000.0;

  [[nodiscard]] bool IsValid() const noexcept;
};

struct RankingFeatures {
  double pinyin_match = 0.0;
  double static_frequency = 0.0;
  double user_frequency = 0.0;
  double recency = 0.0;
  double context = 0.0;
  double application = 0.0;
  double sentence_coverage = 0.0;
  double correction_cost = 0.0;
  double completion = 0.0;
  double negative_feedback = 0.0;
};

struct ScoreBreakdown {
  RankingFeatures weighted;
  double total = 0.0;
};

ScoreBreakdown ScoreCandidate(const RankingFeatures& features,
                              const RankingWeights& weights);
double NormalizeStaticFrequency(double frequency,
                                const RankingWeights& weights) noexcept;
double TimeDecay(std::int64_t last_used_seconds,
                 std::int64_t now_seconds,
                 const RankingWeights& weights) noexcept;

}  // namespace zrinput::core

