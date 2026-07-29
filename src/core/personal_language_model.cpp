#include "core/personal_language_model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zrinput {
namespace {
constexpr char kSeparator = '\x1f';
constexpr double kHalfLifeSeconds = 120.0 * 24.0 * 60.0 * 60.0;

bool IsSentenceBoundary(const std::string& token) {
  return token.find_first_of(".!?;\n\r") != std::string::npos ||
         token.find("。") != std::string::npos ||
         token.find("！") != std::string::npos ||
         token.find("？") != std::string::npos ||
         token.find("；") != std::string::npos;
}

std::string Escape(const std::string& value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const unsigned char ch : value) {
    if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r')
      output << '%' << std::setw(2) << static_cast<unsigned int>(ch);
    else
      output << static_cast<char>(ch);
  }
  return output.str();
}

bool Unescape(const std::string& value, std::string& output) {
  output.clear();
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      output.push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size())
      return false;
    unsigned int decoded = 0;
    std::istringstream hex(value.substr(i + 1, 2));
    hex >> std::hex >> decoded;
    if (!hex || decoded > 0xff)
      return false;
    output.push_back(static_cast<char>(decoded));
    i += 2;
  }
  return true;
}

std::uint64_t Checksum(const std::string& data) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char ch : data) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const auto tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab - start));
    if (tab == std::string::npos)
      return fields;
    start = tab + 1;
  }
}

#ifdef _WIN32
class InterprocessSaveLock {
 public:
  explicit InterprocessSaveLock(const std::filesystem::path& path) {
    std::error_code error;
    auto normalized = std::filesystem::absolute(path, error);
    if (error)
      normalized = path;
    normalized = normalized.lexically_normal();

    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t ch : normalized.native()) {
      hash ^= static_cast<std::uint16_t>(ch);
      hash *= 1099511628211ull;
    }
    std::wostringstream name;
    name << L"Local\\ZRinput.PersonalModel." << std::hex << hash;
    mutex_ = CreateMutexW(nullptr, FALSE, name.str().c_str());
    if (!mutex_)
      return;
    const DWORD wait = WaitForSingleObject(mutex_, INFINITE);
    locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
  }

  ~InterprocessSaveLock() {
    if (locked_)
      ReleaseMutex(mutex_);
    if (mutex_)
      CloseHandle(mutex_);
  }

  bool locked() const { return locked_; }

 private:
  HANDLE mutex_ = nullptr;
  bool locked_ = false;
};
#else
std::mutex g_save_mutex;

class InterprocessSaveLock {
 public:
  explicit InterprocessSaveLock(const std::filesystem::path&)
      : lock_(g_save_mutex) {}
  bool locked() const { return true; }

 private:
  std::unique_lock<std::mutex> lock_;
};
#endif
}

void PersonalLanguageModel::Accept(const LearningEvent& event) {
  if (event.private_mode || event.text.empty())
    return;
  std::unique_lock lock(mutex_);
  Record("global", event.text, true, event.timestamp);
  if (!event.input.empty())
    Record("input" + std::string(1, kSeparator) + event.input, event.text,
           true, event.timestamp);
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(event));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    Record(ContextKey(event, depth, false), event.text, true, event.timestamp);
    if (!event.application.empty())
      Record(ContextKey(event, depth, true), event.text, true,
             event.timestamp);
  }
}

void PersonalLanguageModel::Reject(const LearningEvent& event) {
  if (event.private_mode || event.text.empty())
    return;
  std::unique_lock lock(mutex_);
  Record("global", event.text, false, event.timestamp);
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(event));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    Record(ContextKey(event, depth, false), event.text, false,
           event.timestamp);
    if (!event.application.empty())
      Record(ContextKey(event, depth, true), event.text, false,
             event.timestamp);
  }
}

double PersonalLanguageModel::Score(const std::string& candidate,
                                    const LearningEvent& context) const {
  std::shared_lock lock(mutex_);
  return ScoreUnlocked(candidate, context);
}

