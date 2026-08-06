#include "core/candidate_pipeline.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zrinput::core {

struct CandidatePipeline::SharedState {
  struct Job {
    DecodeRequest request;
    std::shared_ptr<const DictionarySnapshot> dictionary;
    std::shared_ptr<const PersonalizationView> personalization;
    std::uint64_t submission_id = 0;
  };

  SharedState(DecodeFunction decode_function, ResultCallback result_callback)
      : decode(std::move(decode_function)),
        publish(std::move(result_callback)) {}

  std::mutex mutex;
  std::condition_variable_any work_cv;
  std::condition_variable idle_cv;
  DecodeFunction decode;
  ResultCallback publish;
  std::optional<Job> pending;
  std::optional<std::stop_source> active_stop;
  std::uint64_t latest_submission_id = 0;
  bool active = false;
  bool stopping = false;
};

CandidatePipeline::CandidatePipeline(Decoder decoder, ResultCallback publish)
    : CandidatePipeline(
          [decoder = std::move(decoder)](
              const DecodeRequest& request,
              const std::shared_ptr<const DictionarySnapshot>& dictionary,
              const PersonalizationView* personalization,
              std::stop_token stop) {
            if (stop.stop_requested()) {
              return DecodeResult{request.composition_version, {}};
            }
            return decoder.Decode(request, dictionary, personalization, stop);
          },
          std::move(publish)) {}

CandidatePipeline::CandidatePipeline(DecodeFunction decode,
                                     ResultCallback publish) {
  if (!decode) {
    throw std::invalid_argument("candidate pipeline requires a decoder");
  }
  if (!publish) {
    throw std::invalid_argument(
        "candidate pipeline requires a publication callback");
  }

  state_ = std::make_shared<SharedState>(std::move(decode),
                                         std::move(publish));
  worker_ = std::jthread(
      [state = state_](std::stop_token stop) { WorkerMain(state, stop); });
  worker_id_ = worker_.get_id();
}

CandidatePipeline::~CandidatePipeline() {
  Stop();
}

bool CandidatePipeline::Submit(
    DecodeRequest request,
    std::shared_ptr<const DictionarySnapshot> dictionary,
    std::shared_ptr<const PersonalizationView> personalization) {
  std::optional<std::stop_source> active_stop;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) {
      return false;
    }

    ++state_->latest_submission_id;
    state_->pending.emplace(SharedState::Job{
        std::move(request), std::move(dictionary),
        std::move(personalization), state_->latest_submission_id});
    active_stop = state_->active_stop;
  }

  // request_stop may synchronously invoke user stop callbacks, so it must not
  // run while the pipeline mutex is held.
  if (active_stop.has_value()) {
    static_cast<void>(active_stop->request_stop());
  }
  state_->work_cv.notify_one();
  return true;
}

void CandidatePipeline::Stop() noexcept {
  std::optional<std::stop_source> active_stop;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->stopping) {
      state_->stopping = true;
      ++state_->latest_submission_id;
      state_->pending.reset();
    }
    active_stop = state_->active_stop;
  }

  if (active_stop.has_value()) {
    static_cast<void>(active_stop->request_stop());
  }
  state_->work_cv.notify_all();
  FinalizeWorker();
}

bool CandidatePipeline::WaitUntilIdle(
    std::chrono::milliseconds timeout) const {
  std::unique_lock lock(state_->mutex);
  return state_->idle_cv.wait_for(lock, timeout, [this] {
    return !state_->active && !state_->pending.has_value();
  });
}

void CandidatePipeline::WorkerMain(
    const std::shared_ptr<SharedState>& state,
    std::stop_token worker_stop) noexcept {
  for (;;) {
    SharedState::Job job;
    std::stop_source request_stop;
    {
      std::unique_lock lock(state->mutex);
      const bool has_work = state->work_cv.wait(
          lock, worker_stop,
          [&state] { return state->stopping || state->pending.has_value(); });
      if (!has_work || state->stopping) {
        state->pending.reset();
        state->active = false;
        state->active_stop.reset();
        state->idle_cv.notify_all();
        return;
      }

      job = std::move(*state->pending);
      state->pending.reset();
      state->active = true;
      state->active_stop.emplace();
      request_stop = *state->active_stop;
    }

    std::optional<DecodeResult> result;
    try {
      result.emplace(state->decode(
          job.request, job.dictionary, job.personalization.get(),
          request_stop.get_token()));
    } catch (...) {
      // A decoder failure invalidates only this request. The permanent worker
      // remains available for the latest pending composition.
    }

    bool should_publish = false;
    {
      std::lock_guard lock(state->mutex);
      should_publish =
          result.has_value() && !state->stopping &&
          !worker_stop.stop_requested() &&
          !request_stop.stop_requested() &&
          job.submission_id == state->latest_submission_id &&
          result->composition_version == job.request.composition_version;
    }

    if (should_publish) {
      try {
        // Publication is deliberately outside the state lock. The submission
        // id comparison above is the publication linearization point.
        state->publish(std::move(*result));
      } catch (...) {
        // UI callback failures must not terminate the permanent worker.
      }
    }

    {
      std::lock_guard lock(state->mutex);
      state->active = false;
      state->active_stop.reset();
      if (!state->pending.has_value()) {
        state->idle_cv.notify_all();
      }
    }
  }
}

void CandidatePipeline::FinalizeWorker() noexcept {
  std::unique_lock lock(lifecycle_mutex_);
  if (worker_finalized_) {
    return;
  }

  const bool called_on_worker =
      std::this_thread::get_id() == worker_id_;
  if (join_in_progress_) {
    if (called_on_worker) {
      // A different thread is already joining us. Let the callback unwind so
      // that join can complete.
      return;
    }
    lifecycle_cv_.wait(lock, [this] { return !join_in_progress_; });
    return;
  }

  if (!worker_.joinable()) {
    worker_finalized_ = true;
    return;
  }

  if (called_on_worker) {
    try {
      worker_.detach();
    } catch (...) {
      return;
    }
    worker_finalized_ = true;
    lifecycle_cv_.notify_all();
    return;
  }

  join_in_progress_ = true;
  lock.unlock();
  try {
    worker_.join();
  } catch (...) {
    try {
      if (worker_.joinable()) {
        worker_.detach();
      }
    } catch (...) {
      // There is no recovery action available in a noexcept shutdown path.
    }
  }
  lock.lock();
  join_in_progress_ = false;
  worker_finalized_ = !worker_.joinable();
  lifecycle_cv_.notify_all();
}

}  // namespace zrinput::core
