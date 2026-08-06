#include "common/utf.h"
#include "core/decoder.h"
#include "core/dictionary.h"
#include "core/pinyin_parser.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::vector<std::string> SyllablesFrom(
    const zrinput::core::DictionarySnapshot& dictionary) {
  std::unordered_set<std::string> unique;
  for (const auto& entry : dictionary.entries()) {
    std::size_t begin = 0;
    while (begin < entry.reading.size()) {
      const std::size_t end = entry.reading.find(' ', begin);
      unique.insert(entry.reading.substr(
          begin, end == std::string::npos ? end : end - begin));
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    }
  }
  return {unique.begin(), unique.end()};
}

}  // namespace

int main(int argument_count, char** arguments) {
  if (argument_count < 3) {
    std::cerr << "usage: zrinput_query <dictionary.zrdict> <pinyin> [...]\n";
    return 1;
  }
  std::vector<zrinput::core::DictionaryEntry> entries;
  const auto load = zrinput::core::DictionaryPackage::Load(
      std::filesystem::path(arguments[1]),
      zrinput::core::DictionaryLayer::kSystem, &entries);
  if (!load) {
    std::cerr << "dictionary load failed: " << load.detail << '\n';
    return 2;
  }
  auto dictionary =
      std::make_shared<zrinput::core::DictionarySnapshot>(std::move(entries));
  zrinput::core::PinyinParser parser;
  parser.ReplaceSyllables(SyllablesFrom(*dictionary));
  zrinput::core::Decoder decoder;
  for (int index = 2; index < argument_count; ++index) {
    zrinput::core::DecodeRequest request;
    try {
      request.analysis = parser.Analyze(zrinput::utf::FromUtf8(arguments[index]));
    } catch (const std::exception& error) {
      std::cerr << arguments[index] << ": " << error.what() << '\n';
      continue;
    }
    request.candidate_limit = 10;
    const auto result = decoder.Decode(request, dictionary);
    std::cout << arguments[index] << ":\n";
    for (const auto& candidate : result.candidates) {
      std::cout << "  " << candidate.text << "\t" << candidate.reading
                << "\t" << candidate.score.total << '\n';
    }
  }
  return 0;
}
