#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace zrinput {

class CompositionState {
 public:
  static constexpr std::size_t kMaxInputLength = 128;

  bool SetInput(std::string input);
  bool Append(char symbol);
  bool Backspace();
  void Reset();

  bool ChangePage(int delta,
                  std::size_t candidate_count,
                  std::size_t page_size);
  void CandidatesChanged();
  std::optional<std::size_t> CandidateIndex(
      std::size_t slot,
      std::size_t candidate_count,
      std::size_t page_size) const;

  const std::string& input() const { return input_; }
  bool empty() const { return input_.empty(); }
  std::size_t page() const { return page_; }

 private:
  static bool IsValidInput(const std::string& input);

  std::string input_;
  std::size_t page_ = 0;
};

}  // namespace zrinput
