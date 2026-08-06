#include "common/utf.h"
#include "core/composition_buffer.h"
#include "core/pinyin_parser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using zrinput::core::CompositionBuffer;
using zrinput::core::CursorMove;
using zrinput::core::EditOutcome;
using zrinput::core::PinyinParser;

class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed ? seed : 1) {}

  std::uint64_t Next() {
    state_ ^= state_ >> 12u;
    state_ ^= state_ << 25u;
    state_ ^= state_ >> 27u;
    return state_ * 0x2545F4914F6CDD1Dull;
  }

  std::size_t Bounded(std::size_t limit) {
    return limit == 0 ? 0 : static_cast<std::size_t>(Next() % limit);
  }

 private:
  std::uint64_t state_;
};

bool ValidUtf16(std::u16string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (zrinput::utf::IsHighSurrogate(text[index])) {
      if (index + 1 >= text.size() ||
          !zrinput::utf::IsLowSurrogate(text[index + 1])) {
        return false;
      }
      ++index;
    } else if (zrinput::utf::IsLowSurrogate(text[index])) {
      return false;
    }
  }
  return true;
}

PinyinParser MakeParser() {
  PinyinParser parser;
  parser.ReplaceSyllables({
      "a", "ai", "an", "ang", "ao", "ba", "bei", "chi", "de",
      "fa", "guo", "hao", "he", "jie", "jin", "ma", "men", "ni",
      "ren", "shi", "shu", "tian", "wan", "wo", "xian", "yin",
      "zai", "zhong"});
  return parser;
}

struct RunResult {
  bool passed = true;
  std::string failure;
  std::size_t applied = 0;
  std::size_t rejected = 0;
  std::size_t splits = 0;
  std::u16string committed;
};

RunResult RunSeed(std::uint64_t seed, std::size_t event_count) {
  Random random(seed);
  CompositionBuffer buffer;
  PinyinParser parser = MakeParser();
  RunResult result;
  std::uint64_t previous_version = buffer.version();
  for (std::size_t event = 0; event < event_count; ++event) {
    const std::size_t operation = random.Bounded(1000);
    if (operation < 650) {
      const char16_t value = static_cast<char16_t>(
          u'a' + static_cast<char16_t>(random.Bounded(26)));
      EditOutcome outcome = buffer.Insert(std::u16string_view(&value, 1));
      if (outcome == EditOutcome::kNeedsStablePrefixCommit) {
        const auto analysis = parser.Analyze(buffer.text());
        const auto split = parser.FindStableSplit(
            analysis, buffer.limits().active_units / 2,
            buffer.limits().active_units / 4);
        if (split) {
          const auto prefix = buffer.CommitPrefix(*split);
          if (!prefix) {
            result.passed = false;
            result.failure = "parser selected an invalid UTF-16 split";
            return result;
          }
          result.committed += *prefix;
          ++result.splits;
          outcome = buffer.Insert(std::u16string_view(&value, 1));
        }
      }
      if (outcome == EditOutcome::kApplied) {
        ++result.applied;
      } else {
        ++result.rejected;
      }
    } else if (operation < 700) {
      const char16_t separator = u'\'';
      const auto outcome =
          buffer.Insert(std::u16string_view(&separator, 1));
      if (outcome == EditOutcome::kApplied) {
        ++result.applied;
      } else {
        ++result.rejected;
      }
    } else if (operation < 750) {
      static_cast<void>(buffer.EraseBackward(random.Bounded(5) == 0));
    } else if (operation < 790) {
      static_cast<void>(buffer.EraseForward(random.Bounded(5) == 0));
    } else if (operation < 840) {
      static_cast<void>(buffer.Move(CursorMove::kPreviousCodePoint,
                                    random.Bounded(4) == 0));
    } else if (operation < 890) {
      static_cast<void>(buffer.Move(CursorMove::kNextCodePoint,
                                    random.Bounded(4) == 0));
    } else if (operation < 920) {
      static_cast<void>(buffer.Move(CursorMove::kHome,
                                    random.Bounded(4) == 0));
    } else if (operation < 950) {
      static_cast<void>(buffer.Move(CursorMove::kEnd,
                                    random.Bounded(4) == 0));
    } else if (operation < 955 && !buffer.empty()) {
      const std::size_t first = random.Bounded(buffer.text().size() + 1);
      const std::size_t second = random.Bounded(buffer.text().size() + 1);
      static_cast<void>(buffer.SetSelection({first, second}));
    } else if (operation == 955) {
      result.committed += buffer.text();
      buffer.Clear();
    } else if (operation == 956) {
      buffer.Clear();
    } else {
      // Space, paging, selection, focus and mode events do not mutate the raw
      // portable buffer; TSF integration tests cover their external effects.
    }

    const auto selection = buffer.selection();
    if (selection.anchor > buffer.text().size() ||
        selection.caret > buffer.text().size() ||
        buffer.text().size() > buffer.limits().hard_units ||
        !ValidUtf16(buffer.text()) || buffer.version() < previous_version) {
      result.passed = false;
      result.failure = "composition invariant failed at event " +
                       std::to_string(event);
      return result;
    }
    previous_version = buffer.version();
  }
  return result;
}

