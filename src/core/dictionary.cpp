#include "core/dictionary.h"

#include "common/crc32.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>

namespace zrinput::core {
namespace {

constexpr std::array<std::byte, 8> kMagic = {
    std::byte{'Z'}, std::byte{'R'}, std::byte{'D'}, std::byte{'I'},
    std::byte{'C'}, std::byte{'T'}, std::byte{0},   std::byte{0}};
constexpr std::size_t kHeaderSize = 32;

template <typename Integer>
void AppendLittleEndian(std::vector<std::byte>* bytes, Integer value) {
  static_assert(std::is_integral_v<Integer>);
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned unsigned_value = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bytes->push_back(static_cast<std::byte>(unsigned_value & 0xFFu));
    unsigned_value >>= 8u;
  }
}

void AppendFloat(std::vector<std::byte>* bytes, float value) {
  AppendLittleEndian(bytes, std::bit_cast<std::uint32_t>(value));
}

template <typename Integer>
bool ReadLittleEndian(std::span<const std::byte> bytes,
                      std::size_t* offset,
                      Integer* value) {
  static_assert(std::is_integral_v<Integer>);
  if (!offset || !value || *offset > bytes.size() ||
      sizeof(Integer) > bytes.size() - *offset) {
    return false;
  }
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned result = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    result |= static_cast<Unsigned>(
                  std::to_integer<std::uint8_t>(bytes[*offset + index]))
              << (index * 8u);
  }
  *offset += sizeof(Integer);
  *value = static_cast<Integer>(result);
  return true;
}

bool ReadFloat(std::span<const std::byte> bytes,
               std::size_t* offset,
               float* value) {
  std::uint32_t bits = 0;
  if (!ReadLittleEndian(bytes, offset, &bits)) {
    return false;
  }
  *value = std::bit_cast<float>(bits);
  return std::isfinite(*value) && *value >= 0.0F;
}

bool IsReadingValid(std::string_view reading) {
  if (reading.empty() || reading.front() == ' ' || reading.back() == ' ') {
    return false;
  }
  bool previous_space = false;
  for (const char value : reading) {
    if (value == ' ') {
      if (previous_space) {
        return false;
      }
      previous_space = true;
    } else if (value < 'a' || value > 'z') {
      return false;
    } else {
      previous_space = false;
    }
  }
  return true;
}

DictionaryLoadReport Error(DictionaryError error, std::string detail) {
  DictionaryLoadReport report;
  report.error = error;
  report.detail = std::move(detail);
  return report;
}

std::vector<std::byte> EncodePayload(std::span<const DictionaryEntry> entries,
                                     std::uint32_t version,
                                     DictionaryPackageLimits limits,
                                     DictionaryLoadReport* report) {
  std::vector<std::byte> payload;
  for (const auto& entry : entries) {
    if (!IsReadingValid(entry.reading) || entry.text.empty() ||
        entry.reading.size() > limits.maximum_reading_bytes ||
        entry.text.size() > limits.maximum_text_bytes ||
        entry.reading.size() > std::numeric_limits<std::uint16_t>::max() ||
        entry.text.size() > std::numeric_limits<std::uint16_t>::max() ||
        !std::isfinite(entry.frequency) || entry.frequency < 0.0F) {
      *report = Error(DictionaryError::kInvalidRecord,
                      "entry does not satisfy package constraints");
      return {};
    }
    AppendLittleEndian(&payload,
                       static_cast<std::uint16_t>(entry.reading.size()));
    AppendLittleEndian(&payload,
                       static_cast<std::uint16_t>(entry.text.size()));
    AppendFloat(&payload, entry.frequency);
    if (version >= 2) {
      AppendLittleEndian(&payload, entry.flags);
      AppendLittleEndian(&payload, std::uint16_t{0});
    }
    const auto* reading_begin =
        reinterpret_cast<const std::byte*>(entry.reading.data());
    payload.insert(payload.end(), reading_begin,
                   reading_begin + entry.reading.size());
    const auto* text_begin =
        reinterpret_cast<const std::byte*>(entry.text.data());
    payload.insert(payload.end(), text_begin, text_begin + entry.text.size());
  }
  return payload;
}

}  // namespace

