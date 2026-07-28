#include "core/personal_language_model.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace zrinput {
namespace {
constexpr char kSeparator = '\x1f';
constexpr double kHalfLifeSeconds = 120.0 * 24.0 * 60.0 * 60.0;

bool IsSentenceBoundary(const std::string& token) {
  return token.find_first_of(".!?;\n\r") != std::string::npos ||
         token.find("。") != std::string::npos ||
         token.find("！") != std::string::npos ||
         token.find("？") != std::string::npos ||
         token.find("；") != std::string::npos;
}
}

void PersonalLanguageModel::Accept(const LearningEvent& event) {
  if (event.private_mode || event.text.empty())
    return;
  Update(usage_["global"][event.text], true, event.timestamp);
  if (!event.input.empty())
    Update(usage_["input" + std::string(1, kSeparator) + event.input]
                 [event.text],
           true, event.timestamp);
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(event));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    Update(usage_[ContextKey(event, depth, false)][event.text], true,
           event.timestamp);
    if (!event.application.empty())
      Update(usage_[ContextKey(event, depth, true)][event.text], true,
             event.timestamp);
  }
}

void PersonalLanguageModel::Reject(const LearningEvent& event) {
  if (event.private_mode || event.text.empty())
    return;
  Update(usage_["global"][event.text], false, event.timestamp);
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(event));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    Update(usage_[ContextKey(event, depth, false)][event.text], false,
           event.timestamp);
    if (!event.application.empty())
      Update(usage_[ContextKey(event, depth, true)][event.text], false,
             event.timestamp);
  }
}

double PersonalLanguageModel::Score(const std::string& candidate,
                                    const LearningEvent& context) const {
  double score = 0;
  const auto add = [&](const std::string& key, double weight) {
    const auto group = usage_.find(key);
    if (group == usage_.end())
      return;
    const auto item = group->second.find(candidate);
    if (item != group->second.end())
      score += weight * UsageScore(item->second, context.timestamp);
  };
  add("global", 0.2);
  if (!context.input.empty())
    add("input" + std::string(1, kSeparator) + context.input, 0.8);
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(context));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    const double weight = 0.7 + 0.35 * static_cast<double>(depth);
    add(ContextKey(context, depth, false), weight);
    if (!context.application.empty())
      add(ContextKey(context, depth, true), weight * 1.35);
  }
  return score;
}

std::vector<std::string> PersonalLanguageModel::Predict(
    const LearningEvent& context,
    std::size_t limit) const {
  std::set<std::string> candidates;
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(context));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    for (const bool app : {false, true}) {
      if (app && context.application.empty())
        continue;
      const auto found = usage_.find(ContextKey(context, depth, app));
      if (found != usage_.end())
        for (const auto& [text, unused] : found->second)
          candidates.insert(text);
    }
  }
  std::vector<std::string> result(candidates.begin(), candidates.end());
  std::stable_sort(result.begin(), result.end(), [&](const auto& left,
                                                     const auto& right) {
    return Score(left, context) > Score(right, context);
  });
  if (result.size() > limit)
    result.resize(limit);
  return result;
}

std::size_t PersonalLanguageModel::size() const {
  std::size_t result = 0;
  for (const auto& [unused, candidates] : usage_)
    result += candidates.size();
  return result;
}

std::string PersonalLanguageModel::ContextKey(const LearningEvent& event,
                                              std::size_t depth,
                                              bool include_application) {
  std::string key = include_application ? "app" : "context";
  if (include_application)
    key += std::string(1, kSeparator) + event.application;
  key += std::string(1, kSeparator) + std::to_string(depth);
  const auto begin = event.context.size() - depth;
  for (std::size_t i = begin; i < event.context.size(); ++i)
    key += std::string(1, kSeparator) + event.context[i];
  return key;
}

std::size_t PersonalLanguageModel::EffectiveContextSize(
    const LearningEvent& event) {
  std::size_t available = 0;
  for (auto it = event.context.rbegin(); it != event.context.rend(); ++it) {
    if (IsSentenceBoundary(*it))
      break;
    ++available;
  }
  return available;
}

void PersonalLanguageModel::Update(Usage& usage,
                                   bool accepted,
                                   std::int64_t timestamp) {
  accepted ? usage.accepted += 1 : usage.rejected += 1;
  usage.last_used = std::max(usage.last_used, timestamp);
}

double PersonalLanguageModel::UsageScore(const Usage& usage,
                                         std::int64_t now) {
  const auto age = std::max<std::int64_t>(0, now - usage.last_used);
  const double decay = std::exp2(-static_cast<double>(age) / kHalfLifeSeconds);
  return decay * (std::log1p(usage.accepted) -
                  2.5 * std::log1p(usage.rejected));
}

}  // namespace zrinput
