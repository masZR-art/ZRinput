#include "core/pinyin_engine.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}
}

int main() {
  zrinput::PinyinEngine engine;
  engine.AddEntry("xianzai", "现在", 1.0);
  engine.AddEntry("xianzai", "先在", 1.1);

  zrinput::LearningEvent learned{"现在", "xianzai", "editor", {"我们"},
                                 1'700'000'000, false};
  engine.memory().Accept(learned);
  engine.memory().Accept(learned);

  auto request = learned;
  request.text.clear();
  const auto ranked = engine.Query(request, 5);
  Check(!ranked.empty() && ranked.front().text == "现在",
        "personal context should override a small dictionary difference");

  engine.memory().Reject(learned);
  engine.memory().Reject(learned);
  const auto corrected = engine.Query(request, 5);
  Check(!corrected.empty() && corrected.front().text == "先在",
        "negative feedback should correct the learned preference");

  auto private_event = learned;
  private_event.text = "秘密";
  private_event.private_mode = true;
  const auto before = engine.memory().size();
  engine.memory().Accept(private_event);
  Check(engine.memory().size() == before,
        "private mode must not alter personal memory");

  zrinput::PersonalLanguageModel contextual;
  zrinput::LearningEvent hotpot{"火锅", "huoguo", "chat",
                                {"今天", "晚上", "吃"}, 1'700'000'100,
                                false};
  zrinput::LearningEvent rice{"米饭", "mifan", "chat", {"中午", "吃"},
                              1'700'000'100, false};
  for (int i = 0; i < 3; ++i)
    contextual.Accept(hotpot);
  for (int i = 0; i < 4; ++i)
    contextual.Accept(rice);
  auto evening = hotpot;
  evening.text.clear();
  const auto evening_prediction = contextual.Predict(evening, 2);
  Check(!evening_prediction.empty() && evening_prediction.front() == "火锅",
        "longer ordered context should beat a more frequent one-word match");

  zrinput::LearningEvent after_boundary{
      "天气", "tianqi", "chat", {"旧话题", "。", "今天"},
      1'700'000'200, false};
  contextual.Accept(after_boundary);
  after_boundary.text.clear();
  after_boundary.context = {"完全不同", "。", "今天"};
  const auto boundary_prediction = contextual.Predict(after_boundary, 3);
  Check(!boundary_prediction.empty() && boundary_prediction.front() == "天气",
        "sentence boundary should discard context from the previous sentence");

  const auto memory_path = std::filesystem::temp_directory_path() /
                           "zrinput-personal-memory-test.dat";
  Check(contextual.Save(memory_path), "personal memory should save");
  zrinput::PersonalLanguageModel restored;
  Check(restored.Load(memory_path), "personal memory should load");
  const auto restored_prediction = restored.Predict(evening, 2);
  Check(restored_prediction == evening_prediction,
        "predictions should survive a save and load round trip");

  {
    std::ofstream corrupt(memory_path, std::ios::binary | std::ios::app);
    corrupt << "corruption";
  }
  const auto entries_before_failed_load = restored.size();
  Check(!restored.Load(memory_path), "corrupted memory must be rejected");
  Check(restored.size() == entries_before_failed_load,
        "a failed load must not replace active memory");
  std::error_code cleanup_error;
  std::filesystem::remove(memory_path, cleanup_error);

  if (failures == 0)
    std::cout << "All ZRinput core tests passed.\n";
  return failures == 0 ? 0 : 1;
}
