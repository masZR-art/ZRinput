#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace zrinput {

using SyllablePath = std::vector<std::string>;

class PinyinParser {
 public:
  bool RegisterSyllable(std::string syllable);
  std::vector<SyllablePath> Parse(const std::string& input,
                                  std::size_t max_paths = 32) const;

  static std::string Normalize(const std::string& input,
                               bool keep_boundaries);
  static std::string Key(const SyllablePath& syllables);
  static SyllablePath CanonicalSyllables(const std::string& input);

 private:
  std::vector<SyllablePath> ParseChunk(const std::string& chunk,
                                       std::size_t max_paths) const;

  std::unordered_set<std::string> syllables_;
};

}  // namespace zrinput
