#include "core/pinyin_engine.h"

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

  if (failures == 0)
    std::cout << "All ZRinput core tests passed.\n";
  return failures == 0 ? 0 : 1;
}
