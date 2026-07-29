#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace zrinput::core {

struct SyllableSpan {
  std::string normalized;
  std::size_t source_begin = 0;
  std::size_t source_end = 0;
  bool complete = true;
  bool abbreviated = false;
  double correction_cost = 0.0;
};

struct ParsePath {
  std::vector<SyllableSpan> syllables;
  std::size_t consumed_units = 0;
  double cost = 0.0;

  [[nodiscard]] bool fully_parsed(std::size_t raw_size) const noexcept {
    return consumed_units == raw_size;
  }
};

struct PinyinAnalysis {
  std::u16string raw;
  std::vector<ParsePath> paths;
  std::size_t legal_prefix_units = 0;
  std::u16string unparsed_tail;

  [[nodiscard]] bool has_legal_prefix() const noexcept {
    return legal_prefix_units > 0 && !paths.empty();
  }
};

struct PinyinParserOptions {
  std::size_t max_paths = 16;
  std::size_t max_syllable_units = 7;
  bool allow_abbreviations = true;
  bool allow_transposition_correction = true;
  bool allow_duplicate_letter_correction = true;
  double abbreviation_cost = 0.75;
  double incomplete_cost = 0.9;
  double correction_cost = 1.2;
  double separator_cost = 0.01;
};

class PinyinParser {
 public:
  explicit PinyinParser(PinyinParserOptions options = {});

  void ReplaceSyllables(std::vector<std::string> syllables);
  [[nodiscard]] bool AddSyllable(std::string syllable);
  [[nodiscard]] PinyinAnalysis Analyze(std::u16string_view raw) const;

  [[nodiscard]] std::optional<std::size_t> FindStableSplit(
      const PinyinAnalysis& analysis,
      std::size_t preferred_units,
      std::size_t minimum_tail_units = 1) const;

 private:
  [[nodiscard]] bool IsSyllable(std::string_view value) const;
  [[nodiscard]] bool IsSyllablePrefix(std::string_view value) const;

  PinyinParserOptions options_;
  std::unordered_set<std::string> syllables_;
  std::unordered_set<std::string> prefixes_;
};

}  // namespace zrinput::core

