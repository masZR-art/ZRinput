#include "core/pinyin_parser.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

namespace zrinput {
namespace {

bool IsBoundary(char ch) {
  return ch == '\'' || ch == ' ' || ch == '\t' || ch == '-';
}

std::vector<std::string> SplitBoundaries(const std::string& normalized) {
  std::vector<std::string> chunks;
  std::size_t start = 0;
  while (start < normalized.size()) {
    const auto end = normalized.find('\'', start);
    const auto chunk = normalized.substr(start, end - start);
    if (!chunk.empty())
      chunks.push_back(chunk);
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return chunks;
}

}  // namespace

bool PinyinParser::RegisterSyllable(std::string syllable) {
  syllable = Normalize(syllable, false);
  if (syllable.empty() ||
      !std::all_of(syllable.begin(), syllable.end(), [](unsigned char ch) {
        return ch >= 'a' && ch <= 'z';
      }))
    return false;
  return syllables_.insert(std::move(syllable)).second;
}

std::vector<SyllablePath> PinyinParser::Parse(const std::string& input,
                                              std::size_t max_paths) const {
  if (max_paths == 0)
    return {};
  const auto chunks = SplitBoundaries(Normalize(input, true));
  if (chunks.empty())
    return {};

  std::vector<SyllablePath> combined(1);
  for (const auto& chunk : chunks) {
    const auto paths = ParseChunk(chunk, max_paths);
    if (paths.empty())
      return {};
    std::vector<SyllablePath> next;
    for (const auto& prefix : combined) {
      for (const auto& suffix : paths) {
        auto path = prefix;
        path.insert(path.end(), suffix.begin(), suffix.end());
        next.push_back(std::move(path));
        if (next.size() >= max_paths)
          break;
      }
      if (next.size() >= max_paths)
        break;
    }
    combined = std::move(next);
  }
  std::stable_sort(combined.begin(), combined.end(), [](const auto& left,
                                                        const auto& right) {
    if (left.size() != right.size())
      return left.size() < right.size();
    return left < right;
  });
  return combined;
}

std::string PinyinParser::Normalize(const std::string& input,
                                    bool keep_boundaries) {
  std::string result;
  bool pending_boundary = false;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const unsigned char ch = input[i];
    if (i + 1 < input.size() && ch == 0xc3 &&
        static_cast<unsigned char>(input[i + 1]) == 0xbc) {
      if (pending_boundary && keep_boundaries && !result.empty())
        result.push_back('\'');
      result.push_back('v');
      pending_boundary = false;
      ++i;
      continue;
    }
    if ((ch == 'u' || ch == 'U') && i + 1 < input.size() &&
        input[i + 1] == ':') {
      if (pending_boundary && keep_boundaries && !result.empty())
        result.push_back('\'');
      result.push_back('v');
      pending_boundary = false;
      ++i;
      continue;
    }
    if (IsBoundary(static_cast<char>(ch))) {
      pending_boundary = true;
      continue;
    }
    if (ch >= '1' && ch <= '5')
      continue;
    if (!std::isalpha(ch))
      continue;
    if (pending_boundary && keep_boundaries && !result.empty())
      result.push_back('\'');
    result.push_back(static_cast<char>(std::tolower(ch)));
    pending_boundary = false;
  }
  return result;
}

std::string PinyinParser::Key(const SyllablePath& syllables) {
  std::string key;
  for (const auto& syllable : syllables) {
    if (!key.empty())
      key.push_back('\'');
    key += syllable;
  }
  return key;
}

SyllablePath PinyinParser::CanonicalSyllables(const std::string& input) {
  return SplitBoundaries(Normalize(input, true));
}

std::vector<SyllablePath> PinyinParser::ParseChunk(
    const std::string& chunk,
    std::size_t max_paths) const {
  std::vector<SyllablePath> result;
  SyllablePath current;
  std::function<void(std::size_t)> visit = [&](std::size_t offset) {
    if (result.size() >= max_paths)
      return;
    if (offset == chunk.size()) {
      result.push_back(current);
      return;
    }
    for (std::size_t end = chunk.size(); end > offset; --end) {
      const auto syllable = chunk.substr(offset, end - offset);
      if (syllables_.find(syllable) == syllables_.end())
        continue;
      current.push_back(syllable);
      visit(end);
      current.pop_back();
    }
  };
  visit(0);
  return result;
}

}  // namespace zrinput
