#include "test_harness.h"
#include "windows/tsf/module_state.h"
#include "windows/tsf/text_service.h"
#include "windows/tsf/tsf_guids.h"

#include <inputscope.h>
#include <msctf.h>
#include <olectl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using DllGetClassObjectFn = HRESULT(__stdcall *)(REFCLSID, REFIID, void **);
using DllCanUnloadNowFn = HRESULT(__stdcall *)();
using DllRegistrationFn = HRESULT(__stdcall *)();

HMODULE g_module = nullptr;
DllGetClassObjectFn g_get_class_object = nullptr;
DllCanUnloadNowFn g_can_unload = nullptr;
DllRegistrationFn g_register_server = nullptr;
DllRegistrationFn g_unregister_server = nullptr;

template <typename Function> Function LoadExport(const char *name) {
  return reinterpret_cast<Function>(GetProcAddress(g_module, name));
}

class FakeThreadManager final : public ITfThreadMgr,
                                public ITfSource,
                                public ITfKeystrokeMgr {
public:
  static constexpr TfClientId kClientId = 42;

  explicit FakeThreadManager(bool reject_key_sink = false) noexcept
      : reject_key_sink_(reject_key_sink) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfThreadMgr)) {
      *object = static_cast<ITfThreadMgr *>(this);
    } else if (IsEqualIID(iid, IID_ITfSource)) {
      *object = static_cast<ITfSource *>(this);
    } else if (IsEqualIID(iid, IID_ITfKeystrokeMgr)) {
      *object = static_cast<ITfKeystrokeMgr *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
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

  HRESULT STDMETHODCALLTYPE Activate(TfClientId *client_id) noexcept override {
    if (client_id == nullptr) {
      return E_POINTER;
    }
    *client_id = kClientId;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Deactivate() noexcept override { return S_OK; }
  HRESULT STDMETHODCALLTYPE
  CreateDocumentMgr(ITfDocumentMgr **value) noexcept override {
    return ClearOutput(value);
  }
  HRESULT STDMETHODCALLTYPE
  EnumDocumentMgrs(IEnumTfDocumentMgrs **value) noexcept override {
    return ClearOutput(value);
  }
  HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr **value) noexcept override {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = focused_document_;
    if (*value != nullptr) {
      (*value)->AddRef();
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE AssociateFocus(
      HWND, ITfDocumentMgr *, ITfDocumentMgr **previous) noexcept override {
    return ClearOutput(previous);
  }
  HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL *focused) noexcept override {
    if (focused == nullptr) {
      return E_POINTER;
    }
    *focused = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetFunctionProvider(REFCLSID, ITfFunctionProvider **value) noexcept override {
    return ClearOutput(value);
  }
  HRESULT STDMETHODCALLTYPE
  EnumFunctionProviders(IEnumTfFunctionProviders **value) noexcept override {
    return ClearOutput(value);
  }
  HRESULT STDMETHODCALLTYPE
  GetGlobalCompartment(ITfCompartmentMgr **value) noexcept override {
    return ClearOutput(value);
  }

  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown *sink,
                                       DWORD *cookie) noexcept override {
    if (sink == nullptr || cookie == nullptr) {
      return E_POINTER;
    }
    if (!IsEqualIID(iid, IID_ITfThreadMgrEventSink)) {
      return E_NOINTERFACE;
    }
    if (thread_sink_ != nullptr) {
      return CONNECT_E_ADVISELIMIT;
    }
    HRESULT result = sink->QueryInterface(
        IID_ITfThreadMgrEventSink, reinterpret_cast<void **>(&thread_sink_));
    if (FAILED(result)) {
      return result;
    }
    *cookie = kThreadCookie;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) noexcept override {
    ++thread_unadvise_attempts_;
    if (fail_thread_unadvise_) {
      return E_ACCESSDENIED;
    }
    if (cookie != kThreadCookie || thread_sink_ == nullptr) {
      return CONNECT_E_NOCONNECTION;
    }
    thread_sink_->Release();
    thread_sink_ = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId client_id,
                                               ITfKeyEventSink *sink,
                                               BOOL) noexcept override {
    if (client_id != kClientId || sink == nullptr) {
      return E_INVALIDARG;
    }
    if (reject_key_sink_) {
      return E_ACCESSDENIED;
    }
    if (key_sink_ != nullptr) {
      return CONNECT_E_ADVISELIMIT;
    }
    key_sink_ = sink;
    key_sink_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  UnadviseKeyEventSink(TfClientId client_id) noexcept override {
    ++key_unadvise_attempts_;
    if (fail_key_unadvise_) {
      return E_ACCESSDENIED;
    }
    if (client_id != kClientId || key_sink_ == nullptr) {
      return CONNECT_E_NOCONNECTION;
    }
    key_sink_->Release();
    key_sink_ = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetForeground(CLSID *clsid) noexcept override {
    if (clsid == nullptr) {
      return E_POINTER;
    }
    *clsid = CLSID_NULL;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE TestKeyDown(WPARAM, LPARAM,
                                        BOOL *eaten) noexcept override {
    return NotEaten(eaten);
  }
  HRESULT STDMETHODCALLTYPE TestKeyUp(WPARAM, LPARAM,
                                      BOOL *eaten) noexcept override {
    return NotEaten(eaten);
  }
  HRESULT STDMETHODCALLTYPE KeyDown(WPARAM, LPARAM,
                                    BOOL *eaten) noexcept override {
    return NotEaten(eaten);
  }
  HRESULT STDMETHODCALLTYPE KeyUp(WPARAM, LPARAM,
                                  BOOL *eaten) noexcept override {
    return NotEaten(eaten);
  }
  HRESULT STDMETHODCALLTYPE GetPreservedKey(ITfContext *,
                                            const TF_PRESERVEDKEY *,
                                            GUID *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE IsPreservedKey(REFGUID, const TF_PRESERVEDKEY *,
                                           BOOL *registered) noexcept override {
    if (registered == nullptr) {
      return E_POINTER;
    }
    *registered = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId, REFGUID,
                                        const TF_PRESERVEDKEY *, const WCHAR *,
                                        ULONG) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  UnpreserveKey(REFGUID, const TF_PRESERVEDKEY *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  SetPreservedKeyDescription(REFGUID, const WCHAR *, ULONG) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  GetPreservedKeyDescription(REFGUID, BSTR *description) noexcept override {
    if (description == nullptr) {
      return E_POINTER;
    }
    *description = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  SimulatePreservedKey(ITfContext *, REFGUID, BOOL *eaten) noexcept override {
    return NotEaten(eaten);
  }

  [[nodiscard]] bool HasThreadSink() const noexcept {
    return thread_sink_ != nullptr;
  }
  [[nodiscard]] bool HasKeySink() const noexcept {
    return key_sink_ != nullptr;
  }
  void set_fail_thread_unadvise(bool fail) noexcept {
    fail_thread_unadvise_ = fail;
  }
  void set_fail_key_unadvise(bool fail) noexcept { fail_key_unadvise_ = fail; }
  [[nodiscard]] std::size_t thread_unadvise_attempts() const noexcept {
    return thread_unadvise_attempts_;
  }
  [[nodiscard]] std::size_t key_unadvise_attempts() const noexcept {
    return key_unadvise_attempts_;
  }
  void SetFocusedDocument(ITfDocumentMgr *document) noexcept {
    if (document != nullptr) {
      document->AddRef();
    }
    if (focused_document_ != nullptr) {
      focused_document_->Release();
    }
    focused_document_ = document;
  }

private:
  template <typename Interface>
  static HRESULT ClearOutput(Interface **value) noexcept {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = nullptr;
    return E_NOTIMPL;
  }

  static HRESULT NotEaten(BOOL *eaten) noexcept {
    if (eaten == nullptr) {
      return E_POINTER;
    }
    *eaten = FALSE;
    return S_OK;
  }

  ~FakeThreadManager() noexcept {
    if (key_sink_ != nullptr) {
      key_sink_->Release();
    }
    if (thread_sink_ != nullptr) {
      thread_sink_->Release();
    }
    if (focused_document_ != nullptr) {
      focused_document_->Release();
    }
  }

  static constexpr DWORD kThreadCookie = 7;
  std::atomic<ULONG> reference_count_{1};
  ITfThreadMgrEventSink *thread_sink_ = nullptr;
  ITfKeyEventSink *key_sink_ = nullptr;
  ITfDocumentMgr *focused_document_ = nullptr;
  bool reject_key_sink_ = false;
  bool fail_thread_unadvise_ = false;
  bool fail_key_unadvise_ = false;
  std::size_t thread_unadvise_attempts_ = 0;
  std::size_t key_unadvise_attempts_ = 0;
};

enum class PostWriteFailureStage {
  kNone,
  kClone,
  kCollapse,
  kShiftEnd,
  kShiftStart,
  kSetSelection,
};

struct PostWriteFailureState {
  void Arm(PostWriteFailureStage requested,
           std::size_t failure_count = 1) noexcept {
    stage = requested;
    remaining_failures = failure_count;
    text_was_written = false;
  }

  void RecordTextWrite() noexcept { text_was_written = true; }

  [[nodiscard]] bool Consume(PostWriteFailureStage requested) noexcept {
    if (!text_was_written || stage != requested || remaining_failures == 0) {
      return false;
    }
    --remaining_failures;
    if (remaining_failures == 0) {
      stage = PostWriteFailureStage::kNone;
    }
    return true;
  }

  PostWriteFailureStage stage = PostWriteFailureStage::kNone;
  std::size_t remaining_failures = 0;
  bool text_was_written = false;
};

struct SetTextTraceState {
  std::vector<std::u16string> payloads;
  std::size_t failures_before_write = 0;
};

class FakeRange final : public ITfRange {
public:
  explicit FakeRange(std::shared_ptr<std::u16string> text,
                      std::shared_ptr<PostWriteFailureState> failure_state,
                      std::size_t begin = 0, std::size_t end = 0,
                      std::shared_ptr<SetTextTraceState> set_text_trace = {})
      noexcept
      : text_(std::move(text)), failure_state_(std::move(failure_state)),
        set_text_trace_(std::move(set_text_trace)), begin_(begin), end_(end) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_ITfRange)) {
      return E_NOINTERFACE;
    }
    *object = static_cast<ITfRange *>(this);
    AddRef();
    return S_OK;
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

  HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD flags, WCHAR *text,
                                     ULONG maximum,
                                    ULONG *copied) noexcept override {
    if (copied == nullptr || (maximum != 0 && text == nullptr)) {
      return E_POINTER;
    }
    const std::size_t count =
        (std::min)(static_cast<std::size_t>(maximum), end_ - begin_);
    if (count != 0) {
      std::copy_n(reinterpret_cast<const WCHAR *>(text_->data() + begin_),
                  count, text);
    }
    *copied = static_cast<ULONG>(count);
    if ((flags & TF_TF_MOVESTART) != 0) {
      begin_ += count;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR *text,
                                    LONG count) noexcept override {
    if (count < 0 || (count != 0 && text == nullptr) || begin_ > end_ ||
        end_ > text_->size()) {
      return E_INVALIDARG;
    }
    try {
      const auto *begin = reinterpret_cast<const char16_t *>(text);
      if (set_text_trace_) {
        set_text_trace_->payloads.emplace_back(
            count == 0 ? std::u16string{} : std::u16string(begin, begin + count));
        if (set_text_trace_->failures_before_write != 0) {
          --set_text_trace_->failures_before_write;
          return E_ACCESSDENIED;
        }
      }
      if (count == 0) {
        text_->erase(begin_, end_ - begin_);
      } else {
        text_->replace(begin_, end_ - begin_, begin,
                       static_cast<std::size_t>(count));
      }
      end_ = begin_ + static_cast<std::size_t>(count);
      failure_state_->RecordTextWrite();
      return S_OK;
    } catch (...) {
      return E_OUTOFMEMORY;
    }
  }
  HRESULT STDMETHODCALLTYPE
  GetFormattedText(TfEditCookie, IDataObject **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE GetEmbedded(TfEditCookie, REFGUID, REFIID,
                                        IUnknown **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE InsertEmbedded(TfEditCookie, DWORD,
                                           IDataObject *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG requested,
                                       LONG *shifted,
                                       const TF_HALTCOND *) noexcept override {
    if (shifted == nullptr) {
      return E_POINTER;
    }
    if (failure_state_->Consume(PostWriteFailureStage::kShiftStart)) {
      *shifted = 0;
      return E_ACCESSDENIED;
    }
    const std::int64_t target = static_cast<std::int64_t>(begin_) + requested;
    const std::size_t bounded = static_cast<std::size_t>(
        (std::clamp)(target, std::int64_t{0}, static_cast<std::int64_t>(end_)));
    *shifted = static_cast<LONG>(static_cast<std::int64_t>(bounded) -
                                 static_cast<std::int64_t>(begin_));
    begin_ = bounded;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, LONG requested,
                                     LONG *shifted,
                                     const TF_HALTCOND *) noexcept override {
    if (shifted == nullptr) {
      return E_POINTER;
    }
    if (failure_state_->Consume(PostWriteFailureStage::kShiftEnd)) {
      *shifted = 0;
      return E_ACCESSDENIED;
    }
    const std::int64_t target = static_cast<std::int64_t>(end_) + requested;
    const std::size_t bounded = static_cast<std::size_t>(
        (std::clamp)(target, static_cast<std::int64_t>(begin_),
                     static_cast<std::int64_t>(text_->size())));
    *shifted = static_cast<LONG>(static_cast<std::int64_t>(bounded) -
                                 static_cast<std::int64_t>(end_));
    end_ = bounded;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ShiftStartToRange(TfEditCookie, ITfRange *,
                                              TfAnchor) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE ShiftEndToRange(TfEditCookie, ITfRange *,
                                            TfAnchor) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE ShiftStartRegion(
      TfEditCookie, TfShiftDir, BOOL *no_region) noexcept override {
    return SetValue(no_region, TRUE, E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE ShiftEndRegion(TfEditCookie, TfShiftDir,
                                           BOOL *no_region) noexcept override {
    return SetValue(no_region, TRUE, E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie,
                                    BOOL *empty) noexcept override {
    return SetValue(empty, begin_ == end_ ? TRUE : FALSE, S_OK);
  }
  HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie,
                                     TfAnchor anchor) noexcept override {
    if (failure_state_->Consume(PostWriteFailureStage::kCollapse)) {
      return E_ACCESSDENIED;
    }
    if (anchor == TF_ANCHOR_START) {
      end_ = begin_;
    } else {
      begin_ = end_;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IsEqualStart(TfEditCookie, ITfRange *, TfAnchor,
                                         BOOL *equal) noexcept override {
    return SetValue(equal, FALSE, E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE IsEqualEnd(TfEditCookie, ITfRange *, TfAnchor,
                                       BOOL *equal) noexcept override {
    return SetValue(equal, FALSE, E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange *, TfAnchor,
                                         LONG *result) noexcept override {
    return SetValue(result, LONG{0}, E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange *, TfAnchor,
                                       LONG *result) noexcept override {
    return SetValue(result, LONG{0}, E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE AdjustForInsert(TfEditCookie, ULONG,
                                            BOOL *allowed) noexcept override {
    return SetValue(allowed, TRUE, S_OK);
  }
  HRESULT STDMETHODCALLTYPE GetGravity(TfGravity *start,
                                       TfGravity *end) noexcept override {
    if (start == nullptr || end == nullptr) {
      return E_POINTER;
    }
    *start = TF_GRAVITY_BACKWARD;
    *end = TF_GRAVITY_FORWARD;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetGravity(TfEditCookie, TfGravity,
                                       TfGravity) noexcept override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(ITfRange **clone) noexcept override {
    if (clone == nullptr) {
      return E_POINTER;
    }
    if (failure_state_->Consume(PostWriteFailureStage::kClone)) {
      *clone = nullptr;
      return E_ACCESSDENIED;
    }
    *clone =
        new (std::nothrow) FakeRange(text_, failure_state_, begin_, end_,
                                     set_text_trace_);
    return *clone == nullptr ? E_OUTOFMEMORY : S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetContext(ITfContext **value) noexcept override {
    return Clear(value);
  }

  [[nodiscard]] std::size_t begin() const noexcept { return begin_; }
  [[nodiscard]] std::size_t end() const noexcept { return end_; }

private:
  template <typename Interface>
  static HRESULT Clear(Interface **value) noexcept {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = nullptr;
    return E_NOTIMPL;
  }
  template <typename Value>
  static HRESULT SetValue(Value *output, Value value, HRESULT result) noexcept {
    if (output == nullptr) {
      return E_POINTER;
    }
    *output = value;
    return result;
  }

  ~FakeRange() noexcept = default;
  std::atomic<ULONG> reference_count_{1};
  std::shared_ptr<std::u16string> text_;
  std::shared_ptr<PostWriteFailureState> failure_state_;
  std::shared_ptr<SetTextTraceState> set_text_trace_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
};

class FakeComposition final : public ITfComposition {
public:
  FakeComposition(
      ITfCompositionSink *sink,
      std::shared_ptr<PostWriteFailureState> failure_state,
      std::size_t initial_set_text_failures = 0,
      bool fail_end = false) noexcept
      : sink_(sink), failure_state_(std::move(failure_state)) {
    set_text_trace_->failures_before_write = initial_set_text_failures;
    fail_end_ = fail_end;
    if (sink_ != nullptr) {
      sink_->AddRef();
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) &&
        !IsEqualIID(iid, IID_ITfComposition)) {
      return E_NOINTERFACE;
    }
    *object = static_cast<ITfComposition *>(this);
    AddRef();
    return S_OK;
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
  HRESULT STDMETHODCALLTYPE GetRange(ITfRange **range) noexcept override {
    if (range == nullptr) {
      return E_POINTER;
    }
    *range = new (std::nothrow)
        FakeRange(text_, failure_state_, 0, text_->size(), set_text_trace_);
    return *range == nullptr ? E_OUTOFMEMORY : S_OK;
  }
  HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie,
                                       ITfRange *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie,
                                     ITfRange *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EndComposition(TfEditCookie) noexcept override {
    ++end_attempts_;
    if (fail_end_) {
      return E_ACCESSDENIED;
    }
    ended_ = true;
    return S_OK;
  }

  void set_fail_end(bool fail) noexcept { fail_end_ = fail; }
  [[nodiscard]] bool ended() const noexcept { return ended_; }
  [[nodiscard]] ULONG end_attempts() const noexcept { return end_attempts_; }
  [[nodiscard]] const std::u16string &text() const noexcept { return *text_; }
  [[nodiscard]] std::size_t set_text_call_count() const noexcept {
    return set_text_trace_->payloads.size();
  }
  [[nodiscard]] const std::u16string &
  set_text_payload(std::size_t index) const noexcept {
    return set_text_trace_->payloads[index];
  }
  void FailNextSetText(std::size_t count = 1) noexcept {
    set_text_trace_->failures_before_write = count;
  }
  void SimulateHostText(std::u16string text) { *text_ = std::move(text); }
  HRESULT NotifyTerminated() noexcept {
    ended_ = true;
    return sink_ == nullptr ? E_UNEXPECTED
                            : sink_->OnCompositionTerminated(1, this);
  }

private:
  ~FakeComposition() noexcept {
    if (sink_ != nullptr) {
      sink_->Release();
    }
  }

  std::atomic<ULONG> reference_count_{1};
  std::shared_ptr<std::u16string> text_ = std::make_shared<std::u16string>();
  std::shared_ptr<PostWriteFailureState> failure_state_;
  std::shared_ptr<SetTextTraceState> set_text_trace_ =
      std::make_shared<SetTextTraceState>();
  ITfCompositionSink *sink_ = nullptr;
  ULONG end_attempts_ = 0;
  bool fail_end_ = false;
  bool ended_ = false;
};

enum class ScopeBehavior { kEmpty, kPassword, kGetValueFailure, kScopeFailure };

class FakeInputScope final : public ITfInputScope {
public:
  FakeInputScope(InputScope scope, bool fail) noexcept
      : scope_(scope), fail_(fail) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_ITfInputScope)) {
      return E_NOINTERFACE;
    }
    *object = static_cast<ITfInputScope *>(this);
    AddRef();
    return S_OK;
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
  HRESULT STDMETHODCALLTYPE GetInputScopes(InputScope **scopes,
                                           UINT *count) noexcept override {
    if (scopes == nullptr || count == nullptr) {
      return E_POINTER;
    }
    *scopes = nullptr;
    *count = 0;
    if (fail_) {
      return E_FAIL;
    }
    *scopes = static_cast<InputScope *>(CoTaskMemAlloc(sizeof(InputScope)));
    if (*scopes == nullptr) {
      return E_OUTOFMEMORY;
    }
    **scopes = scope_;
    *count = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPhrase(BSTR **phrases,
                                      UINT *count) noexcept override {
    return ClearPair(phrases, count);
  }
  HRESULT STDMETHODCALLTYPE
  GetRegularExpression(BSTR *value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE GetSRGS(BSTR *value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE GetXML(BSTR *value) noexcept override {
    return Clear(value);
  }

private:
  static HRESULT Clear(BSTR *value) noexcept {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = nullptr;
    return E_NOTIMPL;
  }
  static HRESULT ClearPair(BSTR **values, UINT *count) noexcept {
    if (values == nullptr || count == nullptr) {
      return E_POINTER;
    }
    *values = nullptr;
    *count = 0;
    return E_NOTIMPL;
  }
  ~FakeInputScope() noexcept = default;
  std::atomic<ULONG> reference_count_{1};
  InputScope scope_;
  bool fail_;
};

class FakeProperty final : public ITfProperty {
public:
  explicit FakeProperty(ScopeBehavior behavior) noexcept
      : behavior_(behavior) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfProperty) ||
        IsEqualIID(iid, IID_ITfReadOnlyProperty)) {
      *object = static_cast<ITfProperty *>(this);
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
  HRESULT STDMETHODCALLTYPE GetType(GUID *type) noexcept override {
    if (type == nullptr) {
      return E_POINTER;
    }
    *type = GUID_PROP_INPUTSCOPE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumRanges(TfEditCookie, IEnumTfRanges **ranges,
                                       ITfRange *) noexcept override {
    return Clear(ranges);
  }
  HRESULT STDMETHODCALLTYPE GetValue(TfEditCookie, ITfRange *,
                                     VARIANT *value) noexcept override {
    if (value == nullptr) {
      return E_POINTER;
    }
    VariantInit(value);
    if (behavior_ == ScopeBehavior::kGetValueFailure) {
      return E_FAIL;
    }
    if (behavior_ == ScopeBehavior::kEmpty) {
      value->vt = VT_EMPTY;
      return S_OK;
    }
    value->vt = VT_UNKNOWN;
    value->punkVal = new (std::nothrow)
        FakeInputScope(IS_PASSWORD, behavior_ == ScopeBehavior::kScopeFailure);
    return value->punkVal == nullptr ? E_OUTOFMEMORY : S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetContext(ITfContext **context) noexcept override {
    return Clear(context);
  }
  HRESULT STDMETHODCALLTYPE FindRange(TfEditCookie, ITfRange *,
                                      ITfRange **range,
                                      TfAnchor) noexcept override {
    return Clear(range);
  }
  HRESULT STDMETHODCALLTYPE SetValueStore(
      TfEditCookie, ITfRange *, ITfPropertyStore *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetValue(TfEditCookie, ITfRange *,
                                     const VARIANT *) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Clear(TfEditCookie, ITfRange *) noexcept override {
    return E_NOTIMPL;
  }

private:
  template <typename Interface>
  static HRESULT Clear(Interface **value) noexcept {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = nullptr;
    return E_NOTIMPL;
  }
  ~FakeProperty() noexcept = default;
  std::atomic<ULONG> reference_count_{1};
  ScopeBehavior behavior_;
};

class FakeContext final : public ITfContext, public ITfContextComposition {
public:
  enum class RequestMode {
    kSynchronous,
    kAsynchronous,
    kRequestFailure,
    kSessionFailure
  };

  FakeContext() : property_(new FakeProperty(ScopeBehavior::kEmpty)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfContext)) {
      *object = static_cast<ITfContext *>(this);
    } else if (IsEqualIID(iid, IID_ITfContextComposition)) {
      *object = static_cast<ITfContextComposition *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
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
  RequestEditSession(TfClientId client_id, ITfEditSession *session, DWORD,
                     HRESULT *session_result) noexcept override {
    if (client_id != FakeThreadManager::kClientId) {
      return E_INVALIDARG;
    }
    if (session == nullptr || session_result == nullptr) {
      return E_POINTER;
    }
    ++request_count_;
    if (request_mode_ == RequestMode::kRequestFailure) {
      return E_ACCESSDENIED;
    }
    if (request_mode_ == RequestMode::kSessionFailure) {
      *session_result = E_FAIL;
      return S_OK;
    }
    if (request_mode_ == RequestMode::kSynchronous) {
      *session_result = session->DoEditSession(1);
      return S_OK;
    }
    try {
      sessions_.push_back(session);
    } catch (...) {
      return E_OUTOFMEMORY;
    }
    session->AddRef();
    *session_result = TF_S_ASYNC;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId,
                                           BOOL *writing) noexcept override {
    if (writing == nullptr) {
      return E_POINTER;
    }
    *writing = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG count,
                                         TF_SELECTION *selection,
                                         ULONG *fetched) noexcept override {
    if (selection == nullptr || fetched == nullptr || count == 0) {
      return E_POINTER;
    }
    selection[0] = {};
    selection[0].range = new (std::nothrow) FakeRange(
        selection_text_, post_write_failure_, 0, selection_text_->size());
    if (selection[0].range == nullptr) {
      *fetched = 0;
      return E_OUTOFMEMORY;
    }
    selection[0].style.ase = TF_AE_END;
    *fetched = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetSelection(TfEditCookie, ULONG count,
               const TF_SELECTION *selection) noexcept override {
    if (count != 1 || selection == nullptr || selection[0].range == nullptr) {
      return E_INVALIDARG;
    }
    if (post_write_failure_->Consume(
            PostWriteFailureStage::kSetSelection)) {
      return E_ACCESSDENIED;
    }
    const auto *range = static_cast<const FakeRange *>(selection[0].range);
    selection_caret_ = selection[0].style.ase == TF_AE_START ? range->begin()
                                                             : range->end();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie,
                                     ITfRange **range) noexcept override {
    return NewSelectionRange(range);
  }
  HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie,
                                   ITfRange **range) noexcept override {
    return NewSelectionRange(range);
  }
  HRESULT STDMETHODCALLTYPE
  GetActiveView(ITfContextView **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE
  EnumViews(IEnumTfContextViews **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS *status) noexcept override {
    if (status == nullptr) {
      return E_POINTER;
    }
    *status = {};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetProperty(REFGUID guid, ITfProperty **property) noexcept override {
    if (property == nullptr) {
      return E_POINTER;
    }
    *property = nullptr;
    if (!IsEqualGUID(guid, GUID_PROP_INPUTSCOPE)) {
      return E_INVALIDARG;
    }
    if (property_query_failure_) {
      return E_FAIL;
    }
    *property = property_;
    property_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetAppProperty(REFGUID, ITfReadOnlyProperty **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE
  TrackProperties(const GUID **, ULONG, const GUID **, ULONG,
                  ITfReadOnlyProperty **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE
  EnumProperties(IEnumTfProperties **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE
  GetDocumentMgr(ITfDocumentMgr **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE CreateRangeBackup(
      TfEditCookie, ITfRange *, ITfRangeBackup **value) noexcept override {
    return Clear(value);
  }

  HRESULT STDMETHODCALLTYPE
  StartComposition(TfEditCookie, ITfRange *, ITfCompositionSink *sink,
                   ITfComposition **composition) noexcept override {
    if (composition == nullptr) {
      return E_POINTER;
    }
    if (fail_start_composition_) {
      *composition = nullptr;
      return E_ACCESSDENIED;
    }
    *composition =
        new (std::nothrow) FakeComposition(
            sink, post_write_failure_, next_composition_set_text_failures_,
            next_composition_fail_end_);
    if (*composition == nullptr) {
      return E_OUTOFMEMORY;
    }
    auto *fake = static_cast<FakeComposition *>(*composition);
    try {
      compositions_.push_back(fake);
    } catch (...) {
      fake->Release();
      *composition = nullptr;
      return E_OUTOFMEMORY;
    }
    fake->AddRef();
    next_composition_set_text_failures_ = 0;
    next_composition_fail_end_ = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  EnumCompositions(IEnumITfCompositionView **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE
  FindComposition(TfEditCookie, ITfRange *,
                  IEnumITfCompositionView **value) noexcept override {
    return Clear(value);
  }
  HRESULT STDMETHODCALLTYPE
  TakeOwnership(TfEditCookie, ITfCompositionView *, ITfCompositionSink *,
                ITfComposition **value) noexcept override {
    return Clear(value);
  }

  void set_request_mode(RequestMode mode) noexcept { request_mode_ = mode; }
  void set_fail_start_composition(bool fail) noexcept {
    fail_start_composition_ = fail;
  }
  void FailNextStartedCompositionSetText(
      std::size_t failure_count = 1) noexcept {
    next_composition_set_text_failures_ = failure_count;
  }
  void FailNextStartedCompositionEnd() noexcept {
    next_composition_fail_end_ = true;
  }
  void SetScopeBehavior(ScopeBehavior behavior) {
    FakeProperty *replacement = new FakeProperty(behavior);
    property_->Release();
    property_ = replacement;
    property_query_failure_ = false;
  }
  void SetPropertyQueryFailure(bool fail) noexcept {
    property_query_failure_ = fail;
  }
  void FailNextPostWriteOperation(PostWriteFailureStage stage,
                                  std::size_t failure_count = 1) noexcept {
    post_write_failure_->Arm(stage, failure_count);
  }
  [[nodiscard]] std::size_t selection_caret() const noexcept {
    return selection_caret_;
  }
  [[nodiscard]] std::size_t pending_sessions() const noexcept {
    return sessions_.size();
  }
  [[nodiscard]] std::size_t request_count() const noexcept {
    return request_count_;
  }
  HRESULT ExecuteSession(std::size_t index) noexcept {
    if (index >= sessions_.size()) {
      return E_INVALIDARG;
    }
    ITfEditSession *session = sessions_[index];
    sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
    const HRESULT result = session->DoEditSession(1);
    session->Release();
    return result;
  }
  HRESULT DiscardSession(std::size_t index) noexcept {
    if (index >= sessions_.size()) {
      return E_INVALIDARG;
    }
    ITfEditSession *session = sessions_[index];
    sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
    session->Release();
    return S_OK;
  }
  [[nodiscard]] std::size_t composition_count() const noexcept {
    return compositions_.size();
  }
  [[nodiscard]] FakeComposition *composition(std::size_t index) const noexcept {
    return index < compositions_.size() ? compositions_[index] : nullptr;
  }

private:
  template <typename Interface>
  static HRESULT Clear(Interface **value) noexcept {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = nullptr;
    return E_NOTIMPL;
  }
  HRESULT NewSelectionRange(ITfRange **range) noexcept {
    if (range == nullptr) {
      return E_POINTER;
    }
    *range = new (std::nothrow) FakeRange(
        selection_text_, post_write_failure_, 0, selection_text_->size());
    return *range == nullptr ? E_OUTOFMEMORY : S_OK;
  }
  ~FakeContext() noexcept {
    for (ITfEditSession *session : sessions_) {
      session->Release();
    }
    for (FakeComposition *composition : compositions_) {
      composition->Release();
    }
    property_->Release();
  }

  std::atomic<ULONG> reference_count_{1};
  RequestMode request_mode_ = RequestMode::kSynchronous;
  bool fail_start_composition_ = false;
  std::size_t next_composition_set_text_failures_ = 0;
  bool next_composition_fail_end_ = false;
  std::deque<ITfEditSession *> sessions_;
  std::vector<FakeComposition *> compositions_;
  std::shared_ptr<std::u16string> selection_text_ =
      std::make_shared<std::u16string>();
  std::shared_ptr<PostWriteFailureState> post_write_failure_ =
      std::make_shared<PostWriteFailureState>();
  std::size_t selection_caret_ = 0;
  FakeProperty *property_;
  bool property_query_failure_ = false;
  std::size_t request_count_ = 0;
};

class FakeDocumentManager final : public ITfDocumentMgr {
public:
  explicit FakeDocumentManager(ITfContext *top = nullptr) noexcept {
    SetTop(top);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) &&
        !IsEqualIID(iid, IID_ITfDocumentMgr)) {
      return E_NOINTERFACE;
    }
    *object = static_cast<ITfDocumentMgr *>(this);
    AddRef();
    return S_OK;
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
  CreateContext(TfClientId, DWORD, IUnknown *, ITfContext **context,
                TfEditCookie *cookie) noexcept override {
    if (context == nullptr || cookie == nullptr) {
      return E_POINTER;
    }
    *context = nullptr;
    *cookie = 0;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Push(ITfContext *context) noexcept override {
    SetTop(context);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Pop(DWORD) noexcept override {
    SetTop(nullptr);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTop(ITfContext **context) noexcept override {
    if (context == nullptr) {
      return E_POINTER;
    }
    *context = top_;
    if (top_ != nullptr) {
      top_->AddRef();
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetBase(ITfContext **context) noexcept override {
    return GetTop(context);
  }
  HRESULT STDMETHODCALLTYPE
  EnumContexts(IEnumTfContexts **value) noexcept override {
    if (value == nullptr) {
      return E_POINTER;
    }
    *value = nullptr;
    return E_NOTIMPL;
  }
  void SetTop(ITfContext *context) noexcept {
    if (context != nullptr) {
      context->AddRef();
    }
    if (top_ != nullptr) {
      top_->Release();
    }
    top_ = context;
  }

private:
  ~FakeDocumentManager() noexcept { SetTop(nullptr); }
  std::atomic<ULONG> reference_count_{1};
  ITfContext *top_ = nullptr;
};

struct DirectServiceFixture {
  DirectServiceFixture() {
    service = new zrinput::windows::tsf::TextService();
    context = new FakeContext();
    document = new FakeDocumentManager(context);
    manager = new FakeThreadManager();
    manager->SetFocusedDocument(document);
  }
  ~DirectServiceFixture() {
    if (service != nullptr) {
      (void)service->Deactivate();
      service->Release();
    }
    manager->Release();
    document->Release();
    context->Release();
  }
  HRESULT Activate(DWORD flags = 0) noexcept {
    return service->ActivateEx(manager, FakeThreadManager::kClientId, flags);
  }
  HRESULT Key(WPARAM key, BOOL *eaten = nullptr) noexcept {
    BOOL local = FALSE;
    return service->OnKeyDown(context, key, 0,
                              eaten == nullptr ? &local : eaten);
  }

  zrinput::windows::tsf::TextService *service = nullptr;
  FakeContext *context = nullptr;
  FakeDocumentManager *document = nullptr;
  FakeThreadManager *manager = nullptr;
};

void DrainAsyncSessions(FakeContext *context) {
  while (context->pending_sessions() != 0) {
    ZR_EXPECT_TRUE(SUCCEEDED(context->ExecuteSession(0)));
  }
}

void FillSplittableComposition(DirectServiceFixture *fixture) {
  for (std::size_t index = 0; index < 128; ++index) {
    ZR_EXPECT_TRUE(SUCCEEDED(fixture->Key('A')));
  }
  ZR_EXPECT_TRUE(SUCCEEDED(fixture->Key(VK_OEM_7)));
  for (std::size_t index = 0; index < 127; ++index) {
    ZR_EXPECT_TRUE(SUCCEEDED(fixture->Key('A')));
  }
}

ZR_TEST(InactiveTeardownExhaustsQueuedStaleSplitFailuresAndCanUnload) {
  {
    DirectServiceFixture fixture;
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    FillSplittableComposition(&fixture);

    fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_LEFT)));
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
    ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
    ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);

    fixture.context->set_request_mode(
        FakeContext::RequestMode::kRequestFailure);
    const std::size_t requests_before_completion =
        fixture.context->request_count();
    ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
    ZR_EXPECT_EQ(fixture.context->request_count() - requests_before_completion,
                 std::size_t{2});
    ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});

    const std::size_t requests_after_retirement =
        fixture.context->request_count();
    ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
    ZR_EXPECT_EQ(fixture.context->request_count(), requests_after_retirement);
  }
  ZR_EXPECT_TRUE(zrinput::windows::tsf::CanUnload());
}

void ExpectPostWriteSelectionFailureRemainsConsumed(
    PostWriteFailureStage stage) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->FailNextPostWriteOperation(stage);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"ab"));
  ZR_EXPECT_EQ(fixture.context->selection_caret(), std::size_t{2});

  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(fixture.context->selection_caret(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
}

ZR_TEST(PostWriteCloneFailureKeepsTheKeyConsumed) {
  ExpectPostWriteSelectionFailureRemainsConsumed(
      PostWriteFailureStage::kClone);
}

ZR_TEST(PostWriteCollapseFailureKeepsTheKeyConsumed) {
  ExpectPostWriteSelectionFailureRemainsConsumed(
      PostWriteFailureStage::kCollapse);
}

ZR_TEST(PostWriteShiftEndFailureKeepsTheKeyConsumed) {
  ExpectPostWriteSelectionFailureRemainsConsumed(
      PostWriteFailureStage::kShiftEnd);
}

ZR_TEST(PostWriteShiftStartFailureKeepsTheKeyConsumed) {
  ExpectPostWriteSelectionFailureRemainsConsumed(
      PostWriteFailureStage::kShiftStart);
}

ZR_TEST(PostWriteSetSelectionFailureKeepsTheKeyConsumed) {
  ExpectPostWriteSelectionFailureRemainsConsumed(
      PostWriteFailureStage::kSetSelection);
}

ZR_TEST(PostWriteNavigationFailureResynchronizesBeforeBackspace) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  ZR_EXPECT_EQ(fixture.context->selection_caret(), std::size_t{3});

  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_LEFT, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(fixture.context->selection_caret(), std::size_t{2});

  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"ac"));
  ZR_EXPECT_EQ(fixture.context->selection_caret(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
}

ZR_TEST(AsyncPostWriteFailureResynchronizesAndRetiresTheUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kCollapse);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  BOOL eaten = FALSE;
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B', &eaten)));
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"ab"));
  ZR_EXPECT_EQ(fixture.context->selection_caret(), std::size_t{2});

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
}

ZR_TEST(PersistentPostWriteSelectionFailureCommitsToAStableBoundary) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"ab"));

  BOOL test_eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                   fixture.context, VK_BACK, 0, &test_eaten),
               S_OK);
  ZR_EXPECT_TRUE(test_eaten == FALSE);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key(VK_BACK, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten == FALSE);

  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"c"));
}

ZR_TEST(DirtySelectionPreservesHostInputWhenEnterRecovers) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  composition->set_fail_end(true);

  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"ab"));

  BOOL test_eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                   fixture.context, 'C', 0, &test_eaten),
               S_OK);
  ZR_EXPECT_TRUE(test_eaten == FALSE);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('C', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten == FALSE);
  composition->SimulateHostText(u"abc");

  composition->set_fail_end(false);
  eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_RETURN, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"abc"));
}

ZR_TEST(DirtySelectionCancelsTheActualHostTextAfterHostBackspace) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  composition->set_fail_end(true);

  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  BOOL test_eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                   fixture.context, VK_BACK, 0, &test_eaten),
               S_OK);
  ZR_EXPECT_TRUE(test_eaten == FALSE);
  BOOL eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key(VK_BACK, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten == FALSE);
  composition->SimulateHostText(u"a");

  composition->set_fail_end(false);
  eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"c"));
}

ZR_TEST(EditRequestFailuresPropagateAndRollBackTheBuffer) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);

  BOOL eaten = FALSE;
  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  ZR_EXPECT_EQ(fixture.Key('A', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{0});

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"b"));

  fixture.context->set_request_mode(FakeContext::RequestMode::kSessionFailure);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('C', &eaten), E_FAIL);
  ZR_EXPECT_TRUE(eaten == FALSE);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('D'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"bd"));
}

ZR_TEST(SynchronousFirstWriteFailureEndsTheEmptyCompositionBeforeRollback) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->FailNextStartedCompositionSetText();

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('A', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  FakeComposition *composition = fixture.context->composition(0);
  ZR_EXPECT_EQ(composition->text(), std::u16string());
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(composition->set_text_call_count(), std::size_t{1});

  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(SynchronousFirstWriteAndCleanupFailureKeepsTheKeyConsumed) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->FailNextStartedCompositionSetText();
  fixture.context->FailNextStartedCompositionEnd();

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('A', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  FakeComposition *composition = fixture.context->composition(0);
  ZR_EXPECT_EQ(composition->text(), std::u16string());
  ZR_EXPECT_TRUE(!composition->ended());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"ab"));
  ZR_EXPECT_EQ(composition->set_text_call_count(), std::size_t{3});
}

