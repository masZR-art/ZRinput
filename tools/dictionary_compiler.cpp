#include "core/dictionary.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool ParseFrequency(std::string_view text, float* value) {
  if (!value || text.empty()) {
    return false;
  }
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

int Compile(const std::filesystem::path& source,
            const std::filesystem::path& destination) {
  std::ifstream input(source, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open TSV source\n";
    return 2;
  }
  std::vector<zrinput::core::DictionaryEntry> entries;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t first = line.find('\t');
    const std::size_t second =
        first == std::string::npos ? first : line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        line.find('\t', second + 1) != std::string::npos) {
      std::cerr << "invalid field count at line " << line_number << '\n';
      return 3;
    }
    zrinput::core::DictionaryEntry entry;
    entry.reading = line.substr(0, first);
    entry.text = line.substr(first + 1, second - first - 1);
    if (!ParseFrequency(std::string_view(line).substr(second + 1),
                        &entry.frequency)) {
      std::cerr << "invalid frequency at line " << line_number << '\n';
      return 3;
    }
    entry.layer = zrinput::core::DictionaryLayer::kSystem;
    entries.push_back(std::move(entry));
  }
  if (!input.eof()) {
    std::cerr << "failed while reading TSV source\n";
    return 2;
  }
  const auto report = zrinput::core::DictionaryPackage::WriteAtomic(
      destination, entries);
  if (!report) {
    std::cerr << "package write failed: " << report.detail << '\n';
    return 4;
  }
  std::vector<zrinput::core::DictionaryEntry> verified;
  const auto verification = zrinput::core::DictionaryPackage::Load(
      destination, zrinput::core::DictionaryLayer::kSystem, &verified);
  if (!verification || verified.size() != entries.size()) {
    std::cerr << "package verification failed: " << verification.detail
              << '\n';
    return 5;
  }
  std::cout << "compiled " << verified.size() << " entries to "
            << destination.string() << '\n';
  return 0;
}

}  // namespace

int main(int argument_count, char** arguments) {
  if (argument_count != 3) {
    std::cerr << "usage: zrinput_dictionary_compiler <source.tsv> "
                 "<destination.zrdict>\n";
    return 1;
  }
  return Compile(arguments[1], arguments[2]);
}

