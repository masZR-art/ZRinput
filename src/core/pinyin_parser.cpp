#include "core/pinyin_parser.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string_view>

namespace zrinput {
namespace {

bool IsBoundary(char ch) {
  return ch == '\'' || ch == ' ' || ch == '\t' || ch == '-';
}

bool IsStandaloneOnly(const std::string& syllable) {
  return syllable == "m" || syllable == "n" || syllable == "ng" ||
         syllable == "hm" || syllable == "hng";
}

std::vector<std::string> SplitBoundaries(const std::string& normalized) {
  std::vector<std::string> chunks;
  std::size_t start = 0;
  while (start < normalized.size()) {
    const auto end = normalized.find('\'', start);
    const auto chunk = normalized.substr(start, end - start);
    if (!chunk.empty())
      chunks.push_back(chunk);
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return chunks;
}

}  // namespace

PinyinParser::PinyinParser() {
  for (const auto& syllable : StandardSyllables())
    RegisterSyllable(syllable);
}

const std::vector<std::string>& PinyinParser::StandardSyllables() {
  // Keep parsing independent from the installed lexicon. A small dictionary
  // must still understand every standard spelling so it can compose words
  // added by another dictionary or learned later.
  static const std::vector<std::string> syllables = [] {
    constexpr std::string_view kStandardSyllables =
      "a ai an ang ao "
      "ba bai ban bang bao bei ben beng bi bian biang biao bie bin bing bo "
      "bong bu "
      "ca cai can cang cao ce cei cen ceng cha chai chan chang chao che chen "
      "cheng chi chong chou chu chua chuai chuan chuang chui chun chuo ci "
      "cong cou cu cuan cui cun cuo "
      "da dai dan dang dao de dei den deng di dia dian diao die din ding diu "
      "dong dou du duan dui dun duo "
      "e ei en eng er "
      "fa fan fang fei fen feng fiao fo fou fu "
      "ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan "
      "guang gui gun guo "
      "ha hai han hang hao he hei hen heng hm hng hong hou hu hua huai huan "
      "huang hui hun huo "
      "ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun "
      "ka kai kan kang kao ke kei ken keng kong kou ku kua kuai kuan kuang "
      "kui kun kuo "
      "la lai lan lang lao le lei len leng li lia lian liang liao lie lin ling "
      "liu lo long lou lu luan lun luo lv lve "
      "m ma mai man mang mao me mei men meng mi mian miao mie min ming miu "
      "mo mou mu "
      "n na nai nan nang nao ne nei nen neng ng ni nia nian niang niao nie "
      "nin ning niu nong nou nu nuan nun nuo nv nve "
      "o ou "
      "pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu "
      "qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun "
      "ran rang rao re ren reng ri rong rou ru rua ruan rui run ruo "
      "sa sai san sang sao se sen seng sha shai shan shang shao she shei "
      "shen sheng shi shou shu shua shuai shuan shuang shui shun shuo si "
      "song sou su suan sui sun suo "
      "ta tai tan tang tao te tei teng ti tian tiao tie ting tong tou tu tuan "
      "tui tun tuo "
      "wa wai wan wang wei wen weng wo wong wu "
      "xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun "
      "ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun "
      "za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe "
      "zhei zhen zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui "
      "zhun zhuo zi zong zou zu zuan zui zun zuo";
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < kStandardSyllables.size()) {
      const auto end = kStandardSyllables.find(' ', start);
      result.emplace_back(kStandardSyllables.substr(start, end - start));
      if (end == std::string_view::npos)
        break;
      start = end + 1;
    }
    return result;
  }();
  return syllables;
}

bool PinyinParser::RegisterSyllable(std::string syllable) {
  syllable = Normalize(syllable, false);
  if (syllable.empty() ||
      !std::all_of(syllable.begin(), syllable.end(), [](unsigned char ch) {
        return ch >= 'a' && ch <= 'z';
      }))
    return false;
  return syllables_.insert(std::move(syllable)).second;
}

void PinyinParser::Clear() {
  syllables_.clear();
}

std::vector<SyllablePath> PinyinParser::Parse(const std::string& input,
                                              std::size_t max_paths) const {
  if (max_paths == 0)
    return {};
  const auto chunks = SplitBoundaries(Normalize(input, true));
  if (chunks.empty())
    return {};

  std::vector<SyllablePath> combined(1);
  for (const auto& chunk : chunks) {
    const auto paths = ParseChunk(chunk, max_paths);
    if (paths.empty())
      return {};
    std::vector<SyllablePath> next;
    for (const auto& prefix : combined) {
      for (const auto& suffix : paths) {
        auto path = prefix;
        path.insert(path.end(), suffix.begin(), suffix.end());
        next.push_back(std::move(path));
        if (next.size() >= max_paths)
          break;
      }
      if (next.size() >= max_paths)
        break;
    }
    combined = std::move(next);
  }
  std::stable_sort(combined.begin(), combined.end(), [](const auto& left,
                                                        const auto& right) {
    if (left.size() != right.size())
      return left.size() < right.size();
    return left < right;
  });
  return combined;
}

std::string PinyinParser::Normalize(const std::string& input,
                                    bool keep_boundaries) {
  std::string result;
  bool pending_boundary = false;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const unsigned char ch = input[i];
    if (i + 1 < input.size() && ch == 0xc3 &&
        static_cast<unsigned char>(input[i + 1]) == 0xbc) {
      if (pending_boundary && keep_boundaries && !result.empty())
        result.push_back('\'');
      result.push_back('v');
      pending_boundary = false;
      ++i;
      continue;
    }
    if ((ch == 'u' || ch == 'U') && i + 1 < input.size() &&
        input[i + 1] == ':') {
      if (pending_boundary && keep_boundaries && !result.empty())
        result.push_back('\'');
      result.push_back('v');
      pending_boundary = false;
      ++i;
      continue;
    }
    if (IsBoundary(static_cast<char>(ch))) {
      pending_boundary = true;
      continue;
    }
    if (ch >= '1' && ch <= '5')
      continue;
    if (!std::isalpha(ch))
      continue;
    if (pending_boundary && keep_boundaries && !result.empty())
      result.push_back('\'');
    result.push_back(static_cast<char>(std::tolower(ch)));
    pending_boundary = false;
  }
  return result;
}