ZR_TEST(AsynchronousFirstWriteFailureRetriesWithoutReturningTheKey) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->FailNextStartedCompositionSetText();
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);

  BOOL eaten = FALSE;
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A', &eaten)));
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  FakeComposition *failed = fixture.context->composition(0);
  ZR_EXPECT_EQ(failed->text(), std::u16string());
  ZR_EXPECT_TRUE(failed->ended());

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  FakeComposition *retried = fixture.context->composition(1);
  ZR_EXPECT_EQ(retried->text(), std::u16string(u"ab"));
  ZR_EXPECT_EQ(retried->set_text_call_count(), std::size_t{1});
  ZR_EXPECT_EQ(retried->set_text_payload(0), std::u16string(u"ab"));
}

ZR_TEST(EscapeSafelyCancelsAStartedCompositionAfterFirstWriteFailure) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->FailNextStartedCompositionSetText();
  fixture.context->FailNextStartedCompositionEnd();
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  composition->set_fail_end(false);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{2});
}

ZR_TEST(DeactivateCancelsAnAsyncStartedCompositionAndReleasesEveryReference) {
  {
    DirectServiceFixture fixture;
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    fixture.context->FailNextStartedCompositionSetText();
    fixture.context->FailNextStartedCompositionEnd();
    fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A')));
    ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
    FakeComposition *composition = fixture.context->composition(0);
    ZR_EXPECT_EQ(composition->text(), std::u16string());
    ZR_EXPECT_TRUE(!composition->ended());

    composition->set_fail_end(false);
    ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
    ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
    ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_OK);
    ZR_EXPECT_TRUE(composition->ended());
    ZR_EXPECT_EQ(composition->end_attempts(), ULONG{2});
    ZR_EXPECT_EQ(composition->text(), std::u16string());
    ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  }
  ZR_EXPECT_TRUE(zrinput::windows::tsf::CanUnload());
}

