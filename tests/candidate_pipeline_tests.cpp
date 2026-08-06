#include "core/candidate_pipeline.h"

#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace {

using namespace std::chrono_literals;
using zrinput::core::CandidatePipeline;
using zrinput::core::DecodeRequest;
using zrinput::core::DecodeResult;
using zrinput::core::DictionarySnapshot;

DecodeRequest Request(std::uint64_t composition_version) {
  DecodeRequest request;
  request.composition_version = composition_version;
  return request;
}

std::shared_ptr<const DictionarySnapshot> EmptyDictionary() {
  return std::make_shared<const DictionarySnapshot>();
}

ZR_TEST(OlderCandidateResultCannotOverwriteANewerVersion) {
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool first_started = false;
  bool first_cancelled = false;
  std::vector<std::uint64_t> published;

  CandidatePipeline pipeline(
      [&](const DecodeRequest& request,
          const std::shared_ptr<const DictionarySnapshot>&,
          const zrinput::core::PersonalizationView*, std::stop_token stop) {
        if (request.composition_version == 1) {
          std::unique_lock lock(gate_mutex);
          first_started = true;
          gate_cv.notify_all();
          std::stop_callback wake_on_cancel(stop, [&gate_cv] {
            gate_cv.notify_all();
          });
          gate_cv.wait(lock, [&stop] { return stop.stop_requested(); });
          first_cancelled = true;
        }
        return DecodeResult{request.composition_version, {}};
      },
      [&](DecodeResult result) {
        std::lock_guard lock(gate_mutex);
        published.push_back(result.composition_version);
      });

  ZR_EXPECT_TRUE(pipeline.Submit(Request(1), EmptyDictionary()));
  {
    std::unique_lock lock(gate_mutex);
    ZR_EXPECT_TRUE(gate_cv.wait_for(lock, 2s,
                                    [&first_started] { return first_started; }));
  }
  ZR_EXPECT_TRUE(pipeline.Submit(Request(2), EmptyDictionary()));
  ZR_EXPECT_TRUE(pipeline.WaitUntilIdle(2s));

  std::lock_guard lock(gate_mutex);
  ZR_EXPECT_TRUE(first_cancelled);
  ZR_EXPECT_EQ(published.size(), std::size_t{1});
  ZR_EXPECT_EQ(published.front(), std::uint64_t{2});
}

ZR_TEST(RapidSubmissionsKeepOnlyTheLatestPendingRequest) {
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool first_started = false;
  bool release_first = false;
  std::vector<std::uint64_t> decoded;
  std::vector<std::uint64_t> published;

  CandidatePipeline pipeline(
      [&](const DecodeRequest& request,
          const std::shared_ptr<const DictionarySnapshot>&,
          const zrinput::core::PersonalizationView*, std::stop_token) {
        std::unique_lock lock(gate_mutex);
        decoded.push_back(request.composition_version);
        if (request.composition_version == 1) {
          first_started = true;
          gate_cv.notify_all();
          gate_cv.wait(lock, [&release_first] { return release_first; });
        }
        return DecodeResult{request.composition_version, {}};
      },
      [&](DecodeResult result) {
        std::lock_guard lock(gate_mutex);
        published.push_back(result.composition_version);
      });

  ZR_EXPECT_TRUE(pipeline.Submit(Request(1), EmptyDictionary()));
  {
    std::unique_lock lock(gate_mutex);
    ZR_EXPECT_TRUE(gate_cv.wait_for(lock, 2s,
                                    [&first_started] { return first_started; }));
  }
  for (std::uint64_t version = 2; version <= 1000; ++version) {
    ZR_EXPECT_TRUE(pipeline.Submit(Request(version), EmptyDictionary()));
  }
  {
    std::lock_guard lock(gate_mutex);
    release_first = true;
  }
  gate_cv.notify_all();

  ZR_EXPECT_TRUE(pipeline.WaitUntilIdle(2s));
  std::lock_guard lock(gate_mutex);
  ZR_EXPECT_EQ(decoded.size(), std::size_t{2});
  ZR_EXPECT_EQ(decoded.front(), std::uint64_t{1});
  ZR_EXPECT_EQ(decoded.back(), std::uint64_t{1000});
  ZR_EXPECT_EQ(published.size(), std::size_t{1});
  ZR_EXPECT_EQ(published.front(), std::uint64_t{1000});
}

