#include "core/pinyin_engine.h"

#include <algorithm>
#include <unordered_map>

namespace zrinput {
void PinyinEngine::AddEntry(std::string pinyin,
                            std::string text,
                            double frequency) {
  const auto syllables = PinyinParser::CanonicalSyllables(pinyin);
  if (syllables.empty() || text.empty())
    return;
  for (const auto& syllable : syllables)
    parser_.RegisterSyllable(syllable);
  dictionary_[PinyinParser::Key(syllables)].push_back(
      {std::move(text), frequency, 0});
}

std::vector<Candidate> PinyinEngine::Query(const LearningEvent& request,
                                           std::size_t limit) const {
  std::unordered_map<std::string, Candidate> unique;
  for (const auto& path : parser_.Parse(request.input)) {
    const auto found = dictionary_.find(PinyinParser::Key(path));
    if (found == dictionary_.end())
      continue;
    for (const auto& candidate : found->second) {
      const auto existing = unique.find(candidate.text);
      if (existing == unique.end() ||
          existing->second.dictionary_score < candidate.dictionary_score)
        unique[candidate.text] = candidate;
    }
  }
  std::vector<Candidate> result;
  result.reserve(unique.size());
  for (auto& [unused, candidate] : unique)
    result.push_back(std::move(candidate));
  for (auto& candidate : result)
    candidate.personal_score = memory_.Score(candidate.text, request);
  std::stable_sort(result.begin(), result.end(), [](const auto& left,
                                                    const auto& right) {
    return left.dictionary_score + left.personal_score >
           right.dictionary_score + right.personal_score;
  });
  if (result.size() > limit)
    result.resize(limit);
  return result;
}

}  // namespace zrinput
