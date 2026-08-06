#pragma once

#include "core/decoder.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace zrinput::core {

struct PersonalizationContext {
  std::string preceding_text;
  std::string application_id;
  std::int64_t timestamp_seconds = 0;
  bool sensitive = false;
};

struct UserMemoryView {
  // Scores are normalized to [0, 1]. Positive features use
  // log1p(count) / log1p(saturation) multiplied by half-life decay;
  // negative feedback applies delete_penalty before the same normalization.
  double user_frequency = 0.0;
  double recency = 0.0;
  double context = 0.0;
  double application = 0.0;
  double negative_feedback = 0.0;
  std::uint64_t accepted_count = 0;
  std::uint64_t rejected_count = 0;
  std::uint64_t deleted_count = 0;
  bool suppressed = false;
};

enum class MemoryRecordResult {
  kQueued,
  kLearningDisabled,
  kPrivacyMode,
  kSensitiveContext,
  kApplicationExcluded,
  kQueueFull,
  kBusy,
  kInvalidInput,
  kShuttingDown,
  kUnavailable,
};

struct UserMemoryOptions {
  std::filesystem::path storage_directory;
  std::size_t queue_capacity = 2048;
  std::size_t worker_batch_size = 128;
  std::size_t maximum_entries = 200'000;
  std::size_t maximum_feature_values_per_candidate = 32;
  std::size_t maximum_total_feature_values = 1'000'000;
  std::size_t maximum_total_string_bytes = 64 * 1024 * 1024;
  std::size_t maximum_journal_records = 1'000'000;
  std::size_t maximum_reading_bytes = 256;
  std::size_t maximum_text_bytes = 1024;
  std::size_t maximum_context_bytes = 512;
  std::size_t maximum_application_bytes = 512;
  std::uint64_t maximum_storage_bytes = 128ull * 1024ull * 1024ull;

  double half_life_seconds = 30.0 * 24.0 * 60.0 * 60.0;
  double accepted_saturation_count = 32.0;
  double context_saturation_count = 8.0;
  double application_saturation_count = 16.0;
  double negative_saturation_count = 8.0;
  double delete_penalty = 4.0;

  [[nodiscard]] bool IsValid() const noexcept;
};

struct UserMemoryDiagnostics {
  bool loaded_snapshot = false;
  bool recovered_from_backup = false;
  bool recovered_corrupt_journal_tail = false;
  std::uint64_t loaded_journal_records = 0;
  std::uint64_t dropped_records = 0;
  std::uint64_t persistence_errors = 0;
  std::size_t queued_records = 0;
};

class UserMemory : public PersonalizationView {
 public:
  static constexpr std::string_view kSnapshotFileName =
      "user-memory.snapshot";
  static constexpr std::string_view kBackupFileName =
      "user-memory.snapshot.bak";
  static constexpr std::string_view kJournalFileName =
      "user-memory.journal";
  static constexpr std::string_view kLockFileName = "user-memory.lock";

  explicit UserMemory(UserMemoryOptions options = {});
  ~UserMemory() override;

  UserMemory(const UserMemory&) = delete;
  UserMemory& operator=(const UserMemory&) = delete;
  UserMemory(UserMemory&&) = delete;
  UserMemory& operator=(UserMemory&&) = delete;

  [[nodiscard]] MemoryRecordResult RecordAccepted(
      std::string_view reading,
      std::string_view text,
      const PersonalizationContext& context = {});
  [[nodiscard]] MemoryRecordResult RecordRejected(
      std::string_view reading,
      std::string_view text,
      const PersonalizationContext& context = {});
  [[nodiscard]] MemoryRecordResult RecordDeleted(
      std::string_view reading,
      std::string_view text,
      const PersonalizationContext& context = {});

  [[nodiscard]] UserMemoryView View(
      std::string_view reading,
      std::string_view text,
      const PersonalizationContext& context = {}) const noexcept;

  [[nodiscard]] PersonalizationFeatures FeaturesFor(
      std::string_view input,
      std::string_view candidate,
      std::string_view application,
      std::span<const std::string> context,
      std::int64_t now_seconds) const noexcept override;

  void SetLearningEnabled(bool enabled) noexcept;
  [[nodiscard]] bool learning_enabled() const noexcept;
  void SetPrivacyMode(bool enabled) noexcept;
  [[nodiscard]] bool privacy_mode() const noexcept;
  // Disabled applications neither learn from nor consume personalization.
  void SetApplicationLearningEnabled(std::string application_id,
                                     bool enabled);
  [[nodiscard]] bool IsApplicationLearningEnabled(
      std::string_view application_id) const noexcept;

  // Waits for queued changes and a durable snapshot. This never runs on the
  // keystroke path and is safe to call more than once.
  [[nodiscard]] bool Flush() noexcept;
  [[nodiscard]] UserMemoryDiagnostics diagnostics() const noexcept;

 private:
  enum class EventType : std::uint8_t;
  class Impl;

  [[nodiscard]] MemoryRecordResult Record(
      EventType type,
      std::string_view reading,
      std::string_view text,
      const PersonalizationContext& context);

  std::unique_ptr<Impl> impl_;
};

}  // namespace zrinput::core
