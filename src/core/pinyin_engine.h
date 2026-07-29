#pragma once

#include "core/personal_language_model.h"
#include "core/pinyin_parser.h"

#include <string>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace zrinput {

struct Candidate {
  std::string text;
  double dictionary_score = 0;
  double personal_score = 0;
  bool is_completion = false;
};

struct DictionaryLoadResult {
  std::size_t loaded = 0;
  std::size_t skipped = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

class PinyinEngine {
 public:
  void AddEntry(std::string pinyin, std::string text, double frequency);
  DictionaryLoadResult LoadDictionary(const std::filesystem::path& path,
                                      bool replace_existing = true);
  void ClearDictionary();
  std::vector<Candidate> Query(const LearningEvent& request,
                               std::size_t limit) const;
  bool HasCompleteStandardSyllableCoverage() const;
  PersonalLanguageModel& memory() { return memory_; }

 private:
  std::unordered_map<std::string, std::vector<Candidate>> dictionary_;
  std::unordered_map<std::string, std::vector<std::string>> prefix_index_;
  PinyinParser parser_;
  PersonalLanguageModel memory_;
};

bool IsRuntimeDictionaryUsable(const PinyinEngine& engine,
                               const DictionaryLoadResult& load_result,
                               std::size_t minimum_entries = 60'000);

}  // namespace zrinput