double PersonalLanguageModel::ScoreUnlocked(
    const std::string& candidate,
    const LearningEvent& context) const {
  double score = 0;
  const auto add = [&](const std::string& key, double weight) {
    const auto group = usage_.find(key);
    if (group == usage_.end())
      return;
    const auto item = group->second.find(candidate);
    if (item != group->second.end())
      score += weight * UsageScore(item->second, context.timestamp);
  };
  add("global", 0.2);
  if (!context.input.empty())
    add("input" + std::string(1, kSeparator) + context.input, 0.8);
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(context));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    const double weight = 0.7 + 0.35 * static_cast<double>(depth);
    add(ContextKey(context, depth, false), weight);
    if (!context.application.empty())
      add(ContextKey(context, depth, true), weight * 1.35);
  }
  return score;
}

std::vector<std::string> PersonalLanguageModel::Predict(
    const LearningEvent& context,
    std::size_t limit) const {
  std::shared_lock lock(mutex_);
  std::set<std::string> candidates;
  const auto depth_limit =
      std::min<std::size_t>(4, EffectiveContextSize(context));
  for (std::size_t depth = 1; depth <= depth_limit; ++depth) {
    for (const bool app : {false, true}) {
      if (app && context.application.empty())
        continue;
      const auto found = usage_.find(ContextKey(context, depth, app));
      if (found != usage_.end())
        for (const auto& [text, unused] : found->second)
          candidates.insert(text);
    }
  }
  std::vector<std::string> result(candidates.begin(), candidates.end());
  std::stable_sort(result.begin(), result.end(), [&](const auto& left,
                                                     const auto& right) {
    return ScoreUnlocked(left, context) > ScoreUnlocked(right, context);
  });
  if (result.size() > limit)
    result.resize(limit);
  return result;
}

bool PersonalLanguageModel::Save(const std::filesystem::path& path) {
  std::unique_lock lock(mutex_);
  InterprocessSaveLock save_lock(path);
  if (!save_lock.locked())
    return false;

  std::unordered_map<std::string, CandidateUsage> merged;
  const bool loaded_disk = !clear_pending_ && ReadSnapshot(path, merged);
  if (clear_pending_) {
    merged.clear();
    MergeUsage(merged, pending_);
  } else if (loaded_disk) {
    MergeUsage(merged, pending_);
  } else {
    merged = usage_;
  }
  if (!WriteSnapshot(path, merged))
    return false;

  usage_ = std::move(merged);
  pending_.clear();
  clear_pending_ = false;
  return true;
}

bool PersonalLanguageModel::WriteSnapshot(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, CandidateUsage>& usage) {
  std::vector<std::string> keys;
  keys.reserve(usage.size());
  for (const auto& [key, unused] : usage)
    keys.push_back(key);
  std::sort(keys.begin(), keys.end());

  std::ostringstream payload;
  for (const auto& key : keys) {
    std::vector<std::string> candidates;
    for (const auto& [candidate, unused] : usage.at(key))
      candidates.push_back(candidate);
    std::sort(candidates.begin(), candidates.end());
    for (const auto& candidate : candidates) {
      const auto& item = usage.at(key).at(candidate);
      payload << Escape(key) << '\t' << Escape(candidate) << '\t'
              << std::setprecision(17) << item.accepted << '\t'
              << item.rejected << '\t' << item.last_used << '\n';
    }
  }
  const std::string body = payload.str();
  std::ostringstream checksum;
  checksum << std::hex << Checksum(body);

  std::error_code error;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output << "ZRINPUT_PLM\t1\t" << checksum.str() << '\n' << body;
  output.flush();
  if (!output)
    return false;
  output.close();

#ifdef _WIN32
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::filesystem::rename(temporary, path, error);
  return !error;
#endif
}

