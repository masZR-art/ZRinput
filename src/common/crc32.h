#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace zrinput {

std::uint32_t Crc32(std::span<const std::byte> bytes) noexcept;
std::uint32_t Crc32(std::string_view bytes) noexcept;

}  // namespace zrinput