std::string CompactReading(std::string_view reading) {
  std::string compact;
  compact.reserve(reading.size());
  for (const char value : reading) {
    if (value != ' ') {
      compact.push_back(value);
    }
  }
  return compact;
}

std::string ReadingInitials(std::string_view reading) {
  std::string initials;
  bool at_start = true;
  for (const char value : reading) {
    if (value == ' ') {
      at_start = true;
    } else if (at_start) {
      initials.push_back(value);
      at_start = false;
    }
  }
  return initials;
}

DictionaryLoadReport DictionaryPackage::Load(
    const std::filesystem::path& path,
    DictionaryLayer layer,
    std::vector<DictionaryEntry>* entries,
    DictionaryPackageLimits limits) {
  if (!entries) {
    return Error(DictionaryError::kInvalidRecord, "null output collection");
  }
  std::error_code filesystem_error;
  const std::uint64_t file_size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    return Error(DictionaryError::kOpenFailed, "cannot read package size");
  }
  if (file_size > limits.maximum_file_bytes) {
    return Error(DictionaryError::kTooLarge, "package exceeds byte budget");
  }
  if (file_size < kHeaderSize) {
    return Error(DictionaryError::kTruncated, "package header is truncated");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Error(DictionaryError::kOpenFailed, "cannot open package");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
  stream.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
    return Error(DictionaryError::kTruncated, "package read was incomplete");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return Error(DictionaryError::kBadMagic, "package magic does not match");
  }

  std::size_t offset = kMagic.size();
  std::uint32_t version = 0;
  std::uint32_t header_size = 0;
  std::uint64_t payload_size = 0;
  std::uint32_t checksum = 0;
  std::uint32_t entry_count = 0;
  if (!ReadLittleEndian(bytes, &offset, &version) ||
      !ReadLittleEndian(bytes, &offset, &header_size) ||
      !ReadLittleEndian(bytes, &offset, &payload_size) ||
      !ReadLittleEndian(bytes, &offset, &checksum) ||
      !ReadLittleEndian(bytes, &offset, &entry_count)) {
    return Error(DictionaryError::kTruncated, "package metadata is truncated");
  }
  if ((version != 1 && version != kCurrentVersion) ||
      header_size != kHeaderSize) {
    return Error(DictionaryError::kUnsupportedVersion,
                 "package version or header size is unsupported");
  }
  if (entry_count > limits.maximum_entries) {
    return Error(DictionaryError::kEntryBudgetExceeded,
                 "package exceeds entry budget");
  }
  if (payload_size != bytes.size() - kHeaderSize) {
    return Error(DictionaryError::kTruncated, "payload length does not match");
  }
  const std::span<const std::byte> payload(bytes.data() + kHeaderSize,
                                           bytes.size() - kHeaderSize);
  if (Crc32(payload) != checksum) {
    return Error(DictionaryError::kChecksumMismatch,
                 "payload checksum does not match");
  }

  std::vector<DictionaryEntry> decoded;
  decoded.reserve(entry_count);
  offset = 0;
  for (std::uint32_t index = 0; index < entry_count; ++index) {
    std::uint16_t reading_size = 0;
    std::uint16_t text_size = 0;
    float frequency = 0.0F;
    std::uint16_t flags = 0;
    std::uint16_t reserved = 0;
    if (!ReadLittleEndian(payload, &offset, &reading_size) ||
        !ReadLittleEndian(payload, &offset, &text_size) ||
        !ReadFloat(payload, &offset, &frequency) ||
        (version >= 2 &&
         (!ReadLittleEndian(payload, &offset, &flags) ||
          !ReadLittleEndian(payload, &offset, &reserved))) ||
        reading_size == 0 || text_size == 0 ||
        reading_size > limits.maximum_reading_bytes ||
        text_size > limits.maximum_text_bytes ||
        offset > payload.size() ||
        static_cast<std::size_t>(reading_size) + text_size >
            payload.size() - offset) {
      return Error(DictionaryError::kInvalidRecord,
                   "record metadata is invalid");
    }
    DictionaryEntry entry;
    entry.reading.assign(
        reinterpret_cast<const char*>(payload.data() + offset), reading_size);
    offset += reading_size;
    entry.text.assign(reinterpret_cast<const char*>(payload.data() + offset),
                      text_size);
    offset += text_size;
    entry.frequency = frequency;
    entry.layer = layer;
    entry.flags = flags;
    if (!IsReadingValid(entry.reading)) {
      return Error(DictionaryError::kInvalidRecord,
                   "record reading contains invalid bytes");
    }
    decoded.push_back(std::move(entry));
  }
  if (offset != payload.size()) {
    return Error(DictionaryError::kInvalidRecord,
                 "package contains trailing record bytes");
  }
  *entries = std::move(decoded);
  DictionaryLoadReport report;
  report.source_version = version;
  report.loaded_entries = entries->size();
  report.migrated = version < kCurrentVersion;
  return report;
}

