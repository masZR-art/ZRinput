#include "core/prediction_service.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace zrinput::core {
namespace {

bool Weight(double value) {
  return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

double FrequencyFeature(double frequency, double scale) {
  if (!std::isfinite(frequency) || frequency <= 0.0) {
    return 0.0;
  }
  return std::clamp(std::log1p(frequency) / std::log1p(scale), 0.0, 1.5);
}

double Recency(std::int64_t last_used,
               std::int64_t now,
               double half_life) {
  if (last_used <= 0 || now < last_used) {
    return 0.0;
  }
  return std::exp2(-static_cast<double>(now - last_used) / half_life);
}

}  // namespace

bool PredictionWeights::IsValid() const noexcept {
  return Weight(static_frequency) && Weight(session_frequency) &&
         Weight(recency) && Weight(context_depth) && Weight(application) &&
         std::isfinite(half_life_seconds) && half_life_seconds > 0.0 &&
         std::isfinite(frequency_scale) && frequency_scale >= 1.0;
}

PredictionService::PredictionService(PredictionWeights weights)
    : weights_(weights) {
  if (!weights_.IsValid()) {
    throw std::invalid_argument("invalid prediction weights");
  }
}

void PredictionService::ReplaceSystemEntries(
    std::vector<PredictionEntry> entries) {
  std::unordered_map<std::string, std::vector<PredictionEntry>> replacement;
  for (auto& entry : entries) {
    if (entry.context.empty() ||
        entry.context.size() > kMaximumContextDepth ||
        entry.candidate.empty() || !std::isfinite(entry.frequency) ||
        entry.frequency < 0.0) {
      continue;
    }
    const std::string key = ContextKey(entry.context, entry.context.size());
    replacement[key].push_back(std::move(entry));
  }
  for (auto& [key, values] : replacement) {
    static_cast<void>(key);
    std::stable_sort(values.begin(), values.end(),
                     [](const PredictionEntry& left,
                        const PredictionEntry& right) {
                       if (left.frequency != right.frequency) {
                         return left.frequency > right.frequency;
                       }
                       return left.candidate < right.candidate;
                     });
  }
  std::unique_lock lock(mutex_);
  system_ = std::move(replacement);
}

bool PredictionService::LearnSession(std::span<const std::string> context,
                                     std::string candidate,
                                     std::string application,
                                     std::int64_t timestamp_seconds) {
  if (context.empty() || candidate.empty() || timestamp_seconds <= 0) {
    return false;
  }
  const std::size_t depth =
      std::min(context.size(), kMaximumContextDepth);
  const std::string context_key = ContextKey(context, depth);
  std::unique_lock lock(mutex_);
  auto& usages = session_[context_key];
  auto found = std::find_if(
      usages.begin(), usages.end(),
      [&](const SessionUsage& usage) {
        return usage.candidate == candidate &&
               usage.application == application;
      });
  if (found == usages.end()) {
    if (session_entry_count_ >= kMaximumSessionEntries) {
      EvictOldestSessionEntry();
    }
    auto& destination = session_[context_key];
    destination.push_back(
        {.candidate = std::move(candidate),
         .application = std::move(application),
         .count = 0,
         .last_used = 0});
    ++session_entry_count_;
    found = std::prev(destination.end());
  }
  if (found->count != std::numeric_limits<std::uint64_t>::max()) {
    ++found->count;
  }
  found->last_used = std::max(found->last_used, timestamp_seconds);
  return true;
}

std::vector<PredictionCandidate> PredictionService::Predict(
    std::span<const std::string> context,
    std::string_view application,
    std::int64_t now_seconds,
    std::size_t limit) const {
  if (context.empty() || limit == 0) {
    return {};
  }
  std::shared_lock lock(mutex_);
  std::unordered_map<std::string, PredictionCandidate> candidates;
  const std::size_t maximum_depth =
      std::min(context.size(), kMaximumContextDepth);
  for (std::size_t depth = maximum_depth; depth > 0; --depth) {
    const std::string context_key = ContextKey(context, depth);
    const auto system = system_.find(context_key);
    if (system != system_.end()) {
      for (const PredictionEntry& entry : system->second) {
        if (!entry.application.empty() && entry.application != application) {
          continue;
        }
        PredictionCandidate& candidate = candidates[entry.candidate];
        candidate.text = entry.candidate;
        candidate.static_term = std::max(
            candidate.static_term,
            FrequencyFeature(entry.frequency, weights_.frequency_scale) *
                weights_.static_frequency);
        candidate.context_term = std::max(
            candidate.context_term,
            static_cast<double>(depth) /
                static_cast<double>(kMaximumContextDepth) *
                weights_.context_depth);
        if (!entry.application.empty() && entry.application == application) {
          candidate.application_term = weights_.application;
        }
      }
    }

    const auto session = session_.find(context_key);
    if (session == session_.end()) {
      continue;
    }
    for (const SessionUsage& usage : session->second) {
      if (!usage.application.empty() && usage.application != application) {
        continue;
      }
      PredictionCandidate& candidate = candidates[usage.candidate];
      candidate.text = usage.candidate;
      candidate.session_term = std::max(
          candidate.session_term,
          FrequencyFeature(static_cast<double>(usage.count),
                           weights_.frequency_scale) *
              weights_.session_frequency);
      candidate.recency_term = std::max(
          candidate.recency_term,
          Recency(usage.last_used, now_seconds, weights_.half_life_seconds) *
              weights_.recency);
      candidate.context_term = std::max(
          candidate.context_term,
          static_cast<double>(depth) /
              static_cast<double>(kMaximumContextDepth) *
              weights_.context_depth);
      if (!usage.application.empty() && usage.application == application) {
        candidate.application_term = weights_.application;
      }
    }
  }
  std::vector<PredictionCandidate> result;
  result.reserve(candidates.size());
  for (auto& [text, candidate] : candidates) {
    static_cast<void>(text);
    candidate.score = candidate.static_term + candidate.session_term +
                      candidate.recency_term + candidate.context_term +
                      candidate.application_term;
    result.push_back(std::move(candidate));
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const PredictionCandidate& left,
                      const PredictionCandidate& right) {
                     if (left.score != right.score) {
                       return left.score > right.score;
                     }
                     return left.text < right.text;
                   });
  if (result.size() > limit) {
    result.resize(limit);
  }
  return result;
}

