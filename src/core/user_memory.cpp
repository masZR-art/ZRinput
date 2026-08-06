#include "core/user_memory.h"

#include "common/crc32.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zrinput::core {
namespace {

constexpr std::uint32_t kSnapshotMagic = 0x534D525Au;  // "ZRMS"
constexpr std::uint32_t kJournalMagic = 0x4A4D525Au;   // "ZRMJ"
constexpr std::uint16_t kStorageVersion = 1;
constexpr std::size_t kFrameHeaderBytes = 16;
constexpr std::size_t kSnapshotFixedBytes = kFrameHeaderBytes + 12;
constexpr std::size_t kSnapshotEntryFixedBytes = 64;
constexpr std::size_t kSnapshotFeatureFixedBytes = 20;
constexpr std::size_t kMinimumSnapshotEntryBytes = 66;
constexpr std::size_t kJournalRecordFixedBytes = 56;

class ExclusiveStorageLock {
 public:
  ExclusiveStorageLock() = default;
  ~ExclusiveStorageLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#endif
  }

  ExclusiveStorageLock(const ExclusiveStorageLock&) = delete;
  ExclusiveStorageLock& operator=(const ExclusiveStorageLock&) = delete;

  [[nodiscard]] bool Acquire(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      return false;
    }
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
#else
    static_cast<void>(path);
    return true;
#endif
  }

 private:
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

std::int64_t CurrentTimeSeconds() noexcept {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string NormalizeApplicationId(std::string_view application_id) {
  std::string normalized(application_id);
  for (char& character : normalized) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    } else if (character == '/') {
      character = '\\';
    }
  }
  return normalized;
}

class BinaryWriter {
 public:
  void PutU8(std::uint8_t value) {
    bytes_.push_back(static_cast<std::byte>(value));
  }

  void PutU16(std::uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
      PutU8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void PutU32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      PutU8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void PutU64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      PutU8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void PutI64(std::int64_t value) {
    PutU64(static_cast<std::uint64_t>(value));
  }

  void PutString(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("user-memory string exceeds format limit");
    }
    PutU32(static_cast<std::uint32_t>(value.size()));
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    bytes_.insert(bytes_.end(), begin, begin + value.size());
  }

  void PutBytes(std::span<const std::byte> value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
    return bytes_;
  }

  [[nodiscard]] std::vector<std::byte> Take() noexcept {
    return std::move(bytes_);
  }

 private:
  std::vector<std::byte> bytes_;
};

class BinaryReader {
 public:
  explicit BinaryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool ReadU8(std::uint8_t* value) noexcept {
    if (remaining() < 1) {
      return false;
    }
    *value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }

  bool ReadU16(std::uint16_t* value) noexcept {
    std::uint64_t decoded = 0;
    if (!ReadInteger(2, &decoded)) {
      return false;
    }
    *value = static_cast<std::uint16_t>(decoded);
    return true;
  }

  bool ReadU32(std::uint32_t* value) noexcept {
    std::uint64_t decoded = 0;
    if (!ReadInteger(4, &decoded)) {
      return false;
    }
    *value = static_cast<std::uint32_t>(decoded);
    return true;
  }

  bool ReadU64(std::uint64_t* value) noexcept {
    return ReadInteger(8, value);
  }

  bool ReadI64(std::int64_t* value) noexcept {
    std::uint64_t decoded = 0;
    if (!ReadU64(&decoded)) {
      return false;
    }
    *value = static_cast<std::int64_t>(decoded);
    return true;
  }

  bool ReadString(std::size_t maximum_bytes, std::string* value) {
    std::uint32_t size = 0;
    if (!ReadU32(&size) || size > maximum_bytes || remaining() < size) {
      return false;
    }
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + position_);
    value->assign(begin, size);
    position_ += size;
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

  [[nodiscard]] std::size_t position() const noexcept { return position_; }

 private:
  bool ReadInteger(std::size_t width, std::uint64_t* value) noexcept {
    if (remaining() < width) {
      return false;
    }
    std::uint64_t decoded = 0;
    for (std::size_t index = 0; index < width; ++index) {
      decoded |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes_[position_ + index]))
                 << (index * 8);
    }
    position_ += width;
    *value = decoded;
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
};

struct CandidateKey {
  std::string reading;
  std::string text;

  bool operator==(const CandidateKey&) const = default;
};

struct CandidateKeyHash {
  std::size_t operator()(const CandidateKey& key) const noexcept {
    const std::size_t first = std::hash<std::string>{}(key.reading);
    const std::size_t second = std::hash<std::string>{}(key.text);
    return first ^ (second + 0x9e3779b9u + (first << 6u) + (first >> 2u));
  }
};

struct FeatureStat {
  std::uint64_t count = 0;
  std::int64_t last_used_seconds = 0;
};

struct EntryData {
  std::uint64_t accepted_count = 0;
  std::uint64_t rejected_count = 0;
  std::uint64_t deleted_count = 0;
  std::int64_t last_accepted_seconds = 0;
  std::int64_t last_negative_seconds = 0;
  std::int64_t last_deleted_seconds = 0;
  std::unordered_map<std::string, FeatureStat> contexts;
  std::unordered_map<std::string, FeatureStat> applications;
};

struct ResourceUsage {
  std::size_t candidate_entries = 0;
  std::size_t feature_values = 0;
  std::size_t string_bytes = 0;
};

using MutableEntries =
    std::unordered_map<CandidateKey, EntryData, CandidateKeyHash>;

struct EntrySlot {
  explicit EntrySlot(std::shared_ptr<const EntryData> initial)
      : data(std::move(initial)) {}

  std::atomic<std::shared_ptr<const EntryData>> data;
};

struct PublishedIndex {
  std::unordered_map<CandidateKey, std::shared_ptr<EntrySlot>, CandidateKeyHash>
      entries;
};

std::int64_t LastActivity(const EntryData& entry) noexcept {
  return std::max({entry.last_accepted_seconds, entry.last_negative_seconds,
                   entry.last_deleted_seconds});
}

bool CandidateKeyLess(const CandidateKey& left,
                      const CandidateKey& right) noexcept {
  return left.reading < right.reading ||
         (left.reading == right.reading && left.text < right.text);
}

MutableEntries::iterator FindOldest(MutableEntries* entries) {
  return std::min_element(
      entries->begin(), entries->end(), [](const auto& left,
                                           const auto& right) {
        const std::int64_t left_activity = LastActivity(left.second);
        const std::int64_t right_activity = LastActivity(right.second);
        return left_activity != right_activity
                   ? left_activity < right_activity
                   : CandidateKeyLess(left.first, right.first);
      });
}

decltype(PublishedIndex::entries)::iterator FindOldest(PublishedIndex* index) {
  auto oldest = index->entries.end();
  std::int64_t oldest_activity = 0;
  for (auto iterator = index->entries.begin();
       iterator != index->entries.end(); ++iterator) {
    const auto data = iterator->second->data.load(std::memory_order_acquire);
    const std::int64_t activity = data ? LastActivity(*data) : 0;
    if (oldest == index->entries.end() || activity < oldest_activity ||
        (activity == oldest_activity &&
         CandidateKeyLess(iterator->first, oldest->first))) {
      oldest = iterator;
      oldest_activity = activity;
    }
  }
  return oldest;
}

bool ConsumeBudget(std::size_t amount,
                   std::size_t maximum,
                   std::size_t* used) noexcept {
  if (*used > maximum || amount > maximum - *used) {
    return false;
  }
  *used += amount;
  return true;
}