ZR_TEST(AsyncCommitRetainsItsSnapshotWhenFirstWriteAndCleanupBothFail) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A')));
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  fixture.context->FailNextStartedCompositionSetText();
  fixture.context->FailNextStartedCompositionEnd();
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  FakeComposition *commit = fixture.context->composition(0);
  ZR_EXPECT_EQ(commit->text(), std::u16string());
  ZR_EXPECT_TRUE(!commit->ended());
  ZR_EXPECT_EQ(commit->end_attempts(), ULONG{1});

  commit->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(commit->ended());
  ZR_EXPECT_EQ(commit->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(commit->set_text_call_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(AsyncSplitRetainsItsPrefixWhenFirstWriteAndCleanupBothFail) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  FillSplittableComposition(&fixture);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));

  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{0});

  fixture.context->FailNextStartedCompositionSetText();
  fixture.context->FailNextStartedCompositionEnd();
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  FakeComposition *prefix = fixture.context->composition(0);
  ZR_EXPECT_EQ(prefix->text(), std::u16string());
  ZR_EXPECT_TRUE(!prefix->ended());
  ZR_EXPECT_EQ(prefix->end_attempts(), ULONG{1});

  prefix->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(prefix->text().size(), std::size_t{129});
  ZR_EXPECT_EQ(prefix->text().back(), u'\'');
  ZR_EXPECT_EQ(prefix->set_text_call_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  const std::u16string &remainder = fixture.context->composition(1)->text();
  ZR_EXPECT_EQ(remainder.size(), std::size_t{129});
  ZR_EXPECT_EQ(remainder[remainder.size() - 2], u'b');
  ZR_EXPECT_EQ(remainder.back(), u'c');
}