std::string PinyinParser::Key(const SyllablePath& syllables) {
  std::string key;
  for (const auto& syllable : syllables) {
    if (!key.empty())
      key.push_back('\'');
    key += syllable;
  }
  return key;
}

SyllablePath PinyinParser::CanonicalSyllables(const std::string& input) {
  return SplitBoundaries(Normalize(input, true));
}

std::vector<SyllablePath> PinyinParser::ParseChunk(
    const std::string& chunk,
    std::size_t max_paths) const {
  std::vector<bool> can_finish(chunk.size() + 1, false);
  can_finish.back() = true;
  for (std::size_t offset = chunk.size(); offset-- > 0;) {
    for (std::size_t end = chunk.size(); end > offset; --end) {
      if (!can_finish[end])
        continue;
      const auto syllable = chunk.substr(offset, end - offset);
      if (syllables_.find(syllable) == syllables_.end())
        continue;
      if (IsStandaloneOnly(syllable) &&
          (offset != 0 || end != chunk.size()))
        continue;
      can_finish[offset] = true;
      break;
    }
  }
  if (!can_finish.front())
    return {};

  std::vector<SyllablePath> result;
  SyllablePath current;
  std::function<void(std::size_t)> visit = [&](std::size_t offset) {
    if (result.size() >= max_paths)
      return;
    if (offset == chunk.size()) {
      result.push_back(current);
      return;
    }
    for (std::size_t end = chunk.size(); end > offset; --end) {
      if (!can_finish[end])
        continue;
      const auto syllable = chunk.substr(offset, end - offset);
      if (syllables_.find(syllable) == syllables_.end())
        continue;
      if (IsStandaloneOnly(syllable) &&
          (offset != 0 || end != chunk.size()))
        continue;
      current.push_back(syllable);
      visit(end);
      current.pop_back();
    }
  };
  visit(0);
  return result;
}

}  // namespace zrinput
