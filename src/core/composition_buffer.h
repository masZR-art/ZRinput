#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

namespace zrinput::core {

struct CompositionLimits {
  std::size_t display_units = 96;
  std::size_t active_units = 256;
  std::size_t parser_units = 1024;
  std::size_t hard_units = 4096;

  [[nodiscard]] bool IsValid() const noexcept;
};

struct TextSelection {
  std::size_t anchor = 0;
  std::size_t caret = 0;

  [[nodiscard]] std::size_t begin() const noexcept;
  [[nodiscard]] std::size_t end() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return anchor == caret; }
};

enum class EditOutcome {
  kApplied,
  kNoChange,
  kNeedsStablePrefixCommit,
  kRejectedAtHardLimit,
  kInvalidSelection,
};

enum class CursorMove {
  kPreviousCodePoint,
  kNextCodePoint,
  kHome,
  kEnd,
};

class CompositionBuffer {
 public:
  explicit CompositionBuffer(CompositionLimits limits = {});

  [[nodiscard]] EditOutcome Insert(std::u16string_view text);
  [[nodiscard]] EditOutcome EraseBackward(bool by_word = false);
  [[nodiscard]] EditOutcome EraseForward(bool by_word = false);
  [[nodiscard]] bool Move(CursorMove move, bool extend_selection = false);
  [[nodiscard]] bool SetSelection(TextSelection selection);
  [[nodiscard]] bool ReplaceForReplay(std::u16string text,
                                      TextSelection selection);
  [[nodiscard]] std::optional<std::u16string> CommitPrefix(
      std::size_t units);
  void Clear() noexcept;

  [[nodiscard]] const std::u16string& text() const noexcept { return text_; }
  [[nodiscard]] TextSelection selection() const noexcept { return selection_; }
  [[nodiscard]] std::size_t cursor() const noexcept { return selection_.caret; }
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
  [[nodiscard]] const CompositionLimits& limits() const noexcept {
    return limits_;
  }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] bool at_soft_limit() const noexcept {
    return text_.size() >= limits_.active_units;
  }

 private:
  [[nodiscard]] EditOutcome EraseSelection();
  void CollapseOrExtend(std::size_t position, bool extend_selection);
  void MarkChanged() noexcept;

  CompositionLimits limits_;
  std::u16string text_;
  TextSelection selection_;
  std::uint64_t version_ = 0;
};

}  // namespace zrinput::core
