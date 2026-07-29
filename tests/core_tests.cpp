#include "core/composition_state.h"
#include "core/pinyin_engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

int RunMemoryWriter(const std::filesystem::path& memory_path,
                    const std::filesystem::path& ready_path,
                    const std::filesystem::path& go_path,
                    const std::string& writer) {
  zrinput::PersonalLanguageModel model;
  model.Load(memory_path);
  {
    std::ofstream ready(ready_path, std::ios::binary | std::ios::trunc);
    if (!ready)
      return 2;
    ready << "ready";
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  while (!std::filesystem::exists(go_path)) {
    if (std::chrono::steady_clock::now() >= deadline)
      return 3;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  for (int index = 0; index < 500; ++index) {
    const std::string suffix = std::to_string(index);
    zrinput::LearningEvent event{writer + "-candidate-" + suffix,
                                 writer + "-input-" + suffix,
                                 writer,
                                 {},
                                 1'700'001'000 + index,
                                 false};
    model.Accept(event);
  }
  return model.Save(memory_path) ? 0 : 4;
}

#ifdef _WIN32
int LaunchMemoryWriterProcess(const std::filesystem::path& executable,
                              const std::filesystem::path& memory_path,
                              const std::filesystem::path& ready_path,
                              const std::filesystem::path& go_path,
                              const std::wstring& writer) {
  std::wstring command = L"\"" + executable.wstring() +
                         L"\" --memory-writer \"" + memory_path.wstring() +
                         L"\" \"" + ready_path.wstring() + L"\" \"" +
                         go_path.wstring() + L"\" \"" + writer + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return -1;
  }
  const DWORD wait = WaitForSingleObject(process.hProcess, 30'000);
  DWORD exit_code = static_cast<DWORD>(-1);
  if (wait == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}
#endif
}

int main(int argc, char** argv) {
  if (argc == 6 && std::string(argv[1]) == "--memory-writer") {
    return RunMemoryWriter(argv[2], argv[3], argv[4], argv[5]);
  }

  const std::string test_suffix = "-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
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

  const std::vector<std::string> coverage_syllables{
      "a",     "ang",   "beng", "chuang", "dia",   "diu",
      "fo",    "guang", "jiong", "kuang",  "lia",   "lve",
      "miu",   "nve",   "qiong", "rua",    "shuang", "te",
      "weng",  "xun",   "yo",    "zhei",   "zhuang"};
  for (const auto& syllable : coverage_syllables) {
    Check(!parser.Parse(syllable).empty(),
          "standard syllable inventory should not depend on dictionary data");
  }
  Check(!parser.Parse("zhuangkuang").empty(),
        "continuous standard syllables should parse without registration");
  std::string ambiguous_invalid;
  for (int repetition = 0; repetition < 22; ++repetition)
    ambiguous_invalid += "xian";
  ambiguous_invalid += 'q';
  const auto invalid_parse_started = std::chrono::steady_clock::now();
  const auto invalid_paths = parser.Parse(ambiguous_invalid, 8);
  const auto invalid_parse_elapsed = std::chrono::steady_clock::now() -
                                     invalid_parse_started;
  Check(invalid_paths.empty() &&
            invalid_parse_elapsed < std::chrono::milliseconds(500),
        "invalid tails should not trigger exponential ambiguous parsing");

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

  zrinput::PinyinEngine sentence_engine;
  sentence_engine.AddEntry("wo", "我", 10'000);
  sentence_engine.AddEntry("shi", "是", 10'000);
  sentence_engine.AddEntry("zhong guo", "中国", 9'800);
  sentence_engine.AddEntry("ren", "人", 9'700);
  zrinput::LearningEvent sentence_request;
  sentence_request.input = "woshizhongguoren";
  sentence_request.timestamp = 1'700'000'000;
  const auto sentence_candidates = sentence_engine.Query(sentence_request, 10);
  Check(!sentence_candidates.empty() &&
            sentence_candidates.front().text == "我是中国人",
        "separate dictionary words should compose into a continuous sentence");
  sentence_request.input = "woshizho";
  const auto sentence_completion = sentence_engine.Query(sentence_request, 10);
  Check(!sentence_completion.empty() &&
            sentence_completion.front().text == "我是中国" &&
            sentence_completion.front().is_completion,
        "an incomplete final syllable should complete after composed words");

  zrinput::PinyinEngine production_scores;
  production_scores.AddEntry("ce shi", "测试", 10'000);
  production_scores.AddEntry("ce shi", "侧视", 7'000);
  zrinput::LearningEvent preferred{"侧视", "ceshi", "editor", {},
                                  1'700'000'000, false};
  production_scores.memory().Accept(preferred);
  preferred.text.clear();
  const auto learned_production_ranking = production_scores.Query(preferred, 2);
  Check(!learned_production_ranking.empty() &&
            learned_production_ranking.front().text == "侧视",
        "personal memory should influence production-scale dictionary scores");

  zrinput::PinyinEngine deep_personalization;
  for (int index = 0; index < 200; ++index) {
    deep_personalization.AddEntry("yi", "候选" + std::to_string(index),
                                  10'000 - index * 5);
  }
  zrinput::LearningEvent rare_choice{"候选199", "yi", "editor", {},
                                     1'700'000'000, false};
  deep_personalization.memory().Accept(rare_choice);
  rare_choice.text.clear();
  const auto deep_ranking = deep_personalization.Query(rare_choice, 5);
  Check(!deep_ranking.empty() && deep_ranking.front().text == "候选199",
        "memory should recover a learned candidate beyond the composition beam");

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
                               ("zrinput-dictionary-test" + test_suffix +
                                ".tsv");
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
  Check(!zrinput::IsRuntimeDictionaryUsable(loaded_engine, load_result, 2),
        "a partial dictionary must not be treated as a usable TSF lexicon");
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
                           ("zrinput-personal-memory-test" + test_suffix +
                            ".dat");
  std::error_code stale_memory_error;
  std::filesystem::remove(memory_path, stale_memory_error);
  Check(contextual.Save(memory_path), "personal memory should save");
  zrinput::PersonalLanguageModel restored;
  Check(restored.Load(memory_path), "personal memory should load");
  const auto restored_prediction = restored.Predict(evening, 2);
  Check(restored_prediction == evening_prediction,
        "predictions should survive a save and load round trip");

  const auto merged_memory_path = std::filesystem::temp_directory_path() /
                                  ("zrinput-personal-memory-merge-test" +
                                   test_suffix + ".dat");
  std::error_code stale_merge_error;
  std::filesystem::remove(merged_memory_path, stale_merge_error);
  zrinput::PersonalLanguageModel first_process;
  zrinput::PersonalLanguageModel second_process;
  zrinput::LearningEvent first_process_choice{
      "第一个进程", "diyige", "editor", {}, 1'700'000'250, false};
  zrinput::LearningEvent second_process_choice{
      "第二个进程", "dierge", "browser", {}, 1'700'000'251, false};
  first_process.Accept(first_process_choice);
  second_process.Accept(second_process_choice);
  Check(first_process.Save(merged_memory_path) &&
            second_process.Save(merged_memory_path) &&
            first_process.Save(merged_memory_path),
        "interleaved process snapshots should merge without overwriting");
  zrinput::PersonalLanguageModel merged_memory;
  Check(merged_memory.Load(merged_memory_path),
        "merged process memory should remain readable");
  first_process_choice.text.clear();
  second_process_choice.text.clear();
  Check(merged_memory.Score("第一个进程", first_process_choice) > 0 &&
            merged_memory.Score("第二个进程", second_process_choice) > 0,
        "merged memory should retain learning from both host processes");
  std::error_code merge_cleanup_error;
  std::filesystem::remove(merged_memory_path, merge_cleanup_error);

#ifdef _WIN32
  std::wstring executable_buffer(32768, L'\0');
  const DWORD executable_length = GetModuleFileNameW(
      nullptr, executable_buffer.data(),
      static_cast<DWORD>(executable_buffer.size()));
  executable_buffer.resize(executable_length);
  const std::filesystem::path executable_path(executable_buffer);
  const auto concurrent_memory_path =
      std::filesystem::temp_directory_path() /
      ("zrinput-personal-memory-concurrent" + test_suffix + ".dat");
  const auto first_ready_path = concurrent_memory_path.string() + ".one.ready";
  const auto second_ready_path = concurrent_memory_path.string() + ".two.ready";
  const auto go_path = concurrent_memory_path.string() + ".go";
  std::error_code concurrent_cleanup_error;
  for (const auto& path : {concurrent_memory_path,
                           std::filesystem::path(first_ready_path),
                           std::filesystem::path(second_ready_path),
                           std::filesystem::path(go_path)}) {
    std::filesystem::remove(path, concurrent_cleanup_error);
  }
  const auto launch_writer = [&](const std::filesystem::path& ready_path,
                                 const std::wstring& writer) {
    return std::async(std::launch::async, [&, ready_path, writer] {
      return LaunchMemoryWriterProcess(
          executable_path, concurrent_memory_path, ready_path, go_path,
          writer);
    });
  };
  auto first_writer = launch_writer(first_ready_path, L"writer-one");
  auto second_writer = launch_writer(second_ready_path, L"writer-two");
  const auto writers_ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while ((!std::filesystem::exists(first_ready_path) ||
          !std::filesystem::exists(second_ready_path)) &&
         std::chrono::steady_clock::now() < writers_ready_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const bool writers_ready = std::filesystem::exists(first_ready_path) &&
                             std::filesystem::exists(second_ready_path);
  {
    std::ofstream go(go_path, std::ios::binary | std::ios::trunc);
    go << "go";
  }
  const int first_writer_result = first_writer.get();
  const int second_writer_result = second_writer.get();
  Check(writers_ready && first_writer_result == 0 &&
            second_writer_result == 0,
        "concurrent memory writer processes should both save successfully");
  zrinput::PersonalLanguageModel concurrent_memory;
  Check(concurrent_memory.Load(concurrent_memory_path),
        "concurrently merged memory should remain readable");
  zrinput::LearningEvent first_writer_request;
  first_writer_request.input = "writer-one-input-0";
  first_writer_request.timestamp = 1'700'002'000;
  zrinput::LearningEvent second_writer_request;
  second_writer_request.input = "writer-two-input-499";
  second_writer_request.timestamp = 1'700'002'000;
  Check(concurrent_memory.size() >= 2'000 &&
            concurrent_memory.Score("writer-one-candidate-0",
                                    first_writer_request) > 0 &&
            concurrent_memory.Score("writer-two-candidate-499",
                                    second_writer_request) > 0,
        "cross-process locking should retain both concurrent snapshots");
  for (const auto& path : {concurrent_memory_path,
                           std::filesystem::path(first_ready_path),
                           std::filesystem::path(second_ready_path),
                           std::filesystem::path(go_path)}) {
    std::filesystem::remove(path, concurrent_cleanup_error);
  }
#endif

  zrinput::PersonalLanguageModel application_memory;
  zrinput::LearningEvent editor_choice{"编辑器候选", "houxuan", "editor",
                                       {"当前"}, 1'700'000'300, false};
  zrinput::LearningEvent chat_choice{"聊天候选", "houxuan", "chat",
                                     {"当前"}, 1'700'000'300, false};
  application_memory.Accept(editor_choice);
  application_memory.Accept(editor_choice);
  application_memory.Accept(chat_choice);
  application_memory.Accept(chat_choice);
  application_memory.Accept(chat_choice);
  editor_choice.text.clear();
  const auto editor_prediction = application_memory.Predict(editor_choice, 2);
  Check(!editor_prediction.empty() &&
            editor_prediction.front() == "编辑器候选",
        "application-local context should beat a stronger global count");

  for (int iteration = 0; iteration < 25; ++iteration) {
    zrinput::LearningEvent durable{
        "持久候选" + std::to_string(iteration), "chijiu", "stress", {},
        1'700'000'400 + iteration, false};
    contextual.Accept(durable);
    Check(contextual.Save(memory_path),
          "repeated atomic memory replacement should succeed");
    zrinput::PersonalLanguageModel checkpoint;
    Check(checkpoint.Load(memory_path),
          "every atomic memory checkpoint should be readable");
    Check(checkpoint.size() == contextual.size(),
          "atomic checkpoints should not lose accepted events");
  }

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

  zrinput::CompositionState state;
  Check(!state.Backspace(), "backspace should be ignored while idle");
  Check(state.Append('W') && state.Append('o') && state.Append('\'') &&
            state.Append('n'),
        "letters and an explicit pinyin boundary should append");
  Check(state.input() == "wo'n" && state.Append('\'') &&
            !state.Append('\''),
        "input should normalize case and reject consecutive boundaries");
  Check(!state.SetInput("'invalid") && state.input() == "wo'n'",
        "invalid replacement must preserve the active input");
  state.CandidatesChanged();
  Check(!state.ChangePage(-1, 15, 7) && state.page() == 0,
        "previous page should stop at the first page");
  Check(state.ChangePage(1, 15, 7) && state.page() == 1,
        "next page should advance within bounds");
  Check(state.CandidateIndex(6, 15, 7) == 13,
        "candidate slot should map to the current page");
  Check(state.ChangePage(1, 15, 7) && state.page() == 2 &&
            state.CandidateIndex(0, 15, 7) == 14 &&
            !state.CandidateIndex(1, 15, 7).has_value(),
        "partial last pages should expose only valid candidate slots");
  Check(!state.ChangePage(1, 15, 7) && state.page() == 2,
        "next page should stop at the final page");
  state.Reset();
  const std::string burst = "woshizhongguoren";
  for (int iteration = 0; iteration < 10'000; ++iteration) {
    for (const char symbol : burst)
      Check(state.Append(symbol), "rapid input should retain every letter");
    for (int removed = 0; removed < 3; ++removed)
      Check(state.Backspace(), "rapid backspace should retain valid state");
    Check(state.input() == "woshizhongguo",
          "rapid input and backspace should remain deterministic");
    state.Reset();
  }
  for (std::size_t index = 0;
       index < zrinput::CompositionState::kMaxInputLength; ++index)
    Check(state.Append('a'), "input should accept data through its size limit");
  Check(!state.Append('a') &&
            state.input().size() == zrinput::CompositionState::kMaxInputLength,
        "input limit should not corrupt the composition state");

  if (failures == 0)
    std::cout << "All ZRinput core tests passed.\n";
  return failures == 0 ? 0 : 1;
}
