#include "theme/theme_manager.h"

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace zrinput::theme {
namespace {

struct FileStamp {
  bool regular_file = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type write_time{};
  std::error_code error;

  bool operator==(const FileStamp&) const = default;
};

FileStamp ObserveFile(const std::filesystem::path& path) noexcept {
  FileStamp stamp;
  std::error_code error;
  stamp.regular_file = std::filesystem::is_regular_file(path, error);
  if (error) {
    stamp.error = error;
    return stamp;
  }
  if (!stamp.regular_file) {
    return stamp;
  }
  stamp.size = std::filesystem::file_size(path, error);
  if (error) {
    stamp.error = error;
    return stamp;
  }
  stamp.write_time = std::filesystem::last_write_time(path, error);
  if (error) {
    stamp.error = error;
  }
  return stamp;
}

std::string DescribeObservationFailure(const FileStamp& stamp) {
  if (stamp.error) {
    return "cannot inspect theme file: " + stamp.error.message();
  }
  return "theme source is not a regular file";
}

std::string ReadBoundedFile(const std::filesystem::path& path,
                             const FileStamp& stamp) {
  if (stamp.size > ThemeSecurityLimits::kMaxThemeJsonBytes) {
    throw std::runtime_error("theme file exceeds the 512 KiB manifest budget");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("cannot open theme file");
  }

  std::string contents;
  contents.reserve(static_cast<std::size_t>(stamp.size));
  std::array<char, 4096> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count <= 0) {
      break;
    }
    const auto bytes = static_cast<std::size_t>(count);
    if (contents.size() > ThemeSecurityLimits::kMaxThemeJsonBytes - bytes) {
      throw std::runtime_error(
          "theme file grew beyond the 512 KiB manifest budget while reading");
    }
    contents.append(buffer.data(), bytes);
  }
  if (!input.eof()) {
    throw std::runtime_error("I/O error while reading theme file");
  }
  return contents;
}

std::optional<std::string> ReadStableContents(
    const std::filesystem::path& path, const FileStamp& stamp) noexcept {
  try {
    std::string contents = ReadBoundedFile(path, stamp);
    if (ObserveFile(path) != stamp) {
      return std::nullopt;
    }
    return contents;
  } catch (...) {
    return std::nullopt;
  }
}

std::string DescribeThemeFailure(const ThemeLoadResult& result) {
  if (result.issues.empty()) {
    return "theme validation failed without a diagnostic";
  }
  const auto& first = result.issues.front();
  std::ostringstream message;
  message << first.path << ": " << first.message;
  if (result.issues.size() > 1) {
    message << " (and " << (result.issues.size() - 1) << " more issue(s))";
  }
  return message.str();
}

void ValidateOptions(const ThemeManagerOptions& options) {
  if (options.poll_interval <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("theme poll interval must be positive");
  }
  if (options.settle_interval < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("theme settle interval must not be negative");
  }
  if (options.full_rescan_interval <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("theme full rescan interval must be positive");
  }
}

}  // namespace

ThemeManager::ThemeManager()
    : snapshot_(std::make_shared<const Theme>(SafeDefaultTheme())) {}

ThemeManager::ThemeManager(std::filesystem::path source_path,
                           ThemeManagerOptions options)
    : ThemeManager() {
  ValidateOptions(options);
  if (source_path.empty()) {
    throw std::invalid_argument("theme source path must not be empty");
  }
  std::error_code path_error;
  source_path_ = std::filesystem::absolute(source_path, path_error);
  if (path_error) {
    throw std::invalid_argument("theme source path cannot be made absolute: " +
                                path_error.message());
  }
  source_path_ = source_path_.lexically_normal();
  options_ = options;
  {
    std::lock_guard lock(state_mutex_);
    status_.source_path = source_path_;
    status_.watching = true;
  }
  watcher_ = std::jthread([this](std::stop_token token) { WatchLoop(token); });
}

ThemeManager::~ThemeManager() { Stop(); }

std::shared_ptr<const Theme> ThemeManager::Snapshot() const noexcept {
  return snapshot_.load(std::memory_order_acquire);
}

ThemeManagerStatus ThemeManager::Status() const {
  std::lock_guard lock(state_mutex_);
  return status_;
}

bool ThemeManager::ReloadNow() {
  return ReloadSource(nullptr);
}