ZR_TEST(FailedCurrentUpdateStopsEatingKeysAtTheFailureBudget) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A')));
  fixture.context->set_fail_start_composition(true);
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);

  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  for (std::size_t attempt = 0; attempt < 2; ++attempt) {
    BOOL eaten = TRUE;
    ZR_EXPECT_EQ(fixture.Key('B', &eaten), E_ACCESSDENIED);
    ZR_EXPECT_TRUE(eaten == FALSE);
  }
  const std::size_t requests_at_budget = fixture.context->request_count();
  for (std::size_t attempt = 0; attempt < 8; ++attempt) {
    BOOL test_eaten = TRUE;
    ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                     fixture.context, 'B', 0, &test_eaten),
                 S_OK);
    ZR_EXPECT_TRUE(test_eaten == FALSE);
    BOOL eaten = TRUE;
    ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
    ZR_EXPECT_TRUE(eaten == FALSE);
  }
  BOOL test_eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                   fixture.context, VK_OEM_7, 0, &test_eaten),
               S_OK);
  ZR_EXPECT_TRUE(test_eaten == FALSE);
  BOOL apostrophe_eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key(VK_OEM_7, &apostrophe_eaten), S_OK);
  ZR_EXPECT_TRUE(apostrophe_eaten == FALSE);
  ZR_EXPECT_EQ(fixture.context->request_count(), requests_at_budget);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{0});

  fixture.context->set_fail_start_composition(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_BACK, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"c"));
}

