#include "core/pinyin_engine.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
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
  const auto key = PinyinParser::Key(syllables);
  dictionary_[key].push_back({std::move(text), frequency, 0, false});
  std::string continuous;
  for (const auto& syllable : syllables)
    continuous += syllable;
  for (std::size_t length = 1; length < continuous.size(); ++length) {
    auto& keys = prefix_index_[continuous.substr(0, length)];
    if (std::find(keys.begin(), keys.end(), key) == keys.end())
      keys.push_back(key);
  }
}

DictionaryLoadResult PinyinEngine::LoadDictionary(
    const std::filesystem::path& path,
    bool replace_existing) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {0, 0, "cannot open dictionary"};

  struct PendingEntry {
    std::string pinyin;
    std::string text;
    double frequency;
  };
  std::vector<PendingEntry> pending;
  DictionaryLoadResult result;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line.front() == '#')
      continue;
    std::istringstream fields(line);
    std::string pinyin;
    std::string text;
    std::string frequency_text;
    if (!std::getline(fields, pinyin, '\t') ||
        !std::getline(fields, text, '\t') ||
        !std::getline(fields, frequency_text) || pinyin.empty() ||
        text.empty()) {
      ++result.skipped;
      continue;
    }
    try {
      std::size_t parsed = 0;
      const double frequency = std::stod(frequency_text, &parsed);
      if (parsed != frequency_text.size() || !std::isfinite(frequency) ||
          frequency < 0) {
        ++result.skipped;
        continue;
      }
      pending.push_back({std::move(pinyin), std::move(text), frequency});
    } catch (...) {
      ++result.skipped;
    }
  }
  if (!input.eof())
    return {0, result.skipped, "dictionary read failed"};
  if (replace_existing)
    ClearDictionary();
  for (auto& entry : pending) {
    AddEntry(std::move(entry.pinyin), std::move(entry.text), entry.frequency);
    ++result.loaded;
  }
  return result;
}

void PinyinEngine::ClearDictionary() {
  dictionary_.clear();
  prefix_index_.clear();
  parser_.Clear();
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
  const auto normalized_prefix = PinyinParser::Normalize(request.input, false);
  const auto prefix = prefix_index_.find(normalized_prefix);
  if (prefix != prefix_index_.end()) {
    for (const auto& key : prefix->second) {
      const auto found = dictionary_.find(key);
      if (found == dictionary_.end())
        continue;
      for (auto candidate : found->second) {
        candidate.dictionary_score -= 0.25;
        candidate.is_completion = true;
        const auto existing = unique.find(candidate.text);
        if (existing == unique.end() ||
            existing->second.dictionary_score < candidate.dictionary_score)
          unique[candidate.text] = std::move(candidate);
      }
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
