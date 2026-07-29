#include "core/pinyin_parser.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <queue>
#include <utility>

namespace zrinput::core {
namespace {

struct NormalizedInput {
  std::string text;
  std::vector<std::size_t> source_offsets;
  std::size_t stopped_at = 0;
};

bool IsAsciiLetter(char value) {
  return value >= 'a' && value <= 'z';
}

bool IsInitial(char value) {
  constexpr std::string_view initials = "bpmfdtnlgkhjqxrzcsyw";
  return initials.find(value) != std::string_view::npos;
}

NormalizedInput Normalize(std::u16string_view raw) {
  NormalizedInput result;
  result.text.reserve(raw.size());
  result.source_offsets.reserve(raw.size() + 1);
  result.source_offsets.push_back(0);
  std::size_t index = 0;
  while (index < raw.size()) {
    const char16_t value = raw[index];
    if ((value >= u'a' && value <= u'z') ||
        (value >= u'A' && value <= u'Z')) {
      char normalized = static_cast<char>(value);
      if (normalized >= 'A' && normalized <= 'Z') {
        normalized = static_cast<char>(normalized - 'A' + 'a');
      }
      result.text.push_back(normalized);
      ++index;
    } else if (value == u'\'' || value == u' ') {
      result.text.push_back('\'');
      ++index;
    } else if (value == u'v' || value == u'V' || value == u'\u00fc' ||
               value == u'\u00dc') {
      result.text.push_back('v');
      ++index;
    } else if ((value == u'u' || value == u'U') && index + 1 < raw.size() &&
               raw[index + 1] == u':') {
      result.text.push_back('v');
      index += 2;
    } else {
      break;
    }
    result.source_offsets.push_back(index);
  }
  result.stopped_at = index;
  return result;
}

struct WorkState {
  std::size_t offset = 0;
  std::vector<SyllableSpan> syllables;
  double cost = 0.0;
};

bool BetterPath(const ParsePath& left, const ParsePath& right) {
  if (left.consumed_units != right.consumed_units) {
    return left.consumed_units > right.consumed_units;
  }
  if (left.cost != right.cost) {
    return left.cost < right.cost;
  }
  return left.syllables.size() < right.syllables.size();
}

}  // namespace

PinyinParser::PinyinParser(PinyinParserOptions options) : options_(options) {
  if (options_.max_paths == 0) {
    options_.max_paths = 1;
  }
  if (options_.max_syllable_units == 0) {
    options_.max_syllable_units = 1;
  }
}

void PinyinParser::ReplaceSyllables(std::vector<std::string> syllables) {
  syllables_.clear();
  prefixes_.clear();
  for (auto& syllable : syllables) {
    const bool inserted = AddSyllable(std::move(syllable));
    static_cast<void>(inserted);
  }
}

bool PinyinParser::AddSyllable(std::string syllable) {
  std::transform(syllable.begin(), syllable.end(), syllable.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  if (syllable.empty() || syllable.size() > options_.max_syllable_units ||
      !std::all_of(syllable.begin(), syllable.end(), IsAsciiLetter)) {
    return false;
  }
  if (!syllables_.insert(syllable).second) {
    return false;
  }
  for (std::size_t size = 1; size < syllable.size(); ++size) {
    prefixes_.insert(syllable.substr(0, size));
  }
  return true;
}

PinyinAnalysis PinyinParser::Analyze(std::u16string_view raw) const {
  PinyinAnalysis analysis;
  analysis.raw.assign(raw);
  if (raw.empty()) {
    return analysis;
  }

  const NormalizedInput normalized = Normalize(raw);
  std::vector<WorkState> frontier(1);
  std::vector<ParsePath> terminal;
  std::vector<double> best_cost(normalized.text.size() + 1,
                                std::numeric_limits<double>::infinity());
  best_cost[0] = 0.0;
  std::size_t cursor = 0;

  while (cursor < frontier.size()) {
    WorkState state = std::move(frontier[cursor++]);
    if (state.offset >= normalized.text.size()) {
      ParsePath path;
      path.syllables = std::move(state.syllables);
      path.consumed_units = normalized.stopped_at;
      path.cost = state.cost;
      terminal.push_back(std::move(path));
      continue;
    }

    if (normalized.text[state.offset] == '\'') {
      WorkState next = state;
      ++next.offset;
      next.cost += options_.separator_cost;
      if (next.cost <= best_cost[next.offset] + 2.0) {
        best_cost[next.offset] = std::min(best_cost[next.offset], next.cost);
        frontier.push_back(std::move(next));
      }
      continue;
    }

    bool extended = false;
    bool exact_extended = false;
    const std::size_t remaining = normalized.text.size() - state.offset;
    const std::size_t maximum =
        std::min(options_.max_syllable_units, remaining);
    for (std::size_t length = 1; length <= maximum; ++length) {
      const std::string_view candidate(normalized.text.data() + state.offset,
                                       length);
      if (!IsSyllable(candidate)) {
        continue;
      }
      WorkState next = state;
      SyllableSpan span;
      span.normalized.assign(candidate);
      span.source_begin = normalized.source_offsets[state.offset];
      span.source_end = normalized.source_offsets[state.offset + length];
      next.syllables.push_back(std::move(span));
      next.offset += length;
      if (next.cost <= best_cost[next.offset] + 2.0) {
        best_cost[next.offset] = std::min(best_cost[next.offset], next.cost);
        frontier.push_back(std::move(next));
      }
      extended = true;
      exact_extended = true;
    }

    if (options_.allow_abbreviations &&
        IsInitial(normalized.text[state.offset])) {
      WorkState next = state;
      SyllableSpan span;
      span.normalized.assign(1, normalized.text[state.offset]);
      span.source_begin = normalized.source_offsets[state.offset];
      span.source_end = normalized.source_offsets[state.offset + 1];
      span.complete = false;
      span.abbreviated = true;
      span.correction_cost = options_.abbreviation_cost;
      next.syllables.push_back(std::move(span));
      ++next.offset;
      next.cost += options_.abbreviation_cost;
      if (next.cost <= best_cost[next.offset] + 2.0) {
        best_cost[next.offset] = std::min(best_cost[next.offset], next.cost);
        frontier.push_back(std::move(next));
      }
      extended = true;
    }

    if (state.offset + maximum == normalized.text.size()) {
      for (std::size_t length = maximum; length > 0; --length) {
        const std::string_view candidate(
            normalized.text.data() + state.offset, length);
        if (!IsSyllablePrefix(candidate)) {
          continue;
        }
        WorkState next = state;
        SyllableSpan span;
        span.normalized.assign(candidate);
        span.source_begin = normalized.source_offsets[state.offset];
        span.source_end = normalized.source_offsets[state.offset + length];
        span.complete = false;
        span.correction_cost = options_.incomplete_cost;
        next.syllables.push_back(std::move(span));
        next.offset += length;
        next.cost += options_.incomplete_cost;
        frontier.push_back(std::move(next));
        extended = true;
        break;
      }
    }

    if (!exact_extended && options_.allow_transposition_correction) {
      for (std::size_t length = 2; length <= maximum; ++length) {
        std::string corrected = normalized.text.substr(state.offset, length);
        std::swap(corrected[length - 2], corrected[length - 1]);
        if (!IsSyllable(corrected)) {
          continue;
        }
        WorkState next = state;
        SyllableSpan span;
        span.normalized = std::move(corrected);
        span.source_begin = normalized.source_offsets[state.offset];
        span.source_end = normalized.source_offsets[state.offset + length];
        span.correction_cost = options_.correction_cost;
        next.syllables.push_back(std::move(span));
        next.offset += length;
        next.cost += options_.correction_cost;
        frontier.push_back(std::move(next));
        extended = true;
      }
    }

    if (!exact_extended && options_.allow_duplicate_letter_correction) {
      for (std::size_t length = 2; length <= maximum; ++length) {
        const std::string original = normalized.text.substr(state.offset, length);
        for (std::size_t duplicate = 1; duplicate < original.size(); ++duplicate) {
          if (original[duplicate] != original[duplicate - 1]) {
            continue;
          }
          std::string corrected = original;
          corrected.erase(duplicate, 1);
          if (!IsSyllable(corrected)) {
            continue;
          }
          WorkState next = state;
          SyllableSpan span;
          span.normalized = std::move(corrected);
          span.source_begin = normalized.source_offsets[state.offset];
          span.source_end = normalized.source_offsets[state.offset + length];
          span.correction_cost = options_.correction_cost;
          next.syllables.push_back(std::move(span));
          next.offset += length;
          next.cost += options_.correction_cost;
          frontier.push_back(std::move(next));
          extended = true;
          break;
        }
      }
    }

    if (!extended && !state.syllables.empty()) {
      ParsePath path;
      path.syllables = std::move(state.syllables);
      path.consumed_units = normalized.source_offsets[state.offset];
      path.cost = state.cost;
      terminal.push_back(std::move(path));
    }

    if (frontier.size() > options_.max_paths *
                              (normalized.text.size() + std::size_t{1}) * 4) {
      break;
    }
  }

  std::sort(terminal.begin(), terminal.end(), BetterPath);
  terminal.erase(
      std::unique(terminal.begin(), terminal.end(),
                  [](const ParsePath& left, const ParsePath& right) {
                    if (left.consumed_units != right.consumed_units ||
                        left.syllables.size() != right.syllables.size()) {
                      return false;
                    }
                    for (std::size_t index = 0; index < left.syllables.size();
                         ++index) {
                      if (left.syllables[index].normalized !=
                              right.syllables[index].normalized ||
                          left.syllables[index].source_end !=
                              right.syllables[index].source_end) {
                        return false;
                      }
                    }
                    return true;
                  }),
      terminal.end());
  if (terminal.size() > options_.max_paths) {
    terminal.resize(options_.max_paths);
  }

  analysis.paths = std::move(terminal);
  for (const auto& path : analysis.paths) {
    analysis.legal_prefix_units =
        std::max(analysis.legal_prefix_units, path.consumed_units);
  }
  if (analysis.legal_prefix_units < raw.size()) {
    analysis.unparsed_tail = raw.substr(analysis.legal_prefix_units);
  }
  return analysis;
}

std::optional<std::size_t> PinyinParser::FindStableSplit(
    const PinyinAnalysis& analysis,
    std::size_t preferred_units,
    std::size_t minimum_tail_units) const {
  std::optional<std::size_t> best;
  std::size_t best_distance = std::numeric_limits<std::size_t>::max();
  for (const auto& path : analysis.paths) {
    for (const auto& syllable : path.syllables) {
      if (!syllable.complete || syllable.abbreviated ||
          syllable.source_end == 0 ||
          syllable.source_end + minimum_tail_units > analysis.raw.size()) {
        continue;
      }
      const std::size_t distance = syllable.source_end > preferred_units
                                       ? syllable.source_end - preferred_units
                                       : preferred_units - syllable.source_end;
      if (!best || distance < best_distance ||
          (distance == best_distance && syllable.source_end > *best)) {
        best = syllable.source_end;
        best_distance = distance;
      }
    }
  }
  return best;
}

bool PinyinParser::IsSyllable(std::string_view value) const {
  return syllables_.find(std::string(value)) != syllables_.end();
}

bool PinyinParser::IsSyllablePrefix(std::string_view value) const {
  return prefixes_.find(std::string(value)) != prefixes_.end();
}

}  // namespace zrinput::core
