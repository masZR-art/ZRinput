#include "core/user_memory.h"

#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using zrinput::core::MemoryRecordResult;
using zrinput::core::PersonalizationContext;
using zrinput::core::UserMemory;
using zrinput::core::UserMemoryOptions;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> next_id{0};
    const auto timestamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
    path_ = std::filesystem::temp_directory_path() /
            ("zrinput-user-memory-test-" + std::to_string(timestamp) + "-" +
             std::to_string(next_id.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

UserMemoryOptions DeterministicOptions() {
  UserMemoryOptions options;
  options.half_life_seconds = 100.0;
  options.accepted_saturation_count = 1.0;
  options.context_saturation_count = 1.0;
  options.application_saturation_count = 1.0;
  options.negative_saturation_count = 1.0;
  return options;
}

PersonalizationContext Context(std::int64_t timestamp,
                               std::string preceding = "before",
                               std::string application = "test.exe") {
  PersonalizationContext context;
  context.preceding_text = std::move(preceding);
  context.application_id = std::move(application);
  context.timestamp_seconds = timestamp;
  return context;
}

std::vector<char> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<char>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

ZR_TEST(UserMemoryOptionsRejectUnsafeResourceBudgets) {
  auto options = DeterministicOptions();
  ZR_EXPECT_TRUE(options.IsValid());

  options.maximum_total_feature_values = 0;
  ZR_EXPECT_TRUE(!options.IsValid());
  options = DeterministicOptions();
  options.maximum_total_string_bytes = 0;
  ZR_EXPECT_TRUE(!options.IsValid());
  options = DeterministicOptions();
  options.maximum_journal_records = options.worker_batch_size - 1;
  ZR_EXPECT_TRUE(!options.IsValid());
  options = DeterministicOptions();
  options.maximum_total_feature_values =
      options.maximum_feature_values_per_candidate * 2 - 1;
  ZR_EXPECT_TRUE(!options.IsValid());
  options = DeterministicOptions();
  options.maximum_total_string_bytes = options.maximum_storage_bytes + 1;
  ZR_EXPECT_TRUE(!options.IsValid());

  options = DeterministicOptions();
  options.maximum_entries = 1;
  options.maximum_feature_values_per_candidate = 1;
  options.maximum_total_feature_values = 2;
  options.maximum_reading_bytes = 1;
  options.maximum_text_bytes = 1;
  options.maximum_context_bytes = 1;
  options.maximum_application_bytes = 1;
  options.maximum_storage_bytes = 4096;
  options.maximum_total_string_bytes = 3964;
  options.worker_batch_size = 1;
  options.maximum_journal_records = 1;
  ZR_EXPECT_TRUE(options.IsValid());
  ++options.maximum_total_string_bytes;
  ZR_EXPECT_TRUE(!options.IsValid());
  --options.maximum_total_string_bytes;
  options.maximum_context_bytes = 3961;
  ZR_EXPECT_TRUE(options.IsValid());
  options.worker_batch_size = 2;
  options.maximum_journal_records = 2;
  ZR_EXPECT_TRUE(!options.IsValid());
}

ZR_TEST(UserMemoryLearnsContextApplicationAndTimeDecay) {
  UserMemory memory(DeterministicOptions());
  ZR_EXPECT_EQ(memory.RecordAccepted("nihao", "candidate", Context(1000)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_TRUE(memory.Flush());

  const auto fresh = memory.View("nihao", "candidate", Context(1000));
  ZR_EXPECT_EQ(fresh.accepted_count, std::uint64_t{1});
  ZR_EXPECT_TRUE(fresh.user_frequency > 0.999);
  ZR_EXPECT_TRUE(fresh.recency > 0.999);
  ZR_EXPECT_TRUE(fresh.context > 0.999);
  ZR_EXPECT_TRUE(fresh.application > 0.999);

  const std::vector<std::string> decoder_context{"before"};
  const zrinput::core::PersonalizationView& adapter = memory;
  const auto decoder_features = adapter.FeaturesFor(
      "nihao", "candidate", "test.exe", decoder_context, 1000);
  ZR_EXPECT_TRUE(decoder_features.user_frequency > 0.999);
  ZR_EXPECT_TRUE(decoder_features.context > 0.999);
  ZR_EXPECT_TRUE(decoder_features.application > 0.999);

  const auto unrelated =
      memory.View("nihao", "candidate", Context(1000, "other", "other.exe"));
  ZR_EXPECT_EQ(unrelated.context, 0.0);
  ZR_EXPECT_EQ(unrelated.application, 0.0);

  const auto decayed = memory.View("nihao", "candidate", Context(1100));
  ZR_EXPECT_TRUE(decayed.user_frequency > 0.499);
  ZR_EXPECT_TRUE(decayed.user_frequency < 0.501);
  ZR_EXPECT_TRUE(decayed.context > 0.499);
  ZR_EXPECT_TRUE(decayed.context < 0.501);
}

ZR_TEST(UserMemoryAppliesNegativeFeedbackAndAllowsRehabilitation) {
  UserMemory memory(DeterministicOptions());
  ZR_EXPECT_EQ(memory.RecordRejected("ni", "candidate", Context(100)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_EQ(memory.RecordDeleted("ni", "candidate", Context(200)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_TRUE(memory.Flush());

  const auto deleted = memory.View("ni", "candidate", Context(200));
  ZR_EXPECT_EQ(deleted.rejected_count, std::uint64_t{1});
  ZR_EXPECT_EQ(deleted.deleted_count, std::uint64_t{1});
  ZR_EXPECT_TRUE(deleted.negative_feedback > 0.999);
  ZR_EXPECT_TRUE(deleted.suppressed);
  const zrinput::core::PersonalizationView& adapter = memory;
  const std::vector<std::string> decoder_context;
  ZR_EXPECT_TRUE(adapter.FeaturesFor("ni", "candidate", "test.exe",
                                     decoder_context, 200)
                     .suppressed);

  // A later event in the same wall-clock second must clear the tombstone.
  ZR_EXPECT_EQ(memory.RecordAccepted("ni", "candidate", Context(200)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_TRUE(memory.Flush());
  const auto rehabilitated = memory.View("ni", "candidate", Context(300));
  ZR_EXPECT_EQ(rehabilitated.accepted_count, std::uint64_t{1});
  ZR_EXPECT_TRUE(!rehabilitated.suppressed);
  ZR_EXPECT_TRUE(!adapter.FeaturesFor("ni", "candidate", "test.exe",
                                      decoder_context, 300)
                      .suppressed);
  ZR_EXPECT_TRUE(rehabilitated.negative_feedback > 0.49);
  ZR_EXPECT_TRUE(rehabilitated.negative_feedback < 0.51);
}

ZR_TEST(UserMemoryHonorsPrivacyLearningAndApplicationPolicies) {
  UserMemory memory(DeterministicOptions());
  ZR_EXPECT_EQ(memory.RecordAccepted("hao", "candidate", Context(100)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_TRUE(memory.Flush());

  memory.SetLearningEnabled(false);
  ZR_EXPECT_EQ(memory.RecordAccepted("hao", "candidate", Context(101)),
               MemoryRecordResult::kLearningDisabled);
  ZR_EXPECT_EQ(memory.View("hao", "candidate", Context(101)).accepted_count,
               std::uint64_t{1});

  memory.SetLearningEnabled(true);
  memory.SetPrivacyMode(true);
  ZR_EXPECT_EQ(memory.RecordAccepted("hao", "candidate", Context(102)),
               MemoryRecordResult::kPrivacyMode);
  ZR_EXPECT_EQ(memory.View("hao", "candidate", Context(102)).accepted_count,
               std::uint64_t{0});

  memory.SetPrivacyMode(false);
  auto sensitive = Context(103);
  sensitive.sensitive = true;
  ZR_EXPECT_EQ(memory.RecordAccepted("hao", "candidate", sensitive),
               MemoryRecordResult::kSensitiveContext);
  ZR_EXPECT_EQ(memory.View("hao", "candidate", sensitive).accepted_count,
               std::uint64_t{0});

  memory.SetApplicationLearningEnabled("test.exe", false);
  ZR_EXPECT_TRUE(!memory.IsApplicationLearningEnabled("test.exe"));
  ZR_EXPECT_EQ(memory.RecordAccepted("hao", "candidate", Context(104)),
               MemoryRecordResult::kApplicationExcluded);
  ZR_EXPECT_EQ(memory.View("hao", "candidate", Context(104)).accepted_count,
               std::uint64_t{0});
  memory.SetApplicationLearningEnabled("test.exe", true);
  ZR_EXPECT_TRUE(memory.IsApplicationLearningEnabled("test.exe"));
}

ZR_TEST(UserMemoryNormalizesWindowsApplicationAliases) {
  UserMemory memory(DeterministicOptions());
  memory.SetApplicationLearningEnabled("C:/Apps/TEST.EXE", false);
  ZR_EXPECT_TRUE(
      !memory.IsApplicationLearningEnabled("c:\\apps\\test.exe"));
  ZR_EXPECT_EQ(memory.RecordAccepted(
                   "hao", "candidate",
                   Context(100, "before", "c:\\apps\\test.exe")),
               MemoryRecordResult::kApplicationExcluded);
  memory.SetApplicationLearningEnabled("c:\\APPS\\test.exe", true);
  ZR_EXPECT_TRUE(memory.IsApplicationLearningEnabled("C:/Apps/Test.exe"));
}

ZR_TEST(UserMemoryEvictsTheOldestCandidateAtEntryLimit) {
  auto options = DeterministicOptions();
  options.maximum_entries = 2;
  UserMemory memory(options);
  ZR_EXPECT_EQ(memory.RecordAccepted("a", "first", Context(100)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_EQ(memory.RecordAccepted("b", "second", Context(200)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_EQ(memory.RecordAccepted("c", "third", Context(300)),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_TRUE(memory.Flush());
  ZR_EXPECT_EQ(memory.View("a", "first", Context(300)).accepted_count,
               std::uint64_t{0});
  ZR_EXPECT_EQ(memory.View("b", "second", Context(300)).accepted_count,
               std::uint64_t{1});
  ZR_EXPECT_EQ(memory.View("c", "third", Context(300)).accepted_count,
               std::uint64_t{1});
}

ZR_TEST(UserMemoryEnforcesGlobalFeatureBudgetDuringLearning) {
  auto options = DeterministicOptions();
  options.maximum_feature_values_per_candidate = 1;
  options.maximum_total_feature_values = 2;
  UserMemory memory(options);
  ZR_EXPECT_EQ(memory.RecordAccepted("a", "first", Context(100, "x", "a")),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_EQ(memory.RecordAccepted("b", "second", Context(200, "y", "b")),
               MemoryRecordResult::kQueued);
  ZR_EXPECT_TRUE(memory.Flush());
  ZR_EXPECT_EQ(memory.View("a", "first", Context(200)).accepted_count,
               std::uint64_t{0});
  ZR_EXPECT_EQ(memory.View("b", "second", Context(200)).accepted_count,
               std::uint64_t{1});
}

ZR_TEST(UserMemoryEnforcesGlobalStringBudgetDuringLearning) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  options.maximum_feature_values_per_candidate = 1;
  options.maximum_total_feature_values = 2;
  options.maximum_reading_bytes = 4;
  options.maximum_text_bytes = 4;
  options.maximum_context_bytes = 4;
  options.maximum_application_bytes = 4;
  options.maximum_total_string_bytes = 16;
  ZR_EXPECT_TRUE(options.IsValid());
  {
    UserMemory memory(options);
    const auto no_features = Context(100, "", "");
    ZR_EXPECT_EQ(memory.RecordAccepted("aaaa", "1111", no_features),
                 MemoryRecordResult::kQueued);
    ZR_EXPECT_EQ(
        memory.RecordAccepted("bbbb", "2222", Context(200, "", "")),
        MemoryRecordResult::kQueued);
    ZR_EXPECT_EQ(
        memory.RecordAccepted("cccc", "3333", Context(300, "", "")),
        MemoryRecordResult::kQueued);
    ZR_EXPECT_TRUE(memory.Flush());
  }
  UserMemory reopened(options);
  ZR_EXPECT_EQ(reopened.View("aaaa", "1111", Context(300)).accepted_count,
               std::uint64_t{0});
  ZR_EXPECT_EQ(reopened.View("bbbb", "2222", Context(300)).accepted_count,
               std::uint64_t{1});
  ZR_EXPECT_EQ(reopened.View("cccc", "3333", Context(300)).accepted_count,
               std::uint64_t{1});
}

ZR_TEST(UserMemoryRejectsInvalidAndOversizedRecords) {
  auto options = DeterministicOptions();
  options.maximum_reading_bytes = 4;
  options.maximum_text_bytes = 8;
  UserMemory memory(options);
  ZR_EXPECT_EQ(memory.RecordAccepted("", "candidate", Context(100)),
               MemoryRecordResult::kInvalidInput);
  ZR_EXPECT_EQ(memory.RecordAccepted("12345", "candidate", Context(100)),
               MemoryRecordResult::kInvalidInput);
  ZR_EXPECT_EQ(memory.RecordAccepted("1234", "123456789", Context(100)),
               MemoryRecordResult::kInvalidInput);
}

ZR_TEST(UserMemoryBoundedQueueNeverLosesAnAcknowledgedRecord) {
  auto options = DeterministicOptions();
  options.queue_capacity = 16;
  options.worker_batch_size = 8;
  UserMemory memory(options);
  std::uint64_t queued = 0;
  for (std::uint64_t index = 0; index < 5000; ++index) {
    const auto result =
        memory.RecordAccepted(
            "ni", "candidate",
            Context(100 + static_cast<std::int64_t>(index)));
    if (result == MemoryRecordResult::kQueued) {
      ++queued;
    } else {
      ZR_EXPECT_TRUE(result == MemoryRecordResult::kBusy ||
                     result == MemoryRecordResult::kQueueFull);
    }
  }
  ZR_EXPECT_TRUE(queued > 0);
  ZR_EXPECT_TRUE(memory.Flush());
  ZR_EXPECT_EQ(memory.View("ni", "candidate", Context(6000)).accepted_count,
               queued);
  ZR_EXPECT_TRUE(memory.diagnostics().dropped_records <= 5000 - queued);
}

ZR_TEST(UserMemoryFlushBarrierCompletesDuringContinuousProduction) {
  auto options = DeterministicOptions();
  options.queue_capacity = 256;
  options.worker_batch_size = 16;
  UserMemory memory(options);
  std::atomic<bool> producing{true};
  std::atomic<std::uint64_t> queued{0};
  std::thread producer([&] {
    std::int64_t timestamp = 100;
    while (producing.load(std::memory_order_relaxed)) {
      if (memory.RecordAccepted("ni", "candidate", Context(timestamp)) ==
          MemoryRecordResult::kQueued) {
        queued.fetch_add(1, std::memory_order_relaxed);
        ++timestamp;
      } else {
        std::this_thread::yield();
      }
    }
  });
  while (queued.load(std::memory_order_relaxed) < 100) {
    std::this_thread::yield();
  }

  auto barrier = std::async(std::launch::async, [&] { return memory.Flush(); });
  const auto status = barrier.wait_for(std::chrono::seconds(2));
  producing.store(false, std::memory_order_relaxed);
  producer.join();
  ZR_EXPECT_TRUE(status == std::future_status::ready);
  ZR_EXPECT_TRUE(barrier.get());
  ZR_EXPECT_TRUE(memory.Flush());
  ZR_EXPECT_EQ(memory.View("ni", "candidate", Context(100000)).accepted_count,
               queued.load(std::memory_order_relaxed));
}

ZR_TEST(UserMemoryDestructorDrainsAndPersistsQueuedWrites) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  {
    UserMemory memory(options);
    ZR_EXPECT_EQ(memory.RecordAccepted("nihao", "candidate", Context(1000)),
                 MemoryRecordResult::kQueued);
  }

  UserMemory reopened(options);
  const auto view = reopened.View("nihao", "candidate", Context(1000));
  ZR_EXPECT_EQ(view.accepted_count, std::uint64_t{1});
  ZR_EXPECT_TRUE(reopened.diagnostics().loaded_snapshot);
}

ZR_TEST(UserMemoryJournalRecordBudgetCheckpointsWithoutDataLoss) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  options.worker_batch_size = 1;
  options.maximum_journal_records = 1;
  {
    UserMemory memory(options);
    for (std::int64_t index = 0; index < 5; ++index) {
      const std::string reading = "r" + std::to_string(index);
      const std::string text = "candidate" + std::to_string(index);
      ZR_EXPECT_EQ(memory.RecordAccepted(reading, text,
                                        Context(100 + index)),
                   MemoryRecordResult::kQueued);
      ZR_EXPECT_TRUE(memory.Flush());
    }
  }

  UserMemory reopened(options);
  for (std::int64_t index = 0; index < 5; ++index) {
    const std::string reading = "r" + std::to_string(index);
    const std::string text = "candidate" + std::to_string(index);
    ZR_EXPECT_EQ(reopened.View(reading, text, Context(200)).accepted_count,
                 std::uint64_t{1});
  }
}

ZR_TEST(UserMemoryPreCompressionSnapshotNeverSkipsTheIncomingBatch) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  options.worker_batch_size = 1;
  options.maximum_journal_records = 1;
  const auto snapshot =
      directory.path() / std::string(UserMemory::kSnapshotFileName);
  const auto backup =
      directory.path() / std::string(UserMemory::kBackupFileName);
  const auto journal =
      directory.path() / std::string(UserMemory::kJournalFileName);
  std::vector<char> crash_snapshot;
  std::vector<char> crash_backup;
  std::vector<char> crash_journal;
  {
    UserMemory memory(options);
    ZR_EXPECT_EQ(memory.RecordAccepted("first", "candidate1", Context(100)),
                 MemoryRecordResult::kQueued);
    const auto first_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (memory.View("first", "candidate1", Context(100)).accepted_count ==
               0 &&
           std::chrono::steady_clock::now() < first_deadline) {
      std::this_thread::yield();
    }
    ZR_EXPECT_EQ(memory.View("first", "candidate1", Context(100))
                     .accepted_count,
                 std::uint64_t{1});
    MemoryRecordResult second_result = MemoryRecordResult::kBusy;
    const auto enqueue_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (second_result != MemoryRecordResult::kQueued &&
           std::chrono::steady_clock::now() < enqueue_deadline) {
      second_result =
          memory.RecordAccepted("second", "candidate2", Context(200));
      if (second_result != MemoryRecordResult::kQueued) {
        ZR_EXPECT_TRUE(second_result == MemoryRecordResult::kBusy ||
                       second_result == MemoryRecordResult::kQueueFull);
        std::this_thread::yield();
      }
    }
    ZR_EXPECT_EQ(second_result, MemoryRecordResult::kQueued);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (memory.View("second", "candidate2", Context(200)).accepted_count ==
               0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ZR_EXPECT_EQ(memory.View("second", "candidate2", Context(200))
                     .accepted_count,
                 std::uint64_t{1});
    crash_snapshot = ReadFile(snapshot);
    crash_backup = ReadFile(backup);
    crash_journal = ReadFile(journal);
  }

  ZR_EXPECT_TRUE(!crash_snapshot.empty());
  ZR_EXPECT_TRUE(!crash_backup.empty());
  ZR_EXPECT_TRUE(!crash_journal.empty());
  WriteFile(snapshot, crash_snapshot);
  WriteFile(backup, crash_backup);
  WriteFile(journal, crash_journal);

  UserMemory recovered(options);
  ZR_EXPECT_EQ(recovered.View("first", "candidate1", Context(200))
                   .accepted_count,
               std::uint64_t{1});
  ZR_EXPECT_EQ(recovered.View("second", "candidate2", Context(200))
                   .accepted_count,
               std::uint64_t{1});
}

ZR_TEST(UserMemoryJournalReplayMatchesLiveEvictionSemantics) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  options.queue_capacity = 4096;
  options.worker_batch_size = 128;
  options.maximum_entries = 1;
  const auto snapshot =
      directory.path() / std::string(UserMemory::kSnapshotFileName);
  const auto backup =
      directory.path() / std::string(UserMemory::kBackupFileName);
  const auto journal =
      directory.path() / std::string(UserMemory::kJournalFileName);
  std::vector<char> crash_journal;
  std::uint64_t live_a = 0;
  std::uint64_t live_b = 0;
  constexpr std::int64_t kEventCount = 3000;
  {
    UserMemory memory(options);
    for (std::int64_t index = 0; index < kEventCount; ++index) {
      const bool use_b = index % 3 == 1;
      MemoryRecordResult result = MemoryRecordResult::kBusy;
      while (result != MemoryRecordResult::kQueued) {
        result = memory.RecordAccepted(use_b ? "b" : "a",
                                       use_b ? "second" : "first",
                                       Context(100 + index, "", ""));
        if (result != MemoryRecordResult::kQueued) {
          ZR_EXPECT_TRUE(result == MemoryRecordResult::kBusy ||
                         result == MemoryRecordResult::kQueueFull);
          std::this_thread::yield();
        }
      }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((memory.diagnostics().queued_records != 0 ||
            memory.View("a", "first", Context(100 + kEventCount - 1))
                    .recency < 0.999999) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    const auto before_a =
        memory.View("a", "first", Context(100 + kEventCount - 1));
    const auto before_b =
        memory.View("b", "second", Context(100 + kEventCount - 1));
    ZR_EXPECT_TRUE(before_a.recency > 0.999999);
    live_a = before_a.accepted_count;
    live_b = before_b.accepted_count;
    crash_journal = ReadFile(journal);
  }

  ZR_EXPECT_EQ(live_a, std::uint64_t{1});
  ZR_EXPECT_EQ(live_b, std::uint64_t{0});
  ZR_EXPECT_TRUE(!crash_journal.empty());
  std::error_code ignored;
  std::filesystem::remove(snapshot, ignored);
  std::filesystem::remove(backup, ignored);
  WriteFile(journal, crash_journal);
  UserMemory replayed(options);
  ZR_EXPECT_EQ(replayed.View("a", "first", Context(4000)).accepted_count,
               live_a);
  ZR_EXPECT_EQ(replayed.View("b", "second", Context(4000)).accepted_count,
               live_b);
}

ZR_TEST(UserMemoryPreservesAValidJournalThatExceedsConfiguredRecordBudget) {
  TemporaryDirectory directory;
  auto writer_options = DeterministicOptions();
  writer_options.storage_directory = directory.path();
  writer_options.worker_batch_size = 1;
  writer_options.maximum_journal_records = 2;
  const auto journal =
      directory.path() / std::string(UserMemory::kJournalFileName);
  std::vector<char> two_records;
  {
    UserMemory memory(writer_options);
    ZR_EXPECT_EQ(memory.RecordAccepted("first", "candidate1", Context(100)),
                 MemoryRecordResult::kQueued);
    ZR_EXPECT_EQ(memory.RecordAccepted("second", "candidate2", Context(200)),
                 MemoryRecordResult::kQueued);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (memory.View("second", "candidate2", Context(200)).accepted_count ==
               0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ZR_EXPECT_EQ(memory.View("first", "candidate1", Context(200))
                     .accepted_count,
                 std::uint64_t{1});
    ZR_EXPECT_EQ(memory.View("second", "candidate2", Context(200))
                     .accepted_count,
                 std::uint64_t{1});
    two_records = ReadFile(journal);
  }

  ZR_EXPECT_TRUE(!two_records.empty());
  std::error_code ignored;
  std::filesystem::remove(
      directory.path() / std::string(UserMemory::kSnapshotFileName), ignored);
  std::filesystem::remove(
      directory.path() / std::string(UserMemory::kBackupFileName), ignored);
  WriteFile(journal, two_records);
  const auto original_size = std::filesystem::file_size(journal);

  auto reader_options = writer_options;
  reader_options.maximum_journal_records = 1;
  bool rejected = false;
  try {
    UserMemory over_budget(reader_options);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  ZR_EXPECT_TRUE(rejected);
  ZR_EXPECT_EQ(std::filesystem::file_size(journal), original_size);
}

ZR_TEST(UserMemoryRefusesToAppendWhenExistingJournalCannotBeRead) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  options.maximum_entries = 1;
  options.maximum_feature_values_per_candidate = 1;
  options.maximum_total_feature_values = 2;
  options.maximum_reading_bytes = 1;
  options.maximum_text_bytes = 1;
  options.maximum_context_bytes = 1;
  options.maximum_application_bytes = 1;
  options.maximum_total_string_bytes = 4;
  options.maximum_storage_bytes = 4096;
  options.worker_batch_size = 1;
  options.maximum_journal_records = 1;
  ZR_EXPECT_TRUE(options.IsValid());
  const auto journal =
      directory.path() / std::string(UserMemory::kJournalFileName);
  WriteFile(journal, std::vector<char>(4097, 'x'));
  const auto original_size = std::filesystem::file_size(journal);

  bool rejected = false;
  try {
    UserMemory unsafe_writer(options);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  ZR_EXPECT_TRUE(rejected);
  ZR_EXPECT_EQ(std::filesystem::file_size(journal), original_size);
}

ZR_TEST(UserMemoryHistoricalRecoveryErrorDoesNotPoisonNewFlush) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  {
    std::ofstream snapshot(
        directory.path() / std::string(UserMemory::kSnapshotFileName),
        std::ios::binary | std::ios::trunc);
    snapshot.write("invalid", 7);
    std::ofstream backup(
        directory.path() / std::string(UserMemory::kBackupFileName),
        std::ios::binary | std::ios::trunc);
    backup.write("invalid", 7);
  }

  UserMemory memory(options);
  ZR_EXPECT_TRUE(memory.diagnostics().persistence_errors > 0);
  ZR_EXPECT_TRUE(memory.Flush());
}

ZR_TEST(UserMemoryStorageAllowsOnlyOneWriter) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  UserMemory owner(options);
  bool rejected = false;
  try {
    UserMemory competing_writer(options);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  ZR_EXPECT_TRUE(rejected);
}

ZR_TEST(UserMemoryRecoversChecksummedJournalTail) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  const auto snapshot =
      directory.path() / std::string(UserMemory::kSnapshotFileName);
  const auto backup =
      directory.path() / std::string(UserMemory::kBackupFileName);
  const auto journal =
      directory.path() / std::string(UserMemory::kJournalFileName);
  std::vector<char> backup_at_first_record;
  std::vector<char> valid_journal_prefix;
  {
    UserMemory memory(options);
    ZR_EXPECT_EQ(memory.RecordAccepted("first", "candidate1", Context(1000)),
                 MemoryRecordResult::kQueued);
    ZR_EXPECT_TRUE(memory.Flush());
    ZR_EXPECT_EQ(memory.RecordAccepted("second", "candidate2", Context(2000)),
                 MemoryRecordResult::kQueued);
    ZR_EXPECT_TRUE(memory.Flush());
    backup_at_first_record = ReadFile(backup);
    valid_journal_prefix = ReadFile(journal);
  }

  ZR_EXPECT_TRUE(!backup_at_first_record.empty());
  ZR_EXPECT_TRUE(!valid_journal_prefix.empty());
  WriteFile(backup, backup_at_first_record);
  WriteFile(snapshot, std::vector<char>{'i', 'n', 'v', 'a', 'l', 'i', 'd'});
  valid_journal_prefix.insert(valid_journal_prefix.end(),
                              {'b', 'r', 'o', 'k', 'e', 'n'});
  WriteFile(journal, valid_journal_prefix);

  UserMemory recovered(options);
  ZR_EXPECT_TRUE(recovered.diagnostics().recovered_from_backup);
  ZR_EXPECT_TRUE(recovered.diagnostics().recovered_corrupt_journal_tail);
  ZR_EXPECT_EQ(recovered.View("first", "candidate1", Context(2000))
                   .accepted_count,
               std::uint64_t{1});
  ZR_EXPECT_EQ(recovered.View("second", "candidate2", Context(2000))
                   .accepted_count,
               std::uint64_t{1});
  ZR_EXPECT_EQ(std::filesystem::file_size(journal),
               static_cast<std::uintmax_t>(valid_journal_prefix.size() - 6));
}

#if defined(_WIN32)
ZR_TEST(UserMemoryRefusesToWriteBehindAnUntruncatableCorruptTail) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  const auto journal =
      directory.path() / std::string(UserMemory::kJournalFileName);
  std::vector<char> valid_record;
  {
    UserMemory memory(options);
    ZR_EXPECT_EQ(memory.RecordAccepted("nihao", "candidate", Context(1000)),
                 MemoryRecordResult::kQueued);
    ZR_EXPECT_TRUE(memory.Flush());
    valid_record = ReadFile(journal);
  }
  ZR_EXPECT_TRUE(!valid_record.empty());
  valid_record.insert(valid_record.end(), {'b', 'r', 'o', 'k', 'e', 'n'});
  WriteFile(journal, valid_record);
  const auto original_size = std::filesystem::file_size(journal);

  const HANDLE blocker =
      CreateFileW(journal.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ZR_EXPECT_TRUE(blocker != INVALID_HANDLE_VALUE);
  bool rejected = false;
  try {
    UserMemory unsafe_writer(options);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  ZR_EXPECT_TRUE(CloseHandle(blocker) != 0);
  ZR_EXPECT_TRUE(rejected);
  ZR_EXPECT_EQ(std::filesystem::file_size(journal), original_size);
}
#endif

ZR_TEST(UserMemoryFallsBackToPreviousAtomicSnapshot) {
  TemporaryDirectory directory;
  auto options = DeterministicOptions();
  options.storage_directory = directory.path();
  {
    UserMemory memory(options);
    ZR_EXPECT_EQ(memory.RecordAccepted("nihao", "candidate", Context(1000)),
                 MemoryRecordResult::kQueued);
    ZR_EXPECT_TRUE(memory.Flush());
  }

  const auto snapshot =
      directory.path() / std::string(UserMemory::kSnapshotFileName);
  const auto backup =
      directory.path() / std::string(UserMemory::kBackupFileName);
  ZR_EXPECT_TRUE(std::filesystem::exists(backup));
  {
    std::ofstream output(snapshot, std::ios::binary | std::ios::trunc);
    output.write("invalid", 7);
  }

  UserMemory recovered(options);
  ZR_EXPECT_TRUE(recovered.diagnostics().recovered_from_backup);
  ZR_EXPECT_EQ(recovered.View("nihao", "candidate", Context(1000))
                   .accepted_count,
               std::uint64_t{1});
}

}  // namespace

#if defined(ZRINPUT_USER_MEMORY_STANDALONE_TEST_MAIN)
int main() {
  return zrinput::test::RunAll();
}
#endif
