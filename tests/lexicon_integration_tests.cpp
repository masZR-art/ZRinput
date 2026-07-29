#include "core/pinyin_engine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: zrinput_lexicon_tests LEXICON\n";
    return 2;
  }
  zrinput::PinyinEngine engine;
  const std::filesystem::path dictionary(argv[1]);
  const auto loaded = engine.LoadDictionary(dictionary);
  Check(loaded && loaded.loaded >= 50'000 && loaded.skipped == 0,
        "packaged lexicon should load at least 50,000 valid rows");

  zrinput::LearningEvent request;
  request.timestamp = 1'700'000'000;
  std::vector<std::string> uncovered;
  for (const auto& syllable : zrinput::PinyinParser::StandardSyllables()) {
    request.input = syllable;
    if (engine.Query(request, 1).empty())
      uncovered.push_back(syllable);
  }
  if (!uncovered.empty()) {
    std::string message = "packaged lexicon lacks candidates for:";
    for (const auto& syllable : uncovered)
      message += " " + syllable;
    Check(false, message);
  }

  request.input = "shi";
  Check(engine.Query(request, 50).size() == 50,
        "a common syllable should fill multiple candidate pages");

  request.input = "woshizhongguoren";
  const auto sentence = engine.Query(request, 100);
  Check(std::any_of(sentence.begin(), sentence.end(), [](const auto& item) {
          return item.text == "我是中国人";
        }),
        "packaged words should compose into a full continuous sentence");

  request.input = "woshizho";
  const auto completion = engine.Query(request, 100);
  Check(std::any_of(completion.begin(), completion.end(), [](const auto& item) {
          return item.is_completion && item.text.starts_with("我是中");
        }),
        "continuous sentence composition should complete a partial tail");

  request.input = "jintianwomenyiqichifan";
  const auto daily_sentence = engine.Query(request, 10);
  Check(!daily_sentence.empty() &&
            daily_sentence.front().text == "今天我们一起吃饭",
        "trusted multi-syllable words should beat high-frequency homophones");

  request.input = "zhegeshurufahenhaoyong";
  const auto product_sentence = engine.Query(request, 10);
  Check(!product_sentence.empty() &&
            product_sentence.front().text == "这个输入法很好用",
        "low-confidence embedded words must not poison sentence ranking");

  request.input = "womenmingtianyiqiqugongyuan";
  auto park_sentence = engine.Query(request, 20);
  const auto preferred_park =
      std::find_if(park_sentence.begin(), park_sentence.end(),
                   [](const auto& item) {
                     return item.text == "我们明天一起去公园";
                   });
  Check(preferred_park != park_sentence.end(),
        "a contextually plausible sentence alternative should be available");
  if (preferred_park != park_sentence.end()) {
    auto learned = request;
    learned.text = preferred_park->text;
    engine.memory().Accept(learned);
    park_sentence = engine.Query(request, 20);
    Check(!park_sentence.empty() &&
              park_sentence.front().text == "我们明天一起去公园",
          "one correction should personalize a close full-sentence choice");
  }

  const std::vector<std::string> burst_inputs{
      "w", "wo", "wos", "wosh", "woshi", "woshiz", "woshizh",
      "woshizho", "woshizhon", "woshizhong", "woshizhongg",
      "woshizhonggu", "woshizhongguo", "woshizhongguor",
      "woshizhongguore", "woshizhongguoren"};
  std::chrono::microseconds maximum_query_latency{};
  std::string maximum_latency_input;
  for (int iteration = 0; iteration < 50; ++iteration) {
    for (const auto& input : burst_inputs) {
      request.input = input;
      const auto started = std::chrono::steady_clock::now();
      const auto candidates = engine.Query(request, 50);
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started);
      if (elapsed > maximum_query_latency) {
        maximum_query_latency = elapsed;
        maximum_latency_input = input;
      }
      Check(!candidates.empty(),
            "each rapid-input prefix should keep a usable candidate list");
    }
  }

  if (failures == 0) {
    std::cout << "ZRinput packaged lexicon integration tests passed; maximum "
              << "burst query latency " << maximum_query_latency.count()
              << " us for " << maximum_latency_input << ".\n";
  }
  return failures == 0 ? 0 : 1;
}
