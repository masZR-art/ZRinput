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

struct SentenceState {
  std::string text;
  std::string reading;
  double normalized_frequency_sum = 0.0;
  std::size_t segment_count = 0;
  DictionaryLayer strongest_layer = DictionaryLayer::kSystem;
};

std::string JoinSyllableRange(const ParsePath& path,
                              std::size_t begin,
                              std::size_t end) {
  std::string reading;
  for (std::size_t index = begin; index < end; ++index) {
    if (!reading.empty()) {
      reading.push_back(' ');
    }
    reading += path.syllables[index].normalized;
  }
  return reading;
}

std::vector<SentenceState> DecodeSentence(
    const ParsePath& path,
    const DictionarySnapshot& dictionary,
    const RankingWeights& weights,
    const DecoderOptions& options,
    std::stop_token stop) {
  if (path.syllables.size() < 2 || HasIncomplete(path)) {
    return {};
  }
  std::vector<std::vector<SentenceState>> beams(path.syllables.size() + 1);
  beams.front().push_back({});
  for (std::size_t begin = 0; begin < path.syllables.size(); ++begin) {
    if (stop.stop_requested()) {
      return {};
    }
    if (beams[begin].empty()) {
      continue;
    }
    const std::size_t maximum_end = std::min(
        path.syllables.size(), begin + options.maximum_phrase_syllables);
    for (std::size_t end = begin + 1; end <= maximum_end; ++end) {
      if (stop.stop_requested()) {
        return {};
      }
      const std::string segment_reading = JoinSyllableRange(path, begin, end);
      const auto matches =
          dictionary.LookupExact(segment_reading, options.entries_per_segment);
      for (const SentenceState& previous : beams[begin]) {
        for (const DictionaryEntry* entry : matches) {
          SentenceState next = previous;
          next.text += entry->text;
          if (!next.reading.empty()) {
            next.reading.push_back(' ');
          }
          next.reading += entry->reading;
          next.normalized_frequency_sum +=
              NormalizeStaticFrequency(entry->frequency, weights);
          ++next.segment_count;
          next.strongest_layer =
              std::max(next.strongest_layer, entry->layer);
          beams[end].push_back(std::move(next));
        }
      }
      auto& destination = beams[end];
      if (destination.size() > options.sentence_beam_width) {
        const auto compare = [](const SentenceState& left,
                                const SentenceState& right) {
          const double left_quality =
              left.normalized_frequency_sum /
              static_cast<double>(std::max<std::size_t>(1, left.segment_count));
          const double right_quality =
              right.normalized_frequency_sum /
              static_cast<double>(std::max<std::size_t>(1, right.segment_count));
          if (left_quality != right_quality) {
            return left_quality > right_quality;
          }
          if (left.segment_count != right.segment_count) {
            return left.segment_count < right.segment_count;
          }
          return left.text < right.text;
        };
        std::partial_sort(
            destination.begin(),
            destination.begin() + static_cast<std::ptrdiff_t>(
                                      options.sentence_beam_width),
            destination.end(), compare);
        destination.resize(options.sentence_beam_width);
      }
    }
  }
  return std::move(beams.back());
}

}  // namespace

Decoder::Decoder(RankingWeights weights, DecoderOptions options)
    : weights_(weights), options_(options) {
  if (!weights_.IsValid() || !options_.IsValid()) {
    throw std::invalid_argument("invalid decoder configuration");
  }
}

DecodeResult Decoder::Decode(
    const DecodeRequest& request,
    const std::shared_ptr<const DictionarySnapshot>& dictionary,
    const PersonalizationView* personalization,
    std::stop_token stop) const {
  DecodeResult result;
  result.composition_version = request.composition_version;
  if (!dictionary || request.candidate_limit == 0 || stop.stop_requested()) {
    return result;
  }

  std::unordered_map<std::string, DecodedCandidate> unique;
  for (const auto& path : request.analysis.paths) {
    if (stop.stop_requested()) {
      result.candidates.clear();
      return result;
    }
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

    const auto sentences =
        DecodeSentence(path, *dictionary, weights_, options_, stop);
    if (stop.stop_requested()) {
      result.candidates.clear();
      return result;
    }
    for (const auto& sentence : sentences) {
      if (sentence.segment_count < 2 || sentence.text.empty()) {
        continue;
      }
      DecodedCandidate candidate;
      candidate.text = sentence.text;
      candidate.reading = sentence.reading;
      candidate.annotation = "sentence";
      candidate.consumed_units = path.consumed_units;
      candidate.residual_raw = request.analysis.raw.substr(
          std::min(path.consumed_units, request.analysis.raw.size()));
      candidate.strongest_layer = sentence.strongest_layer;
      candidate.features.pinyin_match =
          std::clamp(std::exp(-path.cost), 0.0, 1.0);
      candidate.features.static_frequency =
          sentence.normalized_frequency_sum /
          static_cast<double>(sentence.segment_count);
      candidate.features.sentence_coverage = request.analysis.raw.empty()
                                                 ? 0.0
                                                 : static_cast<double>(
                                                       path.consumed_units) /
                                                       static_cast<double>(
                                                           request.analysis.raw
                                                               .size());
      candidate.features.correction_cost = CorrectionCost(path);
      if (personalization) {
        const PersonalizationFeatures personal = personalization->FeaturesFor(
            CompactPath(path), candidate.text, request.application,
            request.context, request.now_seconds);
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
