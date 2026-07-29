#include "windows/candidate_layout.h"

#include <algorithm>

namespace zrinput::windows {
namespace {

constexpr int kReferenceHeight = 41;
constexpr int kOuterLeft = 6;
constexpr int kSelectedVerticalInset = 3;
constexpr int kMinimumItemWidth = 83;
constexpr int kMaximumItemWidth = 160;
constexpr int kItemHorizontalPadding = 45;
constexpr int kCandidateTrailingSpace = 5;
constexpr int kSeparatorWidth = 1;
constexpr int kSeparatorTopInset = 3;
constexpr int kSeparatorBottomInset = 2;
constexpr int kPagerLeadingSpace = 1;
constexpr int kPageButtonWidth = 32;
constexpr int kToolsLeadingSpace = 0;
constexpr int kSymbolsButtonWidth = 36;
constexpr int kMenuButtonWidth = 35;
constexpr int kAccentLeftInset = 6;
constexpr int kAccentWidth = 3;
constexpr int kAccentReferenceHeight = 19;

int ScaleVertical(int value, int height) {
  return value * height / kReferenceHeight;
}

CandidateRect SeparatorAt(int x, int height) {
  const int top = std::max(1, ScaleVertical(kSeparatorTopInset, height));
  const int bottom =
      std::max(1, ScaleVertical(kSeparatorBottomInset, height));
  return {x, top, x + kSeparatorWidth, std::max(top + 1, height - bottom)};
}

}  // namespace

CandidateLayout CalculateCandidateLayout(const std::vector<int>& label_widths,
                                         int window_height) {
  CandidateLayout layout;
  layout.height = std::max(window_height, 1);

  const int selected_inset =
      std::max(1, ScaleVertical(kSelectedVerticalInset, layout.height));
  int x = kOuterLeft;
  layout.candidate_items.reserve(label_widths.size());
  for (const int text_width : label_widths) {
    const int item_width = std::clamp(
        std::max(text_width, 0) + kItemHorizontalPadding,
        kMinimumItemWidth, kMaximumItemWidth);
    layout.candidate_items.push_back(
        {x, selected_inset, x + item_width, layout.height - selected_inset});
    x += item_width;
  }

  if (!layout.candidate_items.empty()) {
    const CandidateRect& selected = layout.candidate_items.front();
    const int accent_height = std::min(
        std::max(1, ScaleVertical(kAccentReferenceHeight, layout.height)),
        selected.Height());
    const int accent_top = (layout.height - accent_height) / 2;
    layout.accent = {selected.left + kAccentLeftInset, accent_top,
                     selected.left + kAccentLeftInset + kAccentWidth,
                     accent_top + accent_height};
  }

  x += kCandidateTrailingSpace;
  layout.candidate_separator = SeparatorAt(x, layout.height);
  x += kSeparatorWidth + kPagerLeadingSpace;
  layout.previous_page = {x, 0, x + kPageButtonWidth, layout.height};
  x += kPageButtonWidth;
  layout.next_page = {x, 0, x + kPageButtonWidth, layout.height};
  x += kPageButtonWidth + kToolsLeadingSpace;
  layout.tools_separator = SeparatorAt(x, layout.height);
  x += kSeparatorWidth;
  layout.symbols = {x, 0, x + kSymbolsButtonWidth, layout.height};
  x += kSymbolsButtonWidth;
  layout.menu_separator = SeparatorAt(x, layout.height);
  x += kSeparatorWidth;
  layout.menu = {x, 0, x + kMenuButtonWidth, layout.height};
  x += kMenuButtonWidth;
  layout.width = x;
  return layout;
}

}  // namespace zrinput::windows
