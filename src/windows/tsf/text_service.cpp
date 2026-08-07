#include "windows/tsf/text_service.h"

#include "windows/tsf/module_state.h"

#include <initguid.h>
#include <inputscope.h>
#include <oleauto.h>
#include <olectl.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

namespace zrinput::windows::tsf {
namespace {

class EditSession final : public ITfEditSession {
public:
  EditSession(TextService *service, ITfContext *context, std::uint64_t sequence,
              std::uint64_t context_epoch, std::uint64_t generation,
              EditOperation operation, CompositionSnapshot snapshot,
              ITfComposition *captured_composition) noexcept
      : service_(service), context_(context), sequence_(sequence),
        context_epoch_(context_epoch), generation_(generation),
        operation_(operation), snapshot_(std::move(snapshot)),
        captured_composition_(captured_composition) {
    AddLiveObject();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfEditSession)) {
      *object = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override {
    return reference_count_.fetch_add(1) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() noexcept override {
    const ULONG remaining = reference_count_.fetch_sub(1) - 1;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE
  DoEditSession(TfEditCookie cookie) noexcept override {
    const HRESULT result = ComBoundary([&]() {
      return service_->RunEditSession(cookie, context_.Get(), sequence_,
                                      context_epoch_, generation_, operation_,
                                      snapshot_, captured_composition_.Get());
    });
    service_->CompleteEditSession(sequence_, result);
    return result;
  }

private:
  ~EditSession() noexcept { RemoveLiveObject(); }

  std::atomic<ULONG> reference_count_{1};
  ComPtr<TextService> service_;
  ComPtr<ITfContext> context_;
  std::uint64_t sequence_;
  std::uint64_t context_epoch_;
  std::uint64_t generation_;
  EditOperation operation_;
  CompositionSnapshot snapshot_;
  ComPtr<ITfComposition> captured_composition_;
};

[[nodiscard]] bool IsKeyPressed(int virtual_key) noexcept {
  return (GetKeyState(virtual_key) & 0x8000) != 0;
}

[[nodiscard]] bool IsSensitiveScope(InputScope scope) noexcept {
  switch (scope) {
  case IS_PASSWORD:
  case IS_PRIVATE:
  case IS_NUMERIC_PASSWORD:
  case IS_NUMERIC_PIN:
  case IS_ALPHANUMERIC_PIN:
  case IS_ALPHANUMERIC_PIN_SET:
    return true;
  default:
    return false;
  }
}

} // namespace

TextService::TextService() { AddLiveObject(); }

TextService::~TextService() noexcept {
  DeactivateImpl();
  RemoveLiveObject();
}

HRESULT TextService::QueryInterface(REFIID iid, void **object) noexcept {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (IsEqualIID(iid, IID_IUnknown) ||
      IsEqualIID(iid, IID_ITfTextInputProcessor) ||
      IsEqualIID(iid, IID_ITfTextInputProcessorEx)) {
    *object = static_cast<ITfTextInputProcessorEx *>(this);
  } else if (IsEqualIID(iid, IID_ITfKeyEventSink)) {
    *object = static_cast<ITfKeyEventSink *>(this);
  } else if (IsEqualIID(iid, IID_ITfCompositionSink)) {
    *object = static_cast<ITfCompositionSink *>(this);
  } else if (IsEqualIID(iid, IID_ITfThreadMgrEventSink)) {
    *object = static_cast<ITfThreadMgrEventSink *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG TextService::AddRef() noexcept {
  return reference_count_.fetch_add(1) + 1;
}

ULONG TextService::Release() noexcept {
  const ULONG remaining = reference_count_.fetch_sub(1) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT TextService::Activate(ITfThreadMgr *thread_manager,
                              TfClientId client_id) noexcept {
  return ActivateEx(thread_manager, client_id, 0);
}

HRESULT TextService::ActivateEx(ITfThreadMgr *thread_manager,
                                TfClientId client_id, DWORD flags) noexcept {
  return ComBoundary(
      [&]() { return ActivateImpl(thread_manager, client_id, flags); });
}

HRESULT TextService::ActivateImpl(ITfThreadMgr *thread_manager,
                                  TfClientId client_id, DWORD flags) {
  if (thread_manager == nullptr || client_id == TF_CLIENTID_NULL) {
    return E_INVALIDARG;
  }
  if (active_ || thread_manager_) {
    return E_UNEXPECTED;
  }

  thread_manager_.Assign(thread_manager);
  client_id_ = client_id;
  secure_activation_ = (flags & TF_TMAE_SECUREMODE) != 0;
  sensitive_context_.store(true);

  ComPtr<ITfSource> source;
  HRESULT result = thread_manager->QueryInterface(
      IID_ITfSource, reinterpret_cast<void **>(source.Put()));
  if (FAILED(result)) {
    DeactivateImpl();
    return result;
  }
  result = source->AdviseSink(IID_ITfThreadMgrEventSink,
                              static_cast<ITfThreadMgrEventSink *>(this),
                              &thread_sink_cookie_);
  if (FAILED(result)) {
    DeactivateImpl();
    return result;
  }

  ComPtr<ITfKeystrokeMgr> keystroke_manager;
  result = thread_manager->QueryInterface(
      IID_ITfKeystrokeMgr, reinterpret_cast<void **>(keystroke_manager.Put()));
  if (FAILED(result)) {
    DeactivateImpl();
    return result;
  }
  result = keystroke_manager->AdviseKeyEventSink(
      client_id_, static_cast<ITfKeyEventSink *>(this), TRUE);
  if (FAILED(result)) {
    DeactivateImpl();
    return result;
  }
  key_sink_advised_ = true;

  active_ = true;
  foreground_ = true;
  ComPtr<ITfDocumentMgr> document_manager;
  if (SUCCEEDED(thread_manager->GetFocus(document_manager.Put()))) {
    const HRESULT focus_result = SetFocusedDocument(document_manager.Get());
    if (FAILED(focus_result)) {
      DeactivateImpl();
      return focus_result;
    }
  }
  return S_OK;
}

HRESULT TextService::Deactivate() noexcept {
  return ComBoundary([&]() { return DeactivateImpl(); });
}

HRESULT TextService::DeactivateImpl() noexcept {
  if (!thread_manager_) {
    active_ = false;
    foreground_ = false;
    learning_context_available_.store(false);
    key_sink_advised_ = false;
    thread_sink_cookie_ = TF_INVALID_COOKIE;
    client_id_ = TF_CLIENTID_NULL;
    secure_activation_ = false;
    sensitive_context_.store(false);
    selection_dirty_ = false;
    return S_OK;
  }

  active_ = false;
  foreground_ = false;
  learning_context_available_.store(false);
  AbandonContext(true);
  const bool split_remainders_preserved =
      PreserveSplitRemaindersBeforeDroppingUpdates();
  auto pending = pending_edits_.begin();
  if (in_flight_edit_sequence_.has_value() && !pending_edits_.empty() &&
      pending_edits_.front().sequence == *in_flight_edit_sequence_) {
    ++pending;
  }
  while (pending != pending_edits_.end()) {
    if (pending->operation == EditOperation::kUpdate &&
        !IsPendingEditIrreversible(*pending) && split_remainders_preserved) {
      pending = pending_edits_.erase(pending);
    } else {
      ++pending;
    }
  }
  focused_document_.Reset();

  HRESULT first_failure = S_OK;
  const auto record_failure = [&](HRESULT result) noexcept {
    if (FAILED(result) && SUCCEEDED(first_failure)) {
      first_failure = result;
    }
  };

  if (!in_flight_edit_sequence_.has_value() && !pending_edits_.empty()) {
    std::uint64_t failed_sequence = 0;
    record_failure(DispatchPendingEdits(&failed_sequence));
  }

  if (key_sink_advised_) {
    ComPtr<ITfKeystrokeMgr> keystroke_manager;
    HRESULT result = thread_manager_->QueryInterface(
        IID_ITfKeystrokeMgr,
        reinterpret_cast<void **>(keystroke_manager.Put()));
    if (SUCCEEDED(result)) {
      result = keystroke_manager->UnadviseKeyEventSink(client_id_);
    }
    if (SUCCEEDED(result) || result == CONNECT_E_NOCONNECTION) {
      key_sink_advised_ = false;
    } else {
      record_failure(result);
    }
  }

  if (thread_sink_cookie_ != TF_INVALID_COOKIE) {
    ComPtr<ITfSource> source;
    HRESULT result = thread_manager_->QueryInterface(
        IID_ITfSource, reinterpret_cast<void **>(source.Put()));
    if (SUCCEEDED(result)) {
      result = source->UnadviseSink(thread_sink_cookie_);
    }
    if (SUCCEEDED(result) || result == CONNECT_E_NOCONNECTION) {
      thread_sink_cookie_ = TF_INVALID_COOKIE;
    } else {
      record_failure(result);
    }
  }

  intentionally_ending_composition_.Reset();
  if (key_sink_advised_ || thread_sink_cookie_ != TF_INVALID_COOKIE) {
    return FAILED(first_failure) ? first_failure : E_FAIL;
  }

  thread_manager_.Reset();
  client_id_ = TF_CLIENTID_NULL;
  secure_activation_ = false;
  sensitive_context_.store(false);
  return first_failure;
}

HRESULT TextService::OnSetFocus(BOOL foreground) noexcept {
  return ComBoundary([&]() -> HRESULT {
    foreground_ = foreground != FALSE;
    if (!foreground_) {
      AbandonContext(true);
      return S_OK;
    }
    if (!thread_manager_) {
      return S_OK;
    }
    ComPtr<ITfDocumentMgr> document_manager;
    const HRESULT result = thread_manager_->GetFocus(document_manager.Put());
    return FAILED(result) ? result : SetFocusedDocument(document_manager.Get());
  });
}

bool TextService::ShouldEatKey(ITfContext *context, WPARAM key) const noexcept {
  if (!active_ || !foreground_ || context == nullptr) {
    return false;
  }

  const bool control = IsKeyPressed(VK_CONTROL);
  const bool alt = IsKeyPressed(VK_MENU);
  const bool windows_key = IsKeyPressed(VK_LWIN) || IsKeyPressed(VK_RWIN);
  if (alt || windows_key || (control && key != VK_BACK)) {
    return false;
  }
  const bool same_context = SameComIdentity(context, focused_context_.Get());
  const bool current_edit_faulted =
      same_context && CurrentEditFailureBudgetExhausted();
  if (same_context && selection_dirty_) {
    return key == VK_ESCAPE || key == VK_RETURN;
  }
  if (key >= static_cast<WPARAM>('A') && key <= static_cast<WPARAM>('Z')) {
    return !control && !current_edit_faulted;
  }
  if (key == VK_OEM_7) {
    return !control && !IsKeyPressed(VK_SHIFT) && !current_edit_faulted;
  }
  if (!same_context) {
    return false;
  }
  if (current_edit_faulted) {
    if (key == VK_ESCAPE) {
      return !in_flight_edit_sequence_.has_value();
    }
    return CanRecoverFaultedEditWithKey(key);
  }
  if (buffer_.empty()) {
    return false;
  }
  switch (key) {
  case VK_BACK:
  case VK_DELETE:
  case VK_LEFT:
  case VK_RIGHT:
  case VK_HOME:
  case VK_END:
  case VK_ESCAPE:
  case VK_RETURN:
    return true;
  default:
    return false;
  }
}

HRESULT TextService::OnTestKeyDown(ITfContext *context, WPARAM key, LPARAM,
                                   BOOL *eaten) noexcept {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = ShouldEatKey(context, key) ? TRUE : FALSE;
  return S_OK;
}

HRESULT TextService::OnTestKeyUp(ITfContext *, WPARAM, LPARAM,
                                 BOOL *eaten) noexcept {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  return S_OK;
}

HRESULT TextService::OnKeyDown(ITfContext *context, WPARAM key, LPARAM,
                               BOOL *eaten) noexcept {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  const HRESULT result =
      ComBoundary([&]() { return HandleKey(context, key, eaten); });
  if (FAILED(result)) {
    *eaten = FALSE;
  }
  return result;
}

HRESULT TextService::OnKeyUp(ITfContext *, WPARAM, LPARAM,
                             BOOL *eaten) noexcept {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  return S_OK;
}

HRESULT TextService::OnPreservedKey(ITfContext *, REFGUID,
                                    BOOL *eaten) noexcept {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  return S_OK;
}

HRESULT TextService::HandleKey(ITfContext *context, WPARAM key, BOOL *eaten) {
  if (!ShouldEatKey(context, key)) {
    return S_OK;
  }
  HRESULT result = AdoptContext(context);
  if (FAILED(result)) {
    return result;
  }

  *eaten = TRUE;
  if (CurrentEditFailureBudgetExhausted()) {
    if (key == VK_ESCAPE && !in_flight_edit_sequence_.has_value()) {
      return RecoverFaultedCurrentEditWithEscape(context);
    }
    if (key != VK_BACK && key != VK_DELETE) {
      return E_PENDING;
    }
  }
  if (key >= static_cast<WPARAM>('A') && key <= static_cast<WPARAM>('Z')) {
    return InsertCharacter(
        context,
        static_cast<char16_t>(u'a' + (key - static_cast<WPARAM>('A'))));
  }
  if (key == VK_OEM_7) {
    return InsertCharacter(context, u'\'');
  }
  if (key == VK_ESCAPE) {
    return CommitOrCancel(context, false);
  }
  if (key == VK_RETURN) {
    return CommitOrCancel(context, true);
  }

  std::u16string previous_text = buffer_.text();
  const core::TextSelection previous_selection = buffer_.selection();
  core::EditOutcome outcome = core::EditOutcome::kNoChange;
  switch (key) {
  case VK_BACK:
    outcome = buffer_.EraseBackward(IsKeyPressed(VK_CONTROL));
    break;
  case VK_DELETE:
    outcome = buffer_.EraseForward(false);
    break;
  case VK_LEFT:
    if (buffer_.Move(core::CursorMove::kPreviousCodePoint,
                     IsKeyPressed(VK_SHIFT))) {
      outcome = core::EditOutcome::kApplied;
    }
    break;
  case VK_RIGHT:
    if (buffer_.Move(core::CursorMove::kNextCodePoint,
                     IsKeyPressed(VK_SHIFT))) {
      outcome = core::EditOutcome::kApplied;
    }
    break;
  case VK_HOME:
    if (buffer_.Move(core::CursorMove::kHome, IsKeyPressed(VK_SHIFT))) {
      outcome = core::EditOutcome::kApplied;
    }
    break;
  case VK_END:
    if (buffer_.Move(core::CursorMove::kEnd, IsKeyPressed(VK_SHIFT))) {
      outcome = core::EditOutcome::kApplied;
    }
    break;
  default:
    break;
  }
  return ApplyBufferEdit(context, outcome, std::move(previous_text),
                         previous_selection);
}

HRESULT TextService::InsertCharacter(ITfContext *context, char16_t character) {
  std::u16string previous_text = buffer_.text();
  const core::TextSelection previous_selection = buffer_.selection();
  const char16_t value[] = {character};
  const core::EditOutcome outcome =
      buffer_.Insert(std::u16string_view(value, static_cast<std::size_t>(1)));
  if (outcome == core::EditOutcome::kNeedsStablePrefixCommit) {
    return SplitAndInsert(context, character, std::move(previous_text),
                          previous_selection);
  }
  return ApplyBufferEdit(context, outcome, std::move(previous_text),
                         previous_selection);
}

HRESULT TextService::ApplyBufferEdit(ITfContext *context,
                                     core::EditOutcome outcome,
                                     std::u16string previous_text,
                                     core::TextSelection previous_selection) {
  if (outcome == core::EditOutcome::kNoChange) {
    return S_OK;
  }
  if (outcome != core::EditOutcome::kApplied) {
    return S_FALSE;
  }

  CompositionSnapshot snapshot{buffer_.text(), buffer_.selection(), {}};
  const std::uint64_t generation = ++composition_generation_;
  const QueueEditResult queued = QueueEdit(
      context, generation, EditOperation::kUpdate, std::move(snapshot));
  if (queued.disposition == QueueEditDisposition::kRejectedBeforeMutation) {
    (void)buffer_.ReplaceForReplay(std::move(previous_text),
                                   previous_selection);
    ++composition_generation_;
  }
  return queued.result;
}

HRESULT TextService::SplitAndInsert(ITfContext *context, char16_t character,
                                    std::u16string previous_text,
                                    core::TextSelection previous_selection) {
  std::u16string combined = previous_text;
  const std::size_t begin = previous_selection.begin();
  combined.replace(begin, previous_selection.end() - begin, 1, character);
  const std::size_t new_caret = begin + 1;
  const auto &limits = buffer_.limits();
  if (combined.size() > limits.hard_units) {
    return S_FALSE;
  }

  const std::size_t minimum_split = combined.size() - limits.active_units;
  const std::size_t preferred_split = limits.active_units / 2;
  const auto split = FindExplicitStableSplit(combined, minimum_split,
                                             preferred_split, new_caret);
  if (!split.has_value()) {
    return S_FALSE;
  }

  ComPtr<ITfComposition> captured = composition_;
  if (PendingBarrierOwnsComposition(captured.Get())) {
    captured.Reset();
  }
  CompositionSnapshot snapshot;
  snapshot.committed_prefix = combined.substr(0, *split);
  snapshot.text = combined.substr(*split);
  const std::size_t remaining_caret = new_caret - *split;
  snapshot.selection = {remaining_caret, remaining_caret};
  if (!buffer_.ReplaceForReplay(snapshot.text, snapshot.selection)) {
    return E_FAIL;
  }

  const std::uint64_t generation = ++composition_generation_;
  const QueueEditResult queued =
      QueueEdit(context, generation, EditOperation::kSplit, snapshot,
                captured.Get());
  if (queued.disposition == QueueEditDisposition::kRejectedBeforeMutation) {
    (void)buffer_.ReplaceForReplay(std::move(previous_text),
                                   previous_selection);
    ++composition_generation_;
  } else if (queued.disposition == QueueEditDisposition::kCompleted) {
    ReleaseCompositionIfSame(captured.Get());
  }
  return queued.result;
}

HRESULT TextService::CommitOrCancel(ITfContext *context, bool commit) {
  std::u16string previous_text = buffer_.text();
  const core::TextSelection previous_selection = buffer_.selection();
  CompositionSnapshot snapshot;
  if (commit) {
    snapshot.text = previous_text;
    snapshot.selection = previous_selection;
  }
  ComPtr<ITfComposition> captured = composition_;
  if (PendingBarrierOwnsComposition(captured.Get())) {
    captured.Reset();
  }
  buffer_.Clear();
  const std::uint64_t generation = ++composition_generation_;
  const QueueEditResult queued =
      QueueEdit(context, generation,
                commit ? EditOperation::kCommit : EditOperation::kCancel,
                std::move(snapshot), captured.Get());
  if (queued.disposition == QueueEditDisposition::kRejectedBeforeMutation) {
    (void)buffer_.ReplaceForReplay(std::move(previous_text),
                                   previous_selection);
    ++composition_generation_;
  } else if (queued.disposition == QueueEditDisposition::kCompleted) {
    ReleaseCompositionIfSame(captured.Get());
  }
  return queued.result;
}

TextService::QueueEditResult TextService::QueueEdit(
    ITfContext *context, std::uint64_t generation, EditOperation operation,
    CompositionSnapshot snapshot, ITfComposition *captured_composition,
    bool lifecycle_request) {
  if (context == nullptr || client_id_ == TF_CLIENTID_NULL) {
    return {E_UNEXPECTED, QueueEditDisposition::kRejectedBeforeMutation};
  }
  sensitive_context_.store(true);
  if (CurrentEditFailureBudgetExhausted() &&
      !IsShrinkingRecoveryUpdate(context, operation, snapshot)) {
    return {E_PENDING, QueueEditDisposition::kRejectedBeforeMutation};
  }

  try {
    PendingEditRequest request;
    request.context.Assign(context);
    request.client_id = client_id_;
    request.sequence = ++next_edit_sequence_;
    request.context_epoch = context_epoch_;
    request.generation = generation;
    request.operation = operation;
    request.snapshot = std::move(snapshot);
    request.captured_composition.Assign(captured_composition);
    if (operation == EditOperation::kCancel ||
        (operation == EditOperation::kUpdate && request.snapshot.text.empty())) {
      request.phase = PendingEditPhase::kBeforeClear;
    }

    const bool may_coalesce =
        operation == EditOperation::kUpdate && !pending_edits_.empty() &&
        pending_edits_.back().operation == EditOperation::kUpdate &&
        (!in_flight_edit_sequence_.has_value() ||
         pending_edits_.back().sequence != *in_flight_edit_sequence_) &&
         (!submitting_edit_sequence_.has_value() ||
          pending_edits_.back().sequence != *submitting_edit_sequence_) &&
        !IsPendingEditIrreversible(pending_edits_.back()) &&
        SameComIdentity(context, pending_edits_.back().context.Get());
    if (may_coalesce) {
      PendingEditRequest &pending = pending_edits_.back();
      PendingEditRequest previous_pending = pending;
      const std::uint64_t pending_sequence = pending.sequence;
      const bool same_context_epoch =
          pending.context_epoch == request.context_epoch;
      pending.client_id = request.client_id;
      pending.context_epoch = request.context_epoch;
      pending.generation = generation;
      if (!same_context_epoch) {
        pending.failure_count = 0;
      }
      pending.phase = request.phase;
      pending.written_payload = PendingEditPayload::kNone;
      pending.snapshot = std::move(request.snapshot);
      pending.captured_composition = std::move(request.captured_composition);
      if (in_flight_edit_sequence_.has_value() || dispatching_edit_session_) {
        return {S_OK, QueueEditDisposition::kAcceptedPending};
      }
      std::uint64_t failed_sequence = 0;
      const HRESULT result = DispatchPendingEdits(&failed_sequence);
      const auto coalesced = std::find_if(
          pending_edits_.begin(), pending_edits_.end(),
          [&](const PendingEditRequest &candidate) {
            return candidate.sequence == pending_sequence;
          });
      const bool coalesced_is_in_flight =
          in_flight_edit_sequence_.has_value() &&
          *in_flight_edit_sequence_ == pending_sequence;
      if (FAILED(result) && coalesced != pending_edits_.end() &&
          !coalesced_is_in_flight) {
        if (failed_sequence == pending_sequence &&
            IsPendingEditIrreversible(*coalesced)) {
          return {S_OK, QueueEditDisposition::kAcceptedPending};
        }
        const std::size_t failure_count = coalesced->failure_count;
        *coalesced = std::move(previous_pending);
        coalesced->failure_count =
            (std::max)(coalesced->failure_count, failure_count);
        return {result, QueueEditDisposition::kRejectedBeforeMutation};
      }
      if (coalesced != pending_edits_.end() || coalesced_is_in_flight) {
        return {FAILED(result) ? S_OK : result,
                QueueEditDisposition::kAcceptedPending};
      }
      const HRESULT completed_result =
          FAILED(result) && failed_sequence != pending_sequence ? S_OK : result;
      return {completed_result, QueueEditDisposition::kCompleted};
    }

    const std::size_t maximum_pending = lifecycle_request
                                            ? kMaximumLifecyclePendingEdits
                                            : kMaximumPendingEdits;
    if (pending_edits_.size() >= maximum_pending) {
      return {E_PENDING, QueueEditDisposition::kRejectedBeforeMutation};
    }

    const std::uint64_t sequence = request.sequence;
    pending_edits_.push_back(std::move(request));
    if (in_flight_edit_sequence_.has_value() || dispatching_edit_session_) {
      return {S_OK, QueueEditDisposition::kAcceptedPending};
    }

    std::uint64_t failed_sequence = 0;
    const HRESULT result = DispatchPendingEdits(&failed_sequence);
    const auto queued = std::find_if(
        pending_edits_.begin(), pending_edits_.end(),
        [&](const PendingEditRequest &candidate) {
          return candidate.sequence == sequence;
        });
    const bool queued_is_in_flight = in_flight_edit_sequence_.has_value() &&
                                     *in_flight_edit_sequence_ == sequence;
    if (FAILED(result) && queued != pending_edits_.end() &&
        !lifecycle_request && !queued_is_in_flight) {
      if (failed_sequence == sequence && IsPendingEditIrreversible(*queued)) {
        return {S_OK, QueueEditDisposition::kAcceptedPending};
      }
      pending_edits_.erase(queued);
      return {result, QueueEditDisposition::kRejectedBeforeMutation};
    }
    if (queued != pending_edits_.end() || queued_is_in_flight) {
      return {FAILED(result) ? S_OK : result,
              QueueEditDisposition::kAcceptedPending};
    }
    const HRESULT completed_result =
        FAILED(result) && failed_sequence != sequence ? S_OK : result;
    return {completed_result, QueueEditDisposition::kCompleted};
  } catch (const std::bad_alloc &) {
    return {E_OUTOFMEMORY, QueueEditDisposition::kRejectedBeforeMutation};
  } catch (...) {
    return {E_FAIL, QueueEditDisposition::kRejectedBeforeMutation};
  }
}

HRESULT TextService::DispatchPendingEdits(std::uint64_t *failed_sequence) {
  if (failed_sequence == nullptr) {
    return E_POINTER;
  }
  *failed_sequence = 0;
  if (dispatching_edit_session_ || in_flight_edit_sequence_.has_value()) {
    return S_OK;
  }

  dispatching_edit_session_ = true;
  struct DispatchGuard {
    bool *dispatching;
    ~DispatchGuard() noexcept { *dispatching = false; }
  } dispatch_guard{&dispatching_edit_session_};
  HRESULT first_result = S_OK;
  bool have_result = false;
  while (!in_flight_edit_sequence_.has_value() && !pending_edits_.empty()) {
    PendingEditRequest &request = pending_edits_.front();
    if (request.invalidated) {
      pending_edits_.pop_front();
      continue;
    }
    if (!IsPendingEditCurrent(request) && ShouldRetireFailedEdit(request)) {
      pending_edits_.pop_front();
      continue;
    }
    const std::uint64_t sequence = request.sequence;
    EditSession *session = nullptr;
    try {
      session = new (std::nothrow) EditSession(
          this, request.context.Get(), sequence, request.context_epoch,
          request.generation, request.operation, request.snapshot,
          request.captured_composition.Get());
    } catch (const std::bad_alloc &) {
      *failed_sequence = sequence;
      return E_OUTOFMEMORY;
    } catch (...) {
      *failed_sequence = sequence;
      return E_FAIL;
    }
    if (session == nullptr) {
      *failed_sequence = sequence;
      return E_OUTOFMEMORY;
    }

    in_flight_edit_sequence_ = sequence;
    submitting_edit_sequence_ = sequence;
    HRESULT session_result = E_FAIL;
    const HRESULT request_result = request.context->RequestEditSession(
        request.client_id, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
        &session_result);
    session->Release();

    const HRESULT result =
        FAILED(request_result) ? request_result : session_result;
    if (!have_result) {
      first_result = result;
      have_result = true;
    }
    const bool callback_completed = !in_flight_edit_sequence_.has_value() ||
                                    *in_flight_edit_sequence_ != sequence;
    submitting_edit_sequence_.reset();
    if (request.invalidated) {
      if (!callback_completed) {
        in_flight_edit_sequence_.reset();
      }
      pending_edits_.pop_front();
      continue;
    }
    if (FAILED(result)) {
      if (!callback_completed) {
        in_flight_edit_sequence_.reset();
      }
      RecordPendingEditFailure(request);
      *failed_sequence = sequence;
      if (ShouldRetireFailedEdit(request)) {
        pending_edits_.pop_front();
        continue;
      }
      if (!active_) {
        continue;
      }
      return result;
    }
    if (!callback_completed) {
      break;
    }
    pending_edits_.pop_front();
  }
  return have_result ? first_result : S_OK;
}

bool TextService::IsPendingEditCurrent(
    const PendingEditRequest &request) const noexcept {
  return active_ && request.context_epoch == context_epoch_ &&
         SameComIdentity(request.context.Get(), focused_context_.Get());
}

bool TextService::ShouldRetireFailedEdit(
    const PendingEditRequest &request) const noexcept {
  if (IsPendingEditCurrent(request)) {
    return false;
  }
  if (request.operation == EditOperation::kUpdate) {
    return !IsPendingEditIrreversible(request) ||
           request.failure_count >= kMaximumStaleEditFailures;
  }
  if (request.operation == EditOperation::kSplit) {
    // A detached host can permanently reject every recovery edit session.
    // Retire after the stale budget so one unmaterialized remainder cannot
    // starve the newly focused context indefinitely.
    return request.failure_count >= kMaximumStaleEditFailures;
  }
  return request.failure_count >= kMaximumStaleEditFailures;
}

bool TextService::CurrentEditFailureBudgetExhausted() const noexcept {
  return !pending_edits_.empty() &&
         IsPendingEditCurrent(pending_edits_.front()) &&
         pending_edits_.front().failure_count >=
             kMaximumCurrentEditFailures;
}

bool TextService::CanRecoverFaultedEditWithKey(WPARAM key) const noexcept {
  if (!CurrentEditFailureBudgetExhausted() ||
      in_flight_edit_sequence_.has_value() || pending_edits_.empty() ||
      pending_edits_.front().operation != EditOperation::kUpdate) {
    return false;
  }
  const core::TextSelection selection = buffer_.selection();
  if (!selection.empty()) {
    return key == VK_BACK || key == VK_DELETE;
  }
  if (key == VK_BACK) {
    return selection.caret != 0;
  }
  return key == VK_DELETE && selection.caret < buffer_.text().size();
}

bool TextService::IsPendingEditIrreversible(
    const PendingEditRequest &request) const noexcept {
  return request.phase != PendingEditPhase::kBeforeWrite &&
         request.phase != PendingEditPhase::kBeforeClear;
}

bool TextService::IsShrinkingRecoveryUpdate(
    ITfContext *context, EditOperation operation,
    const CompositionSnapshot &snapshot) const noexcept {
  return operation == EditOperation::kUpdate &&
         !in_flight_edit_sequence_.has_value() && !pending_edits_.empty() &&
         pending_edits_.front().operation == EditOperation::kUpdate &&
         IsPendingEditCurrent(pending_edits_.front()) &&
         SameComIdentity(context, pending_edits_.front().context.Get()) &&
         snapshot.text.size() < pending_edits_.front().snapshot.text.size();
}

void TextService::RecordPendingEditFailure(
    PendingEditRequest &request) noexcept {
  if (request.failure_count < (std::numeric_limits<std::size_t>::max)()) {
    ++request.failure_count;
  }
}

HRESULT TextService::RecoverFaultedCurrentEditWithEscape(ITfContext *context) {
  if (context == nullptr || in_flight_edit_sequence_.has_value() ||
      pending_edits_.empty() || !IsPendingEditCurrent(pending_edits_.front())) {
    return E_PENDING;
  }

  PendingEditRequest &initial_request = pending_edits_.front();
  ComPtr<ITfComposition> recovery;
  if (initial_request.phase == PendingEditPhase::kEndedAwaitRemainder &&
      composition_ && SameComIdentity(context, composition_context_.Get())) {
    recovery = composition_;
  } else if (initial_request.captured_composition) {
    recovery = initial_request.captured_composition;
  } else {
    recovery = composition_;
  }

  const std::uint64_t sequence = initial_request.sequence;
  const std::uint64_t epoch = initial_request.context_epoch;
  ComPtr<ITfContext> request_context = initial_request.context;
  auto dependent = std::next(pending_edits_.begin());
  while (dependent != pending_edits_.end()) {
    if (dependent->context_epoch == epoch &&
        SameComIdentity(dependent->context.Get(), request_context.Get())) {
      dependent = pending_edits_.erase(dependent);
    } else {
      ++dependent;
    }
  }

  PendingEditRequest *recovered_request = FindPendingEditRequest(sequence);
  if (recovered_request == nullptr) {
    return E_UNEXPECTED;
  }
  PendingEditRequest &request = *recovered_request;
  request.operation = EditOperation::kCancel;
  request.snapshot = {};
  request.invalidated = false;
  request.failure_count = 0;
  request.generation = ++composition_generation_;
  request.written_payload = PendingEditPayload::kNone;
  if (request.phase != PendingEditPhase::kClearedAwaitEnd) {
    request.phase = PendingEditPhase::kBeforeClear;
  }
  request.captured_composition = recovery;
  buffer_.Clear();
  selection_dirty_ = false;
  if (recovery) {
    composition_ = recovery;
    composition_context_.Assign(context);
  }

  std::uint64_t failed_sequence = 0;
  const HRESULT result = DispatchPendingEdits(&failed_sequence);
  const PendingEditRequest *remaining = FindPendingEditRequest(sequence);
  if (FAILED(result) && remaining != nullptr &&
      IsPendingEditIrreversible(*remaining)) {
    return S_OK;
  }
  return result;
}

TextService::PendingEditRequest *
TextService::FindPendingEditRequest(std::uint64_t sequence) noexcept {
  const auto request = std::find_if(
      pending_edits_.begin(), pending_edits_.end(),
      [&](const PendingEditRequest &candidate) {
        return candidate.sequence == sequence;
      });
  return request == pending_edits_.end() ? nullptr : &*request;
}

const TextService::PendingEditRequest *
TextService::FindPendingEditRequest(std::uint64_t sequence) const noexcept {
  const auto request = std::find_if(
      pending_edits_.begin(), pending_edits_.end(),
      [&](const PendingEditRequest &candidate) {
        return candidate.sequence == sequence;
      });
  return request == pending_edits_.end() ? nullptr : &*request;
}

CompositionSnapshot TextService::BuildLatestSplitRemainder(
    const PendingEditRequest &request) const {
  CompositionSnapshot latest = request.snapshot;
  std::u16string committed_segments;
  bool after_request = false;
  for (const PendingEditRequest &candidate : pending_edits_) {
    if (!after_request) {
      after_request = candidate.sequence == request.sequence;
      continue;
    }
    if (candidate.invalidated || candidate.context_epoch != request.context_epoch ||
        !SameComIdentity(candidate.context.Get(), request.context.Get())) {
      continue;
    }
    switch (candidate.operation) {
    case EditOperation::kUpdate:
      latest.text = candidate.snapshot.text;
      latest.selection = candidate.snapshot.selection;
      break;
    case EditOperation::kSplit:
      committed_segments.append(candidate.snapshot.committed_prefix);
      latest.text = candidate.snapshot.text;
      latest.selection = candidate.snapshot.selection;
      break;
    case EditOperation::kCommit:
      committed_segments.append(candidate.snapshot.text);
      latest.text.clear();
      latest.selection = {};
      break;
    case EditOperation::kCancel:
      latest.text.clear();
      latest.selection = {};
      break;
    }
  }
  latest.text.insert(0, committed_segments);
  latest.selection = {latest.text.size(), latest.text.size()};
  latest.committed_prefix.clear();
  return latest;
}

bool TextService::HasDependentEdit(
    const PendingEditRequest &request) const noexcept {
  bool after_request = false;
  for (const PendingEditRequest &candidate : pending_edits_) {
    if (!after_request) {
      after_request = candidate.sequence == request.sequence;
      continue;
    }
    if (!candidate.invalidated &&
        candidate.context_epoch == request.context_epoch &&
        SameComIdentity(candidate.context.Get(), request.context.Get())) {
      return true;
    }
  }
  return false;
}

void TextService::InvalidateDependentEdits(
    const PendingEditRequest &request) noexcept {
  bool after_request = false;
  for (PendingEditRequest &candidate : pending_edits_) {
    if (!after_request) {
      after_request = candidate.sequence == request.sequence;
      continue;
    }
    if (candidate.context_epoch == request.context_epoch &&
        SameComIdentity(candidate.context.Get(), request.context.Get())) {
      candidate.invalidated = true;
    }
  }
}

bool TextService::PreserveSplitRemaindersBeforeDroppingUpdates() noexcept {
  try {
    for (PendingEditRequest &request : pending_edits_) {
      if (request.invalidated || request.operation != EditOperation::kSplit ||
          request.phase == PendingEditPhase::kDone) {
        continue;
      }
      CompositionSnapshot latest = BuildLatestSplitRemainder(request);
      request.snapshot.text = std::move(latest.text);
      request.snapshot.selection = latest.selection;
      InvalidateDependentEdits(request);
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool TextService::PendingBarrierOwnsComposition(
    ITfComposition *composition) const noexcept {
  if (composition == nullptr) {
    return false;
  }
  return std::any_of(
      pending_edits_.begin(), pending_edits_.end(),
      [&](const PendingEditRequest &request) {
        const bool owns_barrier = request.operation != EditOperation::kUpdate ||
                                  IsPendingEditIrreversible(request);
        return !request.invalidated && owns_barrier &&
               request.phase != PendingEditPhase::kDone &&
               SameComIdentity(request.captured_composition.Get(),
                               composition);
      });
}

void TextService::CompleteEditSession(std::uint64_t sequence,
                                      HRESULT result) noexcept {
  if (!in_flight_edit_sequence_.has_value() ||
      *in_flight_edit_sequence_ != sequence) {
    return;
  }
  in_flight_edit_sequence_.reset();
  if (dispatching_edit_session_) {
    return;
  }
  if (pending_edits_.empty() || pending_edits_.front().sequence != sequence) {
    return;
  }
  if (pending_edits_.front().invalidated) {
    pending_edits_.pop_front();
    (void)ComBoundary([&]() {
      std::uint64_t failed_sequence = 0;
      return DispatchPendingEdits(&failed_sequence);
    });
    return;
  }
  if (FAILED(result)) {
    RecordPendingEditFailure(pending_edits_.front());
    const bool request_is_current =
        IsPendingEditCurrent(pending_edits_.front());
    if (ShouldRetireFailedEdit(pending_edits_.front())) {
      pending_edits_.pop_front();
    }
    if (!request_is_current) {
      (void)ComBoundary([&]() {
        std::uint64_t failed_sequence = 0;
        return DispatchPendingEdits(&failed_sequence);
      });
    }
    return;
  }
  pending_edits_.pop_front();
  (void)ComBoundary([&]() {
    std::uint64_t failed_sequence = 0;
    return DispatchPendingEdits(&failed_sequence);
  });
}

HRESULT TextService::RunEditSession(
    TfEditCookie cookie, ITfContext *context, std::uint64_t sequence,
    std::uint64_t context_epoch, std::uint64_t generation,
    EditOperation, const CompositionSnapshot &snapshot,
    ITfComposition *) noexcept {
  return ComBoundary([&]() -> HRESULT {
    if (context == nullptr) {
      return E_INVALIDARG;
    }
    PendingEditRequest *request = FindPendingEditRequest(sequence);
    if (request == nullptr || request->invalidated) {
      return S_FALSE;
    }
    if (!active_ && request->operation == EditOperation::kUpdate &&
        !IsPendingEditIrreversible(*request)) {
      return S_FALSE;
    }
    if (request->operation == EditOperation::kCancel) {
      const HRESULT result = CancelComposition(cookie, *request);
      if (FAILED(result) && request->captured_composition && active_ &&
          SameComIdentity(context, focused_context_.Get()) && !composition_) {
        composition_ = request->captured_composition;
        composition_context_.Assign(context);
      }
      return result;
    }

    const bool context_is_current =
        active_ && context_epoch == context_epoch_ &&
        SameComIdentity(context, focused_context_.Get());
    const bool operation_is_current =
        context_is_current && generation == composition_generation_;
    if (request->operation == EditOperation::kUpdate &&
        request->phase == PendingEditPhase::kCompositionStartedAwaitWrite &&
        !context_is_current) {
      request->operation = EditOperation::kCancel;
      request->snapshot = {};
      request->phase = PendingEditPhase::kBeforeClear;
      request->written_payload = PendingEditPayload::kNone;
      return CancelComposition(cookie, *request);
    }
    if (request->operation == EditOperation::kUpdate &&
        !operation_is_current && !IsPendingEditIrreversible(*request)) {
      return S_FALSE;
    }

    if (operation_is_current) {
      UpdateSensitiveScope(cookie, context);
    }
    switch (request->operation) {
    case EditOperation::kUpdate:
      return ApplySnapshot(cookie, context, snapshot, sequence);
    case EditOperation::kCommit:
      return CommitSnapshot(cookie, context, *request, operation_is_current,
                            context_is_current);
    case EditOperation::kSplit:
      return CommitSnapshot(cookie, context, *request, operation_is_current,
                            context_is_current);
    case EditOperation::kCancel:
      return S_OK;
    }
    return E_UNEXPECTED;
  });
}

HRESULT TextService::StartComposition(TfEditCookie cookie, ITfContext *context,
                                      ITfComposition **composition) {
  if (context == nullptr || composition == nullptr) {
    return E_INVALIDARG;
  }
  *composition = nullptr;
  ComPtr<ITfContextComposition> context_composition;
  HRESULT result = context->QueryInterface(
      IID_ITfContextComposition,
      reinterpret_cast<void **>(context_composition.Put()));
  if (FAILED(result)) {
    return result;
  }

  TF_SELECTION selection{};
  ULONG fetched = 0;
  result = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                 &fetched);
  if (FAILED(result)) {
    return result;
  }
  ComPtr<ITfRange> range;
  if (fetched == 1) {
    range.Attach(selection.range);
  }
  if (!range) {
    return E_FAIL;
  }

  result = context_composition->StartComposition(
      cookie, range.Get(), static_cast<ITfCompositionSink *>(this),
      composition);
  if (FAILED(result) && *composition != nullptr) {
    (*composition)->Release();
    *composition = nullptr;
  }
  if (SUCCEEDED(result) && *composition == nullptr) {
    return E_FAIL;
  }
  return result;
}

HRESULT TextService::EnsureComposition(TfEditCookie cookie, ITfContext *context,
                                       PendingEditRequest &request,
                                       bool *composition_started) {
  if (composition_started == nullptr) {
    return E_POINTER;
  }
  *composition_started = false;
  if (composition_) {
    return SameComIdentity(context, composition_context_.Get()) ? S_OK
                                                                : E_UNEXPECTED;
  }
  ITfComposition *new_composition = nullptr;
  const HRESULT result = StartComposition(cookie, context, &new_composition);
  if (SUCCEEDED(result)) {
    composition_.Attach(new_composition);
    composition_context_.Assign(context);
    selection_dirty_ = false;
    request.captured_composition = composition_;
    request.phase = PendingEditPhase::kCompositionStartedAwaitWrite;
    *composition_started = true;
  }
  return result;
}

HRESULT TextService::RollBackStartedCompositionAfterWriteFailure(
    TfEditCookie cookie, PendingEditRequest &request,
    HRESULT write_failure) noexcept {
  if (request.phase != PendingEditPhase::kCompositionStartedAwaitWrite ||
      !request.captured_composition) {
    return write_failure;
  }

  ComPtr<ITfComposition> started = request.captured_composition;
  intentionally_ending_composition_.Assign(started.Get());
  const HRESULT end_result = started->EndComposition(cookie);
  if (FAILED(end_result)) {
    intentionally_ending_composition_.Reset();
    return end_result;
  }

  intentionally_ending_composition_.Reset();
  ReleaseCompositionIfSame(started.Get());
  request.captured_composition.Reset();
  request.phase = PendingEditPhase::kBeforeWrite;
  request.written_payload = PendingEditPayload::kNone;
  return write_failure;
}

HRESULT TextService::SetCompositionTextOn(TfEditCookie cookie,
                                           ITfContext *context,
                                           ITfComposition *composition,
                                           const CompositionSnapshot &snapshot) {
  if (composition == nullptr) {
    return E_INVALIDARG;
  }
  if (snapshot.text.size() >
      static_cast<std::size_t>((std::numeric_limits<LONG>::max)())) {
    return E_INVALIDARG;
  }
  const std::size_t end = snapshot.selection.end();
  if (end > snapshot.text.size()) {
    return E_INVALIDARG;
  }
  ComPtr<ITfRange> composition_range;
  HRESULT result = composition->GetRange(composition_range.Put());
  if (FAILED(result) || !composition_range) {
    return FAILED(result) ? result : E_FAIL;
  }
  const auto *text = reinterpret_cast<const wchar_t *>(snapshot.text.data());
  result = composition_range->SetText(cookie, 0, text,
                                      static_cast<LONG>(snapshot.text.size()));
  if (FAILED(result)) {
    return result;
  }

  // SetText is the transaction boundary. Retry selection from a fresh range;
  // reporting a later failure would expose an already-written key to the host
  // while rolling back only the internal buffer.
  result = SetCompositionSelectionOn(cookie, context, composition_range.Get(),
                                     snapshot.selection, snapshot.text.size());
  if (FAILED(result)) {
    result = SetCompositionSelectionOn(
        cookie, context, composition_range.Get(), snapshot.selection,
        snapshot.text.size());
  }
  return SUCCEEDED(result) ? S_OK : S_FALSE;
}

HRESULT TextService::SetCompositionSelectionOn(
    TfEditCookie cookie, ITfContext *context, ITfRange *composition_range,
    const core::TextSelection &selection, std::size_t text_size) {
  if (context == nullptr || composition_range == nullptr ||
      selection.end() > text_size) {
    return E_INVALIDARG;
  }
  ComPtr<ITfRange> selection_range;
  HRESULT result = composition_range->Clone(selection_range.Put());
  if (FAILED(result) || !selection_range) {
    return FAILED(result) ? result : E_FAIL;
  }
  result = selection_range->Collapse(cookie, TF_ANCHOR_START);
  if (FAILED(result)) {
    return result;
  }
  LONG shifted = 0;
  const std::size_t end = selection.end();
  result = selection_range->ShiftEnd(cookie, static_cast<LONG>(end), &shifted,
                                     nullptr);
  if (FAILED(result) || shifted != static_cast<LONG>(end)) {
    return FAILED(result) ? result : E_FAIL;
  }
  const std::size_t begin = selection.begin();
  result = selection_range->ShiftStart(cookie, static_cast<LONG>(begin),
                                       &shifted, nullptr);
  if (FAILED(result) || shifted != static_cast<LONG>(begin)) {
    return FAILED(result) ? result : E_FAIL;
  }

  TF_SELECTION value{};
  value.range = selection_range.Get();
  value.style.ase = selection.anchor <= selection.caret ? TF_AE_END
                                                        : TF_AE_START;
  value.style.fInterimChar = FALSE;
  return context->SetSelection(cookie, 1, &value);
}

HRESULT TextService::ReadCompositionText(TfEditCookie cookie,
                                         ITfComposition *composition,
                                         std::u16string *text) {
  if (composition == nullptr || text == nullptr) {
    return E_INVALIDARG;
  }
  ComPtr<ITfRange> range;
  HRESULT result = composition->GetRange(range.Put());
  if (FAILED(result) || !range) {
    return FAILED(result) ? result : E_FAIL;
  }
  ComPtr<ITfRange> cursor;
  result = range->Clone(cursor.Put());
  if (FAILED(result) || !cursor) {
    return FAILED(result) ? result : E_FAIL;
  }

  text->clear();
  std::array<WCHAR, 256> chunk{};
  const std::size_t maximum_units = buffer_.limits().hard_units;
  for (;;) {
    const std::size_t remaining = maximum_units - text->size();
    const std::size_t requested_size =
        remaining == 0 ? std::size_t{1}
                       : (std::min)(chunk.size(), remaining);
    ULONG copied = 0;
    result = cursor->GetText(cookie, TF_TF_MOVESTART, chunk.data(),
                             static_cast<ULONG>(requested_size), &copied);
    if (FAILED(result) || copied > requested_size) {
      return FAILED(result) ? result : E_FAIL;
    }
    if (copied > remaining) {
      return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }
    text->append(reinterpret_cast<const char16_t *>(chunk.data()), copied);
    if (copied < requested_size) {
      return S_OK;
    }
  }
}

HRESULT TextService::SetCompositionText(TfEditCookie cookie,
                                        ITfContext *context,
                                        const CompositionSnapshot &snapshot,
                                        PendingEditRequest &request) {
  bool composition_started = false;
  const HRESULT result =
      EnsureComposition(cookie, context, request, &composition_started);
  if (FAILED(result)) {
    return result;
  }
  const HRESULT write_result =
      SetCompositionTextOn(cookie, context, composition_.Get(), snapshot);
  if (FAILED(write_result)) {
    return composition_started
               ? RollBackStartedCompositionAfterWriteFailure(
                     cookie, request, write_result)
               : write_result;
  }
  if (write_result == S_OK) {
    selection_dirty_ = false;
    request.phase = PendingEditPhase::kDone;
    return S_OK;
  }

  ComPtr<ITfComposition> completed = composition_;
  intentionally_ending_composition_.Assign(completed.Get());
  const HRESULT end_result = completed->EndComposition(cookie);
  if (FAILED(end_result)) {
    intentionally_ending_composition_.Reset();
    selection_dirty_ = true;
    buffer_.Clear();
    ++composition_generation_;
    request.phase = PendingEditPhase::kDone;
    return S_OK;
  }
  intentionally_ending_composition_.Reset();
  ReleaseCompositionIfSame(completed.Get());
  buffer_.Clear();
  ++composition_generation_;
  request.phase = PendingEditPhase::kDone;
  return S_OK;
}

HRESULT TextService::ApplySnapshot(TfEditCookie cookie, ITfContext *context,
                                   const CompositionSnapshot &snapshot,
                                   std::uint64_t sequence) {
  PendingEditRequest *request = FindPendingEditRequest(sequence);
  if (request == nullptr) {
    return S_FALSE;
  }
  if (snapshot.text.empty()) {
    return CancelComposition(cookie, *request);
  }
  return SetCompositionText(cookie, context, snapshot, *request);
}

HRESULT TextService::CommitSnapshot(TfEditCookie cookie, ITfContext *context,
                                    PendingEditRequest &request,
                                    bool operation_is_current,
                                    bool context_is_current) {
  const bool start_remainder = request.operation == EditOperation::kSplit;

  for (;;) {
    switch (request.phase) {
    case PendingEditPhase::kBeforeWrite:
    case PendingEditPhase::kCompositionStartedAwaitWrite: {
      CompositionSnapshot committed;
      PendingEditPayload payload = PendingEditPayload::kWhole;
      if (start_remainder) {
        committed.text = request.snapshot.committed_prefix;
        if (context_is_current) {
          payload = PendingEditPayload::kPrefix;
        } else {
          CompositionSnapshot remainder = BuildLatestSplitRemainder(request);
          const std::size_t hard_units = buffer_.limits().hard_units;
          if (committed.text.size() > hard_units ||
              remainder.text.size() > hard_units - committed.text.size()) {
            request.invalidated = true;
            InvalidateDependentEdits(request);
            return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
          }
          committed.text.append(remainder.text);
        }
      } else {
        committed.text = request.snapshot.text;
      }
      committed.selection = {committed.text.size(), committed.text.size()};

      ComPtr<ITfComposition> completed = request.captured_composition;
      bool composition_started = false;
      if (!completed && composition_ &&
          SameComIdentity(context, composition_context_.Get()) &&
          !PendingBarrierOwnsComposition(composition_.Get())) {
        completed = composition_;
        request.captured_composition = completed;
      }
      if (!completed) {
        ITfComposition *temporary = nullptr;
        const HRESULT result = StartComposition(cookie, context, &temporary);
        if (FAILED(result)) {
          return result;
        }
        completed.Attach(temporary);
        request.captured_composition = completed;
        request.phase = PendingEditPhase::kCompositionStartedAwaitWrite;
        composition_started = true;
      }
      if (!committed.text.empty()) {
        const HRESULT result =
            SetCompositionTextOn(cookie, context, completed.Get(), committed);
        if (FAILED(result)) {
          if (active_ && SameComIdentity(context, focused_context_.Get()) &&
              !composition_) {
            composition_ = completed;
            composition_context_.Assign(context);
          }
          return composition_started
                     ? RollBackStartedCompositionAfterWriteFailure(
                           cookie, request, result)
                     : result;
        }
        if (request.invalidated) {
          return S_FALSE;
        }
      }
      request.phase = PendingEditPhase::kTextWrittenAwaitEnd;
      request.written_payload = payload;
      if (start_remainder && payload == PendingEditPayload::kWhole) {
        InvalidateDependentEdits(request);
      }
      continue;
    }

    case PendingEditPhase::kTextWrittenAwaitEnd: {
      ComPtr<ITfComposition> completed = request.captured_composition;
      if (!completed) {
        return E_UNEXPECTED;
      }

      if (start_remainder &&
          request.written_payload == PendingEditPayload::kPrefix &&
          !context_is_current) {
        CompositionSnapshot whole;
        const HRESULT read_result =
            ReadCompositionText(cookie, completed.Get(), &whole.text);
        if (FAILED(read_result)) {
          if (read_result == HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW)) {
            request.invalidated = true;
            InvalidateDependentEdits(request);
          }
          return read_result;
        }
        CompositionSnapshot remainder = BuildLatestSplitRemainder(request);
        const std::size_t hard_units = buffer_.limits().hard_units;
        if (whole.text.size() > hard_units ||
            remainder.text.size() > hard_units - whole.text.size()) {
          request.invalidated = true;
          InvalidateDependentEdits(request);
          return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        }
        whole.text.append(remainder.text);
        whole.selection = {whole.text.size(), whole.text.size()};
        const HRESULT result =
            SetCompositionTextOn(cookie, context, completed.Get(), whole);
        if (FAILED(result)) {
          return result;
        }
        if (request.invalidated) {
          return S_FALSE;
        }
        request.written_payload = PendingEditPayload::kWhole;
        InvalidateDependentEdits(request);
      }

      intentionally_ending_composition_.Assign(completed.Get());
      const HRESULT end_result = completed->EndComposition(cookie);
      if (FAILED(end_result)) {
        intentionally_ending_composition_.Reset();
        if (active_ && SameComIdentity(context, focused_context_.Get()) &&
            !composition_) {
          composition_ = completed;
          composition_context_.Assign(context);
        }
        return end_result;
      }
      intentionally_ending_composition_.Reset();
      ReleaseCompositionIfSame(completed.Get());
      request.captured_composition.Reset();
      if (start_remainder &&
          request.written_payload == PendingEditPayload::kPrefix) {
        request.phase = PendingEditPhase::kEndedAwaitRemainder;
        request.written_payload = PendingEditPayload::kNone;
        continue;
      }
      request.phase = PendingEditPhase::kDone;
      return S_OK;
    }

    case PendingEditPhase::kEndedAwaitRemainder: {
      CompositionSnapshot remainder = BuildLatestSplitRemainder(request);
      if (remainder.text.empty()) {
        request.phase = PendingEditPhase::kDone;
        return S_OK;
      }
      if (remainder.text.size() > buffer_.limits().hard_units) {
        request.invalidated = true;
        InvalidateDependentEdits(request);
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
      }
      if (operation_is_current ||
          (context_is_current && !HasDependentEdit(request))) {
        const HRESULT result =
            ApplySnapshot(cookie, context, remainder, request.sequence);
        if (FAILED(result) && composition_ &&
            SameComIdentity(context, composition_context_.Get())) {
          request.captured_composition = composition_;
        }
        if (SUCCEEDED(result)) {
          request.phase = PendingEditPhase::kDone;
        }
        return result;
      }
      if (context_is_current) {
        request.phase = PendingEditPhase::kDone;
        return S_OK;
      }

      ComPtr<ITfComposition> remainder_composition =
          request.captured_composition;
      if (!remainder_composition) {
        ITfComposition *temporary = nullptr;
        const HRESULT result = StartComposition(cookie, context, &temporary);
        if (FAILED(result)) {
          return result;
        }
        remainder_composition.Attach(temporary);
        request.captured_composition = remainder_composition;
      }
      const HRESULT result = SetCompositionTextOn(
          cookie, context, remainder_composition.Get(), remainder);
      if (FAILED(result)) {
        return result;
      }
      if (request.invalidated) {
        return S_FALSE;
      }
      request.phase = PendingEditPhase::kTextWrittenAwaitEnd;
      request.written_payload = PendingEditPayload::kWhole;
      InvalidateDependentEdits(request);
      continue;
    }

    case PendingEditPhase::kDone:
      return S_OK;
    case PendingEditPhase::kBeforeClear:
    case PendingEditPhase::kClearedAwaitEnd:
      return E_UNEXPECTED;
    }
  }
}

HRESULT TextService::CancelComposition(TfEditCookie cookie,
                                       PendingEditRequest &request) noexcept {
  if (request.phase == PendingEditPhase::kDone) {
    return S_OK;
  }
  if (request.phase != PendingEditPhase::kBeforeClear &&
      request.phase != PendingEditPhase::kClearedAwaitEnd) {
    return E_UNEXPECTED;
  }

  if (!request.captured_composition && composition_ &&
      SameComIdentity(request.context.Get(), composition_context_.Get())) {
    request.captured_composition = composition_;
  }
  ComPtr<ITfComposition> composition = request.captured_composition;
  if (!composition) {
    request.phase = PendingEditPhase::kDone;
    return S_OK;
  }

  if (request.phase == PendingEditPhase::kBeforeClear) {
    ComPtr<ITfRange> range;
    HRESULT result = composition->GetRange(range.Put());
    if (SUCCEEDED(result)) {
      result = range->SetText(cookie, 0, L"", 0);
    }
    if (FAILED(result)) {
      return result;
    }
    if (request.invalidated) {
      return S_FALSE;
    }
    request.phase = PendingEditPhase::kClearedAwaitEnd;
  }

  intentionally_ending_composition_.Assign(composition.Get());
  const HRESULT end_result = composition->EndComposition(cookie);
  if (FAILED(end_result)) {
    intentionally_ending_composition_.Reset();
    return end_result;
  }
  intentionally_ending_composition_.Reset();
  ReleaseCompositionIfSame(composition.Get());
  request.captured_composition.Reset();
  request.phase = PendingEditPhase::kDone;
  return S_OK;
}

void TextService::ReleaseCompositionIfSame(
    ITfComposition *composition) noexcept {
  if (composition != nullptr &&
      SameComIdentity(composition, composition_.Get())) {
    composition_.Reset();
    composition_context_.Reset();
    selection_dirty_ = false;
  }
}

std::optional<std::size_t> TextService::FindExplicitStableSplit(
    std::u16string_view text, std::size_t minimum_split,
    std::size_t preferred_split, std::size_t maximum_split) noexcept {
  std::optional<std::size_t> best;
  std::size_t best_distance = (std::numeric_limits<std::size_t>::max)();
  const std::size_t limit = (std::min)(maximum_split, text.size());
  for (std::size_t index = 0; index < limit; ++index) {
    const bool has_pinyin_on_left =
        index != 0 && text[index - 1] >= u'a' && text[index - 1] <= u'z';
    const bool has_pinyin_on_right =
        index + 1 < text.size() && text[index + 1] >= u'a' &&
        text[index + 1] <= u'z';
    if (text[index] != u'\'' || !has_pinyin_on_left ||
        !has_pinyin_on_right) {
      continue;
    }
    const std::size_t boundary = index + 1;
    if (boundary < minimum_split || boundary >= text.size()) {
      continue;
    }
    const std::size_t distance = boundary > preferred_split
                                     ? boundary - preferred_split
                                     : preferred_split - boundary;
    if (!best.has_value() || distance < best_distance ||
        (distance == best_distance && boundary > *best)) {
      best = boundary;
      best_distance = distance;
    }
  }
  return best;
}

HRESULT
TextService::OnCompositionTerminated(TfEditCookie,
                                     ITfComposition *composition) noexcept {
  return ComBoundary([&]() -> HRESULT {
    if (composition == nullptr) {
      return E_INVALIDARG;
    }
    if (SameComIdentity(composition, intentionally_ending_composition_.Get())) {
      intentionally_ending_composition_.Reset();
      ReleaseCompositionIfSame(composition);
      return S_OK;
    }
    bool invalidated_barrier = false;
    std::optional<std::uint64_t> invalidated_split_sequence;
    std::uint64_t invalidated_split_epoch = 0;
    ComPtr<ITfContext> invalidated_split_context;
    for (PendingEditRequest &request : pending_edits_) {
      if ((request.operation == EditOperation::kUpdate &&
           !IsPendingEditIrreversible(request)) ||
          request.invalidated ||
          !SameComIdentity(request.captured_composition.Get(), composition)) {
        continue;
      }
      request.invalidated = true;
      request.captured_composition.Reset();
      invalidated_barrier = true;
      if (request.operation == EditOperation::kSplit &&
          (!invalidated_split_sequence.has_value() ||
           request.sequence < *invalidated_split_sequence)) {
        invalidated_split_sequence = request.sequence;
        invalidated_split_epoch = request.context_epoch;
        invalidated_split_context = request.context;
      }
    }
    if (invalidated_split_sequence.has_value()) {
      for (PendingEditRequest &request : pending_edits_) {
        if (request.sequence > *invalidated_split_sequence &&
            request.context_epoch == invalidated_split_epoch &&
            SameComIdentity(request.context.Get(),
                            invalidated_split_context.Get())) {
          request.invalidated = true;
        }
      }
      buffer_.Clear();
      ++composition_generation_;
    }
    if (invalidated_barrier) {
      ReleaseCompositionIfSame(composition);
      std::uint64_t failed_sequence = 0;
      (void)DispatchPendingEdits(&failed_sequence);
      return S_OK;
    }
    if (SameComIdentity(composition, composition_.Get())) {
      ReleaseCompositionIfSame(composition);
      buffer_.Clear();
      ++composition_generation_;
    }
    return S_OK;
  });
}

HRESULT TextService::SetFocusedDocument(ITfDocumentMgr *document_manager) {
  if (SameComIdentity(document_manager, focused_document_.Get())) {
    if (document_manager == nullptr) {
      return S_OK;
    }
    ComPtr<ITfContext> top;
    const HRESULT result = document_manager->GetTop(top.Put());
    return FAILED(result) ? result : AdoptContext(top.Get());
  }
  AbandonContext(true);
  focused_document_.Assign(document_manager);
  if (document_manager == nullptr) {
    return S_OK;
  }
  ComPtr<ITfContext> context;
  const HRESULT result = document_manager->GetTop(context.Put());
  if (FAILED(result)) {
    return result;
  }
  return AdoptContext(context.Get());
}

HRESULT TextService::AdoptContext(ITfContext *context) {
  if (SameComIdentity(context, focused_context_.Get())) {
    learning_context_available_.store(
        active_ && foreground_ && context != nullptr && !secure_activation_);
    return S_OK;
  }
  AbandonContext(true);
  focused_context_.Assign(context);
  sensitive_context_.store(true);
  learning_context_available_.store(active_ && foreground_ &&
                                    context != nullptr && !secure_activation_);
  return S_OK;
}

void TextService::AbandonContext(bool request_cancel) noexcept {
  ++context_epoch_;
  learning_context_available_.store(false);
  sensitive_context_.store(true);
  try {
    ComPtr<ITfContext> old_context = focused_context_;
    ComPtr<ITfComposition> old_composition = composition_;
    const bool composition_has_pending_barrier =
        PendingBarrierOwnsComposition(old_composition.Get());
    buffer_.Clear();
    const std::uint64_t generation = ++composition_generation_;
    composition_.Reset();
    composition_context_.Reset();
    selection_dirty_ = false;
    focused_context_.Reset();
    learning_context_available_.store(false);
    sensitive_context_.store(true);
    if (request_cancel && old_context && old_composition &&
        !composition_has_pending_barrier &&
        client_id_ != TF_CLIENTID_NULL) {
      (void)QueueEdit(old_context.Get(), generation, EditOperation::kCancel, {},
                      old_composition.Get(), true);
    }
  } catch (...) {
    composition_.Reset();
    composition_context_.Reset();
    selection_dirty_ = false;
    focused_context_.Reset();
    buffer_.Clear();
    ++composition_generation_;
    learning_context_available_.store(false);
    sensitive_context_.store(true);
  }
}

HRESULT TextService::OnInitDocumentMgr(ITfDocumentMgr *) noexcept {
  return S_OK;
}

HRESULT
TextService::OnUninitDocumentMgr(ITfDocumentMgr *document_manager) noexcept {
  return ComBoundary([&]() -> HRESULT {
    if (SameComIdentity(document_manager, focused_document_.Get())) {
      AbandonContext(true);
      focused_document_.Reset();
    }
    return S_OK;
  });
}

HRESULT TextService::OnSetFocus(ITfDocumentMgr *focused,
                                ITfDocumentMgr *) noexcept {
  return ComBoundary([&]() { return SetFocusedDocument(focused); });
}

HRESULT TextService::OnPushContext(ITfContext *context) noexcept {
  return ComBoundary([&]() -> HRESULT {
    if (context == nullptr) {
      return E_INVALIDARG;
    }
    if (!focused_document_) {
      return S_OK;
    }
    ComPtr<ITfContext> top;
    const HRESULT result = focused_document_->GetTop(top.Put());
    if (FAILED(result)) {
      return result;
    }
    return top && SameComIdentity(top.Get(), context) ? AdoptContext(context)
                                                      : S_OK;
  });
}

HRESULT TextService::OnPopContext(ITfContext *context) noexcept {
  return ComBoundary([&]() -> HRESULT {
    if (SameComIdentity(context, focused_context_.Get())) {
      AbandonContext(true);
      if (focused_document_) {
        ComPtr<ITfContext> top;
        if (SUCCEEDED(focused_document_->GetTop(top.Put())) && top &&
            !SameComIdentity(top.Get(), context)) {
          return AdoptContext(top.Get());
        }
      }
    }
    return S_OK;
  });
}

void TextService::UpdateSensitiveScope(TfEditCookie cookie,
                                       ITfContext *context) noexcept {
  if (secure_activation_) {
    sensitive_context_.store(true);
    return;
  }

  bool sensitive = true;
  ComPtr<ITfProperty> property;
  if (FAILED(context->GetProperty(GUID_PROP_INPUTSCOPE, property.Put())) ||
      !property) {
    sensitive_context_.store(true);
    return;
  }
  TF_SELECTION selection{};
  ULONG fetched = 0;
  if (FAILED(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                   &fetched)) ||
      fetched != 1 || selection.range == nullptr) {
    sensitive_context_.store(true);
    return;
  }
  ComPtr<ITfRange> range;
  range.Attach(selection.range);
  VARIANT value;
  VariantInit(&value);
  const HRESULT value_result = property->GetValue(cookie, range.Get(), &value);
  if (SUCCEEDED(value_result) &&
      (value.vt == VT_EMPTY || value.vt == VT_NULL)) {
    sensitive = false;
  } else if (SUCCEEDED(value_result) && value.vt == VT_UNKNOWN &&
             value.punkVal != nullptr) {
    ComPtr<ITfInputScope> input_scope;
    if (SUCCEEDED(value.punkVal->QueryInterface(
            IID_ITfInputScope, reinterpret_cast<void **>(input_scope.Put())))) {
      InputScope *scopes = nullptr;
      UINT count = 0;
      const HRESULT scopes_result =
          input_scope->GetInputScopes(&scopes, &count);
      if (SUCCEEDED(scopes_result) && (scopes != nullptr || count == 0)) {
        sensitive = false;
        for (UINT index = 0; index < count; ++index) {
          if (IsSensitiveScope(scopes[index])) {
            sensitive = true;
            break;
          }
        }
      }
      CoTaskMemFree(scopes);
    }
  }
  VariantClear(&value);
  sensitive_context_.store(sensitive);
}

bool TextService::LearningAllowed() const noexcept {
  return learning_context_available_.load() && !sensitive_context_.load();
}

} // namespace zrinput::windows::tsf