DictionaryLoadReport DictionaryPackage::WriteAtomic(
    const std::filesystem::path& path,
    std::span<const DictionaryEntry> entries,
    std::uint32_t version,
    DictionaryPackageLimits limits) {
  if (version != 1 && version != kCurrentVersion) {
    return Error(DictionaryError::kUnsupportedVersion,
                 "cannot write requested package version");
  }
  if (entries.size() > limits.maximum_entries ||
      entries.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Error(DictionaryError::kEntryBudgetExceeded,
                 "entry collection exceeds package budget");
  }
  DictionaryLoadReport report;
  std::vector<std::byte> payload = EncodePayload(entries, version, limits, &report);
  if (!report) {
    return report;
  }
  if (payload.size() + kHeaderSize > limits.maximum_file_bytes) {
    return Error(DictionaryError::kTooLarge,
                 "encoded package exceeds byte budget");
  }
  std::vector<std::byte> bytes;
  bytes.reserve(kHeaderSize + payload.size());
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendLittleEndian(&bytes, version);
  AppendLittleEndian(&bytes, static_cast<std::uint32_t>(kHeaderSize));
  AppendLittleEndian(&bytes, static_cast<std::uint64_t>(payload.size()));
  AppendLittleEndian(&bytes, Crc32(payload));
  AppendLittleEndian(&bytes, static_cast<std::uint32_t>(entries.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());

  std::error_code filesystem_error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
      return Error(DictionaryError::kWriteFailed,
                   "cannot create package directory");
    }
  }
  const auto temporary = path.wstring() + L".tmp";
  const auto backup = path.wstring() + L".bak";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Error(DictionaryError::kWriteFailed,
                   "cannot create temporary package");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
      return Error(DictionaryError::kWriteFailed,
                   "cannot flush temporary package");
    }
  }
  std::filesystem::remove(backup, filesystem_error);
  filesystem_error.clear();
  if (std::filesystem::exists(path, filesystem_error)) {
    filesystem_error.clear();
    std::filesystem::rename(path, backup, filesystem_error);
    if (filesystem_error) {
      std::filesystem::remove(temporary, filesystem_error);
      return Error(DictionaryError::kWriteFailed,
                   "cannot rotate previous package");
    }
  }
  filesystem_error.clear();
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::error_code restore_error;
    if (std::filesystem::exists(backup, restore_error)) {
      std::filesystem::rename(backup, path, restore_error);
    }
    std::filesystem::remove(temporary, restore_error);
    return Error(DictionaryError::kWriteFailed,
                 "cannot activate temporary package");
  }
  report.source_version = version;
  report.loaded_entries = entries.size();
  return report;
}

