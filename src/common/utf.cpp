#include "common/utf.h"

#include <stdexcept>

namespace zrinput::utf {

bool IsHighSurrogate(char16_t value) noexcept {
  return value >= 0xD800 && value <= 0xDBFF;
}

bool IsLowSurrogate(char16_t value) noexcept {
  return value >= 0xDC00 && value <= 0xDFFF;
}

std::size_t PreviousCodePoint(std::u16string_view text,
                              std::size_t offset) noexcept {
  offset = offset > text.size() ? text.size() : offset;
  if (offset == 0) {
    return 0;
  }
  --offset;
  if (offset > 0 && IsLowSurrogate(text[offset]) &&
      IsHighSurrogate(text[offset - 1])) {
    --offset;
  }
  return offset;
}

std::size_t NextCodePoint(std::u16string_view text,
                          std::size_t offset) noexcept {
  offset = offset > text.size() ? text.size() : offset;
  if (offset == text.size()) {
    return offset;
  }
  if (IsHighSurrogate(text[offset]) && offset + 1 < text.size() &&
      IsLowSurrogate(text[offset + 1])) {
    return offset + 2;
  }
  return offset + 1;
}

bool IsAsciiWord(char16_t value) noexcept {
  return (value >= u'a' && value <= u'z') ||
         (value >= u'A' && value <= u'Z') ||
         (value >= u'0' && value <= u'9') || value == u'_';
}

std::string ToUtf8(std::u16string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    std::uint32_t codepoint = text[index];
    if (IsHighSurrogate(text[index]) && index + 1 < text.size() &&
        IsLowSurrogate(text[index + 1])) {
      codepoint = 0x10000u +
                  ((static_cast<std::uint32_t>(text[index]) - 0xD800u) << 10u) +
                  (static_cast<std::uint32_t>(text[++index]) - 0xDC00u);
    } else if (IsHighSurrogate(text[index]) || IsLowSurrogate(text[index])) {
      throw std::invalid_argument("unpaired UTF-16 surrogate");
    }

    if (codepoint <= 0x7Fu) {
      result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
      result.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
      result.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
      result.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
      result.push_back(
          static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      result.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
      result.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
      result.push_back(
          static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
      result.push_back(
          static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      result.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
  }
  return result;
}

std::u16string FromUtf8(std::string_view text) {
  std::u16string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::uint32_t codepoint = 0;
    std::size_t count = 0;
    if (first <= 0x7F) {
      codepoint = first;
      count = 1;
    } else if ((first & 0xE0u) == 0xC0u) {
      codepoint = first & 0x1Fu;
      count = 2;
    } else if ((first & 0xF0u) == 0xE0u) {
      codepoint = first & 0x0Fu;
      count = 3;
    } else if ((first & 0xF8u) == 0xF0u) {
      codepoint = first & 0x07u;
      count = 4;
    } else {
      throw std::invalid_argument("invalid UTF-8 lead byte");
    }
    if (index + count > text.size()) {
      throw std::invalid_argument("truncated UTF-8 sequence");
    }
    for (std::size_t continuation = 1; continuation < count; ++continuation) {
      const auto byte = static_cast<unsigned char>(text[index + continuation]);
      if ((byte & 0xC0u) != 0x80u) {
        throw std::invalid_argument("invalid UTF-8 continuation byte");
      }
      codepoint = (codepoint << 6u) | (byte & 0x3Fu);
    }
    const bool overlong = (count == 2 && codepoint < 0x80u) ||
                          (count == 3 && codepoint < 0x800u) ||
                          (count == 4 && codepoint < 0x10000u);
    if (overlong || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
      throw std::invalid_argument("invalid UTF-8 scalar value");
    }
    if (codepoint <= 0xFFFFu) {
      result.push_back(static_cast<char16_t>(codepoint));
    } else {
      codepoint -= 0x10000u;
      result.push_back(static_cast<char16_t>(0xD800u + (codepoint >> 10u)));
      result.push_back(static_cast<char16_t>(0xDC00u + (codepoint & 0x3FFu)));
    }
    index += count;
  }
  return result;
}

}  // namespace zrinput::utf