bool AccumulateEntryUsage(const CandidateKey& key,
                          const EntryData& entry,
                          ResourceUsage* usage) noexcept {
  if (!ConsumeBudget(1, std::numeric_limits<std::size_t>::max(),
                     &usage->candidate_entries) ||
      !ConsumeBudget(key.reading.size(),
                     std::numeric_limits<std::size_t>::max(),
                     &usage->string_bytes) ||
      !ConsumeBudget(key.text.size(),
                     std::numeric_limits<std::size_t>::max(),
                     &usage->string_bytes) ||
      !ConsumeBudget(entry.contexts.size(),
                     std::numeric_limits<std::size_t>::max(),
                     &usage->feature_values) ||
      !ConsumeBudget(entry.applications.size(),
                     std::numeric_limits<std::size_t>::max(),
                     &usage->feature_values)) {
    return false;
  }
  for (const auto& [value, stat] : entry.contexts) {
    static_cast<void>(stat);
    if (!ConsumeBudget(value.size(),
                       std::numeric_limits<std::size_t>::max(),
                       &usage->string_bytes)) {
      return false;
    }
  }
  for (const auto& [value, stat] : entry.applications) {
    static_cast<void>(stat);
    if (!ConsumeBudget(value.size(),
                       std::numeric_limits<std::size_t>::max(),
                       &usage->string_bytes)) {
      return false;
    }
  }
  return true;
}

bool CalculateResourceUsage(const MutableEntries& entries,
                            ResourceUsage* usage) noexcept {
  ResourceUsage calculated;
  for (const auto& [key, entry] : entries) {
    if (!AccumulateEntryUsage(key, entry, &calculated)) {
      return false;
    }
  }
  *usage = calculated;
  return true;
}

bool CalculateResourceUsage(const PublishedIndex& index,
                            ResourceUsage* usage) noexcept {
  ResourceUsage calculated;
  for (const auto& [key, slot] : index.entries) {
    const auto entry = slot->data.load(std::memory_order_acquire);
    if (!entry || !AccumulateEntryUsage(key, *entry, &calculated)) {
      return false;
    }
  }
  *usage = calculated;
  return true;
}

bool WithinResourceLimits(const ResourceUsage& usage,
                          const UserMemoryOptions& options) noexcept {
  if (usage.candidate_entries > options.maximum_entries ||
      usage.feature_values > options.maximum_total_feature_values ||
      usage.string_bytes > options.maximum_total_string_bytes) {
    return false;
  }
  const std::size_t maximum_snapshot_bytes =
      static_cast<std::size_t>(options.maximum_storage_bytes);
  std::size_t encoded_bytes = kSnapshotFixedBytes;
  if (encoded_bytes > maximum_snapshot_bytes ||
      usage.candidate_entries >
          (maximum_snapshot_bytes - encoded_bytes) /
              kSnapshotEntryFixedBytes) {
    return false;
  }
  encoded_bytes +=
      usage.candidate_entries * kSnapshotEntryFixedBytes;
  if (usage.feature_values >
      (maximum_snapshot_bytes - encoded_bytes) /
          kSnapshotFeatureFixedBytes) {
    return false;
  }
  encoded_bytes += usage.feature_values * kSnapshotFeatureFixedBytes;
  return usage.string_bytes <= maximum_snapshot_bytes - encoded_bytes;
}

bool ReplaceResourceUsage(const ResourceUsage& current,
                          const ResourceUsage& removed,
                          const ResourceUsage& added,
                          ResourceUsage* result) noexcept {
  if (removed.candidate_entries > current.candidate_entries ||
      removed.feature_values > current.feature_values ||
      removed.string_bytes > current.string_bytes) {
    return false;
  }
  ResourceUsage updated{current.candidate_entries - removed.candidate_entries,
                        current.feature_values - removed.feature_values,
                        current.string_bytes - removed.string_bytes};
  if (!ConsumeBudget(added.candidate_entries,
                     std::numeric_limits<std::size_t>::max(),
                     &updated.candidate_entries) ||
      !ConsumeBudget(added.feature_values,
                     std::numeric_limits<std::size_t>::max(),
                     &updated.feature_values) ||
      !ConsumeBudget(added.string_bytes,
                     std::numeric_limits<std::size_t>::max(),
                     &updated.string_bytes)) {
    return false;
  }
  *result = updated;
  return true;
}

std::uint64_t IncrementSaturating(std::uint64_t value) noexcept {
  return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1;
}

void UpdateFeature(
    std::unordered_map<std::string, FeatureStat>* features,
    const std::string& key,
    std::int64_t timestamp,
    std::size_t maximum_values) {
  if (key.empty()) {
    return;
  }
  auto iterator = features->find(key);
  if (iterator == features->end()) {
    if (features->size() >= maximum_values) {
      const auto oldest = std::min_element(
          features->begin(), features->end(), [](const auto& left,
                                                  const auto& right) {
            if (left.second.last_used_seconds != right.second.last_used_seconds) {
              return left.second.last_used_seconds < right.second.last_used_seconds;
            }
            return left.first < right.first;
          });
      if (oldest != features->end()) {
        features->erase(oldest);
      }
    }
    iterator = features->emplace(key, FeatureStat{}).first;
  }
  iterator->second.count = IncrementSaturating(iterator->second.count);
  iterator->second.last_used_seconds =
      std::max(iterator->second.last_used_seconds, timestamp);
}

double TimeDecay(std::int64_t last_used,
                 std::int64_t now,
                 double half_life_seconds) noexcept {
  if (last_used <= 0) {
    return 0.0;
  }
  const double age = std::max(
      0.0, static_cast<double>(now) - static_cast<double>(last_used));
  return std::exp2(-age / half_life_seconds);
}

double SaturatingCount(std::uint64_t count, double saturation) noexcept {
  if (count == 0) {
    return 0.0;
  }
  return std::clamp(std::log1p(static_cast<double>(count)) /
                        std::log1p(saturation),
                    0.0, 1.0);
}

std::vector<std::byte> MakeFrame(std::uint32_t magic,
                                 std::span<const std::byte> payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("user-memory frame exceeds format limit");
  }
  BinaryWriter frame;
  frame.PutU32(magic);
  frame.PutU16(kStorageVersion);
  frame.PutU16(0);
  frame.PutU32(static_cast<std::uint32_t>(payload.size()));
  frame.PutU32(zrinput::Crc32(payload));
  frame.PutBytes(payload);
  return frame.Take();
}

