#include "common/utf.h"
#include "core/decoder.h"
#include "core/dictionary.h"
#include "core/pinyin_parser.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::string> LoadSyllables(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("cannot open syllable inventory");
  }
  std::vector<std::string> result;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      result.push_back(line);
    }
  }
  return result;
}

double Milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = percentile * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) {
    return values[lower];
  }
  const double fraction = position - static_cast<double>(lower);
  return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::size_t PeakWorkingSetBytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                           static_cast<DWORD>(sizeof(counters)))) {
    return counters.PeakWorkingSetSize;
  }
#endif
  return 0;
}

bool ContainsText(const zrinput::core::DecodeResult& result,
                  const std::string& expected) {
  return std::any_of(result.candidates.begin(), result.candidates.end(),
                     [&expected](const auto& candidate) {
                       return candidate.text == expected;
                     });
}

}  // namespace

int main(int argument_count, char** arguments) {
  if (argument_count != 3) {
    std::cerr << "expected dictionary and syllable paths\n";
    return 2;
  }
  try {
    const auto load_start = Clock::now();
    std::vector<zrinput::core::DictionaryEntry> entries;
    const auto report = zrinput::core::DictionaryPackage::Load(
        arguments[1], zrinput::core::DictionaryLayer::kSystem, &entries);
    const auto load_end = Clock::now();
    if (!report || entries.size() < 300'000) {
      std::cerr << "real system package failed validation: " << report.detail
                << '\n';
      return 3;
    }
    const auto index_start = Clock::now();
    auto dictionary =
        std::make_shared<zrinput::core::DictionarySnapshot>(std::move(entries));
    const auto index_end = Clock::now();

    zrinput::core::PinyinParser parser;
    const auto syllables = LoadSyllables(arguments[2]);
    if (syllables.size() < 400) {
      std::cerr << "syllable inventory is unexpectedly incomplete\n";
      return 4;
    }
    parser.ReplaceSyllables(syllables);
    zrinput::core::Decoder decoder;
    const std::vector<std::pair<std::string, std::string>> known = {
        {"xianzai", "\xE7\x8E\xB0\xE5\x9C\xA8"},
        {"zhongguo", "\xE4\xB8\xAD\xE5\x9B\xBD"},
        {"shurufa", "\xE8\xBE\x93\xE5\x85\xA5\xE6\xB3\x95"},
        {"nihao", "\xE4\xBD\xA0\xE5\xA5\xBD"},
        {"pinyin", "\xE6\x8B\xBC\xE9\x9F\xB3"},
    };

    for (const auto& [input, expected] : known) {
      zrinput::core::DecodeRequest request;
      request.analysis = parser.Analyze(zrinput::utf::FromUtf8(input));
      request.candidate_limit = 10;
      if (!ContainsText(decoder.Decode(request, dictionary), expected)) {
        std::cerr << "expected candidate missing for " << input << '\n';
        return 5;
      }
    }

    std::vector<double> timings;
    timings.reserve(1000);
    for (std::size_t iteration = 0; iteration < 1000; ++iteration) {
      const auto& input = known[iteration % known.size()].first;
      const auto start = Clock::now();
      zrinput::core::DecodeRequest request;
      request.composition_version = iteration + 1;
      request.analysis = parser.Analyze(zrinput::utf::FromUtf8(input));
      request.candidate_limit = 50;
      const auto result = decoder.Decode(request, dictionary);
      const auto end = Clock::now();
      if (result.candidates.empty() ||
          result.composition_version != iteration + 1) {
        std::cerr << "query result lost candidates or version\n";
        return 6;
      }
      timings.push_back(Milliseconds(end - start));
    }

    const double p50 = Percentile(timings, 0.50);
    const double p95 = Percentile(timings, 0.95);
    const double p99 = Percentile(timings, 0.99);
    const double load_ms = Milliseconds(load_end - load_start);
    const double index_ms = Milliseconds(index_end - index_start);
    const double peak_mib =
        static_cast<double>(PeakWorkingSetBytes()) / (1024.0 * 1024.0);
    std::cout << "entries=" << dictionary->entries().size()
              << " syllables=" << syllables.size() << " load_ms=" << load_ms
              << " index_ms=" << index_ms << " query_p50_ms=" << p50
              << " query_p95_ms=" << p95 << " query_p99_ms=" << p99
              << " peak_working_set_mib=" << peak_mib << '\n';

    if (p95 >= 30.0) {
      std::cerr << "candidate query P95 exceeded 30 ms budget\n";
      return 7;
    }
    if (load_ms + index_ms >= 2500.0) {
      std::cerr << "dictionary cold initialization exceeded 2500 ms budget\n";
      return 8;
    }
    if (peak_mib > 140.0) {
      std::cerr << "dictionary process exceeded 140 MiB peak budget\n";
      return 9;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 10;
  }
}

