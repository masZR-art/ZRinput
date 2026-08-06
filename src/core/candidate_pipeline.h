#pragma once

#include "core/decoder.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace zrinput::core {

// Runs candidate decoding away from the TSF edit session. At most one request
// is executing and one (the newest) is waiting at any time.
class CandidatePipeline final {
 public:
  using DecodeFunction = std::function<DecodeResult(
      const DecodeRequest&,
      const std::shared_ptr<const DictionarySnapshot>&,
      const PersonalizationView*,
      std::stop_token)>;
  using ResultCallback = std::function<void(DecodeResult)>;

  CandidatePipeline(Decoder decoder, ResultCallback publish);
  CandidatePipeline(DecodeFunction decode, ResultCallback publish);
  ~CandidatePipeline();

  CandidatePipeline(const CandidatePipeline&) = delete;
  CandidatePipeline& operator=(const CandidatePipeline&) = delete;
  CandidatePipeline(CandidatePipeline&&) = delete;
  CandidatePipeline& operator=(CandidatePipeline&&) = delete;

  // Returns false after Stop begins. A successful submission supersedes every
  // older queued or executing submission, even if a composition version is
  // accidentally reused by the caller.
  [[nodiscard]] bool Submit(
      DecodeRequest request,
      std::shared_ptr<const DictionarySnapshot> dictionary,
      std::shared_ptr<const PersonalizationView> personalization = {});

  // Stop is idempotent. After it returns on a non-worker thread, no decode or
  // publication callback remains in flight.
  void Stop() noexcept;

  // Waits until neither decoding, publication, nor queued work remains.
  [[nodiscard]] bool WaitUntilIdle(
      std::chrono::milliseconds timeout) const;

 private:
  struct SharedState;

  static void WorkerMain(const std::shared_ptr<SharedState>& state,
                         std::stop_token worker_stop) noexcept;
  void FinalizeWorker() noexcept;

  std::shared_ptr<SharedState> state_;
  std::jthread worker_;

  mutable std::mutex lifecycle_mutex_;
  mutable std::condition_variable lifecycle_cv_;
  std::thread::id worker_id_;
  bool join_in_progress_ = false;
  bool worker_finalized_ = false;
};

}  // namespace zrinput::core
