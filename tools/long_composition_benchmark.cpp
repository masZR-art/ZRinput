#include "core/composition_buffer.h"
#include "core/decoder.h"
#include "core/dictionary.h"
#include "core/pinyin_parser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Percentiles {
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
};

double Milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

double AtPercentile(const std::vector<double>& sorted, double percentile) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = percentile * static_cast<double>(sorted.size() - 1);
  const std::size_t lower = static_cast<std::size_t>(std::floor(position));
  const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
  const double fraction = position - static_cast<double>(lower);
  return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

Percentiles Summarize(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return {AtPercentile(values, 0.50), AtPercentile(values, 0.95),
          AtPercentile(values, 0.99)};
}

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

std::u16string InputOfLength(std::size_t length) {
  constexpr std::u16string_view pattern = u"nihao";
  std::u16string result;
  result.reserve(length);
  while (result.size() < length) {
    const std::size_t count = std::min(pattern.size(), length - result.size());
    result.append(pattern.substr(0, count));
  }
  return result;
}

std::size_t Iterations(std::size_t length,
                       std::size_t short_count,
                       std::size_t long_count) {
  return length <= 256 ? short_count : long_count;
}

void PrintMetric(std::size_t length,
                 const char* metric,
                 const Percentiles& values,
                 std::size_t samples) {
  std::cout << "length=" << length << " metric=" << metric
            << " samples=" << samples << " p50_ms=" << values.p50
            << " p95_ms=" << values.p95 << " p99_ms=" << values.p99
            << '\n';
}

}  // namespace

int main(int argument_count, char** arguments) {
  if (argument_count != 3) {
    std::cerr << "usage: zrinput_long_benchmark <system.zrdict> "
                 "<pinyin_syllables.txt>\n";
    return 1;
  }
  try {
    std::vector<zrinput::core::DictionaryEntry> entries;
    const auto load = zrinput::core::DictionaryPackage::Load(
        arguments[1], zrinput::core::DictionaryLayer::kSystem, &entries);
    if (!load) {
      std::cerr << load.detail << '\n';
      return 2;
    }
    auto dictionary =
        std::make_shared<zrinput::core::DictionarySnapshot>(std::move(entries));
    zrinput::core::PinyinParser parser;
    parser.ReplaceSyllables(LoadSyllables(arguments[2]));
    zrinput::core::Decoder decoder;
    constexpr std::array<std::size_t, 5> lengths = {32, 64, 128, 256, 1024};
    bool budget_failed = false;
    for (const std::size_t length : lengths) {
      const std::u16string input = InputOfLength(length);
      zrinput::core::CompositionLimits limits;
      limits.active_units = 1024;
      limits.parser_units = 1024;
      limits.hard_units = 4096;
      zrinput::core::CompositionBuffer buffer(limits);
      if (!buffer.ReplaceForReplay(input, {length / 2, length / 2 + 1})) {
        throw std::runtime_error("cannot initialize benchmark composition");
      }
      const std::size_t edit_iterations = Iterations(length, 5000, 2000);
      std::vector<double> edit_timings;
      edit_timings.reserve(edit_iterations);
      for (std::size_t iteration = 0; iteration < edit_iterations; ++iteration) {
        const std::size_t position = length / 2;
        if (!buffer.SetSelection({position, position + 1})) {
          throw std::runtime_error("benchmark selection failed");
        }
        const char16_t replacement = iteration % 2 == 0 ? u'n' : u'h';
        const auto start = Clock::now();
        const auto outcome =
            buffer.Insert(std::u16string_view(&replacement, 1));
        const auto end = Clock::now();
        if (outcome != zrinput::core::EditOutcome::kApplied) {
          throw std::runtime_error("benchmark edit was rejected");
        }
        edit_timings.push_back(Milliseconds(end - start));
      }
      const Percentiles edit = Summarize(std::move(edit_timings));
      PrintMetric(length, "replace_one", edit, edit_iterations);

      const std::size_t parse_iterations = Iterations(length, 200, 30);
      std::vector<double> parse_timings;
      parse_timings.reserve(parse_iterations);
      zrinput::core::PinyinAnalysis analysis;
      for (std::size_t iteration = 0; iteration < parse_iterations; ++iteration) {
        const auto start = Clock::now();
        analysis = parser.Analyze(input);
        const auto end = Clock::now();
        parse_timings.push_back(Milliseconds(end - start));
      }
      const Percentiles parse = Summarize(std::move(parse_timings));
      PrintMetric(length, "parse", parse, parse_iterations);

      const std::size_t decode_iterations = Iterations(length, 30, 5);
      std::vector<double> decode_timings;
      decode_timings.reserve(decode_iterations);
      for (std::size_t iteration = 0; iteration < decode_iterations;
           ++iteration) {
        zrinput::core::DecodeRequest request;
        request.composition_version = iteration + 1;
        request.analysis = analysis;
        request.candidate_limit = 50;
        const auto start = Clock::now();
        const auto result = decoder.Decode(request, dictionary);
        const auto end = Clock::now();
        if (result.composition_version != iteration + 1) {
          throw std::runtime_error("benchmark decoder lost request version");
        }
        decode_timings.push_back(Milliseconds(end - start));
      }
      const Percentiles decode = Summarize(std::move(decode_timings));
      PrintMetric(length, "decode", decode, decode_iterations);

      if (length == 256 && edit.p95 >= 20.0) {
        budget_failed = true;
      }
      if (length == 32 && decode.p95 >= 30.0) {
        budget_failed = true;
      }
    }
    if (budget_failed) {
      std::cerr << "one or more release latency budgets failed\n";
      return 3;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 4;
  }
}