bool PersonalLanguageModel::ReadSnapshot(
    const std::filesystem::path& path,
    std::unordered_map<std::string, CandidateUsage>& usage) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  std::string header;
  if (!std::getline(input, header))
    return false;
  const auto header_fields = Split(header);
  if (header_fields.size() != 3 || header_fields[0] != "ZRINPUT_PLM" ||
      header_fields[1] != "1")
    return false;
  const std::string body((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  std::ostringstream actual_checksum;
  actual_checksum << std::hex << Checksum(body);
  if (actual_checksum.str() != header_fields[2])
    return false;

  std::unordered_map<std::string, CandidateUsage> loaded;
  std::istringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty())
      continue;
    const auto fields = Split(line);
    if (fields.size() != 5)
      return false;
    std::string key;
    std::string candidate;
    if (!Unescape(fields[0], key) || !Unescape(fields[1], candidate) ||
        candidate.empty())
      return false;
    try {
      Usage item;
      item.accepted = std::stod(fields[2]);
      item.rejected = std::stod(fields[3]);
      item.last_used = std::stoll(fields[4]);
      if (!std::isfinite(item.accepted) || !std::isfinite(item.rejected) ||
          item.accepted < 0 || item.rejected < 0)
        return false;
      loaded[key][candidate] = item;
    } catch (...) {
      return false;
    }
  }
  usage.swap(loaded);
  return true;
}

bool PersonalLanguageModel::Load(const std::filesystem::path& path) {
  std::unordered_map<std::string, CandidateUsage> loaded;
  if (!ReadSnapshot(path, loaded))
    return false;
  std::unique_lock lock(mutex_);
  usage_.swap(loaded);
  pending_.clear();
  clear_pending_ = false;
  return true;
}

void PersonalLanguageModel::Clear() {
  std::unique_lock lock(mutex_);
  usage_.clear();
  pending_.clear();
  clear_pending_ = true;
}

std::size_t PersonalLanguageModel::size() const {
  std::shared_lock lock(mutex_);
  std::size_t result = 0;
  for (const auto& [unused, candidates] : usage_)
    result += candidates.size();
  return result;
}

std::string PersonalLanguageModel::ContextKey(const LearningEvent& event,
                                              std::size_t depth,
                                              bool include_application) {
  std::string key = include_application ? "app" : "context";
  if (include_application)
    key += std::string(1, kSeparator) + event.application;
  key += std::string(1, kSeparator) + std::to_string(depth);
  const auto begin = event.context.size() - depth;
  for (std::size_t i = begin; i < event.context.size(); ++i)
    key += std::string(1, kSeparator) + event.context[i];
  return key;
}

std::size_t PersonalLanguageModel::EffectiveContextSize(
    const LearningEvent& event) {
  std::size_t available = 0;
  for (auto it = event.context.rbegin(); it != event.context.rend(); ++it) {
    if (IsSentenceBoundary(*it))
      break;
    ++available;
  }
  return available;
}

void PersonalLanguageModel::Update(Usage& usage,
                                   bool accepted,
                                   std::int64_t timestamp) {
  accepted ? usage.accepted += 1 : usage.rejected += 1;
  usage.last_used = std::max(usage.last_used, timestamp);
}

void PersonalLanguageModel::Record(const std::string& key,
                                   const std::string& candidate,
                                   bool accepted,
                                   std::int64_t timestamp) {
  Update(usage_[key][candidate], accepted, timestamp);
  Update(pending_[key][candidate], accepted, timestamp);
}

void PersonalLanguageModel::MergeUsage(
    std::unordered_map<std::string, CandidateUsage>& target,
    const std::unordered_map<std::string, CandidateUsage>& delta) {
  for (const auto& [key, candidates] : delta) {
    for (const auto& [candidate, increment] : candidates) {
      auto& item = target[key][candidate];
      item.accepted += increment.accepted;
      item.rejected += increment.rejected;
      item.last_used = std::max(item.last_used, increment.last_used);
    }
  }
}

double PersonalLanguageModel::UsageScore(const Usage& usage,
                                         std::int64_t now) {
  const auto age = std::max<std::int64_t>(0, now - usage.last_used);
  const double decay = std::exp2(-static_cast<double>(age) / kHalfLifeSeconds);
  return decay * (std::log1p(usage.accepted) -
                  2.5 * std::log1p(usage.rejected));
}

}  // namespace zrinput
