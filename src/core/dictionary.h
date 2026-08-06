#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zrinput::core {

enum class DictionaryLayer : std::uint8_t {
  kSystem = 0,
  kDomain = 1,
  kUser = 2,
  kSession = 3,
};

struct DictionaryEntry {
  std::string reading;
  std::string text;
  float frequency = 0.0F;
  DictionaryLayer layer = DictionaryLayer::kSystem;
  std::uint16_t flags = 0;
};

struct DictionaryPackageLimits {
  std::uint64_t maximum_file_bytes = 64ull * 1024ull * 1024ull;
  std::uint32_t maximum_entries = 1'000'000;
  std::uint16_t maximum_reading_bytes = 128;
  std::uint16_t maximum_text_bytes = 512;
};

enum class DictionaryError {
  kNone,
  kOpenFailed,
  kTooLarge,
  kTruncated,
  kBadMagic,
  kUnsupportedVersion,
  kChecksumMismatch,
  kInvalidRecord,
  kEntryBudgetExceeded,
  kWriteFailed,
};

struct DictionaryLoadReport {
  DictionaryError error = DictionaryError::kNone;
  std::uint32_t source_version = 0;
  std::size_t loaded_entries = 0;
  bool migrated = false;
  std::string detail;

  explicit operator bool() const noexcept {
    return error == DictionaryError::kNone;
  }
};

class DictionaryPackage {
 public:
  static constexpr std::uint32_t kCurrentVersion = 2;

  static DictionaryLoadReport Load(
      const std::filesystem::path& path,
      DictionaryLayer layer,
      std::vector<DictionaryEntry>* entries,
      DictionaryPackageLimits limits = {});

  static DictionaryLoadReport WriteAtomic(
      const std::filesystem::path& path,
      std::span<const DictionaryEntry> entries,
      std::uint32_t version = kCurrentVersion,
      DictionaryPackageLimits limits = {});
};

class DictionarySnapshot {
 public:
  DictionarySnapshot() = default;
  explicit DictionarySnapshot(std::vector<DictionaryEntry> entries);

  [[nodiscard]] std::span<const DictionaryEntry> entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] std::vector<const DictionaryEntry*> LookupExact(
      std::string_view reading,
      std::size_t limit = 64) const;
  [[nodiscard]] std::vector<const DictionaryEntry*> LookupCompactPrefix(
      std::string_view compact_reading,
      std::size_t limit = 64) const;
  [[nodiscard]] std::vector<const DictionaryEntry*> LookupInitials(
      std::string_view initials,
      std::size_t limit = 64) const;

 private:
  using Index = std::unordered_map<std::string, std::vector<std::size_t>>;
  [[nodiscard]] std::vector<const DictionaryEntry*> Lookup(
      const Index& index,
      std::string_view key,
      std::size_t limit) const;

  std::vector<DictionaryEntry> entries_;
  Index exact_index_;
  Index initials_index_;
  std::vector<std::string> compact_readings_;
  std::vector<std::size_t> compact_order_;
};

class DictionaryService {
 public:
  DictionaryService();

  DictionaryLoadReport LoadLayer(const std::filesystem::path& path,
                                 DictionaryLayer layer);
  void ReplaceLayer(DictionaryLayer layer,
                    std::vector<DictionaryEntry> entries);
  void ClearLayer(DictionaryLayer layer);

  [[nodiscard]] std::shared_ptr<const DictionarySnapshot> snapshot() const;
  [[nodiscard]] std::uint64_t generation() const noexcept;

 private:
  void RebuildSnapshotLocked();

  mutable std::shared_mutex mutex_;
  std::unordered_map<DictionaryLayer, std::vector<DictionaryEntry>> layers_;
  std::shared_ptr<const DictionarySnapshot> snapshot_;
  std::uint64_t generation_ = 0;
};

std::string CompactReading(std::string_view reading);
std::string ReadingInitials(std::string_view reading);

}  // namespace zrinput::core
