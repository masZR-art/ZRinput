#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace zrinput {

using SyllablePath = std::vector<std::string>;

class PinyinParser {
 public:
  PinyinParser();

  bool RegisterSyllable(std::string syllable);
  void Clear();
  std::vector<SyllablePath> Parse(const std::string& input,
                                  std::size_t max_paths = 32) const;

  static std::string Normalize(const std::string& input,
                               bool keep_boundaries);
  static std::string Key(const SyllablePath& syllables);
  static SyllablePath CanonicalSyllables(const std::string& input);
  static const std::vector<std::string>& StandardSyllables();

 private:
  std::vector<SyllablePath> ParseChunk(const std::string& chunk,
                                       std::size_t max_paths) const;

  std::unordered_set<std::string> syllables_;
};

}  // namespace zrinput
