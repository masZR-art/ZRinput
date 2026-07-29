#pragma once

#include <cstddef>
#include <vector>

namespace zrinput::windows {

inline constexpr std::size_t kMicrosoftCandidatePageSize = 7;

struct CandidateRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  [[nodiscard]] int Width() const { return right - left; }
  [[nodiscard]] int Height() const { return bottom - top; }
};

struct CandidateLayout {
  int width = 0;
  int height = 0;
  std::vector<CandidateRect> candidate_items;
  CandidateRect accent;
  CandidateRect candidate_separator;
  CandidateRect previous_page;
  CandidateRect next_page;
  CandidateRect tools_separator;
  CandidateRect symbols;
  CandidateRect menu_separator;
  CandidateRect menu;
};

// Text widths are measured by the caller so the layout stays deterministic and
// can be tested without creating a desktop window.
[[nodiscard]] CandidateLayout CalculateCandidateLayout(
    const std::vector<int>& label_widths,
    int window_height);

}  // namespace zrinput::windows
