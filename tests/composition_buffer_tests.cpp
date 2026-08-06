#include "core/composition_buffer.h"

#include "test_harness.h"

#include <string>

namespace {

using zrinput::core::CompositionBuffer;
using zrinput::core::CompositionLimits;
using zrinput::core::CursorMove;
using zrinput::core::EditOutcome;
using zrinput::core::TextSelection;

ZR_TEST(InsertsAndReplacesSelection) {
  CompositionBuffer buffer;
  ZR_EXPECT_EQ(buffer.Insert(u"nihao"), EditOutcome::kApplied);
  ZR_EXPECT_TRUE(buffer.SetSelection({2, 5}));
  ZR_EXPECT_EQ(buffer.Insert(u"men"), EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.text(), std::u16string(u"nimen"));
  ZR_EXPECT_EQ(buffer.cursor(), std::size_t{5});
}

ZR_TEST(MovesAcrossSurrogatePairWithoutSplittingIt) {
  CompositionBuffer buffer;
  ZR_EXPECT_EQ(buffer.Insert(u"a\U0001F600b"), EditOutcome::kApplied);
  ZR_EXPECT_TRUE(buffer.Move(CursorMove::kPreviousCodePoint));
  ZR_EXPECT_EQ(buffer.cursor(), std::size_t{3});
  ZR_EXPECT_TRUE(buffer.Move(CursorMove::kPreviousCodePoint));
  ZR_EXPECT_EQ(buffer.cursor(), std::size_t{1});
  ZR_EXPECT_EQ(buffer.EraseForward(), EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.text(), std::u16string(u"ab"));
}

ZR_TEST(SoftLimitRequestsStableSplitWithoutMutation) {
  CompositionLimits limits;
  limits.display_units = 4;
  limits.active_units = 8;
  limits.parser_units = 16;
  limits.hard_units = 32;
  CompositionBuffer buffer(limits);
  ZR_EXPECT_EQ(buffer.Insert(u"abcdefgh"), EditOutcome::kApplied);
  const auto version = buffer.version();
  ZR_EXPECT_EQ(buffer.Insert(u"i"),
               EditOutcome::kNeedsStablePrefixCommit);
  ZR_EXPECT_EQ(buffer.text(), std::u16string(u"abcdefgh"));
  ZR_EXPECT_EQ(buffer.version(), version);
}

ZR_TEST(HardLimitProtectsReplayState) {
  CompositionLimits limits;
  limits.display_units = 4;
  limits.active_units = 8;
  limits.parser_units = 16;
  limits.hard_units = 32;
  CompositionBuffer buffer(limits);
  ZR_EXPECT_TRUE(buffer.ReplaceForReplay(std::u16string(32, u'a'), {32, 32}));
  ZR_EXPECT_EQ(buffer.Insert(u"b"), EditOutcome::kRejectedAtHardLimit);
  ZR_EXPECT_EQ(buffer.EraseBackward(), EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.text().size(), std::size_t{31});
}

ZR_TEST(CtrlBackspaceDeletesWordAndBoundaries) {
  CompositionBuffer buffer;
  ZR_EXPECT_EQ(buffer.Insert(u"ni'''hao"), EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.EraseBackward(true), EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.text(), std::u16string(u"ni'''"));
  ZR_EXPECT_EQ(buffer.EraseBackward(true), EditOutcome::kApplied);
  ZR_EXPECT_TRUE(buffer.empty());
}

ZR_TEST(SupportsA1024UnitInternalBuffer) {
  CompositionLimits limits;
  limits.display_units = 96;
  limits.active_units = 1024;
  limits.parser_units = 1024;
  limits.hard_units = 4096;
  CompositionBuffer buffer(limits);
  ZR_EXPECT_EQ(buffer.Insert(std::u16string(1024, u'a')),
               EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.text().size(), std::size_t{1024});
  ZR_EXPECT_TRUE(buffer.SetSelection({512, 513}));
  ZR_EXPECT_EQ(buffer.Insert(u"z"), EditOutcome::kApplied);
  ZR_EXPECT_EQ(buffer.text().size(), std::size_t{1024});
}

ZR_TEST(CommitsPrefixAndTranslatesSelectionWithoutLosingTail) {
  CompositionBuffer buffer;
  ZR_EXPECT_EQ(buffer.Insert(u"xianzaishang"), EditOutcome::kApplied);
  ZR_EXPECT_TRUE(buffer.SetSelection({5, 10}));
  const auto prefix = buffer.CommitPrefix(7);
  ZR_EXPECT_TRUE(prefix.has_value());
  ZR_EXPECT_EQ(*prefix, std::u16string(u"xianzai"));
  ZR_EXPECT_EQ(buffer.text(), std::u16string(u"shang"));
  ZR_EXPECT_EQ(buffer.selection().anchor, std::size_t{0});
  ZR_EXPECT_EQ(buffer.selection().caret, std::size_t{3});
}

ZR_TEST(RefusesPrefixSplitInsideSurrogatePair) {
  CompositionBuffer buffer;
  ZR_EXPECT_EQ(buffer.Insert(u"a\U0001F600b"), EditOutcome::kApplied);
  ZR_EXPECT_TRUE(!buffer.CommitPrefix(2).has_value());
  ZR_EXPECT_EQ(buffer.text(), std::u16string(u"a\U0001F600b"));
}

}  // namespace

int main() {
  return zrinput::test::RunAll();
}