ZR_TEST(RefocusingTheSameContextStartsAFreshUpdateFailureBudget) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A')));
  fixture.context->set_fail_start_composition(true);
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);

  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  for (std::size_t attempt = 0; attempt < 2; ++attempt) {
    BOOL eaten = TRUE;
    ZR_EXPECT_EQ(fixture.Key('B', &eaten), E_ACCESSDENIED);
    ZR_EXPECT_TRUE(eaten == FALSE);
  }
  BOOL test_eaten = TRUE;
  ZR_EXPECT_EQ(
      fixture.service->OnTestKeyDown(fixture.context, 'B', 0, &test_eaten),
      S_OK);
  ZR_EXPECT_TRUE(test_eaten == FALSE);

  ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(TRUE), S_OK);
  fixture.context->set_fail_start_composition(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  BOOL eaten = FALSE;
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('C', &eaten)));
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  test_eaten = FALSE;
  ZR_EXPECT_EQ(
      fixture.service->OnTestKeyDown(fixture.context, 'D', 0, &test_eaten),
      S_OK);
  ZR_EXPECT_TRUE(test_eaten != FALSE);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"c"));
}

ZR_TEST(OnlyOneEditSessionIsVisibleToTheHostAcrossCommitBarrier) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A')));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.Key(VK_RETURN), S_OK);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(1), E_INVALIDARG);

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{0});
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  ZR_EXPECT_TRUE(fixture.context->composition(0)->ended());
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_TRUE(!fixture.context->composition(1)->ended());
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
}

ZR_TEST(PendingUpdatesCoalesceWithoutOvertakingTheInFlightUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('A')));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{0});
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"abc"));
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
}

ZR_TEST(CancelEndsOnlyItsCapturedCompositionAndOldTerminationIsHarmless) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *old_composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_ESCAPE)));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  DrainAsyncSessions(fixture.context);
  ZR_EXPECT_TRUE(old_composition->ended());
  ZR_EXPECT_EQ(old_composition->text(), std::u16string());
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key(VK_RETURN), S_OK);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  FakeComposition *current_composition = fixture.context->composition(2);
  ZR_EXPECT_EQ(old_composition->NotifyTerminated(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('D'), S_OK);
  ZR_EXPECT_EQ(current_composition->text(), std::u16string(u"cd"));
}

ZR_TEST(SynchronousCommitPostWriteFailureKeepsEnterConsumed) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_commit = composition->set_text_call_count();
  composition->set_fail_end(true);
  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_RETURN, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(!composition->ended());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_commit + 1);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_commit),
               std::u16string(u"a"));
  composition->SimulateHostText(u"host-a");
  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"host-a"));
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_commit + 1);
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(AsyncCommitPostWriteFailureRemainsARetryableBarrier) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *committed_composition = fixture.context->composition(0);
  const std::size_t writes_before_commit =
      committed_composition->set_text_call_count();
  committed_composition->set_fail_end(true);
  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(!committed_composition->ended());
  ZR_EXPECT_EQ(committed_composition->set_text_call_count(),
               writes_before_commit + 1);
  ZR_EXPECT_EQ(committed_composition->set_text_payload(writes_before_commit),
               std::u16string(u"a"));
  committed_composition->SimulateHostText(u"host-a");

  committed_composition->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_TRUE(committed_composition->ended());
  ZR_EXPECT_EQ(committed_composition->text(), std::u16string(u"host-a"));
  ZR_EXPECT_EQ(committed_composition->set_text_call_count(),
               writes_before_commit + 1);
  ZR_EXPECT_EQ(committed_composition->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"bc"));
}

ZR_TEST(CancelClearedAwaitEndRetriesOnlyEnd) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_cancel = composition->set_text_call_count();
  composition->set_fail_end(true);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_cancel + 1);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_cancel),
               std::u16string());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  composition->SimulateHostText(u"host-after-clear");

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_cancel + 1);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"host-after-clear"));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(CoalescedEmptyUpdatePreservesItsClearedAwaitEndPhase) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_failure = composition->set_text_call_count();

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_HOME)));
  composition->FailNextSetText();
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_failure + 1);

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  composition->set_fail_end(true);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_DELETE, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_failure + 2);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_failure + 1),
               std::u16string());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  composition->SimulateHostText(u"host-after-empty-update");

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_failure + 2);
  ZR_EXPECT_EQ(composition->text(),
               std::u16string(u"host-after-empty-update"));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(ExternalTerminationInvalidatesEmptyUpdateAfterClear) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_clear = composition->set_text_call_count();
  composition->set_fail_end(true);

  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_clear + 1);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_clear),
               std::u16string());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(composition->NotifyTerminated(), S_OK);

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_clear + 1);
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(ClearedEmptyUpdateFocusLossRetriesOnlyEndDuringDeactivate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_clear = composition->set_text_call_count();
  composition->set_fail_end(true);

  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_clear + 1);
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  composition->SimulateHostText(u"host-after-focus-loss");
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_clear + 1);
  ZR_EXPECT_EQ(composition->text(),
               std::u16string(u"host-after-focus-loss"));
}

ZR_TEST(CommitBeforeWriteRetriesThePayloadAfterARealWriteFailure) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_commit = composition->set_text_call_count();

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  composition->FailNextSetText();
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_commit + 1);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{0});

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_commit + 2);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_commit),
               std::u16string(u"a"));
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_commit + 1),
               std::u16string(u"a"));
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(CancelBeforeClearRetriesEmptyAfterARealWriteFailure) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_cancel = composition->set_text_call_count();

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_ESCAPE)));
  composition->FailNextSetText();
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_cancel + 1);
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{0});

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_cancel + 2);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_cancel),
               std::u16string());
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_cancel + 1),
               std::u16string());
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(PendingSplitThenCancelDoesNotClearTheCommittedPrefix) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  const std::size_t writes_before_split = prefix->set_text_call_count();

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));

  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(prefix->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(prefix->text().size(), std::size_t{129});
  ZR_EXPECT_EQ(prefix->text().back(), u'\'');
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"c"));
}

ZR_TEST(PendingCommitThenSecondCommitUsesANewComposition) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *first = fixture.context->composition(0);
  const std::size_t first_writes_before_commit =
      first->set_text_call_count();

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.Key(VK_RETURN), S_OK);
  DrainAsyncSessions(fixture.context);

  ZR_EXPECT_TRUE(first->ended());
  ZR_EXPECT_EQ(first->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(first->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(first->set_text_call_count(), first_writes_before_commit + 1);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  FakeComposition *second = fixture.context->composition(1);
  ZR_EXPECT_TRUE(second->ended());
  ZR_EXPECT_EQ(second->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(second->text(), std::u16string(u"b"));
}

ZR_TEST(FailedCommitBarrierIsRetriedDuringDeactivate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_commit = composition->set_text_call_count();
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(!composition->ended());
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_commit + 1);
  composition->SimulateHostText(u"host-a");

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"host-a"));
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_commit + 1);
}

