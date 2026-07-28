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
  zrinput::PinyinParser parser;
  parser.RegisterSyllable("xian");
  parser.RegisterSyllable("xi");
  parser.RegisterSyllable("an");
  const auto ambiguous_paths = parser.Parse("xian");
  Check(ambiguous_paths.size() == 2,
        "continuous pinyin should preserve ambiguous syllable paths");
  const auto separated_paths = parser.Parse("xi'an");
  Check(separated_paths.size() == 1 && separated_paths.front().size() == 2,
        "explicit boundaries should remove segmentation ambiguity");

  zrinput::PinyinEngine engine;
  engine.AddEntry("xian zai", "现在", 1.0);
  engine.AddEntry("xian zai", "先在", 1.1);
  engine.AddEntry("xi an", "西安", 0.9);

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

  auto forced_boundary = request;
  forced_boundary.input = "xi'an";
  const auto xian = engine.Query(forced_boundary, 5);
  Check(!xian.empty() && xian.front().text == "西安",
        "apostrophe should force a syllable boundary");

  forced_boundary.input = "xi1 an1";
  const auto toned_xian = engine.Query(forced_boundary, 5);
  Check(!toned_xian.empty() && toned_xian.front().text == "西安",
        "tone digits should not change dictionary lookup");

  engine.AddEntry("lv se", "绿色", 1.0);
  forced_boundary.input = "lü4se4";
  const auto green = engine.Query(forced_boundary, 5);
  Check(!green.empty() && green.front().text == "绿色",
        "umlaut u should normalize to v");

  const auto dictionary_path = std::filesystem::temp_directory_path() /
                               "zrinput-dictionary-test.tsv";
  {
    std::ofstream dictionary(dictionary_path, std::ios::binary);
    dictionary << "# pinyin\\ttext\\tfrequency\n"
               << "xian zai\t现在\t12.5\n"
               << "xian zhuang\t现状\t11\n"
               << "invalid row\n";
  }
  zrinput::PinyinEngine loaded_engine;
  const auto load_result = loaded_engine.LoadDictionary(dictionary_path);
  Check(load_result && load_result.loaded == 2 && load_result.skipped == 1,
        "dictionary loader should report accepted and malformed rows");
  auto prefix_request = request;
  prefix_request.input = "xianz";
  const auto prefix_candidates = loaded_engine.Query(prefix_request, 5);
  Check(prefix_candidates.size() == 2 &&
            prefix_candidates.front().is_completion,
        "incomplete pinyin should produce marked prefix candidates");
  std::error_code dictionary_cleanup_error;
  std::filesystem::remove(dictionary_path, dictionary_cleanup_error);

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
