#include "core/composition_buffer.h"

#include "common/utf.h"

#include <algorithm>
#include <stdexcept>

namespace zrinput::core {

bool CompositionLimits::IsValid() const noexcept {
  return display_units > 0 && display_units <= active_units &&
         active_units <= parser_units && parser_units <= hard_units;
}

std::size_t TextSelection::begin() const noexcept {
  return std::min(anchor, caret);
}

std::size_t TextSelection::end() const noexcept {
  return std::max(anchor, caret);
}

CompositionBuffer::CompositionBuffer(CompositionLimits limits)
    : limits_(limits) {
  if (!limits_.IsValid()) {
    throw std::invalid_argument("invalid composition limits");
  }
}

EditOutcome CompositionBuffer::Insert(std::u16string_view text) {
  if (text.empty()) {
    return EditOutcome::kNoChange;
  }
  const std::size_t retained = text_.size() -
                               (selection_.end() - selection_.begin());
  if (text.size() > limits_.hard_units - retained) {
    return EditOutcome::kRejectedAtHardLimit;
  }
  if (text.size() > limits_.active_units -
                        std::min(retained, limits_.active_units)) {
    return EditOutcome::kNeedsStablePrefixCommit;
  }
  const std::size_t position = selection_.begin();
  text_.replace(position, selection_.end() - position, text);
  selection_ = {position + text.size(), position + text.size()};
  MarkChanged();
  return EditOutcome::kApplied;
}

EditOutcome CompositionBuffer::EraseSelection() {
  if (selection_.empty()) {
    return EditOutcome::kNoChange;
  }
  const std::size_t position = selection_.begin();
  text_.erase(position, selection_.end() - position);
  selection_ = {position, position};
  MarkChanged();
  return EditOutcome::kApplied;
}

EditOutcome CompositionBuffer::EraseBackward(bool by_word) {
  if (!selection_.empty()) {
    return EraseSelection();
  }
  if (selection_.caret == 0) {
    return EditOutcome::kNoChange;
  }
  std::size_t begin = utf::PreviousCodePoint(text_, selection_.caret);
  if (by_word) {
    while (begin > 0 && !utf::IsAsciiWord(text_[begin])) {
      begin = utf::PreviousCodePoint(text_, begin);
    }
    while (begin > 0) {
      const std::size_t previous = utf::PreviousCodePoint(text_, begin);
      if (!utf::IsAsciiWord(text_[previous])) {
        break;
      }
      begin = previous;
    }
  }
  text_.erase(begin, selection_.caret - begin);
  selection_ = {begin, begin};
  MarkChanged();
  return EditOutcome::kApplied;
}

EditOutcome CompositionBuffer::EraseForward(bool by_word) {
  if (!selection_.empty()) {
    return EraseSelection();
  }
  if (selection_.caret == text_.size()) {
    return EditOutcome::kNoChange;
  }
  std::size_t end = utf::NextCodePoint(text_, selection_.caret);
  if (by_word) {
    while (end < text_.size() && !utf::IsAsciiWord(text_[end])) {
      end = utf::NextCodePoint(text_, end);
    }
    while (end < text_.size() && utf::IsAsciiWord(text_[end])) {
      end = utf::NextCodePoint(text_, end);
    }
  }
  text_.erase(selection_.caret, end - selection_.caret);
  selection_.anchor = selection_.caret;
  MarkChanged();
  return EditOutcome::kApplied;
}

bool CompositionBuffer::Move(CursorMove move, bool extend_selection) {
  std::size_t target = selection_.caret;
  switch (move) {
    case CursorMove::kPreviousCodePoint:
      if (!extend_selection && !selection_.empty()) {
        target = selection_.begin();
      } else {
        target = utf::PreviousCodePoint(text_, target);
      }
      break;
    case CursorMove::kNextCodePoint:
      if (!extend_selection && !selection_.empty()) {
        target = selection_.end();
      } else {
        target = utf::NextCodePoint(text_, target);
      }
      break;
    case CursorMove::kHome:
      target = 0;
      break;
    case CursorMove::kEnd:
      target = text_.size();
      break;
  }
  if ((!extend_selection && selection_.anchor == target &&
       selection_.caret == target) ||
      (extend_selection && selection_.caret == target)) {
    return false;
  }
  CollapseOrExtend(target, extend_selection);
  MarkChanged();
  return true;
}

bool CompositionBuffer::SetSelection(TextSelection selection) {
  if (selection.anchor > text_.size() || selection.caret > text_.size()) {
    return false;
  }
  if (selection_.anchor == selection.anchor &&
      selection_.caret == selection.caret) {
    return true;
  }
  selection_ = selection;
  MarkChanged();
  return true;
}

bool CompositionBuffer::ReplaceForReplay(std::u16string text,
                                         TextSelection selection) {
  if (text.size() > limits_.hard_units || selection.anchor > text.size() ||
      selection.caret > text.size()) {
    return false;
  }
  text_ = std::move(text);
  selection_ = selection;
  MarkChanged();
  return true;
}

std::optional<std::u16string> CompositionBuffer::CommitPrefix(
    std::size_t units) {
  if (units == 0 || units > text_.size() ||
      (units < text_.size() && utf::IsHighSurrogate(text_[units - 1]) &&
       utf::IsLowSurrogate(text_[units]))) {
    return std::nullopt;
  }
  std::u16string prefix = text_.substr(0, units);
  text_.erase(0, units);
  const auto translate = [units](std::size_t offset) {
    return offset <= units ? std::size_t{0} : offset - units;
  };
  selection_.anchor = translate(selection_.anchor);
  selection_.caret = translate(selection_.caret);
  MarkChanged();
  return prefix;
}

void CompositionBuffer::Clear() noexcept {
  if (text_.empty() && selection_.empty()) {
    return;
  }
  text_.clear();
  selection_ = {};
  MarkChanged();
}

void CompositionBuffer::CollapseOrExtend(std::size_t position,
                                         bool extend_selection) {
  if (extend_selection) {
    selection_.caret = position;
  } else {
    selection_ = {position, position};
  }
}

void CompositionBuffer::MarkChanged() noexcept {
  ++version_;
}

}  // namespace zrinput::core