ZR_TEST(FailedCurrentCommitRollsBackBlockedKeysAndEscRecovers) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);

  BOOL eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('C', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);
  const std::size_t requests_at_budget = fixture.context->request_count();
  for (std::size_t attempt = 0; attempt < 8; ++attempt) {
    BOOL test_eaten = TRUE;
    ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                     fixture.context, 'D', 0, &test_eaten),
                 S_OK);
    ZR_EXPECT_TRUE(test_eaten == FALSE);
    eaten = TRUE;
    ZR_EXPECT_EQ(fixture.Key('D', &eaten), S_OK);
    ZR_EXPECT_TRUE(eaten == FALSE);
  }
  ZR_EXPECT_EQ(fixture.context->request_count(), requests_at_budget);
  ZR_EXPECT_TRUE(!composition->ended());

  composition->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
  ZR_EXPECT_EQ(fixture.Key('E'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"e"));
}

ZR_TEST(FailedPredecessorRollsBackACoalescedUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *committed_composition = fixture.context->composition(0);
  committed_composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);

  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  BOOL eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('C', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);

  committed_composition->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('D'), S_OK);
  ZR_EXPECT_TRUE(committed_composition->ended());
  ZR_EXPECT_EQ(committed_composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(),
               std::u16string(u"bd"));
}

ZR_TEST(ExternalTerminationInvalidatesAcceptedCommitBeforeItRuns) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(composition->NotifyTerminated(), S_OK);
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_OK);

  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{0});
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
}

ZR_TEST(ExternalTerminationInvalidatesFocusLossCancelBeforeItRuns) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(composition->NotifyTerminated(), S_OK);
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);

  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{0});
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(TRUE), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"b"));
}

ZR_TEST(ExternalTerminationInvalidatesSplitWithoutReplayingItsRemainder) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_split = composition->set_text_call_count();

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('C')));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(composition->NotifyTerminated(), S_OK);
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{0});
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split);
  ZR_EXPECT_EQ(composition->text().size(), std::size_t{256});
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('D'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"d"));
}

ZR_TEST(PersistentAsyncFailureStopsAtTheBudgetAndEscRecovers) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *committed_composition = fixture.context->composition(0);
  committed_composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  for (std::size_t index = 0; index < 2; ++index) {
    BOOL eaten = FALSE;
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B', &eaten)));
    ZR_EXPECT_TRUE(eaten != FALSE);
    ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
    ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
    ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  }

  const std::size_t requests_before_recovery = fixture.context->request_count();
  for (std::size_t index = 0; index < 80; ++index) {
    BOOL test_eaten = TRUE;
    ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                     fixture.context, 'B', 0, &test_eaten),
                 S_OK);
    ZR_EXPECT_TRUE(test_eaten == FALSE);
    BOOL eaten = TRUE;
    ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
    ZR_EXPECT_TRUE(eaten == FALSE);
  }
  ZR_EXPECT_EQ(fixture.context->request_count(), requests_before_recovery);

  committed_composition->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->request_count() - requests_before_recovery,
               std::size_t{2});
  ZR_EXPECT_TRUE(committed_composition->ended());
  ZR_EXPECT_EQ(committed_composition->text(), std::u16string());
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"c"));
}

ZR_TEST(PendingBarrierQueueAppliesBackpressureWithoutEatingTheKey) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));

  bool rejected = false;
  for (std::size_t index = 0; index < 100 && !rejected; ++index) {
    BOOL eaten = FALSE;
    HRESULT result = fixture.Key('B', &eaten);
    if (FAILED(result)) {
      ZR_EXPECT_EQ(result, E_PENDING);
      ZR_EXPECT_TRUE(eaten == FALSE);
      rejected = true;
      break;
    }
    ZR_EXPECT_TRUE(eaten != FALSE);

    eaten = FALSE;
    result = fixture.Key(VK_RETURN, &eaten);
    if (FAILED(result)) {
      ZR_EXPECT_EQ(result, E_PENDING);
      ZR_EXPECT_TRUE(eaten == FALSE);
      rejected = true;
    } else {
      ZR_EXPECT_TRUE(eaten != FALSE);
    }
  }
  ZR_EXPECT_TRUE(rejected);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
}

ZR_TEST(DeactivatePreservesConfirmedCommitAndDropsLaterUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string(u"a"));
}

ZR_TEST(DeactivateRunsCancelAfterAnInFlightUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(!composition->ended());
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
}

ZR_TEST(InactiveTeardownExhaustsImmediateCancelFailures) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  const std::size_t requests_before_completion =
      fixture.context->request_count();

  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), S_FALSE);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(fixture.context->request_count() - requests_before_completion,
               std::size_t{2});
  ZR_EXPECT_TRUE(!composition->ended());

  const std::size_t requests_after_retirement =
      fixture.context->request_count();
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->request_count(), requests_after_retirement);
}

ZR_TEST(FailedLateUpdateCompletionStillDispatchesDeactivateCancel) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->DiscardSession(0), S_OK);
  fixture.service->CompleteEditSession(2, E_FAIL);

  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
}

ZR_TEST(ExplicitPinyinBoundarySplitsAtTheSoftLimit) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  ZR_EXPECT_EQ(fixture.context->composition(0)->text().size(),
               std::size_t{256});

  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_TRUE(fixture.context->composition(0)->ended());
  ZR_EXPECT_EQ(fixture.context->composition(0)->text().size(),
               std::size_t{129});
  ZR_EXPECT_EQ(fixture.context->composition(0)->text().back(), u'\'');
  ZR_EXPECT_EQ(fixture.context->composition(1)->text().size(),
               std::size_t{128});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text().back(), u'b');
}

ZR_TEST(SynchronousSplitPostWriteFailureKeepsTheTriggerExactlyOnce) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  const std::size_t writes_before_split = prefix->set_text_call_count();
  prefix->set_fail_end(true);
  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(!prefix->ended());
  ZR_EXPECT_EQ(prefix->text().size(), std::size_t{129});
  ZR_EXPECT_EQ(prefix->text().back(), u'\'');
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(prefix->set_text_payload(writes_before_split).size(),
               std::size_t{129});
  prefix->SimulateHostText(u"host-prefix");

  prefix->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(prefix->text(), std::u16string(u"host-prefix"));
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(prefix->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  const std::u16string &remainder = fixture.context->composition(1)->text();
  ZR_EXPECT_EQ(remainder.size(), std::size_t{129});
  ZR_EXPECT_EQ(remainder[remainder.size() - 2], u'b');
  ZR_EXPECT_EQ(remainder.back(), u'c');
  ZR_EXPECT_EQ(static_cast<std::size_t>(
                   std::count(remainder.begin(), remainder.end(), u'b')),
               std::size_t{1});
}

ZR_TEST(AsyncSplitPostWriteFailureKeepsQueuedKeysInOrder) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  const std::size_t writes_before_split = prefix->set_text_call_count();
  prefix->set_fail_end(true);
  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);

  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(!prefix->ended());
  ZR_EXPECT_EQ(prefix->text().size(), std::size_t{129});
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(prefix->set_text_payload(writes_before_split).size(),
               std::size_t{129});
  prefix->SimulateHostText(u"host-prefix");

  prefix->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  ZR_EXPECT_EQ(fixture.Key('D'), S_OK);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(prefix->text(), std::u16string(u"host-prefix"));
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(prefix->end_attempts(), ULONG{2});
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  const std::u16string &remainder = fixture.context->composition(1)->text();
  ZR_EXPECT_EQ(remainder.size(), std::size_t{130});
  ZR_EXPECT_EQ(remainder[remainder.size() - 3], u'b');
  ZR_EXPECT_EQ(remainder[remainder.size() - 2], u'c');
  ZR_EXPECT_EQ(remainder.back(), u'd');
  ZR_EXPECT_EQ(static_cast<std::size_t>(
                   std::count(remainder.begin(), remainder.end(), u'b')),
               std::size_t{1});
}

ZR_TEST(SplitEndedAwaitRemainderHandsOffToTheLatestUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  const std::size_t writes_before_split = prefix->set_text_call_count();
  fixture.context->set_fail_start_composition(true);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(prefix->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});

  fixture.context->set_fail_start_composition(false);
  ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
  ZR_EXPECT_EQ(prefix->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(prefix->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  const std::u16string &remainder = fixture.context->composition(1)->text();
  ZR_EXPECT_EQ(remainder.size(), std::size_t{129});
  ZR_EXPECT_EQ(remainder[remainder.size() - 2], u'b');
  ZR_EXPECT_EQ(remainder.back(), u'c');
}

ZR_TEST(SplitEndedAwaitRemainderMaterializesDuringDeactivate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  fixture.context->set_fail_start_composition(true);

  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});

  fixture.context->set_fail_start_composition(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  FakeComposition *remainder = fixture.context->composition(1);
  ZR_EXPECT_TRUE(remainder->ended());
  ZR_EXPECT_EQ(prefix->text().size() + remainder->text().size(),
               std::size_t{257});
  ZR_EXPECT_EQ(remainder->text().back(), u'b');
  ZR_EXPECT_EQ(remainder->set_text_call_count(), std::size_t{1});
  ZR_EXPECT_EQ(remainder->end_attempts(), ULONG{1});
}

ZR_TEST(SynchronousSplitFailureBudgetAllowsEscapeRecovery) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  prefix->set_fail_end(true);
  fixture.context->FailNextPostWriteOperation(
      PostWriteFailureStage::kSetSelection, 2);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key('B', &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('C', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.Key('D', &eaten), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(eaten == FALSE);

  BOOL test_eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(
                   fixture.context, 'E', 0, &test_eaten),
               S_OK);
  ZR_EXPECT_TRUE(test_eaten == FALSE);
  prefix->set_fail_end(false);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  eaten = FALSE;
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(prefix->text(), std::u16string());

  ZR_EXPECT_EQ(fixture.Key('F'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.context->composition(1)->text(), std::u16string(u"f"));
}

ZR_TEST(AsyncSplitPreservesRemainderAcrossFocusLossAndDeactivate) {
  {
    DirectServiceFixture fixture;
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    FillSplittableComposition(&fixture);
    FakeComposition *composition = fixture.context->composition(0);
    fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
    ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
    ZR_EXPECT_TRUE(composition->ended());
    ZR_EXPECT_EQ(composition->text().size(), std::size_t{257});
    ZR_EXPECT_EQ(composition->text().back(), u'b');
  }
  {
    DirectServiceFixture fixture;
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    FillSplittableComposition(&fixture);
    FakeComposition *composition = fixture.context->composition(0);
    fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
    ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
    ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
    ZR_EXPECT_TRUE(composition->ended());
    ZR_EXPECT_EQ(composition->text().size(), std::size_t{257});
    ZR_EXPECT_EQ(composition->text().back(), u'b');
  }
}

ZR_TEST(FailedSplitBarrierIsRetriedWholeDuringDeactivate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_split = composition->set_text_call_count();
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(!composition->ended());
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_split).size(),
               std::size_t{129});
  composition->SimulateHostText(u"host-prefix");

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text().size(),
               std::u16string(u"host-prefix").size() + std::size_t{128});
  ZR_EXPECT_EQ(composition->text().substr(0, std::size_t{11}),
               std::u16string(u"host-prefix"));
  ZR_EXPECT_EQ(composition->text().back(), u'b');
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 2);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_split + 1),
               composition->text());
}