bool ReadFileBytes(const std::filesystem::path& path,
                   std::uint64_t maximum_bytes,
                   std::vector<std::byte>* bytes,
                   bool* exists) {
  std::error_code error;
  *exists = std::filesystem::exists(path, error);
  if (error || !*exists) {
    return !error;
  }
  const std::uint64_t size = std::filesystem::file_size(path, error);
  if (error || size > maximum_bytes ||
      size > static_cast<std::uint64_t>(
                 std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  bytes->resize(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  if (!bytes->empty()) {
    input.read(reinterpret_cast<char*>(bytes->data()),
               static_cast<std::streamsize>(bytes->size()));
  }
  return input.good() || input.eof();
}

bool WriteAll(const std::filesystem::path& path,
              std::span<const std::byte> bytes,
              bool append) {
#if defined(_WIN32)
  const DWORD disposition = append ? OPEN_ALWAYS : CREATE_ALWAYS;
  const DWORD access = append ? FILE_APPEND_DATA : GENERIC_WRITE;
  HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr,
                            disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool succeeded = true;
  std::size_t written_total = 0;
  while (written_total < bytes.size()) {
    const std::size_t remaining = bytes.size() - written_total;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + written_total, request, &written,
                   nullptr) ||
        written == 0) {
      succeeded = false;
      break;
    }
    written_total += written;
  }
  if (succeeded && !FlushFileBuffers(file)) {
    succeeded = false;
  }
  if (!CloseHandle(file)) {
    succeeded = false;
  }
  return succeeded;
#else
  const auto mode = std::ios::binary | std::ios::out |
                    (append ? std::ios::app : std::ios::trunc);
  std::ofstream output(path, mode);
  if (!output) {
    return false;
  }
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.flush();
  return output.good();
#endif
}

bool ReplaceFile(const std::filesystem::path& source,
                 const std::filesystem::path& destination) {
#if defined(_WIN32)
  return MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (!error) {
    return true;
  }
  std::filesystem::remove(destination, error);
  error.clear();
  std::filesystem::rename(source, destination, error);
  return !error;
#endif
}

bool WriteAtomic(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
  std::filesystem::path temporary = path;
  temporary += L".tmp";
  if (!WriteAll(temporary, bytes, false)) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  if (!ReplaceFile(temporary, path)) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
}

bool DecodeFrame(std::span<const std::byte> bytes,
                 std::uint32_t expected_magic,
                 std::span<const std::byte>* payload) noexcept {
  if (bytes.size() < kFrameHeaderBytes) {
    return false;
  }
  BinaryReader header(bytes.first(kFrameHeaderBytes));
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t reserved = 0;
  std::uint32_t payload_size = 0;
  std::uint32_t checksum = 0;
  if (!header.ReadU32(&magic) || !header.ReadU16(&version) ||
      !header.ReadU16(&reserved) || !header.ReadU32(&payload_size) ||
      !header.ReadU32(&checksum) || magic != expected_magic ||
      version != kStorageVersion || reserved != 0 ||
      payload_size != bytes.size() - kFrameHeaderBytes) {
    return false;
  }
  *payload = bytes.subspan(kFrameHeaderBytes, payload_size);
  return zrinput::Crc32(*payload) == checksum;
}

bool ReadFeatureMap(BinaryReader* reader,
                     std::size_t maximum_key_bytes,
                     std::size_t maximum_values,
                     std::size_t maximum_total_values,
                     std::size_t maximum_total_string_bytes,
                     std::size_t* total_values,
                     std::size_t* total_string_bytes,
  std::unordered_map<std::string, FeatureStat>* values) {
  std::uint32_t count = 0;
  if (!reader->ReadU32(&count) || count > maximum_values ||
      !ConsumeBudget(count, maximum_total_values, total_values)) {
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string key;
    FeatureStat stat;
    if (!reader->ReadString(maximum_key_bytes, &key) || key.empty() ||
        !reader->ReadU64(&stat.count) ||
        !reader->ReadI64(&stat.last_used_seconds) || stat.count == 0 ||
        stat.last_used_seconds <= 0) {
      return false;
    }
    if (!ConsumeBudget(key.size(), maximum_total_string_bytes,
                       total_string_bytes)) {
      return false;
    }
    if (!values->emplace(std::move(key), stat).second) {
      return false;
    }
  }
  return true;
}

void WriteFeatureMap(
    BinaryWriter* writer,
    const std::unordered_map<std::string, FeatureStat>& values) {
  std::vector<std::pair<std::string_view, const FeatureStat*>> ordered;
  ordered.reserve(values.size());
  for (const auto& [key, value] : values) {
    ordered.emplace_back(key, &value);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& left,
                                                const auto& right) {
    return left.first < right.first;
  });
  writer->PutU32(static_cast<std::uint32_t>(ordered.size()));
  for (const auto& [key, value] : ordered) {
    writer->PutString(key);
    writer->PutU64(value->count);
    writer->PutI64(value->last_used_seconds);
  }
}

}  // namespace

enum class UserMemory::EventType : std::uint8_t {
  kAccepted = 1,
  kRejected = 2,
  kDeleted = 3,
};

class UserMemory::Impl {
 public:
  struct Event {
    EventType type = EventType::kAccepted;
    std::string reading;
    std::string text;
    std::string context;
    std::string application;
    std::int64_t timestamp_seconds = 0;
  };

  struct JournalRecord {
    std::uint64_t sequence = 0;
    std::vector<std::byte> bytes;
  };

  struct FlushRequest {
    std::uint64_t ticket = 0;
    std::uint64_t event_target = 0;
  };

  explicit Impl(UserMemoryOptions supplied_options)
      : options_(std::move(supplied_options)),
        excluded_applications_(
            std::make_shared<const std::unordered_set<std::string>>()) {
    if (!options_.IsValid()) {
      throw std::invalid_argument("invalid user memory options");
    }
    storage_enabled_ = !options_.storage_directory.empty();
    if (storage_enabled_) {
      std::error_code path_error;
      options_.storage_directory =
          std::filesystem::absolute(options_.storage_directory, path_error);
      if (path_error) {
        throw std::invalid_argument(
            "user-memory storage path cannot be made absolute: " +
            path_error.message());
      }
      options_.storage_directory =
          options_.storage_directory.lexically_normal();
      snapshot_path_ = options_.storage_directory /
                       std::string(UserMemory::kSnapshotFileName);
      backup_path_ = options_.storage_directory /
                     std::string(UserMemory::kBackupFileName);
      journal_path_ = options_.storage_directory /
                      std::string(UserMemory::kJournalFileName);
      std::error_code directory_error;
      std::filesystem::create_directories(options_.storage_directory,
                                          directory_error);
      if (directory_error) {
        throw std::runtime_error("cannot create user-memory storage directory: " +
                                 directory_error.message());
      }
      const auto lock_path = options_.storage_directory /
                             std::string(UserMemory::kLockFileName);
      if (!storage_lock_.Acquire(lock_path)) {
        throw std::runtime_error(
            "user-memory storage is already owned by another writer");
      }
    }
    Load();
    worker_ = std::thread([this] { WorkerMain(); });
  }

  ~Impl() { Shutdown(); }

  MemoryRecordResult Enqueue(EventType type,
                             std::string_view reading,
                             std::string_view text,
                             const PersonalizationContext& context) {
    if (privacy_mode_.load(std::memory_order_acquire)) {
      return MemoryRecordResult::kPrivacyMode;
    }
    if (context.sensitive) {
      return MemoryRecordResult::kSensitiveContext;
    }
    if (!learning_enabled_.load(std::memory_order_acquire)) {
      return MemoryRecordResult::kLearningDisabled;
    }
    if (!ValidInput(reading, text, context)) {
      return MemoryRecordResult::kInvalidInput;
    }

    Event event;
    try {
      event.type = type;
      event.reading.assign(reading);
      event.text.assign(text);
      event.context = context.preceding_text;
      event.application = NormalizeApplicationId(context.application_id);
      event.timestamp_seconds = context.timestamp_seconds > 0
                                    ? context.timestamp_seconds
                                    : CurrentTimeSeconds();
    } catch (...) {
      dropped_records_.fetch_add(1, std::memory_order_relaxed);
      return MemoryRecordResult::kUnavailable;
    }
    if (!ApplicationAllowedNormalized(event.application)) {
      return MemoryRecordResult::kApplicationExcluded;
    }

    std::unique_lock lock(queue_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      dropped_records_.fetch_add(1, std::memory_order_relaxed);
      return MemoryRecordResult::kBusy;
    }
    if (stopping_ || worker_failed_) {
      return MemoryRecordResult::kShuttingDown;
    }
    if (privacy_mode_.load(std::memory_order_acquire)) {
      return MemoryRecordResult::kPrivacyMode;
    }
    if (!learning_enabled_.load(std::memory_order_acquire)) {
      return MemoryRecordResult::kLearningDisabled;
    }
    if (!ApplicationAllowedNormalized(event.application)) {
      return MemoryRecordResult::kApplicationExcluded;
    }
    if (queue_.size() >= options_.queue_capacity) {
      dropped_records_.fetch_add(1, std::memory_order_relaxed);
      return MemoryRecordResult::kQueueFull;
    }
    if (enqueued_event_count_ == std::numeric_limits<std::uint64_t>::max()) {
      dropped_records_.fetch_add(1, std::memory_order_relaxed);
      return MemoryRecordResult::kUnavailable;
    }
    try {
      queue_.push_back(std::move(event));
      ++enqueued_event_count_;
      queued_records_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      dropped_records_.fetch_add(1, std::memory_order_relaxed);
      return MemoryRecordResult::kUnavailable;
    }
    lock.unlock();
    queue_cv_.notify_one();
    return MemoryRecordResult::kQueued;
  }

