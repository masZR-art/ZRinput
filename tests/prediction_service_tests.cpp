#include "core/prediction_service.h"

#include "test_harness.h"

#include <string>
#include <vector>

namespace {

using zrinput::core::PredictionEntry;
using zrinput::core::PredictionService;

ZR_TEST(PredictionUsesLongestAvailableContextThenBacksOff) {
  PredictionService service;
  service.ReplaceSystemEntries({
      {{"today"}, "weather", 100.0, {}},
      {{"the", "weather"}, "is", 1000.0, {}},
      {{"weather"}, "forecast", 50.0, {}},
  });
  const std::vector<std::string> context = {"the", "weather"};
  const auto predictions = service.Predict(context, {}, 1000, 8);
  ZR_EXPECT_TRUE(!predictions.empty());
  ZR_EXPECT_EQ(predictions.front().text, std::string("is"));
  ZR_EXPECT_TRUE(predictions.front().context_term > 0.0);
}

ZR_TEST(PredictionLearnsRecentSessionTransition) {
  PredictionService service;
  const std::vector<std::string> context = {"input"};
  ZR_EXPECT_TRUE(service.LearnSession(context, "method", "editor.exe", 1000));
  ZR_EXPECT_TRUE(service.LearnSession(context, "method", "editor.exe", 1001));
  const auto predictions =
      service.Predict(context, "editor.exe", 1001, 5);
  ZR_EXPECT_EQ(predictions.size(), std::size_t{1});
  ZR_EXPECT_EQ(predictions.front().text, std::string("method"));
  ZR_EXPECT_TRUE(predictions.front().session_term > 0.0);
  ZR_EXPECT_TRUE(predictions.front().recency_term > 0.99);
  ZR_EXPECT_TRUE(predictions.front().application_term > 0.0);
}

ZR_TEST(PredictionSessionRecencyDecays) {
  zrinput::core::PredictionWeights weights;
  weights.half_life_seconds = 100.0;
  PredictionService service(weights);
  const std::vector<std::string> context = {"a"};
  ZR_EXPECT_TRUE(service.LearnSession(context, "b", {}, 1000));
  const auto fresh = service.Predict(context, {}, 1000, 1);
  const auto old = service.Predict(context, {}, 1100, 1);
  ZR_EXPECT_TRUE(fresh.front().recency_term > 0.999);
  ZR_EXPECT_TRUE(old.front().recency_term > 0.499);
  ZR_EXPECT_TRUE(old.front().recency_term < 0.501);
}

ZR_TEST(PredictionClearSessionDoesNotRemoveSystemModel) {
  PredictionService service;
  service.ReplaceSystemEntries({{{"a"}, "system", 10.0, {}}});
  const std::vector<std::string> context = {"a"};
  ZR_EXPECT_TRUE(service.LearnSession(context, "session", {}, 100));
  service.ClearSession();
  const auto predictions = service.Predict(context, {}, 100, 10);
  ZR_EXPECT_EQ(predictions.size(), std::size_t{1});
  ZR_EXPECT_EQ(predictions.front().text, std::string("system"));
}

ZR_TEST(PredictionRejectsEmptyLearningAndBoundsOutput) {
  PredictionService service;
  const std::vector<std::string> empty;
  ZR_EXPECT_TRUE(!service.LearnSession(empty, "value", {}, 1));
  ZR_EXPECT_TRUE(service.Predict(empty, {}, 1, 5).empty());
  service.ReplaceSystemEntries({
      {{"a"}, "one", 3.0, {}},
      {{"a"}, "two", 2.0, {}},
      {{"a"}, "three", 1.0, {}},
  });
  const std::vector<std::string> context = {"a"};
  ZR_EXPECT_EQ(service.Predict(context, {}, 1, 2).size(), std::size_t{2});
}

ZR_TEST(PredictionKeepsApplicationSpecificModelsIsolated) {
  PredictionService service;
  service.ReplaceSystemEntries({
      {{"open"}, "generic", 10.0, {}},
      {{"open"}, "private-system", 1000.0, "private.exe"},
  });
  const std::vector<std::string> context = {"open"};
  ZR_EXPECT_TRUE(
      service.LearnSession(context, "private-session", "private.exe", 100));
  ZR_EXPECT_TRUE(
      service.LearnSession(context, "public-session", "public.exe", 100));

  const auto public_predictions =
      service.Predict(context, "public.exe", 100, 10);
  ZR_EXPECT_TRUE(!public_predictions.empty());
  for (const auto& candidate : public_predictions) {
    ZR_EXPECT_TRUE(candidate.text != "private-system");
    ZR_EXPECT_TRUE(candidate.text != "private-session");
  }

  const auto private_predictions =
      service.Predict(context, "private.exe", 100, 10);
  ZR_EXPECT_TRUE(private_predictions.size() >= std::size_t{3});
  bool found_system = false;
  bool found_session = false;
  for (const auto& candidate : private_predictions) {
    found_system = found_system || candidate.text == "private-system";
    found_session = found_session || candidate.text == "private-session";
    ZR_EXPECT_TRUE(candidate.text != "public-session");
  }
  ZR_EXPECT_TRUE(found_system);
  ZR_EXPECT_TRUE(found_session);
}

ZR_TEST(PredictionSeparatesApplicationSpecificSessionCounts) {
  PredictionService service;
  const std::vector<std::string> context = {"input"};
  ZR_EXPECT_TRUE(service.LearnSession(context, "method", "a.exe", 100));
  ZR_EXPECT_TRUE(service.LearnSession(context, "method", "a.exe", 101));
  ZR_EXPECT_TRUE(service.LearnSession(context, "method", "b.exe", 102));
  const auto app_a = service.Predict(context, "a.exe", 102, 1);
  const auto app_b = service.Predict(context, "b.exe", 102, 1);
  ZR_EXPECT_EQ(app_a.size(), std::size_t{1});
  ZR_EXPECT_EQ(app_b.size(), std::size_t{1});
  ZR_EXPECT_TRUE(app_a.front().session_term > app_b.front().session_term);
}

ZR_TEST(PredictionContextEncodingDoesNotCollideOnSeparators) {
  PredictionService service;
  const std::vector<std::string> two_tokens = {
      "a", "b"};
  const std::vector<std::string> one_token = {
      std::string("a\x1F" "b", 3)};
  ZR_EXPECT_TRUE(service.LearnSession(two_tokens, "two", {}, 100));
  ZR_EXPECT_TRUE(service.LearnSession(one_token, "one", {}, 100));
  const auto two = service.Predict(two_tokens, {}, 100, 10);
  const auto one = service.Predict(one_token, {}, 100, 10);
  ZR_EXPECT_EQ(two.size(), std::size_t{1});
  ZR_EXPECT_EQ(one.size(), std::size_t{1});
  ZR_EXPECT_EQ(two.front().text, std::string("two"));
  ZR_EXPECT_EQ(one.front().text, std::string("one"));
}

}  // namespace
