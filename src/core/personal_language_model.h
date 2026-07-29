#pragma once

#include <cstdint>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zrinput {

struct LearningEvent {
  std::string text;
  std::string input;
  std::string application;
  std::vector<std::string> context;
  std::int64_t timestamp = 0;
  bool private_mode = false;
};

class PersonalLanguageModel {
 public:
  void Accept(const LearningEvent& event);
  void Reject(const LearningEvent& event);
  double Score(const std::string& candidate,
               const LearningEvent& context) const;
  std::vector<std::string> Predict(const LearningEvent& context,
                                   std::size_t limit) const;
  bool Save(const std::filesystem::path& path);
  bool Load(const std::filesystem::path& path);
  void Clear();
  std::size_t size() const;

 private:
  struct Usage {
    double accepted = 0;
    double rejected = 0;
    std::int64_t last_used = 0;
  };

  using CandidateUsage = std::unordered_map<std::string, Usage>;
  static std::string ContextKey(const LearningEvent& event,
                                std::size_t depth,
                                bool include_application);
  static std::size_t EffectiveContextSize(const LearningEvent& event);
  static void Update(Usage& usage,
                     bool accepted,
                     std::int64_t timestamp);
  void Record(const std::string& key,
              const std::string& candidate,
              bool accepted,
              std::int64_t timestamp);
  static bool ReadSnapshot(const std::filesystem::path& path,
                           std::unordered_map<std::string,
                                              CandidateUsage>& usage);
  static bool WriteSnapshot(
      const std::filesystem::path& path,
      const std::unordered_map<std::string, CandidateUsage>& usage);
  static void MergeUsage(
      std::unordered_map<std::string, CandidateUsage>& target,
      const std::unordered_map<std::string, CandidateUsage>& delta);
  static double UsageScore(const Usage& usage, std::int64_t now);
  double ScoreUnlocked(const std::string& candidate,
                       const LearningEvent& context) const;

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, CandidateUsage> usage_;
  std::unordered_map<std::string, CandidateUsage> pending_;
  bool clear_pending_ = false;
};

}  // namespace zrinput