  UserMemoryView View(std::string_view reading,
                      std::string_view text,
                      const PersonalizationContext& context) const
      noexcept {
    try {
      if (reading.empty() || text.empty() || context.sensitive ||
          privacy_mode_.load(std::memory_order_acquire)) {
        return {};
      }
      const std::string normalized_application =
          NormalizeApplicationId(context.application_id);
      if (!ApplicationAllowedNormalized(normalized_application)) {
        return {};
      }
      const auto index = published_.load(std::memory_order_acquire);
      if (!index) {
        return {};
      }
      const auto iterator =
          index->entries.find(CandidateKey{std::string(reading),
                                           std::string(text)});
      if (iterator == index->entries.end()) {
        return {};
      }
      const auto entry =
          iterator->second->data.load(std::memory_order_acquire);
      if (!entry) {
        return {};
      }
      const std::int64_t now = context.timestamp_seconds > 0
                                   ? context.timestamp_seconds
                                   : CurrentTimeSeconds();
      UserMemoryView view;
      view.accepted_count = entry->accepted_count;
      view.rejected_count = entry->rejected_count;
      view.deleted_count = entry->deleted_count;
      view.recency = TimeDecay(entry->last_accepted_seconds, now,
                               options_.half_life_seconds);
      view.user_frequency =
          SaturatingCount(entry->accepted_count,
                          options_.accepted_saturation_count) *
          view.recency;
      const auto context_stat = entry->contexts.find(context.preceding_text);
      if (context_stat != entry->contexts.end()) {
        view.context =
            SaturatingCount(context_stat->second.count,
                            options_.context_saturation_count) *
            TimeDecay(context_stat->second.last_used_seconds, now,
                      options_.half_life_seconds);
      }
      const auto application_stat =
          entry->applications.find(normalized_application);
      if (application_stat != entry->applications.end()) {
        view.application =
            SaturatingCount(application_stat->second.count,
                            options_.application_saturation_count) *
            TimeDecay(application_stat->second.last_used_seconds, now,
                      options_.half_life_seconds);
      }
      const double weighted_negative =
          static_cast<double>(entry->rejected_count) +
          static_cast<double>(entry->deleted_count) * options_.delete_penalty;
      view.negative_feedback =
          std::clamp(std::log1p(weighted_negative) /
                         std::log1p(options_.negative_saturation_count),
                     0.0, 1.0) *
          TimeDecay(entry->last_negative_seconds, now,
                    options_.half_life_seconds);
      view.suppressed = entry->last_deleted_seconds > 0 &&
                        entry->last_deleted_seconds >=
                            entry->last_accepted_seconds;
      return view;
    } catch (...) {
      return {};
    }
  }

  void SetApplicationLearningEnabled(std::string application_id,
                                     bool enabled) {
    if (application_id.size() > options_.maximum_application_bytes) {
      throw std::invalid_argument("application identity exceeds memory limit");
    }
    application_id = NormalizeApplicationId(application_id);
    std::scoped_lock lock(queue_mutex_, policy_mutex_);
    const auto current =
        excluded_applications_.load(std::memory_order_acquire);
    auto updated = std::make_shared<std::unordered_set<std::string>>(
        current ? *current : std::unordered_set<std::string>{});
    if (enabled) {
      updated->erase(application_id);
    } else if (!application_id.empty()) {
      updated->insert(std::move(application_id));
    }
    excluded_applications_.store(
        std::shared_ptr<const std::unordered_set<std::string>>(
            std::move(updated)),
        std::memory_order_release);
  }

  void SetLearningEnabled(bool enabled) noexcept {
    std::scoped_lock lock(queue_mutex_);
    learning_enabled_.store(enabled, std::memory_order_release);
  }

  void SetPrivacyMode(bool enabled) noexcept {
    std::scoped_lock lock(queue_mutex_);
    privacy_mode_.store(enabled, std::memory_order_release);
  }

  bool ApplicationAllowed(std::string_view application_id) const noexcept {
    if (application_id.empty()) {
      return true;
    }
    try {
      return ApplicationAllowedNormalized(
          NormalizeApplicationId(application_id));
    } catch (...) {
      return false;
    }
  }

  bool Flush() noexcept {
    try {
      std::unique_lock lock(queue_mutex_);
      if (worker_failed_ || stopping_ ||
          next_flush_ticket_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
      }
      const std::uint64_t ticket = ++next_flush_ticket_;
      const auto [result_slot, inserted] =
          flush_results_.emplace(ticket, std::nullopt);
      static_cast<void>(result_slot);
      if (!inserted) {
        return false;
      }
      try {
        flush_requests_.push_back({ticket, enqueued_event_count_});
      } catch (...) {
        flush_results_.erase(ticket);
        throw;
      }
      queue_cv_.notify_one();
      flush_cv_.wait(lock, [this, ticket] {
        const auto result = flush_results_.find(ticket);
        return worker_failed_ ||
               (result != flush_results_.end() && result->second.has_value());
      });
      const auto result = flush_results_.find(ticket);
      if (result == flush_results_.end() || !result->second.has_value()) {
        if (result != flush_results_.end()) {
          flush_results_.erase(result);
        }
        return false;
      }
      const bool succeeded = *result->second;
      flush_results_.erase(result);
      return succeeded;
    } catch (...) {
      return false;
    }
  }

  UserMemoryDiagnostics Diagnostics() const noexcept {
    UserMemoryDiagnostics diagnostics;
    diagnostics.loaded_snapshot =
        loaded_snapshot_.load(std::memory_order_relaxed);
    diagnostics.recovered_from_backup =
        recovered_from_backup_.load(std::memory_order_relaxed);
    diagnostics.recovered_corrupt_journal_tail =
        recovered_tail_.load(std::memory_order_relaxed);
    diagnostics.loaded_journal_records =
        loaded_journal_records_.load(std::memory_order_relaxed);
    diagnostics.dropped_records =
        dropped_records_.load(std::memory_order_relaxed);
    diagnostics.persistence_errors =
        persistence_errors_.load(std::memory_order_relaxed);
    diagnostics.queued_records =
        queued_records_.load(std::memory_order_relaxed);
    return diagnostics;
  }

  std::atomic<bool> learning_enabled_{true};
  std::atomic<bool> privacy_mode_{false};

 private:
  enum class WorkerAction { kBatch, kFlush, kStop };

  bool ValidInput(std::string_view reading,
                  std::string_view text,
                  const PersonalizationContext& context) const noexcept {
    return !reading.empty() && !text.empty() &&
           reading.size() <= options_.maximum_reading_bytes &&
           text.size() <= options_.maximum_text_bytes &&
           context.preceding_text.size() <= options_.maximum_context_bytes &&
           context.application_id.size() <=
               options_.maximum_application_bytes;
  }

  bool ApplicationAllowedNormalized(
      std::string_view application_id) const noexcept {
    const auto excluded =
        excluded_applications_.load(std::memory_order_acquire);
    return !excluded ||
           excluded->find(std::string(application_id)) == excluded->end();
  }

