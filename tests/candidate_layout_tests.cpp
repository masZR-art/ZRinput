#include "windows/candidate_layout.h"

#include <iostream>
#include <vector>

namespace {

bool Expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

}  // namespace

int main() {
  using zrinput::windows::CalculateCandidateLayout;

  bool passed = true;
  const auto reference = CalculateCandidateLayout(
      std::vector<int>(zrinput::windows::kMicrosoftCandidatePageSize, 38), 41);
  passed &= Expect(reference.width == 731,
                   "seven one-character candidates must match 731px reference");
  passed &= Expect(reference.height == 41,
                   "reference candidate bar must be 41px high");
  passed &= Expect(reference.candidate_items.size() == 7,
                   "reference page must contain seven candidate slots");
  passed &= Expect(reference.candidate_items.front().left == 6 &&
                       reference.candidate_items.front().top == 3 &&
                       reference.candidate_items.front().Width() == 83 &&
                       reference.candidate_items.front().Height() == 35,
                   "selected candidate geometry drifted");
  passed &= Expect(reference.accent.left == 12 && reference.accent.top == 11 &&
                       reference.accent.Width() == 3 &&
                       reference.accent.Height() == 19,
                   "left accent geometry drifted");
  passed &= Expect(reference.candidate_separator.left == 592 &&
                       reference.previous_page.left == 594 &&
                       reference.next_page.left == 626,
                   "pager geometry drifted");
  passed &= Expect(reference.tools_separator.left == 658 &&
                       reference.symbols.left == 659 &&
                       reference.menu_separator.left == 695 &&
                       reference.menu.left == 696 && reference.menu.right == 731,
                   "right-hand tools geometry drifted");

  const auto long_label = CalculateCandidateLayout({100, 12}, 41);
  passed &= Expect(long_label.candidate_items[0].Width() == 145,
                   "long labels must grow their candidate slot");
  passed &= Expect(long_label.candidate_items[1].Width() == 83,
                   "short labels must retain the compact minimum slot");
  passed &= Expect(long_label.width == 378,
                   "dynamic candidate width must include the fixed tool strip");

  const auto extreme_label = CalculateCandidateLayout({500}, 41);
  passed &= Expect(extreme_label.candidate_items[0].Width() == 160,
                   "extreme labels must be bounded for small displays");

  const auto scaled_height = CalculateCandidateLayout({38}, 82);
  passed &= Expect(scaled_height.height == 82 &&
                       scaled_height.accent.Height() == 38 &&
                       scaled_height.candidate_items[0].top == 6,
                   "vertical metrics must scale with configured height");

  return passed ? 0 : 1;
}
