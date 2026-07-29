#include "core/composition_state.h"

#include <cctype>
#include <utility>

namespace zrinput {

bool CompositionState::SetInput(std::string input) {
  if (!IsValidInput(input))
    return false;
  input_ = std::move(input);
  page_ = 0;
  return true;
}

bool CompositionState::Append(char symbol) {
  if (input_.size() >= kMaxInputLength)
    return false;
  if (symbol >= 'A' && symbol <= 'Z')
    symbol = static_cast<char>(std::tolower(static_cast<unsigned char>(symbol)));
  if (symbol == '\'') {
    if (input_.empty() || input_.back() == '\'')
      return false;
  } else if (symbol < 'a' || symbol > 'z') {
    return false;
  }
  input_.push_back(symbol);
  page_ = 0;
  return true;
}

bool CompositionState::Backspace() {
  if (input_.empty())
    return false;
  input_.pop_back();
  page_ = 0;
  return true;
}

void CompositionState::Reset() {
  input_.clear();
  page_ = 0;
}

bool CompositionState::ChangePage(int delta,
                                  std::size_t candidate_count,
                                  std::size_t page_size) {
  if (candidate_count == 0 || page_size == 0 || delta == 0)
    return false;
  const std::size_t page_count =
      (candidate_count + page_size - 1) / page_size;
  const std::size_t previous = page_;
  if (delta < 0) {
    page_ = page_ > 0 ? page_ - 1 : 0;
  } else if (page_ + 1 < page_count) {
    ++page_;
  }
  return page_ != previous;
}

void CompositionState::CandidatesChanged() {
  page_ = 0;
}

std::optional<std::size_t> CompositionState::CandidateIndex(
    std::size_t slot,
    std::size_t candidate_count,
    std::size_t page_size) const {
  if (page_size == 0 || slot >= page_size)
    return std::nullopt;
  const std::size_t index = page_ * page_size + slot;
  if (index >= candidate_count)
    return std::nullopt;
  return index;
}

bool CompositionState::IsValidInput(const std::string& input) {
  if (input.size() > kMaxInputLength ||
      (!input.empty() && input.front() == '\''))
    return false;
  bool previous_boundary = false;
  for (const char symbol : input) {
    if (symbol == '\'') {
      if (previous_boundary)
        return false;
      previous_boundary = true;
      continue;
    }
    if (symbol < 'a' || symbol > 'z')
      return false;
    previous_boundary = false;
  }
  return true;
}

}  // namespace zrinput
