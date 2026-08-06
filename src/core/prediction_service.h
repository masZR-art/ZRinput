#pragma once

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zrinput::core {

struct PredictionWeights {
  double static_frequency = 1.0;
  double session_frequency = 1.5;
  double recency = 1.0;
  double context_depth = 0.35;
  double application = 0.6;
  double half_life_seconds = 7.0 * 24.0 * 60.0 * 60.0;
  double frequency_scale = 10000.0;

  [[nodiscard]] bool IsValid() const noexcept;
};

struct PredictionEntry {
  std::vector<std::string> context;
  std::string candidate;
  double frequency = 0.0;
  std::string application;
};

struct PredictionCandidate {
  std::string text;
  double score = 0.0;
  double static_term = 0.0;
  double session_term = 0.0;
  double recency_term = 0.0;
  double context_term = 0.0;
  double application_term = 0.0;
};

class PredictionService {
 public:
  explicit PredictionService(PredictionWeights weights = {});

  void ReplaceSystemEntries(std::vector<PredictionEntry> entries);
  [[nodiscard]] bool LearnSession(std::span<const std::string> context,
                                  std::string candidate,
                                  std::string application,
                                  std::int64_t timestamp_seconds);
  [[nodiscard]] std::vector<PredictionCandidate> Predict(
      std::span<const std::string> context,
      std::string_view application,
      std::int64_t now_seconds,
      std::size_t limit) const;
  void ClearSession();

 private:
  struct SessionUsage {
    std::string candidate;
    std::string application;
    std::uint64_t count = 0;
    std::int64_t last_used = 0;
  };

  static constexpr std::size_t kMaximumContextDepth = 4;
  static constexpr std::size_t kMaximumSessionEntries = 4096;

  [[nodiscard]] static std::string ContextKey(
      std::span<const std::string> context,
      std::size_t depth);
  void EvictOldestSessionEntry();

  PredictionWeights weights_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::vector<PredictionEntry>> system_;
  std::unordered_map<std::string, std::vector<SessionUsage>> session_;
  std::size_t session_entry_count_ = 0;
};

}  // namespace zrinput::core