ZR_TEST(StopCancelsWorkRejectsSubmissionsAndIsIdempotent) {
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool started = false;
  bool cancelled = false;
  std::size_t publications = 0;

  CandidatePipeline pipeline(
      [&](const DecodeRequest& request,
          const std::shared_ptr<const DictionarySnapshot>&,
          const zrinput::core::PersonalizationView*, std::stop_token stop) {
        std::unique_lock lock(gate_mutex);
        started = true;
        gate_cv.notify_all();
        std::stop_callback wake_on_cancel(stop, [&gate_cv] {
          gate_cv.notify_all();
        });
        gate_cv.wait(lock, [&stop] { return stop.stop_requested(); });
        cancelled = true;
        return DecodeResult{request.composition_version, {}};
      },
      [&](DecodeResult) {
        std::lock_guard lock(gate_mutex);
        ++publications;
      });

  ZR_EXPECT_TRUE(pipeline.Submit(Request(7), EmptyDictionary()));
  {
    std::unique_lock lock(gate_mutex);
    ZR_EXPECT_TRUE(
        gate_cv.wait_for(lock, 2s, [&started] { return started; }));
  }

  pipeline.Stop();
  pipeline.Stop();
  ZR_EXPECT_TRUE(pipeline.WaitUntilIdle(50ms));
  ZR_EXPECT_TRUE(!pipeline.Submit(Request(8), EmptyDictionary()));

  std::lock_guard lock(gate_mutex);
  ZR_EXPECT_TRUE(cancelled);
  ZR_EXPECT_EQ(publications, std::size_t{0});
}

ZR_TEST(RejectsADecodeResultWithTheWrongCompositionVersion) {
  std::atomic_size_t publications = 0;
  CandidatePipeline pipeline(
      [](const DecodeRequest& request,
         const std::shared_ptr<const DictionarySnapshot>&,
         const zrinput::core::PersonalizationView*, std::stop_token) {
        return DecodeResult{request.composition_version + 1, {}};
      },
      [&publications](DecodeResult) { ++publications; });

  ZR_EXPECT_TRUE(pipeline.Submit(Request(20), EmptyDictionary()));
  ZR_EXPECT_TRUE(pipeline.WaitUntilIdle(2s));
  ZR_EXPECT_EQ(publications.load(), std::size_t{0});
}

ZR_TEST(PublicationCallbackCanSubmitTheNextComposition) {
  std::mutex result_mutex;
  std::vector<std::uint64_t> published;
  bool reentrant_submit_succeeded = false;
  CandidatePipeline* pipeline_address = nullptr;

  CandidatePipeline pipeline(
      [](const DecodeRequest& request,
         const std::shared_ptr<const DictionarySnapshot>&,
         const zrinput::core::PersonalizationView*, std::stop_token) {
        return DecodeResult{request.composition_version, {}};
      },
      [&](DecodeResult result) {
        {
          std::lock_guard lock(result_mutex);
          published.push_back(result.composition_version);
        }
        if (result.composition_version == 31) {
          reentrant_submit_succeeded = pipeline_address->Submit(
              Request(32), EmptyDictionary());
        }
      });
  pipeline_address = &pipeline;

  ZR_EXPECT_TRUE(pipeline.Submit(Request(31), EmptyDictionary()));
  ZR_EXPECT_TRUE(pipeline.WaitUntilIdle(2s));

  std::lock_guard lock(result_mutex);
  ZR_EXPECT_TRUE(reentrant_submit_succeeded);
  ZR_EXPECT_EQ(published.size(), std::size_t{2});
  ZR_EXPECT_EQ(published.front(), std::uint64_t{31});
  ZR_EXPECT_EQ(published.back(), std::uint64_t{32});
}

ZR_TEST(PipelineCanBeDestroyedFromItsPublicationCallback) {
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  bool callback_completed = false;
  std::unique_ptr<CandidatePipeline> pipeline;

  pipeline = std::make_unique<CandidatePipeline>(
      [](const DecodeRequest& request,
         const std::shared_ptr<const DictionarySnapshot>&,
         const zrinput::core::PersonalizationView*, std::stop_token) {
        return DecodeResult{request.composition_version, {}};
      },
      [&](DecodeResult) {
        pipeline.reset();
        {
          std::lock_guard lock(completion_mutex);
          callback_completed = true;
        }
        completion_cv.notify_all();
      });

  CandidatePipeline* const pipeline_address = pipeline.get();
  ZR_EXPECT_TRUE(
      pipeline_address->Submit(Request(40), EmptyDictionary()));
  {
    std::unique_lock lock(completion_mutex);
    ZR_EXPECT_TRUE(completion_cv.wait_for(
        lock, 2s, [&callback_completed] { return callback_completed; }));
  }
  ZR_EXPECT_TRUE(!pipeline);
}

}  // namespace
