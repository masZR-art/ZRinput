#include "core/pinyin_engine.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace zrinput {
namespace {

constexpr std::size_t kQueryParsePaths = 8;
constexpr std::size_t kMinimumDirectCandidates = 7;
constexpr double kMinimumEmbeddedPhraseScore = 4.61512051684126;

struct PhraseBeam {
  std::string text;
  double weighted_score = 0;
  std::size_t syllables = 0;
  std::size_t segments = 0;
  bool completion = false;
};

double BeamScore(const PhraseBeam& beam) {
  if (beam.syllables == 0)
    return 0;
  return beam.weighted_score / static_cast<double>(beam.syllables) -
         2.5 * static_cast<double>(beam.segments > 0 ? beam.segments - 1 : 0) -
         (beam.completion ? 0.25 : 0.0);
}

std::size_t KeySyllableCount(const std::string& key) {
  return key.empty() ? 0
                     : 1 + static_cast<std::size_t>(
                               std::count(key.begin(), key.end(), '\''));
}

void PruneBeams(std::vector<PhraseBeam>& beams, std::size_t limit) {
  std::unordered_map<std::string, PhraseBeam> unique;
  for (auto& beam : beams) {
    const auto found = unique.find(beam.text);
    if (found == unique.end() || BeamScore(beam) > BeamScore(found->second)) {
      const std::string text = beam.text;
      unique.insert_or_assign(text, std::move(beam));
    }
  }
  beams.clear();
  beams.reserve(unique.size());
  for (auto& [unused, beam] : unique)
    beams.push_back(std::move(beam));
  std::sort(beams.begin(), beams.end(), [](const auto& left,
                                           const auto& right) {
    const double left_score = BeamScore(left);
    const double right_score = BeamScore(right);
    if (left_score != right_score)
      return left_score > right_score;
    return left.text < right.text;
  });
  if (beams.size() > limit)
    beams.resize(limit);
}

}  // namespace

void PinyinEngine::AddEntry(std::string pinyin,
                            std::string text,
                            double frequency) {
  const auto syllables = PinyinParser::CanonicalSyllables(pinyin);
  if (syllables.empty() || text.empty())
    return;
  for (const auto& syllable : syllables)
    parser_.RegisterSyllable(syllable);
  const auto key = PinyinParser::Key(syllables);
  dictionary_[key].push_back(
      {std::move(text), std::log1p(frequency), 0, false});
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
  parser_ = PinyinParser{};
}

