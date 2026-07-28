#pragma once

#include "core/personal_language_model.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace zrinput {

struct Candidate {
  std::string text;
  double dictionary_score = 0;
  double personal_score = 0;
};

class PinyinEngine {
 public:
  void AddEntry(std::string pinyin, std::string text, double frequency);
  std::vector<Candidate> Query(const LearningEvent& request,
                               std::size_t limit) const;
  PersonalLanguageModel& memory() { return memory_; }

 private:
  std::unordered_map<std::string, std::vector<Candidate>> dictionary_;
  PersonalLanguageModel memory_;
};

}  // namespace zrinput
