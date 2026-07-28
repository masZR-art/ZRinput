#pragma once

#include <cstdint>
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
  static double UsageScore(const Usage& usage, std::int64_t now);

  std::unordered_map<std::string, CandidateUsage> usage_;
};

}  // namespace zrinput