void PredictionService::ClearSession() {
  std::unique_lock lock(mutex_);
  session_.clear();
  session_entry_count_ = 0;
}

std::string PredictionService::ContextKey(
    std::span<const std::string> context,
    std::size_t depth) {
  depth = std::min({depth, context.size(), kMaximumContextDepth});
  std::string result;
  const std::size_t begin = context.size() - depth;
  for (std::size_t index = begin; index < context.size(); ++index) {
    result += std::to_string(context[index].size());
    result.push_back(':');
    result += context[index];
  }
  return result;
}

void PredictionService::EvictOldestSessionEntry() {
  auto oldest_group = session_.end();
  std::size_t oldest_index = 0;
  for (auto group = session_.begin(); group != session_.end(); ++group) {
    for (std::size_t index = 0; index < group->second.size(); ++index) {
      if (oldest_group == session_.end() ||
          group->second[index].last_used <
              oldest_group->second[oldest_index].last_used ||
          (group->second[index].last_used ==
               oldest_group->second[oldest_index].last_used &&
           group->second[index].candidate <
               oldest_group->second[oldest_index].candidate)) {
        oldest_group = group;
        oldest_index = index;
      }
    }
  }
  if (oldest_group == session_.end()) {
    return;
  }
  oldest_group->second.erase(
      oldest_group->second.begin() +
      static_cast<std::ptrdiff_t>(oldest_index));
  --session_entry_count_;
  if (oldest_group->second.empty()) {
    session_.erase(oldest_group);
  }
}

}  // namespace zrinput::core
