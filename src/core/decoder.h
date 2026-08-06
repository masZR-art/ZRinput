#pragma once

#include "core/dictionary.h"
#include "core/pinyin_parser.h"
#include "core/ranking.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace zrinput::core {

struct PersonalizationFeatures {
  double user_frequency = 0.0;
  double recency = 0.0;
  double context = 0.0;
  double application = 0.0;
  double negative_feedback = 0.0;
};

class PersonalizationView {
 public:
  virtual ~PersonalizationView() = default;
  [[nodiscard]] virtual PersonalizationFeatures FeaturesFor(
      std::string_view input,
      std::string_view candidate,
      std::string_view application,
      std::span<const std::string> context,
      std::int64_t now_seconds) const = 0;
};

struct DecodeRequest {
  std::uint64_t composition_version = 0;
  PinyinAnalysis analysis;
  std::string application;
  std::vector<std::string> context;
  std::int64_t now_seconds = 0;
  std::size_t candidate_limit = 50;
};

struct DecodedCandidate {
  std::string text;
  std::string reading;
  std::string annotation;
  std::u16string residual_raw;
  std::size_t consumed_units = 0;
  bool completion = false;
  DictionaryLayer strongest_layer = DictionaryLayer::kSystem;
  RankingFeatures features;
  ScoreBreakdown score;
};

struct DecodeResult {
  std::uint64_t composition_version = 0;
  std::vector<DecodedCandidate> candidates;
};

struct DecoderOptions {
  std::size_t sentence_beam_width = 24;
  std::size_t maximum_phrase_syllables = 8;
  std::size_t entries_per_segment = 6;

  [[nodiscard]] bool IsValid() const noexcept {
    return sentence_beam_width > 0 && sentence_beam_width <= 256 &&
           maximum_phrase_syllables > 0 && maximum_phrase_syllables <= 32 &&
           entries_per_segment > 0 && entries_per_segment <= 64;
  }
};

class Decoder {
 public:
  explicit Decoder(RankingWeights weights = {}, DecoderOptions options = {});

  [[nodiscard]] DecodeResult Decode(
      const DecodeRequest& request,
      const std::shared_ptr<const DictionarySnapshot>& dictionary,
      const PersonalizationView* personalization = nullptr,
      std::stop_token stop = {}) const;

  [[nodiscard]] const RankingWeights& weights() const noexcept {
    return weights_;
  }

 private:
  RankingWeights weights_;
  DecoderOptions options_;
};

}  // namespace zrinput::core