ZR_TEST(StaleSplitRejectsAnOversizedWholeWithoutAnotherWrite) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_split = composition->set_text_call_count();
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 1);
  composition->SimulateHostText(std::u16string(4096, u'x'));
  composition->set_fail_end(false);

  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0),
               HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 1);
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
}

ZR_TEST(DeactivateFoldsQueuedKeysIntoAPartiallyWrittenSplit) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_split = composition->set_text_call_count();
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('C')));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text().size(), std::size_t{258});
  ZR_EXPECT_EQ(composition->text()[composition->text().size() - 2], u'b');
  ZR_EXPECT_EQ(composition->text().back(), u'c');
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 2);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_split + 1).size(),
               std::size_t{258});
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
}

ZR_TEST(DeactivatePreservesACommittedDependentBeforeALaterUpdate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_split = composition->set_text_call_count();
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('C')));
  ZR_EXPECT_EQ(fixture.Key(VK_RETURN), S_OK);
  ZR_EXPECT_EQ(fixture.Key('D'), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});

  composition->set_fail_end(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text().size(), std::size_t{259});
  ZR_EXPECT_EQ(composition->text()[composition->text().size() - 3], u'b');
  ZR_EXPECT_EQ(composition->text()[composition->text().size() - 2], u'c');
  ZR_EXPECT_EQ(composition->text().back(), u'd');
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 2);
  ZR_EXPECT_EQ(composition->set_text_payload(writes_before_split + 1),
               composition->text());
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
}

ZR_TEST(StaleSplitWholeWriteFailureRetiresWithinTheBudget) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *composition = fixture.context->composition(0);
  const std::size_t writes_before_split = composition->set_text_call_count();
  composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
  composition->FailNextSetText(4);

  FakeContext *replacement = new FakeContext();
  FakeDocumentManager *replacement_document =
      new FakeDocumentManager(replacement);
  ZR_EXPECT_EQ(
      fixture.service->OnSetFocus(replacement_document, fixture.document),
      S_OK);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, 'C', 0, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 2);
  ZR_EXPECT_EQ(composition->end_attempts(), ULONG{1});
  ZR_EXPECT_EQ(replacement->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"c"));
  ZR_EXPECT_EQ(
      fixture.service->OnKeyDown(replacement, 'D', 0, &eaten), S_OK);
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"cd"));
  ZR_EXPECT_EQ(composition->set_text_call_count(), writes_before_split + 2);

  replacement_document->Release();
  replacement->Release();
}

ZR_TEST(StaleSplitRemainderStartFailureRetiresWithinTheBudget) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  FillSplittableComposition(&fixture);
  FakeComposition *prefix = fixture.context->composition(0);
  fixture.context->set_fail_start_composition(true);

  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_TRUE(prefix->ended());
  ZR_EXPECT_EQ(fixture.context->composition_count(), std::size_t{1});

  FakeContext *replacement = new FakeContext();
  FakeDocumentManager *replacement_document =
      new FakeDocumentManager(replacement);
  ZR_EXPECT_EQ(
      fixture.service->OnSetFocus(replacement_document, fixture.document),
      S_OK);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, 'C', 0, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(replacement->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"c"));
  const std::size_t old_context_requests = fixture.context->request_count();
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->request_count(), old_context_requests);

  replacement_document->Release();
  replacement->Release();
}

ZR_TEST(NoStableBoundaryRejectsGrowthButEditingStillRecovers) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  for (std::size_t index = 0; index < 256; ++index) {
    ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  }
  FakeComposition *composition = fixture.context->composition(0);
  ZR_EXPECT_EQ(composition->text().size(), std::size_t{256});

  ZR_EXPECT_EQ(fixture.Key('B'), S_FALSE);
  ZR_EXPECT_EQ(composition->text(), std::u16string(256, u'a'));
  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->text(), std::u16string(255, u'a'));
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
}

ZR_TEST(SeparatorOnlyCompositionIsNeverAStableSplitBoundary) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  for (std::size_t index = 0; index < 256; ++index) {
    ZR_EXPECT_EQ(fixture.Key(VK_OEM_7), S_OK);
  }
  FakeComposition *composition = fixture.context->composition(0);
  const std::u16string separators(256, u'\'');
  ZR_EXPECT_EQ(composition->text(), separators);

  ZR_EXPECT_EQ(fixture.Key('A'), S_FALSE);
  ZR_EXPECT_EQ(composition->text(), separators);
  ZR_EXPECT_EQ(fixture.Key(VK_BACK), S_OK);
  ZR_EXPECT_EQ(composition->text(), std::u16string(255, u'\''));
  ZR_EXPECT_EQ(fixture.Key(VK_ESCAPE), S_OK);
  ZR_EXPECT_TRUE(composition->ended());
}

ZR_TEST(FailedLifecycleCancelIsRetriedDuringDeactivate) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *composition = fixture.context->composition(0);

  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);
  ZR_EXPECT_TRUE(!composition->ended());

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_EQ(fixture.context->pending_sessions(), std::size_t{0});
  ZR_EXPECT_TRUE(composition->ended());
  ZR_EXPECT_EQ(composition->text(), std::u16string());
}

ZR_TEST(InputScopeQueriesAreFailClosedAndEmptyScopeAllowsLearning) {
  {
    DirectServiceFixture fixture;
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
    ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
    ZR_EXPECT_TRUE(fixture.service->LearningAllowed());
    ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
    ZR_EXPECT_EQ(fixture.service->OnSetFocus(TRUE), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
    ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
    ZR_EXPECT_TRUE(fixture.service->LearningAllowed());
    fixture.context->SetPropertyQueryFailure(true);
    ZR_EXPECT_EQ(fixture.Key('C'), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
  }
  {
    DirectServiceFixture fixture;
    fixture.context->SetScopeBehavior(ScopeBehavior::kPassword);
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
  }
  {
    DirectServiceFixture fixture;
    fixture.context->SetScopeBehavior(ScopeBehavior::kGetValueFailure);
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
  }
  {
    DirectServiceFixture fixture;
    fixture.context->SetScopeBehavior(ScopeBehavior::kScopeFailure);
    ZR_EXPECT_EQ(fixture.Activate(), S_OK);
    ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
    ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
  }
}

ZR_TEST(AsyncEditDisablesLearningBeforePasswordScopeCanBeQueried) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  ZR_EXPECT_TRUE(fixture.service->LearningAllowed());

  fixture.context->SetScopeBehavior(ScopeBehavior::kPassword);
  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key('B')));
  ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.context->ExecuteSession(0)));
  ZR_EXPECT_TRUE(!fixture.service->LearningAllowed());
  fixture.context->set_request_mode(FakeContext::RequestMode::kSynchronous);
}

ZR_TEST(UnadviseFailuresRetainStateForACompleteRetry) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  fixture.manager->set_fail_key_unadvise(true);
  fixture.manager->set_fail_thread_unadvise(true);

  ZR_EXPECT_EQ(fixture.service->Deactivate(), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(fixture.manager->HasKeySink());
  ZR_EXPECT_TRUE(fixture.manager->HasThreadSink());
  ZR_EXPECT_EQ(fixture.manager->key_unadvise_attempts(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.manager->thread_unadvise_attempts(), std::size_t{1});
  ZR_EXPECT_EQ(fixture.Activate(), E_UNEXPECTED);

  fixture.manager->set_fail_key_unadvise(false);
  fixture.manager->set_fail_thread_unadvise(false);
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_TRUE(!fixture.manager->HasKeySink());
  ZR_EXPECT_TRUE(!fixture.manager->HasThreadSink());
  ZR_EXPECT_EQ(fixture.manager->key_unadvise_attempts(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.manager->thread_unadvise_attempts(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.service->Deactivate(), S_OK);
  ZR_EXPECT_EQ(fixture.manager->key_unadvise_attempts(), std::size_t{2});
  ZR_EXPECT_EQ(fixture.manager->thread_unadvise_attempts(), std::size_t{2});
}

ZR_TEST(UnrelatedPushDoesNotReplaceTheFocusedTopContext) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeContext *unrelated = new FakeContext();

  ZR_EXPECT_EQ(fixture.service->OnPushContext(unrelated), S_OK);
  ZR_EXPECT_EQ(fixture.Key('B'), S_OK);
  ZR_EXPECT_EQ(fixture.context->composition(0)->text(), std::u16string(u"ab"));
  ZR_EXPECT_EQ(unrelated->composition_count(), std::size_t{0});
  unrelated->Release();
}

ZR_TEST(ControlKeysFromANewContextAreNotEatenBeforeContextAdoption) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *old_composition = fixture.context->composition(0);
  FakeContext *replacement = new FakeContext();

  BOOL eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnTestKeyDown(replacement, VK_BACK, 0, &eaten),
               S_OK);
  ZR_EXPECT_TRUE(eaten == FALSE);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, VK_BACK, 0, &eaten),
               S_OK);
  ZR_EXPECT_TRUE(eaten == FALSE);
  eaten = TRUE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, VK_RETURN, 0, &eaten),
               S_OK);
  ZR_EXPECT_TRUE(eaten == FALSE);
  ZR_EXPECT_EQ(old_composition->text(), std::u16string(u"a"));
  ZR_EXPECT_TRUE(!old_composition->ended());

  eaten = FALSE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, 'B', 0, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_TRUE(old_composition->ended());
  ZR_EXPECT_EQ(replacement->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"b"));
  replacement->Release();
}

