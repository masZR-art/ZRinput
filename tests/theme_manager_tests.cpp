#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_harness.h"
#include "theme/theme.h"
#include "theme/theme_manager.h"

namespace {

using namespace std::chrono_literals;
using zrinput::theme::SafeDefaultTheme;
using zrinput::theme::SerializeTheme;
using zrinput::theme::Theme;
using zrinput::theme::ThemeManager;
using zrinput::theme::ThemeManagerOptions;
using zrinput::theme::ValidateTheme;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> counter{0};
    const auto suffix =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(counter.fetch_add(1));
    path_ = std::filesystem::temp_directory_path() /
            ("zrinput-theme-manager-tests-" + suffix);
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

Theme MakeTheme(std::string name) {
  Theme theme = SafeDefaultTheme();
  theme.name = std::move(name);
  return theme;
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("failed to open test theme file");
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write test theme file");
  }
}

bool WaitForName(ThemeManager& manager, std::string_view name,
                 std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (manager.Snapshot()->name == name) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return manager.Snapshot()->name == name;
}

ThemeManagerOptions FastOptions() { return ThemeManagerOptions{10ms, 20ms}; }

ZR_TEST(PublishesImmutableSnapshotsAtomically) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  WriteText(source, SerializeTheme(MakeTheme("Atomic A"), false));

  ThemeManager manager(source, FastOptions());
  ZR_EXPECT_TRUE(WaitForName(manager, "Atomic A", 2s));
  ZR_EXPECT_TRUE(manager.Status().source_path.is_absolute());
  const auto first = manager.Snapshot();
  const auto first_generation = manager.Status().generation;

  WriteText(source, SerializeTheme(MakeTheme("Atomic B"), false));
  ZR_EXPECT_TRUE(manager.WaitForGeneration(first_generation + 1, 2s));
  const auto second = manager.Snapshot();
  ZR_EXPECT_EQ(second->name, std::string("Atomic B"));
  ZR_EXPECT_EQ(first->name, std::string("Atomic A"));
  ZR_EXPECT_TRUE(first.get() != second.get());
  ZR_EXPECT_TRUE(ValidateTheme(*second).empty());
}

ZR_TEST(CorruptUpdateKeepsLastValidSnapshotAndReportsError) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  WriteText(source, SerializeTheme(MakeTheme("Stable"), false));

  ThemeManager manager(source, FastOptions());
  ZR_EXPECT_TRUE(WaitForName(manager, "Stable", 2s));
  const auto stable = manager.Snapshot();
  const auto before = manager.Status();

  WriteText(source, "{ definitely not valid json");
  ZR_EXPECT_TRUE(manager.WaitForReloadAttempt(before.reload_attempt + 1, 2s));
  const auto failed = manager.Status();
  ZR_EXPECT_EQ(failed.generation, before.generation);
  ZR_EXPECT_TRUE(failed.last_error.has_value());
  ZR_EXPECT_TRUE(manager.Snapshot().get() == stable.get());
  ZR_EXPECT_EQ(manager.Snapshot()->name, std::string("Stable"));

  WriteText(source, SerializeTheme(MakeTheme("Recovered"), false));
  ZR_EXPECT_TRUE(WaitForName(manager, "Recovered", 2s));
  const auto recovered = manager.Status();
  ZR_EXPECT_TRUE(!recovered.last_error.has_value());
  ZR_EXPECT_TRUE(recovered.generation > failed.generation);
}

ZR_TEST(RapidUpdatesConvergeWithoutPublishingPartialThemes) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  WriteText(source, SerializeTheme(MakeTheme("Initial"), false));
  ThemeManager manager(source, ThemeManagerOptions{5ms, 15ms});
  ZR_EXPECT_TRUE(WaitForName(manager, "Initial", 2s));

  std::atomic<bool> invalid_snapshot{false};
  std::jthread reader([&](std::stop_token token) {
    while (!token.stop_requested()) {
      const auto snapshot = manager.Snapshot();
      if (snapshot->name.empty() || !ValidateTheme(*snapshot).empty()) {
        invalid_snapshot.store(true, std::memory_order_relaxed);
        return;
      }
    }
  });

  for (int index = 0; index < 30; ++index) {
    WriteText(source, SerializeTheme(
                          MakeTheme("Burst " + std::to_string(index)), false));
    std::this_thread::sleep_for(1ms);
  }
  WriteText(source, SerializeTheme(MakeTheme("Burst Final"), false));
  ZR_EXPECT_TRUE(WaitForName(manager, "Burst Final", 3s));
  reader.request_stop();
  reader.join();

  ZR_EXPECT_TRUE(!invalid_snapshot.load(std::memory_order_relaxed));
  ZR_EXPECT_EQ(manager.Snapshot()->name, std::string("Burst Final"));
  ZR_EXPECT_TRUE(!manager.Status().last_error.has_value());
}