  void Shutdown() noexcept {
    (void)Flush();
    {
      std::scoped_lock lock(queue_mutex_);
      stopping_ = true;
    }
    queue_cv_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  WorkerAction WaitForWork(std::vector<Event>* batch,
                           std::uint64_t* flush_ticket) {
    std::unique_lock lock(queue_mutex_);
    queue_cv_.wait(lock, [this] {
      return stopping_ || !queue_.empty() || !flush_requests_.empty();
    });
    if (!flush_requests_.empty() &&
        flush_requests_.front().event_target <= processed_event_count_) {
      *flush_ticket = flush_requests_.front().ticket;
      flush_requests_.pop_front();
      return WorkerAction::kFlush;
    }
    if (!queue_.empty()) {
      const std::size_t count =
          std::min(queue_.size(), options_.worker_batch_size);
      for (std::size_t index = 0; index < count; ++index) {
        batch->push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
      queued_records_.fetch_sub(count, std::memory_order_relaxed);
      return WorkerAction::kBatch;
    }
    return WorkerAction::kStop;
  }

  void MarkBatchProcessed(std::size_t count) noexcept {
    std::scoped_lock lock(queue_mutex_);
    processed_event_count_ += count;
  }

  bool PublishFlushResult(std::uint64_t flush_ticket,
                          bool persisted) noexcept {
    bool found = true;
    {
      std::scoped_lock lock(queue_mutex_);
      const auto result = flush_results_.find(flush_ticket);
      if (result == flush_results_.end()) {
        worker_failed_ = true;
        found = false;
      } else {
        result->second = persisted;
      }
    }
    flush_cv_.notify_all();
    return found;
  }

  void MarkWorkerFailed() noexcept {
    {
      std::scoped_lock lock(queue_mutex_);
      worker_failed_ = true;
    }
    flush_cv_.notify_all();
  }

  void WorkerMain() noexcept {
    try {
      std::vector<Event> batch;
      batch.reserve(options_.worker_batch_size);
      for (;;) {
        batch.clear();
        std::uint64_t flush_ticket = 0;
        const WorkerAction action = WaitForWork(&batch, &flush_ticket);
        if (action == WorkerAction::kStop) {
          break;
        }

        if (action == WorkerAction::kBatch) {
          ProcessBatch(batch);
          MarkBatchProcessed(batch.size());
          continue;
        }

        if (action == WorkerAction::kFlush) {
          bool persisted = true;
          try {
            persisted = PersistSnapshot();
          } catch (...) {
            persisted = false;
          }
          if (!persisted) {
            persistence_errors_.fetch_add(1, std::memory_order_relaxed);
          }
          if (!PublishFlushResult(flush_ticket, persisted)) {
            return;
          }
        }
      }
    } catch (...) {
      MarkWorkerFailed();
    }
  }

  std::vector<std::byte> SerializeEvent(const Event& event,
                                        std::uint64_t sequence) const {
    BinaryWriter payload;
    payload.PutU8(static_cast<std::uint8_t>(event.type));
    for (int index = 0; index < 7; ++index) {
      payload.PutU8(0);
    }
    payload.PutU64(sequence);
    payload.PutI64(event.timestamp_seconds);
    payload.PutString(event.reading);
    payload.PutString(event.text);
    payload.PutString(event.context);
    payload.PutString(event.application);
    return MakeFrame(kJournalMagic, payload.bytes());
  }

  bool DecodeEvent(std::span<const std::byte> payload,
                   Event* event,
                   std::uint64_t* sequence) const {
    BinaryReader reader(payload);
    std::uint8_t type = 0;
    if (!reader.ReadU8(&type)) {
      return false;
    }
    for (int index = 0; index < 7; ++index) {
      std::uint8_t reserved = 0;
      if (!reader.ReadU8(&reserved) || reserved != 0) {
        return false;
      }
    }
    if (type < static_cast<std::uint8_t>(EventType::kAccepted) ||
        type > static_cast<std::uint8_t>(EventType::kDeleted) ||
        !reader.ReadU64(sequence) || *sequence == 0 ||
        !reader.ReadI64(&event->timestamp_seconds) ||
        event->timestamp_seconds <= 0 ||
        !reader.ReadString(options_.maximum_reading_bytes, &event->reading) ||
        !reader.ReadString(options_.maximum_text_bytes, &event->text) ||
        !reader.ReadString(options_.maximum_context_bytes, &event->context) ||
        !reader.ReadString(options_.maximum_application_bytes,
                           &event->application) ||
        reader.remaining() != 0 || event->reading.empty() ||
        event->text.empty()) {
      return false;
    }
    event->type = static_cast<EventType>(type);
    return true;
  }

  void ProcessBatch(const std::vector<Event>& batch) {
    std::vector<JournalRecord> records;
    records.reserve(batch.size());
    BinaryWriter combined;
    std::uint64_t next_sequence = last_sequence_;
    for (const Event& event : batch) {
      if (next_sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("user-memory sequence exhausted");
      }
      ++next_sequence;
      auto bytes = SerializeEvent(event, next_sequence);
      combined.PutBytes(bytes);
      records.push_back(JournalRecord{next_sequence, std::move(bytes)});
    }

    if (storage_enabled_) {
      const std::size_t incoming_bytes = combined.bytes().size();
      const auto has_capacity = [this, incoming_bytes, &records] {
        const std::size_t maximum_bytes =
            static_cast<std::size_t>(options_.maximum_storage_bytes);
        return records.size() <= options_.maximum_journal_records &&
               journal_records_.size() <=
                   options_.maximum_journal_records - records.size() &&
               incoming_bytes <= maximum_bytes &&
               journal_bytes_ <= maximum_bytes - incoming_bytes;
      };
      for (int attempt = 0; attempt < 2 && !has_capacity(); ++attempt) {
        if (!PersistSnapshot()) {
          break;
        }
      }
      if (!has_capacity()) {
        persistence_errors_.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("user-memory journal budget exhausted");
      }
      if (!WriteAll(journal_path_, combined.bytes(), true)) {
        persistence_errors_.fetch_add(1, std::memory_order_relaxed);
        std::error_code error;
        std::filesystem::resize_file(journal_path_, journal_bytes_, error);
        if (error) {
          persistence_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        throw std::runtime_error("user-memory journal append failed");
      }
      journal_bytes_ += incoming_bytes;
      for (auto& record : records) {
        journal_records_.push_back(std::move(record));
      }
    }
    bool index_changed = false;
    for (const Event& event : batch) {
      ApplyRuntimeEvent(event, &index_changed);
      EnforcePublishedLimits(&index_changed);
    }
    last_sequence_ = next_sequence;
    if (index_changed) {
      published_.store(std::shared_ptr<const PublishedIndex>(worker_index_),
                       std::memory_order_release);
    }
  }

  static void ApplyEventToEntry(const Event& event,
                                const UserMemoryOptions& options,
                                EntryData* entry) {
    switch (event.type) {
      case EventType::kAccepted:
        entry->accepted_count = IncrementSaturating(entry->accepted_count);
        entry->last_accepted_seconds =
            std::max(entry->last_accepted_seconds, event.timestamp_seconds);
        if (event.timestamp_seconds >= entry->last_deleted_seconds) {
          entry->last_deleted_seconds = 0;
        }
        UpdateFeature(&entry->contexts, event.context,
                      event.timestamp_seconds,
                      options.maximum_feature_values_per_candidate);
        UpdateFeature(&entry->applications, event.application,
                      event.timestamp_seconds,
                      options.maximum_feature_values_per_candidate);
        break;
      case EventType::kRejected:
        entry->rejected_count = IncrementSaturating(entry->rejected_count);
        entry->last_negative_seconds =
            std::max(entry->last_negative_seconds, event.timestamp_seconds);
        break;
      case EventType::kDeleted:
        entry->deleted_count = IncrementSaturating(entry->deleted_count);
        entry->last_negative_seconds =
            std::max(entry->last_negative_seconds, event.timestamp_seconds);
        entry->last_deleted_seconds =
            std::max(entry->last_deleted_seconds, event.timestamp_seconds);
        break;
    }
  }

  bool ApplyMutableEvent(const Event& event,
                         MutableEntries* entries,
                         ResourceUsage* usage) const {
    CandidateKey key{event.reading, event.text};
    auto iterator = entries->find(key);
    ResourceUsage removed;
    EntryData updated;
    if (iterator != entries->end()) {
      updated = iterator->second;
      if (!AccumulateEntryUsage(iterator->first, iterator->second, &removed)) {
        return false;
      }
    }
    ApplyEventToEntry(event, options_, &updated);
    ResourceUsage added;
    if (!AccumulateEntryUsage(key, updated, &added)) {
      return false;
    }
    ResourceUsage replaced;
    if (!ReplaceResourceUsage(*usage, removed, added, &replaced)) {
      return false;
    }
    if (iterator == entries->end()) {
      if (!entries->emplace(std::move(key), std::move(updated)).second) {
        return false;
      }
    } else {
      iterator->second = std::move(updated);
    }
    *usage = replaced;
    while (entries->size() > options_.maximum_entries ||
           !WithinResourceLimits(*usage, options_)) {
      const auto oldest = FindOldest(entries);
      if (oldest == entries->end()) {
        return false;
      }
      ResourceUsage evicted;
      if (!AccumulateEntryUsage(oldest->first, oldest->second, &evicted) ||
          !ReplaceResourceUsage(*usage, evicted, {}, &replaced)) {
        return false;
      }
      entries->erase(oldest);
      *usage = replaced;
    }
    return true;
  }

  void ApplyRuntimeEvent(const Event& event, bool* index_changed) {
    CandidateKey key{event.reading, event.text};
    auto iterator = worker_index_->entries.find(key);
    if (iterator == worker_index_->entries.end()) {
      if (!*index_changed) {
        worker_index_ = std::make_shared<PublishedIndex>(*worker_index_);
        *index_changed = true;
      }
      auto entry = std::make_shared<EntryData>();
      ApplyEventToEntry(event, options_, entry.get());
      ResourceUsage added;
      ResourceUsage replaced;
      if (!AccumulateEntryUsage(key, *entry, &added) ||
          !ReplaceResourceUsage(worker_resource_usage_, {}, added,
                                &replaced)) {
        throw std::length_error("user-memory resource counter overflow");
      }
      auto slot = std::make_shared<EntrySlot>(entry);
      if (!worker_index_->entries.emplace(std::move(key), std::move(slot))
               .second) {
        throw std::logic_error("user-memory candidate insertion raced");
      }
      worker_resource_usage_ = replaced;
      return;
    }
    const auto current =
        iterator->second->data.load(std::memory_order_acquire);
    auto updated = std::make_shared<EntryData>(*current);
    ApplyEventToEntry(event, options_, updated.get());
    ResourceUsage removed;
    ResourceUsage added;
    ResourceUsage replaced;
    if (!AccumulateEntryUsage(iterator->first, *current, &removed) ||
        !AccumulateEntryUsage(iterator->first, *updated, &added) ||
        !ReplaceResourceUsage(worker_resource_usage_, removed, added,
                              &replaced)) {
      throw std::length_error("user-memory resource counter overflow");
    }
    iterator->second->data.store(
        std::shared_ptr<const EntryData>(std::move(updated)),
        std::memory_order_release);
    worker_resource_usage_ = replaced;
  }

  void EnforcePublishedLimits(bool* index_changed) {
    while (worker_index_->entries.size() > options_.maximum_entries ||
           !WithinResourceLimits(worker_resource_usage_, options_)) {
      if (!*index_changed) {
        worker_index_ = std::make_shared<PublishedIndex>(*worker_index_);
        *index_changed = true;
      }
      const auto oldest = FindOldest(worker_index_.get());
      if (oldest == worker_index_->entries.end()) {
        throw std::logic_error("user-memory limits cannot be satisfied");
      }
      const auto entry = oldest->second->data.load(std::memory_order_acquire);
      ResourceUsage evicted;
      ResourceUsage replaced;
      if (!entry ||
          !AccumulateEntryUsage(oldest->first, *entry, &evicted) ||
          !ReplaceResourceUsage(worker_resource_usage_, evicted, {},
                                &replaced)) {
        throw std::logic_error("user-memory resource counter is inconsistent");
      }
      worker_index_->entries.erase(oldest);
      worker_resource_usage_ = replaced;
    }
  }

  std::vector<std::byte> SerializeSnapshot() const {
    if (worker_index_->entries.size() > options_.maximum_entries ||
        !WithinResourceLimits(worker_resource_usage_, options_)) {
      throw std::logic_error("user-memory snapshot exceeds resource budget");
    }
    BinaryWriter payload;
    payload.PutU64(last_sequence_);
    payload.PutU32(
        static_cast<std::uint32_t>(worker_index_->entries.size()));

    std::vector<std::pair<const CandidateKey*, std::shared_ptr<EntrySlot>>>
        ordered;
    ordered.reserve(worker_index_->entries.size());
    for (const auto& [key, slot] : worker_index_->entries) {
      ordered.emplace_back(&key, slot);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left,
                                                  const auto& right) {
      if (left.first->reading != right.first->reading) {
        return left.first->reading < right.first->reading;
      }
      return left.first->text < right.first->text;
    });

    for (const auto& [key, slot] : ordered) {
      const auto entry = slot->data.load(std::memory_order_acquire);
      payload.PutString(key->reading);
      payload.PutString(key->text);
      payload.PutU64(entry->accepted_count);
      payload.PutU64(entry->rejected_count);
      payload.PutU64(entry->deleted_count);
      payload.PutI64(entry->last_accepted_seconds);
      payload.PutI64(entry->last_negative_seconds);
      payload.PutI64(entry->last_deleted_seconds);
      WriteFeatureMap(&payload, entry->contexts);
      WriteFeatureMap(&payload, entry->applications);
    }
    return MakeFrame(kSnapshotMagic, payload.bytes());
  }

  bool ParseSnapshot(const std::filesystem::path& path,
                     MutableEntries* entries,
                     std::uint64_t* sequence,
                     bool* exists) const {
    std::vector<std::byte> bytes;
    if (!ReadFileBytes(path, options_.maximum_storage_bytes, &bytes, exists)) {
      return false;
    }
    if (!*exists) {
      return true;
    }
    std::span<const std::byte> payload;
    if (!DecodeFrame(bytes, kSnapshotMagic, &payload)) {
      return false;
    }
    BinaryReader reader(payload);
    std::uint64_t decoded_sequence = 0;
    std::uint32_t count = 0;
    if (!reader.ReadU64(&decoded_sequence) || !reader.ReadU32(&count) ||
        count > options_.maximum_entries ||
        count > reader.remaining() / kMinimumSnapshotEntryBytes) {
      return false;
    }
    MutableEntries decoded;
    decoded.reserve(count);
    std::size_t total_feature_values = 0;
    std::size_t total_string_bytes = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
      CandidateKey key;
      EntryData entry;
      if (!reader.ReadString(options_.maximum_reading_bytes, &key.reading) ||
          !reader.ReadString(options_.maximum_text_bytes, &key.text) ||
          key.reading.empty() || key.text.empty() ||
          !ConsumeBudget(key.reading.size(),
                         options_.maximum_total_string_bytes,
                         &total_string_bytes) ||
          !ConsumeBudget(key.text.size(),
                         options_.maximum_total_string_bytes,
                         &total_string_bytes) ||
          !reader.ReadU64(&entry.accepted_count) ||
          !reader.ReadU64(&entry.rejected_count) ||
          !reader.ReadU64(&entry.deleted_count) ||
          !reader.ReadI64(&entry.last_accepted_seconds) ||
          !reader.ReadI64(&entry.last_negative_seconds) ||
          !reader.ReadI64(&entry.last_deleted_seconds) ||
          entry.last_accepted_seconds < 0 || entry.last_negative_seconds < 0 ||
          entry.last_deleted_seconds < 0 ||
          ((entry.accepted_count != 0) !=
           (entry.last_accepted_seconds != 0)) ||
          ((entry.rejected_count != 0 || entry.deleted_count != 0) !=
           (entry.last_negative_seconds != 0)) ||
          (entry.last_deleted_seconds != 0 && entry.deleted_count == 0) ||
          entry.last_deleted_seconds > entry.last_negative_seconds ||
          !ReadFeatureMap(&reader, options_.maximum_context_bytes,
                           options_.maximum_feature_values_per_candidate,
                           options_.maximum_total_feature_values,
                           options_.maximum_total_string_bytes,
                           &total_feature_values, &total_string_bytes,
                           &entry.contexts) ||
          !ReadFeatureMap(&reader, options_.maximum_application_bytes,
                           options_.maximum_feature_values_per_candidate,
                           options_.maximum_total_feature_values,
                           options_.maximum_total_string_bytes,
                           &total_feature_values, &total_string_bytes,
                           &entry.applications)) {
        return false;
      }
      const auto [iterator, inserted] =
          decoded.emplace(std::move(key), std::move(entry));
      static_cast<void>(iterator);
      if (!inserted) {
        return false;
      }
    }
    if (reader.remaining() != 0) {
      return false;
    }
    *entries = std::move(decoded);
    *sequence = decoded_sequence;
    return true;
  }

  void Load() {
    MutableEntries entries;
    MutableEntries main_entries;
    MutableEntries backup_entries;
    std::uint64_t main_sequence = 0;
    std::uint64_t backup_sequence = 0;
    bool main_exists = false;
    bool backup_exists = false;
    const bool main_valid =
        storage_enabled_ &&
        ParseSnapshot(snapshot_path_, &main_entries, &main_sequence,
                      &main_exists);
    const bool backup_valid =
        storage_enabled_ &&
        ParseSnapshot(backup_path_, &backup_entries, &backup_sequence,
                      &backup_exists);

    if (main_valid && main_exists) {
      entries = std::move(main_entries);
      last_sequence_ = main_sequence;
      persisted_snapshot_sequence_ = main_sequence;
      main_snapshot_valid_ = true;
      loaded_snapshot_.store(true, std::memory_order_relaxed);
    } else if (backup_valid && backup_exists) {
      entries = std::move(backup_entries);
      last_sequence_ = backup_sequence;
      persisted_snapshot_sequence_ = backup_sequence;
      recovered_from_backup_.store(true, std::memory_order_relaxed);
      loaded_snapshot_.store(true, std::memory_order_relaxed);
    } else if ((main_exists && !main_valid) ||
               (backup_exists && !backup_valid)) {
      persistence_errors_.fetch_add(1, std::memory_order_relaxed);
    }
    backup_snapshot_valid_ = backup_valid && backup_exists;
    backup_snapshot_sequence_ = backup_snapshot_valid_ ? backup_sequence : 0;

    ResourceUsage usage;
    if (!CalculateResourceUsage(entries, &usage) ||
        !WithinResourceLimits(usage, options_)) {
      entries.clear();
      usage = {};
      last_sequence_ = 0;
      persisted_snapshot_sequence_ = 0;
      main_snapshot_valid_ = false;
      backup_snapshot_valid_ = false;
      persistence_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    if (storage_enabled_) {
      LoadJournal(&entries, &usage);
    }
    BuildPublishedIndex(std::move(entries), usage);
  }

  void LoadJournal(MutableEntries* entries, ResourceUsage* usage) {
    std::vector<std::byte> bytes;
    bool exists = false;
    if (!ReadFileBytes(journal_path_, options_.maximum_storage_bytes, &bytes,
                       &exists)) {
      persistence_errors_.fetch_add(1, std::memory_order_relaxed);
      throw std::runtime_error("user-memory journal cannot be read safely");
    }
    if (!exists) {
      return;
    }
    std::size_t offset = 0;
    std::size_t record_count = 0;
    std::uint64_t previous_sequence = 0;
    while (record_count < options_.maximum_journal_records &&
           bytes.size() - offset >= kFrameHeaderBytes) {
      BinaryReader header(std::span<const std::byte>(bytes).subspan(
          offset, kFrameHeaderBytes));
      std::uint32_t magic = 0;
      std::uint16_t version = 0;
      std::uint16_t reserved = 0;
      std::uint32_t payload_size = 0;
      std::uint32_t checksum = 0;
      if (!header.ReadU32(&magic) || !header.ReadU16(&version) ||
          !header.ReadU16(&reserved) || !header.ReadU32(&payload_size) ||
          !header.ReadU32(&checksum) || magic != kJournalMagic ||
          version != kStorageVersion || reserved != 0 ||
          payload_size > options_.maximum_storage_bytes - kFrameHeaderBytes ||
          bytes.size() - offset - kFrameHeaderBytes < payload_size) {
        break;
      }
      const std::size_t record_size = kFrameHeaderBytes + payload_size;
      const auto record_span =
          std::span<const std::byte>(bytes).subspan(offset, record_size);
      std::span<const std::byte> payload;
      Event event;
      std::uint64_t sequence = 0;
      if (!DecodeFrame(record_span, kJournalMagic, &payload) ||
          !DecodeEvent(payload, &event, &sequence) ||
          sequence <= previous_sequence) {
        break;
      }
      previous_sequence = sequence;
      last_sequence_ = std::max(last_sequence_, sequence);
      journal_records_.push_back(JournalRecord{
          sequence, std::vector<std::byte>(record_span.begin(),
                                           record_span.end())});
      if (sequence > persisted_snapshot_sequence_) {
        if (!ApplyMutableEvent(event, entries, usage)) {
          persistence_errors_.fetch_add(1, std::memory_order_relaxed);
          throw std::runtime_error(
              "user-memory journal replay violated resource invariants");
        }
        loaded_journal_records_.fetch_add(1, std::memory_order_relaxed);
      }
      offset += record_size;
      ++record_count;
    }
    if (record_count == options_.maximum_journal_records &&
        offset != bytes.size() &&
        bytes.size() - offset >= kFrameHeaderBytes) {
      BinaryReader header(std::span<const std::byte>(bytes).subspan(
          offset, kFrameHeaderBytes));
      std::uint32_t magic = 0;
      std::uint16_t version = 0;
      std::uint16_t reserved = 0;
      std::uint32_t payload_size = 0;
      std::uint32_t checksum = 0;
      const bool bounded_header =
          header.ReadU32(&magic) && header.ReadU16(&version) &&
          header.ReadU16(&reserved) && header.ReadU32(&payload_size) &&
          header.ReadU32(&checksum) && magic == kJournalMagic &&
          version == kStorageVersion && reserved == 0 &&
          payload_size <= options_.maximum_storage_bytes - kFrameHeaderBytes &&
          bytes.size() - offset - kFrameHeaderBytes >= payload_size;
      if (bounded_header) {
        const std::size_t record_size = kFrameHeaderBytes + payload_size;
        const auto record_span =
            std::span<const std::byte>(bytes).subspan(offset, record_size);
        std::span<const std::byte> payload;
        Event event;
        std::uint64_t sequence = 0;
        if (DecodeFrame(record_span, kJournalMagic, &payload) &&
            DecodeEvent(payload, &event, &sequence) &&
            sequence > previous_sequence) {
          persistence_errors_.fetch_add(1, std::memory_order_relaxed);
          throw std::runtime_error(
              "user-memory journal record budget exceeded");
        }
      }
    }
    journal_bytes_ = offset;
    if (offset != bytes.size()) {
      recovered_tail_.store(true, std::memory_order_relaxed);
      std::error_code error;
      std::filesystem::resize_file(journal_path_, offset, error);
      if (error) {
        persistence_errors_.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error(
            "user-memory corrupt journal tail cannot be truncated");
      }
    }
  }

  void BuildPublishedIndex(MutableEntries entries, ResourceUsage usage) {
    worker_index_ = std::make_shared<PublishedIndex>();
    worker_index_->entries.reserve(entries.size());
    while (!entries.empty()) {
      auto node = entries.extract(entries.begin());
      auto data = std::make_shared<EntryData>(std::move(node.mapped()));
      worker_index_->entries.emplace(
          std::move(node.key()),
          std::make_shared<EntrySlot>(std::move(data)));
    }
    published_.store(std::shared_ptr<const PublishedIndex>(worker_index_),
                     std::memory_order_release);
    worker_resource_usage_ = usage;
  }

  bool PersistSnapshot() {
    if (!storage_enabled_) {
      return true;
    }
    const auto snapshot = SerializeSnapshot();
    if (snapshot.size() > options_.maximum_storage_bytes) {
      return false;
    }

    if (main_snapshot_valid_) {
      std::vector<std::byte> previous;
      bool exists = false;
      std::span<const std::byte> previous_payload;
      if (ReadFileBytes(snapshot_path_, options_.maximum_storage_bytes,
                        &previous, &exists) &&
          exists && DecodeFrame(previous, kSnapshotMagic, &previous_payload) &&
          WriteAtomic(backup_path_, previous)) {
        backup_snapshot_valid_ = true;
        backup_snapshot_sequence_ = persisted_snapshot_sequence_;
      }
    }

    if (!WriteAtomic(snapshot_path_, snapshot)) {
      return false;
    }
    main_snapshot_valid_ = true;
    persisted_snapshot_sequence_ = last_sequence_;

    const std::uint64_t retain_after =
        backup_snapshot_valid_ ? backup_snapshot_sequence_ : 0;
    BinaryWriter retained;
    for (const JournalRecord& record : journal_records_) {
      if (record.sequence > retain_after) {
        retained.PutBytes(record.bytes);
      }
    }
    if (!WriteAtomic(journal_path_, retained.bytes())) {
      return false;
    }
    while (!journal_records_.empty() &&
           journal_records_.front().sequence <= retain_after) {
      journal_records_.pop_front();
    }
    journal_bytes_ = retained.bytes().size();
    return true;
  }

  UserMemoryOptions options_;
  ExclusiveStorageLock storage_lock_;
  bool storage_enabled_ = false;
  std::filesystem::path snapshot_path_;
  std::filesystem::path backup_path_;
  std::filesystem::path journal_path_;

  std::shared_ptr<PublishedIndex> worker_index_;
  std::atomic<std::shared_ptr<const PublishedIndex>> published_;
  ResourceUsage worker_resource_usage_;

  mutable std::mutex policy_mutex_;
  std::atomic<std::shared_ptr<const std::unordered_set<std::string>>>
      excluded_applications_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::condition_variable flush_cv_;
  std::deque<Event> queue_;
  std::deque<FlushRequest> flush_requests_;
  std::unordered_map<std::uint64_t, std::optional<bool>> flush_results_;
  std::thread worker_;
  bool stopping_ = false;
  bool worker_failed_ = false;
  std::uint64_t enqueued_event_count_ = 0;
  std::uint64_t processed_event_count_ = 0;
  std::uint64_t next_flush_ticket_ = 0;

  std::uint64_t last_sequence_ = 0;
  std::uint64_t persisted_snapshot_sequence_ = 0;
  std::uint64_t backup_snapshot_sequence_ = 0;
  bool main_snapshot_valid_ = false;
  bool backup_snapshot_valid_ = false;
  std::deque<JournalRecord> journal_records_;
  std::size_t journal_bytes_ = 0;

  std::atomic<bool> loaded_snapshot_{false};
  std::atomic<bool> recovered_from_backup_{false};
  std::atomic<bool> recovered_tail_{false};
  std::atomic<std::uint64_t> loaded_journal_records_{0};
  std::atomic<std::uint64_t> dropped_records_{0};
  std::atomic<std::uint64_t> persistence_errors_{0};
  std::atomic<std::size_t> queued_records_{0};
};

bool UserMemoryOptions::IsValid() const noexcept {
  constexpr std::size_t kMaximumU32 =
      std::numeric_limits<std::uint32_t>::max();
  if (queue_capacity == 0 || worker_batch_size == 0 ||
      worker_batch_size > queue_capacity || maximum_entries == 0 ||
      maximum_entries > kMaximumU32 ||
      maximum_feature_values_per_candidate == 0 ||
      maximum_feature_values_per_candidate > kMaximumU32 ||
      maximum_total_feature_values == 0 ||
      maximum_total_feature_values > kMaximumU32 ||
      maximum_total_string_bytes == 0 ||
      maximum_total_string_bytes > kMaximumU32 ||
      maximum_journal_records < worker_batch_size ||
      maximum_journal_records > kMaximumU32 || maximum_reading_bytes == 0 ||
      maximum_text_bytes == 0 || maximum_context_bytes == 0 ||
      maximum_application_bytes == 0 ||
      maximum_reading_bytes > kMaximumU32 ||
      maximum_text_bytes > kMaximumU32 ||
      maximum_context_bytes > kMaximumU32 ||
      maximum_application_bytes > kMaximumU32 ||
      maximum_storage_bytes < 4096 ||
      maximum_storage_bytes > std::numeric_limits<std::uint32_t>::max() ||
      maximum_total_string_bytes > maximum_storage_bytes ||
      !std::isfinite(half_life_seconds) || half_life_seconds <= 0.0 ||
      !std::isfinite(accepted_saturation_count) ||
      accepted_saturation_count <= 0.0 ||
      !std::isfinite(context_saturation_count) ||
      context_saturation_count <= 0.0 ||
      !std::isfinite(application_saturation_count) ||
      application_saturation_count <= 0.0 ||
      !std::isfinite(negative_saturation_count) ||
      negative_saturation_count <= 0.0 || !std::isfinite(delete_penalty) ||
      delete_penalty < 1.0) {
    return false;
  }

  if (maximum_feature_values_per_candidate >
      maximum_total_feature_values / 2) {
    return false;
  }
  std::size_t required_string_bytes = maximum_reading_bytes;
  if (!ConsumeBudget(maximum_text_bytes, maximum_total_string_bytes,
                     &required_string_bytes)) {
    return false;
  }
  if (maximum_context_bytes >
      (maximum_total_string_bytes - required_string_bytes) /
          maximum_feature_values_per_candidate) {
    return false;
  }
  required_string_bytes +=
      maximum_context_bytes * maximum_feature_values_per_candidate;
  if (maximum_application_bytes >
      (maximum_total_string_bytes - required_string_bytes) /
          maximum_feature_values_per_candidate) {
    return false;
  }
  const std::size_t maximum_file_bytes =
      static_cast<std::size_t>(maximum_storage_bytes);
  std::size_t maximum_journal_record_bytes = kJournalRecordFixedBytes;
  if (!ConsumeBudget(maximum_reading_bytes, maximum_file_bytes,
                     &maximum_journal_record_bytes) ||
      !ConsumeBudget(maximum_text_bytes, maximum_file_bytes,
                     &maximum_journal_record_bytes) ||
      !ConsumeBudget(maximum_context_bytes, maximum_file_bytes,
                     &maximum_journal_record_bytes) ||
      !ConsumeBudget(maximum_application_bytes, maximum_file_bytes,
                     &maximum_journal_record_bytes) ||
      worker_batch_size >
          maximum_file_bytes / maximum_journal_record_bytes) {
    return false;
  }
  return WithinResourceLimits(
      ResourceUsage{maximum_entries, maximum_total_feature_values,
                    maximum_total_string_bytes},
      *this);
}

UserMemory::UserMemory(UserMemoryOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

UserMemory::~UserMemory() = default;

MemoryRecordResult UserMemory::RecordAccepted(
    std::string_view reading,
    std::string_view text,
    const PersonalizationContext& context) {
  return Record(EventType::kAccepted, reading, text, context);
}

MemoryRecordResult UserMemory::RecordRejected(
    std::string_view reading,
    std::string_view text,
    const PersonalizationContext& context) {
  return Record(EventType::kRejected, reading, text, context);
}

MemoryRecordResult UserMemory::RecordDeleted(
    std::string_view reading,
    std::string_view text,
    const PersonalizationContext& context) {
  return Record(EventType::kDeleted, reading, text, context);
}

MemoryRecordResult UserMemory::Record(
    EventType type,
    std::string_view reading,
    std::string_view text,
    const PersonalizationContext& context) {
  return impl_->Enqueue(type, reading, text, context);
}

UserMemoryView UserMemory::View(
    std::string_view reading,
    std::string_view text,
    const PersonalizationContext& context) const noexcept {
  return impl_->View(reading, text, context);
}

PersonalizationFeatures UserMemory::FeaturesFor(
    std::string_view input,
    std::string_view candidate,
    std::string_view application,
    std::span<const std::string> context,
    std::int64_t now_seconds) const noexcept {
  try {
    PersonalizationContext query;
    if (!context.empty()) {
      query.preceding_text = context.back();
    }
    query.application_id.assign(application);
    query.timestamp_seconds = now_seconds;
    const UserMemoryView view = View(input, candidate, query);
    PersonalizationFeatures features;
    features.user_frequency = view.user_frequency;
    features.recency = view.recency;
    features.context = view.context;
    features.application = view.application;
    features.negative_feedback =
        view.suppressed ? 1.0 : view.negative_feedback;
    features.suppressed = view.suppressed;
    return features;
  } catch (...) {
    return {};
  }
}

void UserMemory::SetLearningEnabled(bool enabled) noexcept {
  impl_->SetLearningEnabled(enabled);
}

bool UserMemory::learning_enabled() const noexcept {
  return impl_->learning_enabled_.load(std::memory_order_acquire);
}

void UserMemory::SetPrivacyMode(bool enabled) noexcept {
  impl_->SetPrivacyMode(enabled);
}

bool UserMemory::privacy_mode() const noexcept {
  return impl_->privacy_mode_.load(std::memory_order_acquire);
}

void UserMemory::SetApplicationLearningEnabled(std::string application_id,
                                               bool enabled) {
  impl_->SetApplicationLearningEnabled(std::move(application_id), enabled);
}

bool UserMemory::IsApplicationLearningEnabled(
    std::string_view application_id) const noexcept {
  return impl_->ApplicationAllowed(application_id);
}

bool UserMemory::Flush() noexcept {
  return impl_->Flush();
}

UserMemoryDiagnostics UserMemory::diagnostics() const noexcept {
  return impl_->Diagnostics();
}

}  // namespace zrinput::core
