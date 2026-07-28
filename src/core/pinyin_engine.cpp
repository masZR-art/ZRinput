#include "core/pinyin_engine.h"

#include <algorithm>
#include <cctype>

namespace zrinput {
namespace {
std::string Normalize(std::string value) {
  value.erase(std::remove(value.begin(), value.end(), '\''), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}
}

void PinyinEngine::AddEntry(std::string pinyin,
                            std::string text,
                            double frequency) {
  dictionary_[Normalize(std::move(pinyin))].push_back(
      {std::move(text), frequency, 0});
}

std::vector<Candidate> PinyinEngine::Query(const LearningEvent& request,
                                           std::size_t limit) const {
  const auto found = dictionary_.find(Normalize(request.input));
  if (found == dictionary_.end())
    return {};
  auto result = found->second;
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
