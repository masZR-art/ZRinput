#include "core/pinyin_engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

std::vector<std::string> ReadSyllables(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> syllables;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty() && line.front() != '#')
      syllables.push_back(std::move(line));
  }
  return syllables;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: zrinput_lexicon_engine_tests LEXICON SYLLABLES\n";
    return 2;
  }

  const std::filesystem::path lexicon_path(argv[1]);
  const std::filesystem::path syllables_path(argv[2]);

  zrinput::PinyinEngine engine;
  const auto load_started = std::chrono::steady_clock::now();
  const auto loaded = engine.LoadDictionary(lexicon_path);
  const auto load_finished = std::chrono::steady_clock::now();
  const auto load_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(load_finished -
                                                           load_started)
          .count();
  Check(loaded && loaded.loaded >= 60'000 && loaded.skipped == 0,
        "production lexicon should load every generated entry");
  Check(zrinput::IsRuntimeDictionaryUsable(engine, loaded),
        "production lexicon should satisfy the TSF fail-safe gate");

  const auto syllables = ReadSyllables(syllables_path);
  Check(syllables.size() == 425,
        "pinned syllable baseline should contain 425 spellings");

  std::set<std::string> prefixes;
  for (const auto& syllable : syllables) {
    for (std::size_t length = 1; length <= syllable.size(); ++length)
      prefixes.insert(syllable.substr(0, length));
  }
  Check(prefixes.size() == 506,
        "pinned syllable baseline should contain 506 unique prefixes");
  for (const auto& prefix : prefixes) {
    zrinput::LearningEvent request;
    request.input = prefix;
    const auto candidates = engine.Query(request, 5);
    Check(!candidates.empty(), "no engine candidate for prefix " + prefix);
  }

  const std::unordered_map<std::string, std::string> expected_first{
      {"a", "啊"},
      {"de", "的"},
      {"hang", "行"},
      {"hao", "好"},
      {"ni", "你"},
      {"shi", "是"},
      {"wo", "我"},
      {"wei", "为"},
      {"xian", "先"},
      {"xianzai", "现在"},
      {"yinwei", "因为"},
      {"zhongguo", "中国"},
  };
  for (const auto& [input, expected] : expected_first) {
    zrinput::LearningEvent request;
    request.input = input;
    const auto candidates = engine.Query(request, 5);
    const std::string actual =
        candidates.empty() ? "<missing>" : candidates.front().text;
    Check(actual == expected, input + " ranked " + actual + " before " +
                                  expected);
  }

  std::cout << "Production lexicon loaded " << loaded.loaded << " entries in "
            << load_milliseconds << " ms; queried " << syllables.size()
            << " syllables and " << prefixes.size() << " unique prefixes.\n";
  return failures == 0 ? 0 : 1;
}
