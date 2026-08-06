#include "common/crc32.h"
#include "core/decoder.h"
#include "core/dictionary.h"
#include "core/pinyin_parser.h"
#include "core/ranking.h"

#include "test_harness.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zrinput::core::DecodeRequest;
using zrinput::core::Decoder;
using zrinput::core::DictionaryEntry;
using zrinput::core::DictionaryError;
using zrinput::core::DictionaryLayer;
using zrinput::core::DictionaryPackage;
using zrinput::core::DictionaryService;
using zrinput::core::PersonalizationFeatures;
using zrinput::core::PersonalizationView;
using zrinput::core::PinyinParser;
using zrinput::core::RankingFeatures;
using zrinput::core::RankingWeights;

std::string Utf8China() {
  return "\xE4\xB8\xAD\xE5\x9B\xBD";
}

std::string Utf8MiddleKingdom() {
  return "\xE4\xB8\xAD\xE5\x9B\xBD\xE5\xAE\xB6";
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    path_ = std::filesystem::temp_directory_path() /
            ("zrinput-dictionary-test-" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

std::vector<DictionaryEntry> TestEntries() {
  return {
      {"zhong guo", Utf8China(), 100000.0F, DictionaryLayer::kSystem, 0},
      {"zhong guo", Utf8MiddleKingdom(), 100.0F,
       DictionaryLayer::kSystem, 0},
      {"ni hao", "hello", 50000.0F, DictionaryLayer::kSystem, 0},
  };
}

PinyinParser TestParser() {
  PinyinParser parser;
  parser.ReplaceSyllables({"guo", "hao", "ni", "zhong"});
  return parser;
}

class PreferLongCandidate final : public PersonalizationView {
 public:
  PersonalizationFeatures FeaturesFor(
      std::string_view,
      std::string_view candidate,
      std::string_view,
      std::span<const std::string>,
      std::int64_t) const override {
    PersonalizationFeatures features;
    if (candidate == Utf8MiddleKingdom()) {
      features.user_frequency = 10.0;
      features.recency = 1.0;
    }
    return features;
  }
};

ZR_TEST(Crc32MatchesPublishedCheckValue) {
  ZR_EXPECT_EQ(zrinput::Crc32("123456789"), std::uint32_t{0xCBF43926u});
}

ZR_TEST(DictionaryPackageRoundTripsAndBuildsIndexes) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "system.zrdict";
  const auto entries = TestEntries();
  const auto write = DictionaryPackage::WriteAtomic(path, entries);
  ZR_EXPECT_TRUE(static_cast<bool>(write));

  std::vector<DictionaryEntry> loaded;
  const auto read = DictionaryPackage::Load(
      path, DictionaryLayer::kSystem, &loaded);
  ZR_EXPECT_TRUE(static_cast<bool>(read));
  ZR_EXPECT_EQ(read.loaded_entries, entries.size());

  zrinput::core::DictionarySnapshot snapshot(std::move(loaded));
  ZR_EXPECT_EQ(snapshot.LookupExact("zhong guo").size(), std::size_t{2});
  ZR_EXPECT_EQ(snapshot.LookupCompactPrefix("zhongg").front()->text,
               Utf8China());
  ZR_EXPECT_EQ(snapshot.LookupInitials("zg").size(), std::size_t{2});
}

ZR_TEST(DictionaryPackageMigratesVersionOneRecords) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "legacy.zrdict";
  auto entries = TestEntries();
  entries.front().flags = 42;
  ZR_EXPECT_TRUE(static_cast<bool>(DictionaryPackage::WriteAtomic(
      path, entries, 1)));
  std::vector<DictionaryEntry> loaded;
  const auto report = DictionaryPackage::Load(
      path, DictionaryLayer::kDomain, &loaded);
  ZR_EXPECT_TRUE(static_cast<bool>(report));
  ZR_EXPECT_TRUE(report.migrated);
  ZR_EXPECT_EQ(report.source_version, std::uint32_t{1});
  ZR_EXPECT_EQ(loaded.front().flags, std::uint16_t{0});
  ZR_EXPECT_EQ(loaded.front().layer, DictionaryLayer::kDomain);
}

