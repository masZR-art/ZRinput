#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "theme/theme.h"

namespace zrinput::theme {

struct ThemeManagerOptions {
  std::chrono::milliseconds poll_interval{100};
  std::chrono::milliseconds settle_interval{100};
  std::chrono::milliseconds full_rescan_interval{2000};
};

struct ThemeManagerError {
  std::uint64_t reload_attempt = 0;
  std::string message;

  bool operator==(const ThemeManagerError&) const = default;
};

struct ThemeManagerStatus {
  std::uint64_t generation = 1;
  std::uint64_t reload_attempt = 0;
  bool using_safe_default = true;
  bool watching = false;
  std::filesystem::path source_path;
  std::optional<ThemeManagerError> last_error;
};

class ThemeManager final {
 public:
  ThemeManager();
  explicit ThemeManager(std::filesystem::path source_path,
                        ThemeManagerOptions options = {});
  ~ThemeManager();

  ThemeManager(const ThemeManager&) = delete;
  ThemeManager& operator=(const ThemeManager&) = delete;
  ThemeManager(ThemeManager&&) = delete;
  ThemeManager& operator=(ThemeManager&&) = delete;

  [[nodiscard]] std::shared_ptr<const Theme> Snapshot() const noexcept;
  [[nodiscard]] ThemeManagerStatus Status() const;

  [[nodiscard]] bool ReloadNow();
  [[nodiscard]] bool WaitForGeneration(std::uint64_t minimum_generation,
                                       std::chrono::milliseconds timeout) const;
  [[nodiscard]] bool WaitForReloadAttempt(
      std::uint64_t minimum_attempt, std::chrono::milliseconds timeout) const;

  void Stop() noexcept;

 private:
  [[nodiscard]] bool ReloadSource(
      std::optional<std::string>* attempted_contents);
  void WatchLoop(std::stop_token stop_token) noexcept;
  void RecordFailure(std::string message);
  void Publish(Theme theme);

  std::atomic<std::shared_ptr<const Theme>> snapshot_;
  std::filesystem::path source_path_;
  ThemeManagerOptions options_;

  mutable std::mutex state_mutex_;
  mutable std::condition_variable state_changed_;
  ThemeManagerStatus status_;

  std::mutex reload_mutex_;
  std::mutex stop_mutex_;
  std::mutex wake_mutex_;
  std::condition_variable_any wake_condition_;
  std::jthread watcher_;
};

}  // namespace zrinput::theme
