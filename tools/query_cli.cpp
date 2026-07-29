#include "core/pinyin_engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: zrinput_query DICTIONARY PINYIN [PINYIN ...]\n";
    return 2;
  }

  zrinput::PinyinEngine engine;
  const auto load_started = std::chrono::steady_clock::now();
  const auto loaded = engine.LoadDictionary(std::filesystem::path(argv[1]));
  const auto load_elapsed = std::chrono::steady_clock::now() - load_started;
  if (!loaded) {
    std::cerr << "dictionary load failed: " << loaded.error << '\n';
    return 1;
  }
  std::cout << "loaded=" << loaded.loaded << " skipped=" << loaded.skipped
            << " load_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   load_elapsed)
                   .count()
            << '\n';

  for (int argument = 2; argument < argc; ++argument) {
    zrinput::LearningEvent request;
    request.input = argv[argument];
    request.timestamp = 1'800'000'000;
    const auto query_started = std::chrono::steady_clock::now();
    const auto candidates = engine.Query(request, 10);
    const auto query_elapsed = std::chrono::steady_clock::now() - query_started;
    std::cout << request.input << " ("
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     query_elapsed)
                         .count() /
                     1000.0
              << "ms):";
    for (std::size_t index = 0; index < candidates.size(); ++index)
      std::cout << "  " << index + 1 << ':' << candidates[index].text
                << (candidates[index].is_completion ? "*" : "");
    std::cout << '\n';
  }
  return 0;
}