bool DeterministicLimits() {
  CompositionBuffer random_letters;
  Random random(0x256u);
  for (std::size_t index = 0; index < 256; ++index) {
    const char16_t value = static_cast<char16_t>(
        u'a' + static_cast<char16_t>(random.Bounded(26)));
    if (random_letters.Insert(std::u16string_view(&value, 1)) !=
        EditOutcome::kApplied) {
      return false;
    }
  }

  CompositionBuffer separators;
  for (std::size_t index = 0; index < 256; ++index) {
    const char16_t value = u'\'';
    if (separators.Insert(std::u16string_view(&value, 1)) !=
        EditOutcome::kApplied) {
      return false;
    }
  }
  for (std::size_t index = 0; index < 256; ++index) {
    if (separators.EraseBackward() != EditOutcome::kApplied) {
      return false;
    }
  }
  if (random_letters.text().size() != 256 || !separators.empty()) {
    return false;
  }

  CompositionBuffer splittable;
  std::u16string valid;
  for (std::size_t index = 0; index < 51; ++index) {
    valid += u"nihao";
  }
  valid += u"a";
  if (valid.size() != 256 ||
      splittable.Insert(valid) != EditOutcome::kApplied) {
    return false;
  }
  const char16_t extra = u'b';
  if (splittable.Insert(std::u16string_view(&extra, 1)) !=
      EditOutcome::kNeedsStablePrefixCommit) {
    return false;
  }
  PinyinParser parser = MakeParser();
  const auto split = parser.FindStableSplit(
      parser.Analyze(splittable.text()), 128, 64);
  if (!split || !splittable.CommitPrefix(*split) ||
      splittable.Insert(std::u16string_view(&extra, 1)) !=
          EditOutcome::kApplied) {
    return false;
  }
  return splittable.text().size() < 256;
}

}  // namespace

int main() {
  if (!DeterministicLimits()) {
    std::cerr << "deterministic 256-unit limit case failed\n";
    return 1;
  }
  constexpr std::array<std::uint64_t, 12> seeds = {
      1, 2, 3, 0xC0FFEEu, 0x5EEDu, 0xDEADBEEFu,
      0x123456789u, 0xABCDEFu, 20260807u, 50u, 100u, 1024u};
  std::size_t total_applied = 0;
  std::size_t total_rejected = 0;
  std::size_t total_splits = 0;
  for (const std::uint64_t seed : seeds) {
    const RunResult result = RunSeed(seed, 10'000);
    if (!result.passed) {
      std::cerr << "seed=" << seed << ' ' << result.failure << '\n';
      return 2;
    }
    total_applied += result.applied;
    total_rejected += result.rejected;
    total_splits += result.splits;
  }
  if (total_rejected == 0) {
    std::cerr << "random workload never exercised limit rejection\n";
    return 3;
  }
  std::cout << "seeds=" << seeds.size() << " events_per_seed=10000"
            << " applied=" << total_applied
            << " rejected=" << total_rejected
            << " stable_splits=" << total_splits << '\n';
  return 0;
}
