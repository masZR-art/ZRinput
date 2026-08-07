#pragma once

#include "core/composition_buffer.h"
#include "windows/tsf/com_support.h"

#include <msctf.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace zrinput::windows::tsf {

enum class EditOperation {
  kUpdate,
  kCommit,
  kCancel,
  kSplit,
};

struct CompositionSnapshot {
  std::u16string text;
  core::TextSelection selection;
  std::u16string committed_prefix;
};

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfThreadMgrEventSink {
public:
  TextService();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override;
  ULONG STDMETHODCALLTYPE AddRef() noexcept override;
  ULONG STDMETHODCALLTYPE Release() noexcept override;

  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr *thread_manager,
                                     TfClientId client_id) noexcept override;
  HRESULT STDMETHODCALLTYPE Deactivate() noexcept override;
  HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr *thread_manager,
                                       TfClientId client_id,
                                       DWORD flags) noexcept override;

  HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) noexcept override;
  HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext *context, WPARAM key,
                                          LPARAM key_data,
                                          BOOL *eaten) noexcept override;
  HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext *context, WPARAM key,
                                        LPARAM key_data,
                                        BOOL *eaten) noexcept override;
  HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext *context, WPARAM key,
                                      LPARAM key_data,
                                      BOOL *eaten) noexcept override;
  HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext *context, WPARAM key,
                                    LPARAM key_data,
                                    BOOL *eaten) noexcept override;
  HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext *context, REFGUID guid,
                                           BOOL *eaten) noexcept override;

  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(
      TfEditCookie cookie, ITfComposition *composition) noexcept override;

  HRESULT STDMETHODCALLTYPE
  OnInitDocumentMgr(ITfDocumentMgr *document_manager) noexcept override;
  HRESULT STDMETHODCALLTYPE
  OnUninitDocumentMgr(ITfDocumentMgr *document_manager) noexcept override;
  HRESULT STDMETHODCALLTYPE OnSetFocus(
      ITfDocumentMgr *focused, ITfDocumentMgr *previous) noexcept override;
  HRESULT STDMETHODCALLTYPE
  OnPushContext(ITfContext *context) noexcept override;
  HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext *context) noexcept override;

  HRESULT RunEditSession(TfEditCookie cookie, ITfContext *context,
                         std::uint64_t sequence, std::uint64_t context_epoch,
                         std::uint64_t generation, EditOperation operation,
                         const CompositionSnapshot &snapshot,
                         ITfComposition *captured_composition) noexcept;
  void CompleteEditSession(std::uint64_t sequence, HRESULT result) noexcept;

  [[nodiscard]] bool LearningAllowed() const noexcept;