ZR_TEST(DictionaryPackageRejectsChecksumCorruption) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "corrupt.zrdict";
  const auto entries = TestEntries();
  ZR_EXPECT_TRUE(static_cast<bool>(DictionaryPackage::WriteAtomic(path,
                                                                  entries)));
  {
    std::fstream stream(path,
                        std::ios::in | std::ios::out | std::ios::binary);
    stream.seekg(-1, std::ios::end);
    char value = 0;
    stream.read(&value, 1);
    value ^= 0x01;
    stream.seekp(-1, std::ios::end);
    stream.write(&value, 1);
  }
  std::vector<DictionaryEntry> loaded;
  const auto report = DictionaryPackage::Load(
      path, DictionaryLayer::kSystem, &loaded);
  ZR_EXPECT_EQ(report.error, DictionaryError::kChecksumMismatch);
  ZR_EXPECT_TRUE(loaded.empty());
}

ZR_TEST(DictionaryServiceSwapsImmutableLayerSnapshots) {
  DictionaryService service;
  service.ReplaceLayer(DictionaryLayer::kSystem, TestEntries());
  const auto before = service.snapshot();
  const auto generation = service.generation();
  service.ReplaceLayer(DictionaryLayer::kSession,
                       {{"zhong guo", "session", 200000.0F,
                         DictionaryLayer::kSession, 0}});
  const auto after = service.snapshot();
  ZR_EXPECT_TRUE(after != before);
  ZR_EXPECT_EQ(service.generation(), generation + 1);
  ZR_EXPECT_EQ(before->entries().size(), std::size_t{3});
  ZR_EXPECT_EQ(after->entries().size(), std::size_t{4});
}

ZR_TEST(RankingFormulaExposesEveryWeightedTerm) {
  RankingWeights weights;
  RankingFeatures features;
  features.pinyin_match = 1.0;
  features.static_frequency = 0.5;
  features.user_frequency = 2.0;
  features.correction_cost = 1.0;
  features.completion = 1.0;
  const auto score = zrinput::core::ScoreCandidate(features, weights);
  const double expected = weights.pinyin_match +
                          0.5 * weights.static_frequency +
                          2.0 * weights.user_frequency -
                          weights.correction_penalty -
                          weights.completion_penalty;
  ZR_EXPECT_EQ(score.total, expected);
  ZR_EXPECT_EQ(score.weighted.correction_cost,
               -weights.correction_penalty);
}

ZR_TEST(TimeDecayReachesHalfAtConfiguredHalfLife) {
  RankingWeights weights;
  weights.recency_half_life_days = 10.0;
  const std::int64_t now = 2'000'000;
  const std::int64_t last = now - 10 * 86400;
  const double decay = zrinput::core::TimeDecay(last, now, weights);
  ZR_EXPECT_TRUE(decay > 0.499999 && decay < 0.500001);
}

ZR_TEST(DecoderHandlesExactInitialAndIncompleteQueries) {
  auto parser = TestParser();
  auto dictionary =
      std::make_shared<zrinput::core::DictionarySnapshot>(TestEntries());
  Decoder decoder;

  for (const auto& raw : {std::u16string(u"zhongguo"),
                          std::u16string(u"zg"),
                          std::u16string(u"zhongg")}) {
    DecodeRequest request;
    request.composition_version = 17;
    request.analysis = parser.Analyze(raw);
    request.candidate_limit = 8;
    const auto result = decoder.Decode(request, dictionary);
    ZR_EXPECT_EQ(result.composition_version, std::uint64_t{17});
    ZR_EXPECT_TRUE(!result.candidates.empty());
    ZR_EXPECT_EQ(result.candidates.front().text, Utf8China());
  }
}

ZR_TEST(DecoderKeepsInvalidRawTailAfterLegalPrefix) {
  auto parser = TestParser();
  auto dictionary =
      std::make_shared<zrinput::core::DictionarySnapshot>(TestEntries());
  DecodeRequest request;
  request.analysis = parser.Analyze(u"zhongguo#Q7");
  const auto result = Decoder().Decode(request, dictionary);
  ZR_EXPECT_TRUE(!result.candidates.empty());
  ZR_EXPECT_EQ(result.candidates.front().residual_raw,
               std::u16string(u"#Q7"));
}

ZR_TEST(PersonalizationCanOverrideStaticFrequencyWithoutHiddenWeights) {
  auto parser = TestParser();
  auto dictionary =
      std::make_shared<zrinput::core::DictionarySnapshot>(TestEntries());
  DecodeRequest request;
  request.analysis = parser.Analyze(u"zhongguo");
  PreferLongCandidate personalization;
  const auto result = Decoder().Decode(request, dictionary, &personalization);
  ZR_EXPECT_TRUE(!result.candidates.empty());
  ZR_EXPECT_EQ(result.candidates.front().text, Utf8MiddleKingdom());
  ZR_EXPECT_TRUE(result.candidates.front().features.user_frequency > 0.0);
}

}  // namespace
