#include "core/pinyin_parser.h"

#include "test_harness.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

zrinput::core::PinyinParser MakeParser() {
  zrinput::core::PinyinParser parser;
  parser.ReplaceSyllables({"a", "ai", "an", "ang", "ao", "bei", "guo",
                           "hao", "ing", "jie", "min", "ni", "ren",
                           "shang", "shi", "wo", "xian", "zai", "zhong"});
  return parser;
}

ZR_TEST(PreservesRawInputWhileNormalizingQuerySpans) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"NVu:\u00dc");
  ZR_EXPECT_EQ(analysis.raw, std::u16string(u"NVu:\u00dc"));
  ZR_EXPECT_TRUE(!analysis.paths.empty());
}

ZR_TEST(FindsMultipleLegalSegmentations) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"xianzai");
  ZR_EXPECT_TRUE(!analysis.paths.empty());
  ZR_EXPECT_EQ(analysis.paths.front().consumed_units, std::size_t{7});
  ZR_EXPECT_EQ(analysis.paths.front().syllables.front().normalized,
               std::string("xian"));
  ZR_EXPECT_EQ(analysis.paths.front().syllables.back().normalized,
               std::string("zai"));
}

ZR_TEST(ConsecutiveSeparatorsDoNotCorruptParserState) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"ni''''hao");
  ZR_EXPECT_TRUE(!analysis.paths.empty());
  ZR_EXPECT_EQ(analysis.paths.front().consumed_units, std::size_t{9});
  ZR_EXPECT_EQ(analysis.paths.front().syllables.size(), std::size_t{2});
}

ZR_TEST(ReportsLegalPrefixAndLeavesInvalidTailUntouched) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"nihao#Q7");
  ZR_EXPECT_EQ(analysis.legal_prefix_units, std::size_t{5});
  ZR_EXPECT_EQ(analysis.unparsed_tail, std::u16string(u"#Q7"));
}

ZR_TEST(AcceptsSeededTranspositionCorrection) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"shagn");
  const bool found = std::any_of(
      analysis.paths.begin(), analysis.paths.end(), [](const auto& path) {
        return !path.syllables.empty() &&
               path.syllables.front().normalized == "shang" &&
               path.syllables.front().correction_cost > 0.0;
      });
  ZR_EXPECT_TRUE(found);
}

ZR_TEST(RepresentsSimplifiedPinyinAsAbbreviatedSpans) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"zgrm");
  ZR_EXPECT_TRUE(!analysis.paths.empty());
  const auto& path = analysis.paths.front();
  ZR_EXPECT_EQ(path.consumed_units, std::size_t{4});
  ZR_EXPECT_TRUE(std::all_of(path.syllables.begin(), path.syllables.end(),
                             [](const auto& span) {
                               return span.abbreviated && !span.complete;
                             }));
}

ZR_TEST(SplitsOnlyAtCompleteSyllableBoundaries) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(u"xianzaishang");
  const auto split = parser.FindStableSplit(analysis, 7, 2);
  ZR_EXPECT_TRUE(split.has_value());
  ZR_EXPECT_EQ(*split, std::size_t{7});
}

ZR_TEST(ApostropheOnlyInputHasNoUnsafeStableSplit) {
  auto parser = MakeParser();
  const auto analysis = parser.Analyze(std::u16string(256, u'\''));
  ZR_EXPECT_TRUE(!parser.FindStableSplit(analysis, 128).has_value());
}

}  // namespace
