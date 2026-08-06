#include "core/decoder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace zrinput::core {
namespace {

std::string JoinReading(const ParsePath& path) {
  std::string result;
  for (const auto& syllable : path.syllables) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += syllable.normalized;
  }
  return result;
}

std::string CompactPath(const ParsePath& path) {
  std::string result;
  for (const auto& syllable : path.syllables) {
    result += syllable.normalized;
  }
  return result;
}

std::string InitialPath(const ParsePath& path) {
  std::string result;
  for (const auto& syllable : path.syllables) {
    if (!syllable.normalized.empty()) {
      result.push_back(syllable.normalized.front());
    }
  }
  return result;
}

double CorrectionCost(const ParsePath& path) {
  double result = 0.0;
  for (const auto& syllable : path.syllables) {
    result += syllable.correction_cost;
  }
  return result;
}

bool HasIncomplete(const ParsePath& path) {
  return std::any_of(path.syllables.begin(), path.syllables.end(),
                     [](const SyllableSpan& span) { return !span.complete; });
}

bool AllAbbreviated(const ParsePath& path) {
  return !path.syllables.empty() &&
         std::all_of(path.syllables.begin(), path.syllables.end(),
                     [](const SyllableSpan& span) {
                       return span.abbreviated;
                     });
}

}  // namespace

Decoder::Decoder(RankingWeights weights) : weights_(weights) {
  if (!weights_.IsValid()) {
    throw std::invalid_argument("invalid ranking weights");
  }
}

DecodeResult Decoder::Decode(
    const DecodeRequest& request,
    const std::shared_ptr<const DictionarySnapshot>& dictionary,
    const PersonalizationView* personalization) const {
  DecodeResult result;
  result.composition_version = request.composition_version;
  if (!dictionary || request.candidate_limit == 0) {
    return result;
  }

  std::unordered_map<std::string, DecodedCandidate> unique;
  for (const auto& path : request.analysis.paths) {
    if (path.syllables.empty()) {
      continue;
    }
    const std::string reading = JoinReading(path);
    const std::string compact = CompactPath(path);
    const bool abbreviated = AllAbbreviated(path);
    const bool incomplete = HasIncomplete(path);
    std::vector<const DictionaryEntry*> matches;
    if (!incomplete) {
      matches = dictionary->LookupExact(reading, request.candidate_limit * 2);
    }
    if (matches.empty() && abbreviated) {
      matches = dictionary->LookupInitials(InitialPath(path),
                                           request.candidate_limit * 2);
    }
    if (matches.empty()) {
      matches = dictionary->LookupCompactPrefix(compact,
                                                request.candidate_limit * 2);
    }

    for (const DictionaryEntry* entry : matches) {
      DecodedCandidate candidate;
      candidate.text = entry->text;
      candidate.reading = entry->reading;
      candidate.consumed_units = path.consumed_units;
      candidate.residual_raw = request.analysis.raw.substr(
          std::min(path.consumed_units, request.analysis.raw.size()));
      candidate.completion = CompactReading(entry->reading) != compact;
      candidate.strongest_layer = entry->layer;
      const double coverage = request.analysis.raw.empty()
                                  ? 0.0
                                  : static_cast<double>(path.consumed_units) /
                                        static_cast<double>(
                                            request.analysis.raw.size());
      candidate.features.pinyin_match =
          std::clamp(std::exp(-path.cost), 0.0, 1.0);
      candidate.features.static_frequency =
          NormalizeStaticFrequency(entry->frequency, weights_);
      candidate.features.sentence_coverage = std::clamp(coverage, 0.0, 1.0);
      candidate.features.correction_cost = CorrectionCost(path);
      candidate.features.completion = candidate.completion ? 1.0 : 0.0;
      if (personalization) {
        const PersonalizationFeatures personal = personalization->FeaturesFor(
            compact, candidate.text, request.application, request.context,
            request.now_seconds);
        candidate.features.user_frequency = personal.user_frequency;
        candidate.features.recency = personal.recency;
        candidate.features.context = personal.context;
        candidate.features.application = personal.application;
        candidate.features.negative_feedback = personal.negative_feedback;
      }
      candidate.score = ScoreCandidate(candidate.features, weights_);
      auto found = unique.find(candidate.text);
      if (found == unique.end() ||
          candidate.score.total > found->second.score.total) {
        unique[candidate.text] = std::move(candidate);
      }
    }
  }

  result.candidates.reserve(unique.size());
  for (auto& [text, candidate] : unique) {
    static_cast<void>(text);
    result.candidates.push_back(std::move(candidate));
  }
  std::stable_sort(result.candidates.begin(), result.candidates.end(),
                   [](const DecodedCandidate& left,
                      const DecodedCandidate& right) {
                     if (left.score.total != right.score.total) {
                       return left.score.total > right.score.total;
                     }
                     if (left.completion != right.completion) {
                       return !left.completion;
                     }
                     return left.text < right.text;
                   });
  if (result.candidates.size() > request.candidate_limit) {
    result.candidates.resize(request.candidate_limit);
  }
  return result;
}

}  // namespace zrinput::core