ZR_TEST(StalePermanentlyRejectedBarrierDoesNotBlockTheNewFocus) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *old_composition = fixture.context->composition(0);
  old_composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);

  FakeContext *replacement = new FakeContext();
  replacement->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  FakeDocumentManager *replacement_document =
      new FakeDocumentManager(replacement);
  ZR_EXPECT_EQ(
      fixture.service->OnSetFocus(replacement_document, fixture.document),
      S_OK);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, 'B', 0, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(replacement->pending_sessions(), std::size_t{1});
  ZR_EXPECT_TRUE(SUCCEEDED(replacement->ExecuteSession(0)));
  ZR_EXPECT_EQ(replacement->pending_sessions(), std::size_t{0});
  ZR_EXPECT_EQ(replacement->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"b"));
  ZR_EXPECT_TRUE(!old_composition->ended());

  replacement_document->Release();
  replacement->Release();
}

ZR_TEST(SynchronousReplacementDoesNotInheritAStaleBarrierFailure) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  FakeComposition *old_composition = fixture.context->composition(0);
  old_composition->set_fail_end(true);

  fixture.context->set_request_mode(FakeContext::RequestMode::kAsynchronous);
  ZR_EXPECT_TRUE(SUCCEEDED(fixture.Key(VK_RETURN)));
  ZR_EXPECT_EQ(fixture.context->ExecuteSession(0), E_ACCESSDENIED);
  fixture.context->set_request_mode(FakeContext::RequestMode::kRequestFailure);

  FakeContext *replacement = new FakeContext();
  FakeDocumentManager *replacement_document =
      new FakeDocumentManager(replacement);
  ZR_EXPECT_EQ(
      fixture.service->OnSetFocus(replacement_document, fixture.document),
      S_OK);

  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, 'B', 0, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(replacement->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"b"));
  ZR_EXPECT_TRUE(!old_composition->ended());

  replacement_document->Release();
  replacement->Release();
}

ZR_TEST(RestoringFocusToTheSameDocumentReadsItsNewTopContext) {
  DirectServiceFixture fixture;
  ZR_EXPECT_EQ(fixture.Activate(), S_OK);
  ZR_EXPECT_EQ(fixture.Key('A'), S_OK);
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(FALSE), S_OK);

  FakeContext *replacement = new FakeContext();
  fixture.document->SetTop(replacement);
  ZR_EXPECT_EQ(fixture.service->OnSetFocus(TRUE), S_OK);
  BOOL eaten = FALSE;
  ZR_EXPECT_EQ(fixture.service->OnKeyDown(replacement, 'B', 0, &eaten), S_OK);
  ZR_EXPECT_TRUE(eaten != FALSE);
  ZR_EXPECT_EQ(replacement->composition_count(), std::size_t{1});
  ZR_EXPECT_EQ(replacement->composition(0)->text(), std::u16string(u"b"));
  replacement->Release();
}

ZR_TEST(ExportsArePresent) {
  ZR_EXPECT_TRUE(g_get_class_object != nullptr);
  ZR_EXPECT_TRUE(g_can_unload != nullptr);
  ZR_EXPECT_TRUE(g_register_server != nullptr);
  ZR_EXPECT_TRUE(g_unregister_server != nullptr);
}

ZR_TEST(ClassObjectValidatesArgumentsAndClassIdentity) {
  ZR_EXPECT_EQ(g_get_class_object(zrinput::windows::tsf::kTextServiceClsid,
                                  IID_IClassFactory, nullptr),
               E_POINTER);

  void *object = reinterpret_cast<void *>(1);
  const CLSID unknown = {0x915f830a,
                         0x3ab7,
                         0x4fb2,
                         {0x92, 0xea, 0xe4, 0x22, 0xd9, 0x64, 0xc0, 0x2e}};
  ZR_EXPECT_EQ(g_get_class_object(unknown, IID_IClassFactory, &object),
               CLASS_E_CLASSNOTAVAILABLE);
  ZR_EXPECT_TRUE(object == nullptr);
  ZR_EXPECT_EQ(g_can_unload(), S_OK);
}

ZR_TEST(ClassFactoryCreatesAllRequiredTsfInterfaces) {
  IClassFactory *factory = nullptr;
  ZR_EXPECT_EQ(g_get_class_object(zrinput::windows::tsf::kTextServiceClsid,
                                  IID_IClassFactory,
                                  reinterpret_cast<void **>(&factory)),
               S_OK);
  ZR_EXPECT_TRUE(factory != nullptr);
  ZR_EXPECT_EQ(g_can_unload(), S_FALSE);

  void *aggregate = reinterpret_cast<void *>(1);
  ZR_EXPECT_EQ(factory->CreateInstance(reinterpret_cast<IUnknown *>(1),
                                       IID_ITfTextInputProcessorEx, &aggregate),
               CLASS_E_NOAGGREGATION);
  ZR_EXPECT_TRUE(aggregate == nullptr);

  ITfTextInputProcessorEx *processor = nullptr;
  ZR_EXPECT_EQ(factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                                       reinterpret_cast<void **>(&processor)),
               S_OK);
  ZR_EXPECT_TRUE(processor != nullptr);

  ITfKeyEventSink *key_sink = nullptr;
  ITfCompositionSink *composition_sink = nullptr;
  ITfThreadMgrEventSink *thread_sink = nullptr;
  ZR_EXPECT_EQ(processor->QueryInterface(IID_ITfKeyEventSink,
                                         reinterpret_cast<void **>(&key_sink)),
               S_OK);
  ZR_EXPECT_EQ(
      processor->QueryInterface(IID_ITfCompositionSink,
                                reinterpret_cast<void **>(&composition_sink)),
      S_OK);
  ZR_EXPECT_EQ(
      processor->QueryInterface(IID_ITfThreadMgrEventSink,
                                reinterpret_cast<void **>(&thread_sink)),
      S_OK);

  ZR_EXPECT_EQ(processor->ActivateEx(nullptr, TF_CLIENTID_NULL, 0),
               E_INVALIDARG);
  ZR_EXPECT_EQ(processor->Activate(nullptr, 1), E_INVALIDARG);
  ZR_EXPECT_EQ(processor->Deactivate(), S_OK);

  thread_sink->Release();
  composition_sink->Release();
  key_sink->Release();
  processor->Release();
  factory->Release();
  ZR_EXPECT_EQ(g_can_unload(), S_OK);
}

ZR_TEST(ServerLockParticipatesInUnloadDecision) {
  IClassFactory *factory = nullptr;
  ZR_EXPECT_EQ(g_get_class_object(zrinput::windows::tsf::kTextServiceClsid,
                                  IID_IClassFactory,
                                  reinterpret_cast<void **>(&factory)),
               S_OK);
  ZR_EXPECT_EQ(factory->LockServer(TRUE), S_OK);
  factory->Release();
  ZR_EXPECT_EQ(g_can_unload(), S_FALSE);

  factory = nullptr;
  ZR_EXPECT_EQ(g_get_class_object(zrinput::windows::tsf::kTextServiceClsid,
                                  IID_IClassFactory,
                                  reinterpret_cast<void **>(&factory)),
               S_OK);
  ZR_EXPECT_EQ(factory->LockServer(FALSE), S_OK);
  factory->Release();
  ZR_EXPECT_EQ(g_can_unload(), S_OK);
}

ZR_TEST(ActivationAdvisesAndDeactivateUnadvisesEverySink) {
  IClassFactory *factory = nullptr;
  ZR_EXPECT_EQ(g_get_class_object(zrinput::windows::tsf::kTextServiceClsid,
                                  IID_IClassFactory,
                                  reinterpret_cast<void **>(&factory)),
               S_OK);
  ITfTextInputProcessorEx *processor = nullptr;
  ZR_EXPECT_EQ(factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                                       reinterpret_cast<void **>(&processor)),
               S_OK);
  FakeThreadManager *thread_manager = new FakeThreadManager();
  ZR_EXPECT_EQ(processor->ActivateEx(thread_manager,
                                     FakeThreadManager::kClientId,
                                     TF_TMAE_SECUREMODE),
               S_OK);
  ZR_EXPECT_TRUE(thread_manager->HasThreadSink());
  ZR_EXPECT_TRUE(thread_manager->HasKeySink());
  ZR_EXPECT_EQ(
      processor->ActivateEx(thread_manager, FakeThreadManager::kClientId, 0),
      E_UNEXPECTED);
  ZR_EXPECT_EQ(processor->Deactivate(), S_OK);
  ZR_EXPECT_TRUE(!thread_manager->HasThreadSink());
  ZR_EXPECT_TRUE(!thread_manager->HasKeySink());
  ZR_EXPECT_EQ(processor->Deactivate(), S_OK);

  processor->Release();
  factory->Release();
  thread_manager->Release();
  ZR_EXPECT_EQ(g_can_unload(), S_OK);
}

ZR_TEST(ActivationFailureRollsBackEarlierSubscriptions) {
  IClassFactory *factory = nullptr;
  ZR_EXPECT_EQ(g_get_class_object(zrinput::windows::tsf::kTextServiceClsid,
                                  IID_IClassFactory,
                                  reinterpret_cast<void **>(&factory)),
               S_OK);
  ITfTextInputProcessorEx *processor = nullptr;
  ZR_EXPECT_EQ(factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                                       reinterpret_cast<void **>(&processor)),
               S_OK);
  FakeThreadManager *thread_manager = new FakeThreadManager(true);
  ZR_EXPECT_EQ(
      processor->ActivateEx(thread_manager, FakeThreadManager::kClientId, 0),
      E_ACCESSDENIED);
  ZR_EXPECT_TRUE(!thread_manager->HasThreadSink());
  ZR_EXPECT_TRUE(!thread_manager->HasKeySink());
  ZR_EXPECT_EQ(processor->Deactivate(), S_OK);

  processor->Release();
  factory->Release();
  thread_manager->Release();
  ZR_EXPECT_EQ(g_can_unload(), S_OK);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    std::cerr << "usage: zrinput_tsf_smoke_tests <path-to-zrinput-tsf.dll>\n";
    return 2;
  }
  const std::filesystem::path dll_path =
      std::filesystem::absolute(std::filesystem::path(argv[1]));
  g_module = LoadLibraryW(dll_path.c_str());
  if (g_module == nullptr) {
    std::cerr << "LoadLibraryW failed for " << dll_path.string() << ": "
              << GetLastError() << '\n';
    return 2;
  }

  g_get_class_object = LoadExport<DllGetClassObjectFn>("DllGetClassObject");
  g_can_unload = LoadExport<DllCanUnloadNowFn>("DllCanUnloadNow");
  g_register_server = LoadExport<DllRegistrationFn>("DllRegisterServer");
  g_unregister_server = LoadExport<DllRegistrationFn>("DllUnregisterServer");

  const int result = zrinput::test::RunAll();
  if (g_can_unload != nullptr && g_can_unload() != S_OK) {
    std::cerr << "DLL still has live COM objects after tests\n";
    FreeLibrary(g_module);
    return 1;
  }
  FreeLibrary(g_module);
  return result;
}