ZR_TEST(MissingSourceUsesSafeDefaultAndCanRecover) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "missing-theme.json";
  ThemeManager manager(source, FastOptions());
  ZR_EXPECT_EQ(manager.Snapshot()->name, SafeDefaultTheme().name);
  ZR_EXPECT_TRUE(manager.Status().using_safe_default);
  ZR_EXPECT_TRUE(manager.WaitForReloadAttempt(1, 2s));
  ZR_EXPECT_TRUE(manager.Status().last_error.has_value());

  WriteText(source, SerializeTheme(MakeTheme("Created Later"), false));
  ZR_EXPECT_TRUE(WaitForName(manager, "Created Later", 2s));
  ZR_EXPECT_TRUE(!manager.Status().using_safe_default);
}

ZR_TEST(ExternalAssetsCannotPublishBeforePackageValidation) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  Theme with_asset = MakeTheme("Unvalidated Asset Theme");
  with_asset.assets.icons.emplace("custom", "assets/custom.png");
  WriteText(source, SerializeTheme(with_asset, false));

  ThemeManager manager(source, FastOptions());
  ZR_EXPECT_TRUE(manager.WaitForReloadAttempt(1, 2s));
  ZR_EXPECT_EQ(manager.Snapshot()->name, SafeDefaultTheme().name);
  ZR_EXPECT_TRUE(manager.Status().using_safe_default);
  ZR_EXPECT_TRUE(manager.Status().last_error.has_value());
}

ZR_TEST(PeriodicRescanDetectsSameSizeAndTimestampReplacement) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  const std::string first = SerializeTheme(MakeTheme("Same Stamp A"), false);
  const std::string second = SerializeTheme(MakeTheme("Same Stamp B"), false);
  ZR_EXPECT_EQ(first.size(), second.size());
  WriteText(source, first);

  ThemeManager manager(source, ThemeManagerOptions{10ms, 20ms, 50ms});
  ZR_EXPECT_TRUE(WaitForName(manager, "Same Stamp A", 2s));
  const auto original_time = std::filesystem::last_write_time(source);
  WriteText(source, second);
  std::filesystem::last_write_time(source, original_time);
  ZR_EXPECT_TRUE(WaitForName(manager, "Same Stamp B", 2s));
}

ZR_TEST(StopAndDestructionInterruptLongPollingWait) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  WriteText(source, SerializeTheme(MakeTheme("Shutdown"), false));

  const auto started = std::chrono::steady_clock::now();
  {
    auto manager =
        std::make_unique<ThemeManager>(source, ThemeManagerOptions{10s, 10s});
    std::this_thread::sleep_for(20ms);
    manager.reset();
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ZR_EXPECT_TRUE(elapsed < 1s);
}

ZR_TEST(ManualReloadWithoutSourceReportsErrorAndKeepsDefault) {
  ThemeManager manager;
  const auto before = manager.Status();
  ZR_EXPECT_TRUE(!manager.ReloadNow());
  const auto after = manager.Status();
  ZR_EXPECT_EQ(after.generation, before.generation);
  ZR_EXPECT_EQ(after.reload_attempt, before.reload_attempt + 1);
  ZR_EXPECT_TRUE(after.last_error.has_value());
  ZR_EXPECT_TRUE(!after.watching);
}

ZR_TEST(StopIsThreadSafeAndIdempotent) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "theme.json";
  WriteText(source, SerializeTheme(MakeTheme("Concurrent Stop"), false));
  ThemeManager manager(source, ThemeManagerOptions{10s, 10s});

  std::vector<std::jthread> callers;
  for (int index = 0; index < 8; ++index) {
    callers.emplace_back([&] { manager.Stop(); });
  }
  callers.clear();
  manager.Stop();
  ZR_EXPECT_TRUE(!manager.Status().watching);
}

}  // namespace

int main() { return zrinput::test::RunAll(); }