bool ThemeManager::ReloadSource(
    std::optional<std::string>* attempted_contents) {
  std::lock_guard reload_lock(reload_mutex_);
  if (attempted_contents) {
    attempted_contents->reset();
  }
  try {
    if (source_path_.empty()) {
      RecordFailure("no theme source path is configured");
      return false;
    }

    const FileStamp before = ObserveFile(source_path_);
    if (before.error || !before.regular_file) {
      RecordFailure(DescribeObservationFailure(before));
      return false;
    }
    const std::string contents = ReadBoundedFile(source_path_, before);
    const FileStamp after = ObserveFile(source_path_);
    if (!(before == after)) {
      RecordFailure("theme file changed while it was being read");
      return false;
    }
    if (attempted_contents) {
      *attempted_contents = contents;
    }

    ThemeLoadResult loaded = LoadThemeJson(contents);
    if (loaded.used_fallback) {
      RecordFailure(DescribeThemeFailure(loaded));
      return false;
    }
    if (!loaded.theme.assets.icons.empty() || loaded.theme.assets.nine_slice) {
      RecordFailure(
          "external theme assets require the validated package loader");
      return false;
    }
    Publish(std::move(loaded.theme));
    return true;
  } catch (const std::exception& error) {
    RecordFailure(error.what());
    return false;
  } catch (...) {
    RecordFailure("unknown theme reload failure");
    return false;
  }
}

bool ThemeManager::WaitForGeneration(std::uint64_t minimum_generation,
                                     std::chrono::milliseconds timeout) const {
  std::unique_lock lock(state_mutex_);
  return state_changed_.wait_for(
      lock, timeout, [&] { return status_.generation >= minimum_generation; });
}

bool ThemeManager::WaitForReloadAttempt(
    std::uint64_t minimum_attempt, std::chrono::milliseconds timeout) const {
  std::unique_lock lock(state_mutex_);
  return state_changed_.wait_for(
      lock, timeout, [&] { return status_.reload_attempt >= minimum_attempt; });
}

void ThemeManager::Stop() noexcept {
  try {
    std::lock_guard stop_lock(stop_mutex_);
    if (watcher_.joinable()) {
      watcher_.request_stop();
      wake_condition_.notify_all();
      watcher_.join();
    }
    {
      std::lock_guard lock(state_mutex_);
      status_.watching = false;
    }
    state_changed_.notify_all();
  } catch (...) {
    // Destruction must not leak an exception across the TSF host boundary.
  }
}

void ThemeManager::WatchLoop(std::stop_token stop_token) noexcept {
  try {
    std::optional<FileStamp> pending;
    std::optional<FileStamp> processed;
    std::optional<std::string> processed_contents;
    auto pending_since = std::chrono::steady_clock::now();
    auto last_full_rescan = pending_since;

    while (!stop_token.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      const FileStamp observed = ObserveFile(source_path_);
      if (!pending || !(*pending == observed)) {
        pending = observed;
        pending_since = now;
      }

      const bool stable = now - pending_since >= options_.settle_interval;
      const bool not_processed = !processed || !(*processed == observed);
      const bool full_rescan_due =
          now - last_full_rescan >= options_.full_rescan_interval;
      std::optional<std::string> rescanned_contents;
      bool content_changed = false;
      if (stable && full_rescan_due && !not_processed) {
        rescanned_contents = ReadStableContents(source_path_, observed);
        content_changed = !rescanned_contents || !processed_contents ||
                          *rescanned_contents != *processed_contents;
      }
      if (stable && (not_processed || content_changed)) {
        std::optional<std::string> attempted_contents;
        const bool reloaded = ReloadSource(&attempted_contents);
        const FileStamp after_reload = ObserveFile(source_path_);
        if (reloaded || after_reload == observed) {
          processed = observed;
          processed_contents = std::move(attempted_contents);
        }
      }
      if (full_rescan_due) {
        last_full_rescan = now;
      }

      std::unique_lock wake_lock(wake_mutex_);
      wake_condition_.wait_for(wake_lock, stop_token, options_.poll_interval,
                               [] { return false; });
    }
  } catch (const std::exception& error) {
    try {
      RecordFailure(std::string("theme watcher stopped: ") + error.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      RecordFailure("theme watcher stopped after an unknown failure");
    } catch (...) {
    }
  }
  try {
    {
      std::lock_guard lock(state_mutex_);
      status_.watching = false;
    }
    state_changed_.notify_all();
  } catch (...) {
  }
}

void ThemeManager::RecordFailure(std::string message) {
  {
    std::lock_guard lock(state_mutex_);
    ++status_.reload_attempt;
    status_.last_error =
        ThemeManagerError{status_.reload_attempt, std::move(message)};
  }
  state_changed_.notify_all();
}

void ThemeManager::Publish(Theme theme) {
  auto next = std::make_shared<const Theme>(std::move(theme));
  {
    std::lock_guard lock(state_mutex_);
    ++status_.reload_attempt;
    const auto current = snapshot_.load(std::memory_order_relaxed);
    if (!current || !(*current == *next)) {
      snapshot_.store(std::move(next), std::memory_order_release);
      ++status_.generation;
    }
    status_.using_safe_default = false;
    status_.last_error.reset();
  }
  state_changed_.notify_all();
}

}  // namespace zrinput::theme