std::vector<Candidate> PinyinEngine::Query(const LearningEvent& request,
                                           std::size_t limit) const {
  if (limit == 0)
    return {};
  const std::size_t beam_limit = std::max<std::size_t>(32, limit);
  std::unordered_map<std::string, Candidate> unique;
  std::unordered_map<const std::vector<Candidate>*,
                     std::vector<const Candidate*>>
      top_entry_cache;

  const auto top_entries = [&](const std::vector<Candidate>& entries)
      -> const std::vector<const Candidate*>& {
    const auto cached = top_entry_cache.find(&entries);
    if (cached != top_entry_cache.end())
      return cached->second;
    std::vector<const Candidate*> sorted;
    sorted.reserve(entries.size());
    for (const auto& candidate : entries)
      sorted.push_back(&candidate);
    std::sort(sorted.begin(), sorted.end(), [](const auto* left,
                                               const auto* right) {
      if (left->dictionary_score != right->dictionary_score)
        return left->dictionary_score > right->dictionary_score;
      return left->text < right->text;
    });
    if (sorted.size() > beam_limit)
      sorted.resize(beam_limit);
    return top_entry_cache.emplace(&entries, std::move(sorted)).first->second;
  };

  const auto add_result = [&](Candidate candidate) {
    const auto existing = unique.find(candidate.text);
    if (existing == unique.end() ||
        (existing->second.is_completion && !candidate.is_completion) ||
        (existing->second.is_completion == candidate.is_completion &&
         existing->second.dictionary_score < candidate.dictionary_score)) {
      const std::string text = candidate.text;
      unique.insert_or_assign(text, std::move(candidate));
    }
  };

  const auto combine_path = [&](const SyllablePath& path) {
    std::vector<std::vector<PhraseBeam>> beams(path.size() + 1);
    beams[0].push_back({});
    for (std::size_t begin = 0; begin < path.size(); ++begin) {
      if (beams[begin].empty())
        continue;
      PruneBeams(beams[begin], beam_limit);
      std::string key;
      for (std::size_t end = begin; end < path.size(); ++end) {
        if (!key.empty())
          key.push_back('\'');
        key += path[end];
        const auto found = dictionary_.find(key);
        if (found == dictionary_.end())
          continue;
        const auto& entries = top_entries(found->second);
        const std::size_t span = end - begin + 1;
        for (const auto& prefix : beams[begin]) {
          for (const auto* entry : entries) {
            if (span > 1 && span < path.size() &&
                entry->dictionary_score < kMinimumEmbeddedPhraseScore)
              continue;
            auto next = prefix;
            next.text += entry->text;
            next.weighted_score +=
                entry->dictionary_score * static_cast<double>(span);
            next.syllables += span;
            ++next.segments;
            beams[end + 1].push_back(std::move(next));
          }
        }
        if (beams[end + 1].size() > beam_limit * 4)
          PruneBeams(beams[end + 1], beam_limit);
      }
    }
    PruneBeams(beams.back(), beam_limit);
    return beams.back();
  };

  const auto paths = parser_.Parse(request.input, kQueryParsePaths);
  for (const auto& path : paths) {
    const auto exact = dictionary_.find(PinyinParser::Key(path));
    if (exact != dictionary_.end()) {
      for (const auto& candidate : exact->second)
        add_result(candidate);
    }
  }
  if (unique.size() < std::min(limit, kMinimumDirectCandidates)) {
    for (const auto& path : paths) {
      for (const auto& beam : combine_path(path))
        add_result({beam.text, BeamScore(beam), 0, false});
    }
  }

  // Complete only when no fully parsed candidate exists. Search backward for
  // the shortest partial tail that leaves a valid phrase on the left, so
  // "woshizho" combines "wo shi" with a "zho..." entry without exploring
  // unrelated splits or spending that work on already-complete input.
  const auto normalized = PinyinParser::Normalize(request.input, true);
  const auto boundary = normalized.rfind('\'');
  const std::size_t chunk_begin =
      boundary == std::string::npos ? 0 : boundary + 1;
  const bool needs_completion = unique.empty();
  struct PrefixEntry {
    const Candidate* candidate;
    std::size_t syllables;
  };
  for (std::size_t cursor = normalized.size();
       needs_completion && cursor > chunk_begin;) {
    const std::size_t split = --cursor;
    std::string left = normalized.substr(0, split);
    if (!left.empty() && left.back() == '\'')
      left.pop_back();
    const std::string suffix = normalized.substr(split);
    if (suffix.find('\'') != std::string::npos)
      continue;
    const auto prefix = prefix_index_.find(suffix);
    if (prefix == prefix_index_.end())
      continue;
    std::vector<PhraseBeam> left_beams;
    if (left.empty()) {
      left_beams.push_back({});
    } else {
      for (const auto& path : parser_.Parse(left, kQueryParsePaths)) {
        auto combined = combine_path(path);
        left_beams.insert(left_beams.end(),
                          std::make_move_iterator(combined.begin()),
                          std::make_move_iterator(combined.end()));
      }
      PruneBeams(left_beams, beam_limit);
    }
    if (left_beams.empty())
      continue;
    std::vector<PrefixEntry> prefix_entries;
    for (const auto& key : prefix->second) {
      const auto found = dictionary_.find(key);
      if (found == dictionary_.end())
        continue;
      const std::size_t suffix_syllables = KeySyllableCount(key);
      for (const auto& entry : found->second)
        prefix_entries.push_back({&entry, suffix_syllables});
    }
    const std::size_t prefix_limit = std::max<std::size_t>(64, limit * 2);
    const auto better_prefix = [](const PrefixEntry& left,
                                  const PrefixEntry& right) {
      if (left.candidate->dictionary_score !=
          right.candidate->dictionary_score)
        return left.candidate->dictionary_score >
               right.candidate->dictionary_score;
      return left.candidate->text < right.candidate->text;
    };
    if (prefix_entries.size() > prefix_limit) {
      std::nth_element(prefix_entries.begin(),
                       prefix_entries.begin() + prefix_limit,
                       prefix_entries.end(), better_prefix);
      prefix_entries.resize(prefix_limit);
    }
    std::sort(prefix_entries.begin(), prefix_entries.end(), better_prefix);
    for (const auto& prefix_beam : left_beams) {
      for (const auto& prefix_entry : prefix_entries) {
        const auto& entry = *prefix_entry.candidate;
        auto completed = prefix_beam;
        completed.text += entry.text;
        completed.weighted_score +=
            entry.dictionary_score *
            static_cast<double>(prefix_entry.syllables);
        completed.syllables += prefix_entry.syllables;
        ++completed.segments;
        completed.completion = true;
        add_result({completed.text, BeamScore(completed), 0, true});
      }
    }
    if (!unique.empty())
      break;
  }
  std::vector<Candidate> result;
  result.reserve(unique.size());
  for (auto& [unused, candidate] : unique)
    result.push_back(std::move(candidate));
  for (auto& candidate : result)
    candidate.personal_score = memory_.Score(candidate.text, request);
  std::stable_sort(result.begin(), result.end(), [](const auto& left,
                                                    const auto& right) {
    if (left.is_completion != right.is_completion)
      return !left.is_completion;
    const double left_score = left.dictionary_score + left.personal_score;
    const double right_score = right.dictionary_score + right.personal_score;
    if (left_score != right_score)
      return left_score > right_score;
    return left.text < right.text;
  });
  if (result.size() > limit)
    result.resize(limit);
  return result;
}

bool PinyinEngine::HasCompleteStandardSyllableCoverage() const {
  for (const auto& syllable : PinyinParser::StandardSyllables()) {
    const auto found = dictionary_.find(syllable);
    if (found == dictionary_.end() || found->second.empty())
      return false;
  }
  return true;
}

bool IsRuntimeDictionaryUsable(const PinyinEngine& engine,
                               const DictionaryLoadResult& load_result,
                               std::size_t minimum_entries) {
  return load_result && load_result.loaded >= minimum_entries &&
         load_result.skipped == 0 &&
         engine.HasCompleteStandardSyllableCoverage();
}

}  // namespace zrinput