DictionarySnapshot::DictionarySnapshot(std::vector<DictionaryEntry> entries)
    : entries_(std::move(entries)) {
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const auto& entry = entries_[index];
    exact_index_[entry.reading].push_back(index);
    const std::string compact = CompactReading(entry.reading);
    for (std::size_t size = 1; size <= compact.size(); ++size) {
      compact_prefix_index_[compact.substr(0, size)].push_back(index);
    }
    initials_index_[ReadingInitials(entry.reading)].push_back(index);
  }
  const auto compare = [this](std::size_t left, std::size_t right) {
    const auto& lhs = entries_[left];
    const auto& rhs = entries_[right];
    if (lhs.frequency != rhs.frequency) {
      return lhs.frequency > rhs.frequency;
    }
    if (lhs.layer != rhs.layer) {
      return lhs.layer > rhs.layer;
    }
    return lhs.text < rhs.text;
  };
  for (auto* index : {&exact_index_, &compact_prefix_index_, &initials_index_}) {
    for (auto& [key, values] : *index) {
      static_cast<void>(key);
      std::stable_sort(values.begin(), values.end(), compare);
    }
  }
}

std::vector<const DictionaryEntry*> DictionarySnapshot::LookupExact(
    std::string_view reading,
    std::size_t limit) const {
  return Lookup(exact_index_, reading, limit);
}

std::vector<const DictionaryEntry*> DictionarySnapshot::LookupCompactPrefix(
    std::string_view compact_reading,
    std::size_t limit) const {
  return Lookup(compact_prefix_index_, compact_reading, limit);
}

std::vector<const DictionaryEntry*> DictionarySnapshot::LookupInitials(
    std::string_view initials,
    std::size_t limit) const {
  return Lookup(initials_index_, initials, limit);
}

std::vector<const DictionaryEntry*> DictionarySnapshot::Lookup(
    const Index& index,
    std::string_view key,
    std::size_t limit) const {
  std::vector<const DictionaryEntry*> result;
  const auto found = index.find(std::string(key));
  if (found == index.end() || limit == 0) {
    return result;
  }
  result.reserve(std::min(limit, found->second.size()));
  for (const std::size_t entry_index : found->second) {
    result.push_back(&entries_[entry_index]);
    if (result.size() == limit) {
      break;
    }
  }
  return result;
}

DictionaryService::DictionaryService()
    : snapshot_(std::make_shared<DictionarySnapshot>()) {}

DictionaryLoadReport DictionaryService::LoadLayer(
    const std::filesystem::path& path,
    DictionaryLayer layer) {
  std::vector<DictionaryEntry> entries;
  DictionaryLoadReport report = DictionaryPackage::Load(path, layer, &entries);
  if (report) {
    ReplaceLayer(layer, std::move(entries));
  }
  return report;
}

void DictionaryService::ReplaceLayer(DictionaryLayer layer,
                                     std::vector<DictionaryEntry> entries) {
  for (auto& entry : entries) {
    entry.layer = layer;
  }
  std::unique_lock lock(mutex_);
  layers_[layer] = std::move(entries);
  RebuildSnapshotLocked();
}

void DictionaryService::ClearLayer(DictionaryLayer layer) {
  std::unique_lock lock(mutex_);
  layers_.erase(layer);
  RebuildSnapshotLocked();
}

std::shared_ptr<const DictionarySnapshot> DictionaryService::snapshot() const {
  std::shared_lock lock(mutex_);
  return snapshot_;
}

std::uint64_t DictionaryService::generation() const noexcept {
  std::shared_lock lock(mutex_);
  return generation_;
}

void DictionaryService::RebuildSnapshotLocked() {
  std::vector<DictionaryEntry> merged;
  std::size_t count = 0;
  for (const auto& [layer, entries] : layers_) {
    static_cast<void>(layer);
    count += entries.size();
  }
  merged.reserve(count);
  for (const auto& [layer, entries] : layers_) {
    static_cast<void>(layer);
    merged.insert(merged.end(), entries.begin(), entries.end());
  }
  snapshot_ = std::make_shared<DictionarySnapshot>(std::move(merged));
  ++generation_;
}

}  // namespace zrinput::core

