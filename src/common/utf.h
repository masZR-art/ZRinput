#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zrinput::utf {

bool IsHighSurrogate(char16_t value) noexcept;
bool IsLowSurrogate(char16_t value) noexcept;
std::size_t PreviousCodePoint(std::u16string_view text,
                              std::size_t offset) noexcept;
std::size_t NextCodePoint(std::u16string_view text,
                          std::size_t offset) noexcept;
bool IsAsciiWord(char16_t value) noexcept;
std::string ToUtf8(std::u16string_view text);
std::u16string FromUtf8(std::string_view text);

}  // namespace zrinput::utf

