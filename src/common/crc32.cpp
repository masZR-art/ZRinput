#include "common/crc32.h"

#include <array>

namespace zrinput {
namespace {

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> table{};
  std::uint32_t value = 0;
  for (std::uint32_t& entry : table) {
    std::uint32_t remainder = value++;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder >> 1u) ^
                  (0xEDB88320u & (0u - (remainder & 1u)));
    }
    entry = remainder;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

std::uint32_t Crc32(std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte value : bytes) {
    const auto index = static_cast<std::uint8_t>(crc) ^
                       static_cast<std::uint8_t>(value);
    crc = kTable[index] ^ (crc >> 8u);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32(std::string_view bytes) noexcept {
  return Crc32(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

}  // namespace zrinput