private:
  enum class PendingEditPhase {
    kBeforeWrite,
    kCompositionStartedAwaitWrite,
    kTextWrittenAwaitEnd,
    kEndedAwaitRemainder,
    kBeforeClear,
    kClearedAwaitEnd,
    kDone,
  };

  enum class PendingEditPayload {
    kNone,
    kPrefix,
    kWhole,
  };

  enum class QueueEditDisposition {
    kRejectedBeforeMutation,
    kAcceptedPending,
    kCompleted,
  };

  struct QueueEditResult {
    HRESULT result = S_OK;
    QueueEditDisposition disposition = QueueEditDisposition::kCompleted;
  };

  struct PendingEditRequest {
    ComPtr<ITfContext> context;
    TfClientId client_id = TF_CLIENTID_NULL;
    std::uint64_t sequence = 0;
    std::uint64_t context_epoch = 0;
    std::uint64_t generation = 0;
    std::size_t failure_count = 0;
    EditOperation operation = EditOperation::kUpdate;
    bool invalidated = false;
    PendingEditPhase phase = PendingEditPhase::kBeforeWrite;
    PendingEditPayload written_payload = PendingEditPayload::kNone;
    CompositionSnapshot snapshot;
    ComPtr<ITfComposition> captured_composition;
  };

  ~TextService() noexcept;

  HRESULT ActivateImpl(ITfThreadMgr *thread_manager, TfClientId client_id,
                       DWORD flags);
  HRESULT DeactivateImpl() noexcept;
  HRESULT SetFocusedDocument(ITfDocumentMgr *document_manager);
  HRESULT AdoptContext(ITfContext *context);
  void AbandonContext(bool request_cancel) noexcept;
  QueueEditResult QueueEdit(ITfContext *context, std::uint64_t generation,
                            EditOperation operation,
                            CompositionSnapshot snapshot,
                            ITfComposition *captured_composition = nullptr,
                            bool lifecycle_request = false);
  HRESULT DispatchPendingEdits(std::uint64_t *failed_sequence);
  [[nodiscard]] bool
  IsPendingEditCurrent(const PendingEditRequest &request) const noexcept;
  [[nodiscard]] bool
  ShouldRetireFailedEdit(const PendingEditRequest &request) const noexcept;
  [[nodiscard]] bool CurrentEditFailureBudgetExhausted() const noexcept;
  [[nodiscard]] bool
  CanRecoverFaultedEditWithKey(WPARAM key) const noexcept;
  [[nodiscard]] bool IsPendingEditIrreversible(
      const PendingEditRequest &request) const noexcept;
  [[nodiscard]] bool IsShrinkingRecoveryUpdate(
      ITfContext *context, EditOperation operation,
      const CompositionSnapshot &snapshot) const noexcept;
  void RecordPendingEditFailure(PendingEditRequest &request) noexcept;
  HRESULT RecoverFaultedCurrentEditWithEscape(ITfContext *context);
  [[nodiscard]] PendingEditRequest *
  FindPendingEditRequest(std::uint64_t sequence) noexcept;
  [[nodiscard]] const PendingEditRequest *
  FindPendingEditRequest(std::uint64_t sequence) const noexcept;
  [[nodiscard]] CompositionSnapshot BuildLatestSplitRemainder(
      const PendingEditRequest &request) const;
  [[nodiscard]] bool HasDependentEdit(
      const PendingEditRequest &request) const noexcept;
  void InvalidateDependentEdits(const PendingEditRequest &request) noexcept;
  [[nodiscard]] bool
  PreserveSplitRemaindersBeforeDroppingUpdates() noexcept;
  [[nodiscard]] bool PendingBarrierOwnsComposition(
      ITfComposition *composition) const noexcept;
  HRESULT ApplySnapshot(TfEditCookie cookie, ITfContext *context,
                        const CompositionSnapshot &snapshot,
                        std::uint64_t sequence);
  HRESULT CommitSnapshot(TfEditCookie cookie, ITfContext *context,
                         PendingEditRequest &request,
                         bool operation_is_current, bool context_is_current);
  HRESULT CancelComposition(TfEditCookie cookie,
                            PendingEditRequest &request) noexcept;
  HRESULT StartComposition(TfEditCookie cookie, ITfContext *context,
                           ITfComposition **composition);
  HRESULT EnsureComposition(TfEditCookie cookie, ITfContext *context,
                            PendingEditRequest &request,
                            bool *composition_started);
  HRESULT RollBackStartedCompositionAfterWriteFailure(
      TfEditCookie cookie, PendingEditRequest &request,
      HRESULT write_failure) noexcept;
  HRESULT SetCompositionTextOn(TfEditCookie cookie, ITfContext *context,
                               ITfComposition *composition,
                               const CompositionSnapshot &snapshot);
  HRESULT SetCompositionSelectionOn(
      TfEditCookie cookie, ITfContext *context, ITfRange *composition_range,
      const core::TextSelection &selection, std::size_t text_size);
  HRESULT ReadCompositionText(TfEditCookie cookie,
                              ITfComposition *composition,
                              std::u16string *text);
  HRESULT SetCompositionText(TfEditCookie cookie, ITfContext *context,
                             const CompositionSnapshot &snapshot,
                             PendingEditRequest &request);
  [[nodiscard]] bool ShouldEatKey(ITfContext *context,
                                  WPARAM key) const noexcept;
  HRESULT HandleKey(ITfContext *context, WPARAM key, BOOL *eaten);
  HRESULT InsertCharacter(ITfContext *context, char16_t character);
  HRESULT ApplyBufferEdit(ITfContext *context, core::EditOutcome outcome,
                          std::u16string previous_text,
                          core::TextSelection previous_selection);
  HRESULT SplitAndInsert(ITfContext *context, char16_t character,
                         std::u16string previous_text,
                         core::TextSelection previous_selection);
  HRESULT CommitOrCancel(ITfContext *context, bool commit);
  void ReleaseCompositionIfSame(ITfComposition *composition) noexcept;
  [[nodiscard]] static std::optional<std::size_t>
  FindExplicitStableSplit(std::u16string_view text, std::size_t minimum_split,
                          std::size_t preferred_split,
                          std::size_t maximum_split) noexcept;
  void UpdateSensitiveScope(TfEditCookie cookie, ITfContext *context) noexcept;

  static constexpr std::size_t kMaximumPendingEdits = 64;
  static constexpr std::size_t kMaximumLifecyclePendingEdits =
      kMaximumPendingEdits + 8;
  static constexpr std::size_t kMaximumStaleEditFailures = 2;
  static constexpr std::size_t kMaximumCurrentEditFailures = 3;

  std::atomic<ULONG> reference_count_{1};
  ComPtr<ITfThreadMgr> thread_manager_;
  ComPtr<ITfDocumentMgr> focused_document_;
  ComPtr<ITfContext> focused_context_;
  ComPtr<ITfComposition> composition_;
  ComPtr<ITfContext> composition_context_;
  ComPtr<ITfComposition> intentionally_ending_composition_;
  core::CompositionBuffer buffer_;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  DWORD thread_sink_cookie_ = TF_INVALID_COOKIE;
  bool key_sink_advised_ = false;
  std::uint64_t context_epoch_ = 0;
  std::uint64_t composition_generation_ = 0;
  std::uint64_t next_edit_sequence_ = 0;
  std::optional<std::uint64_t> in_flight_edit_sequence_;
  std::optional<std::uint64_t> submitting_edit_sequence_;
  std::deque<PendingEditRequest> pending_edits_;
  bool dispatching_edit_session_ = false;
  bool active_ = false;
  bool foreground_ = false;
  bool selection_dirty_ = false;
  bool secure_activation_ = false;
  std::atomic<bool> learning_context_available_{false};
  std::atomic<bool> sensitive_context_{false};
};

} // namespace zrinput::windows::tsf
