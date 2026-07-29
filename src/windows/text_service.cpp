#include "windows/text_service.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>

namespace zrinput::windows {
extern HMODULE g_module;
namespace {

template <typename T>
void SafeRelease(T*& value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty())
    return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0);
  if (length <= 0)
    return {};
  std::wstring result(length, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), length) !=
      length)
    return {};
  return result;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty())
    return {};
  const int length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (length <= 0)
    return {};
  std::string result(length, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), length,
                          nullptr, nullptr) != length)
    return {};
  return result;
}

std::string HostApplication() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                          static_cast<DWORD>(path.size()));
  if (!length || length >= path.size())
    return {};
  path.resize(length);
  return WideToUtf8(std::filesystem::path(path).filename().wstring());
}

bool IsPrivateInput() {
  GUITHREADINFO info{sizeof(info)};
  if (!GetGUIThreadInfo(0, &info) || !info.hwndFocus)
    return false;
  wchar_t class_name[64] = {};
  const int class_length = GetClassNameW(info.hwndFocus, class_name,
                                         static_cast<int>(_countof(class_name)));
  if (class_length <= 0)
    return false;
  CharLowerBuffW(class_name, class_length);
  if (!wcsstr(class_name, L"edit"))
    return false;
  return (GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_PASSWORD) != 0;
}

std::filesystem::path UserDataPath() {
  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &local_app_data)))
    return {};
  std::filesystem::path result(local_app_data);
  CoTaskMemFree(local_app_data);
  result /= L"ZRinput";
  std::error_code error;
  std::filesystem::create_directories(result, error);
  return error ? std::filesystem::path{} : result;
}

std::filesystem::path ModuleDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(g_module, path.data(),
                                          static_cast<DWORD>(path.size()));
  if (!length || length >= path.size())
    return {};
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

bool IsLegacyMicrosoftDark(const Theme& theme) {
  return theme.background == RGB(32, 32, 32) &&
         theme.selected == RGB(62, 62, 62) &&
         theme.accent == RGB(0, 120, 212) &&
         theme.text == RGB(245, 245, 245) && theme.font_size == 19 &&
         theme.window_height == 44;
}

HRESULT MoveSelectionToRangeEnd(ITfContext* context,
                                TfEditCookie cookie,
                                ITfRange* range,
                                RECT* text_extent,
                                bool* has_text_extent) {
  ITfRange* caret = nullptr;
  HRESULT result = range->Clone(&caret);
  if (FAILED(result))
    return result;
  result = caret->Collapse(cookie, TF_ANCHOR_END);
  if (SUCCEEDED(result)) {
    TF_SELECTION selection{};
    selection.range = caret;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    result = context->SetSelection(cookie, 1, &selection);
  }
  if (SUCCEEDED(result) && text_extent && has_text_extent) {
    ITfContextView* view = nullptr;
    if (SUCCEEDED(context->GetActiveView(&view))) {
      BOOL clipped = FALSE;
      if (SUCCEEDED(view->GetTextExt(cookie, caret, text_extent, &clipped)))
        *has_text_extent = true;
      view->Release();
    }
  }
  caret->Release();
  return result;
}

class CompositionEditSession final : public ITfEditSession {
 public:
  CompositionEditSession(ITfContext* context,
                         ITfComposition** composition,
                         ITfContext** composition_context,
                         ITfCompositionSink* composition_sink,
                         CandidateWindow* candidate_window,
                         bool* ending_composition,
                         std::uint64_t* current_generation,
                         std::uint64_t generation,
                         std::wstring text,
                         bool end_composition)
      : context_(context),
        composition_(composition),
        composition_context_(composition_context),
        composition_sink_(composition_sink),
        candidate_window_(candidate_window),
        ending_composition_(ending_composition),
        current_generation_(current_generation),
        generation_(generation),
        text_(std::move(text)),
        end_composition_(end_composition) {
    context_->AddRef();
    composition_sink_->AddRef();
  }

  STDMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
    if (!object)
      return E_INVALIDARG;
    *object = nullptr;
    if (interface_id == IID_IUnknown || interface_id == IID_ITfEditSession)
      *object = static_cast<ITfEditSession*>(this);
    if (!*object)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++reference_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --reference_count_;
    if (!count)
      delete this;
    return count;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    if (*current_generation_ != generation_)
      return S_OK;

    RECT text_extent{};
    bool has_text_extent = false;
    HRESULT result = S_OK;
    if (*composition_) {
      ITfRange* range = nullptr;
      result = (*composition_)->GetRange(&range);
      if (SUCCEEDED(result)) {
        result = range->SetText(cookie, 0, text_.c_str(),
                                static_cast<LONG>(text_.size()));
        if (SUCCEEDED(result))
          result = MoveSelectionToRangeEnd(context_, cookie, range,
                                           &text_extent, &has_text_extent);
        range->Release();
      }
      if (SUCCEEDED(result) && end_composition_) {
        *ending_composition_ = true;
        result = (*composition_)->EndComposition(cookie);
        *ending_composition_ = false;
        if (SUCCEEDED(result))
          SafeRelease(*composition_);
      }
    } else if (!text_.empty()) {
      ITfInsertAtSelection* insert = nullptr;
      result = context_->QueryInterface(
          IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert));
      if (SUCCEEDED(result)) {
        ITfRange* range = nullptr;
        result = insert->InsertTextAtSelection(
            cookie, TF_IAS_QUERYONLY, nullptr, 0, &range);
        insert->Release();
        if (SUCCEEDED(result)) {
          ITfContextComposition* context_composition = nullptr;
          result = context_->QueryInterface(
              IID_ITfContextComposition,
              reinterpret_cast<void**>(&context_composition));
          if (SUCCEEDED(result)) {
            result = context_composition->StartComposition(
                cookie, range, composition_sink_, composition_);
            context_composition->Release();
          }
          if (SUCCEEDED(result))
            result = range->SetText(cookie, 0, text_.c_str(),
                                    static_cast<LONG>(text_.size()));
          if (SUCCEEDED(result))
            result = MoveSelectionToRangeEnd(context_, cookie, range,
                                             &text_extent, &has_text_extent);
          if (SUCCEEDED(result) && end_composition_) {
            *ending_composition_ = true;
            result = (*composition_)->EndComposition(cookie);
            *ending_composition_ = false;
            if (SUCCEEDED(result))
              SafeRelease(*composition_);
          }
          SafeRelease(range);
        }
      }
    }

    if (SUCCEEDED(result) && *current_generation_ == generation_) {
      if (has_text_extent)
        candidate_window_->SetAnchor(text_extent);
      else
        candidate_window_->ClearAnchor();
      if (end_composition_ || !*composition_) {
        SafeRelease(*composition_context_);
      } else if (*composition_context_ != context_) {
        SafeRelease(*composition_context_);
        *composition_context_ = context_;
        (*composition_context_)->AddRef();
      }
    }
    return result;
  }

 private:
  ~CompositionEditSession() {
    SafeRelease(context_);
    SafeRelease(composition_sink_);
  }
  std::atomic<ULONG> reference_count_{1};
  ITfContext* context_;
  ITfComposition** composition_;
  ITfContext** composition_context_;
  ITfCompositionSink* composition_sink_;
  CandidateWindow* candidate_window_;
  bool* ending_composition_;
  std::uint64_t* current_generation_;
  std::uint64_t generation_;
  std::wstring text_;
  bool end_composition_;
};

}  // namespace

TextService::TextService() {
  ++g_object_count;
  application_ = HostApplication();
  engine_.AddEntry("xian zai", "现在", 100);
  engine_.AddEntry("xian zai", "先在", 40);
  engine_.AddEntry("wo", "我", 100);
  engine_.AddEntry("ni", "你", 100);
  engine_.AddEntry("shi", "是", 100);
  engine_.AddEntry("de", "的", 100);
  engine_.AddEntry("shu ru fa", "输入法", 100);
  engine_.AddEntry("zhong wen", "中文", 100);
  engine_.AddEntry("emoji", "😀", 100);
  engine_.AddEntry("emoji", "😂", 95);
  engine_.AddEntry("emoji", "❤️", 90);
  engine_.AddEntry("emoji", "👍", 85);
  engine_.AddEntry("emoji", "🎉", 80);
  const bool test_mode = GetEnvironmentVariableW(
                             L"ZRINPUT_TEST_MODE", nullptr, 0) != 0;
  const auto module_directory = ModuleDirectory();
  auto dictionary_path = module_directory.empty()
                             ? std::filesystem::path{}
                             : module_directory / L"data" /
                                   L"default_lexicon.tsv";
  if (test_mode) {
    std::wstring override_path(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"ZRINPUT_TEST_DICTIONARY", override_path.data(),
        static_cast<DWORD>(override_path.size()));
    if (length > 0 && length < override_path.size()) {
      override_path.resize(length);
      dictionary_path = override_path;
    }
  }
  DictionaryLoadResult dictionary_load;
  if (!dictionary_path.empty()) {
    dictionary_load = engine_.LoadDictionary(dictionary_path, false);
  } else {
    dictionary_load.error = "cannot resolve module directory";
  }
  dictionary_ready_ =
      IsRuntimeDictionaryUsable(engine_, dictionary_load);
  if (!dictionary_ready_)
    OutputDebugStringW(
        L"ZRinput disabled: the production dictionary failed validation.\n");
  Theme theme;
  if (!module_directory.empty())
    theme.Load(module_directory / L"themes" / L"microsoft-dark.ini");
  const auto data_path = test_mode ? std::filesystem::path{} : UserDataPath();
  if (!data_path.empty()) {
    const auto active_theme_path = data_path / L"themes" / L"active.ini";
    Theme active_theme = theme;
    if (active_theme.Load(active_theme_path)) {
      if (IsLegacyMicrosoftDark(active_theme))
        theme.Save(active_theme_path);
      else
        theme = active_theme;
    }
    memory_path_ = data_path / L"personal-model.zrim";
    engine_.memory().Load(memory_path_);
  }
  candidate_window_.SetTheme(theme);
}

TextService::~TextService() {
  if (!memory_path_.empty())
    engine_.memory().Save(memory_path_);
  Deactivate();
  --g_object_count;
}

STDMETHODIMP TextService::QueryInterface(REFIID interface_id, void** object) {
  if (!object)
    return E_INVALIDARG;
  *object = nullptr;
  if (interface_id == IID_IUnknown ||
      interface_id == IID_ITfTextInputProcessor)
    *object = static_cast<ITfTextInputProcessor*>(this);
  else if (interface_id == IID_ITfKeyEventSink)
    *object = static_cast<ITfKeyEventSink*>(this);
  else if (interface_id == IID_ITfCompositionSink)
    *object = static_cast<ITfCompositionSink*>(this);
  if (!*object)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
  return ++reference_count_;
}

STDMETHODIMP_(ULONG) TextService::Release() {
  const ULONG count = --reference_count_;
  if (!count)
    delete this;
  return count;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* thread_manager,
                                   TfClientId client_id) {
  if (!thread_manager || thread_manager_)
    return E_INVALIDARG;
  thread_manager_ = thread_manager;
  thread_manager_->AddRef();
  client_id_ = client_id;
  ITfKeystrokeMgr* keystroke_manager = nullptr;
  HRESULT result = thread_manager_->QueryInterface(
      IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keystroke_manager));
  if (SUCCEEDED(result)) {
    result = keystroke_manager->AdviseKeyEventSink(client_id_, this, TRUE);
    keystroke_manager->Release();
  }
  return result;
}

STDMETHODIMP TextService::Deactivate() {
  CancelComposition();
  if (thread_manager_) {
    ITfKeystrokeMgr* keystroke_manager = nullptr;
    if (SUCCEEDED(thread_manager_->QueryInterface(
            IID_ITfKeystrokeMgr,
            reinterpret_cast<void**>(&keystroke_manager)))) {
      keystroke_manager->UnadviseKeyEventSink(client_id_);
      keystroke_manager->Release();
    }
  }
  SafeRelease(thread_manager_);
  SafeRelease(composition_);
  SafeRelease(composition_context_);
  client_id_ = TF_CLIENTID_NULL;
  Reset();
  return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
  if (!foreground) {
    CancelComposition();
    Reset();
    committed_context_.clear();
  }
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context,
                                        WPARAM key,
                                        LPARAM,
                                        BOOL* eaten) {
  if (!context || !eaten)
    return E_INVALIDARG;
  *eaten = ShouldHandle(context, key);
  return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context,
                                    WPARAM key,
                                    LPARAM,
                                    BOOL* eaten) {
  if (!context || !eaten)
    return E_INVALIDARG;
  *eaten = HandleKey(context, key);
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext*,
                                      WPARAM,
                                      LPARAM,
                                      BOOL* eaten) {
  if (!eaten)
    return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*,
                                  WPARAM,
                                  LPARAM,
                                  BOOL* eaten) {
  if (!eaten)
    return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
  if (!eaten)
    return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(
    TfEditCookie,
    ITfComposition* composition) {
  if (composition_ == composition) {
    SafeRelease(composition_);
    SafeRelease(composition_context_);
  }
  if (!ending_composition_)
    Reset();
  return S_OK;
}

bool TextService::ShouldHandle(ITfContext* context, WPARAM key) const {
  if (!dictionary_ready_ || !context)
    return false;
  const bool command_modifier = (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool shifted = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  return (!command_modifier && key >= 'A' && key <= 'Z') ||
         (!command_modifier &&
          (key == VK_OEM_COMMA || key == VK_OEM_PERIOD)) ||
         (!command_modifier && !shifted && !state_.empty() &&
          key == VK_OEM_7) ||
         (!state_.empty() && (key == VK_BACK || key == VK_ESCAPE ||
                             key == VK_SPACE || key == VK_RETURN ||
                             key == VK_LEFT ||
                             key == VK_RIGHT || key == VK_PRIOR ||
                             key == VK_NEXT ||
                             (!shifted && key >= '1' &&
                              key < '1' + kPageSize)));
}

bool TextService::HandleKey(ITfContext* context, WPARAM key) {
  if (!dictionary_ready_ || !context)
    return false;
  const bool command_modifier = (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                (GetKeyState(VK_MENU) & 0x8000) != 0;
  if (command_modifier)
    return false;
  const bool shifted = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (key == VK_OEM_COMMA || key == VK_OEM_PERIOD) {
    const std::string punctuation =
        key == VK_OEM_COMMA ? (shifted ? "《" : "，")
                            : (shifted ? "》" : "。");
    if (state_.empty()) {
      if (FAILED(Commit(context, punctuation)))
        return false;
      if (!IsPrivateInput()) {
        committed_context_.push_back(punctuation);
        if (committed_context_.size() > 4) {
          committed_context_.erase(committed_context_.begin(),
                                   committed_context_.end() - 4);
        }
      }
      return true;
    }
    const std::string input = state_.input();
    const std::string selected =
        candidates_.empty() ? input : candidates_.front().text;
    if (SUCCEEDED(Commit(context, selected + punctuation))) {
      const bool retained = candidates_.empty()
                                ? !IsPrivateInput()
                                : LearnSelection(selected, input, 0);
      if (retained) {
        committed_context_.push_back(selected);
        committed_context_.push_back(punctuation);
        if (committed_context_.size() > 4)
          committed_context_.erase(committed_context_.begin(),
                                   committed_context_.end() - 4);
      }
      Reset();
      return true;
    }
    return false;
  }
  if (key >= 'A' && key <= 'Z') {
    if (!state_.Append(static_cast<char>('a' + key - 'A')))
      return true;
    if (FAILED(UpdateComposition(context, Utf8ToWide(state_.input()), false))) {
      state_.Backspace();
      RefreshCandidates();
      return false;
    }
    RefreshCandidates();
    return true;
  }
  if (key == VK_OEM_7) {
    if (shifted)
      return false;
    if (!state_.Append('\''))
      return true;
    if (FAILED(UpdateComposition(context, Utf8ToWide(state_.input()), false))) {
      state_.Backspace();
      RefreshCandidates();
      return false;
    }
    RefreshCandidates();
    return true;
  }
  if (state_.empty())
    return false;
  if (key == VK_BACK) {
    const std::string previous_input = state_.input();
    state_.Backspace();
    if (FAILED(UpdateComposition(context, Utf8ToWide(state_.input()),
                                 state_.empty()))) {
      state_.SetInput(previous_input);
      RefreshCandidates();
      return false;
    }
    RefreshCandidates();
    return true;
  }
  if (key == VK_ESCAPE) {
    if (FAILED(CancelComposition()))
      return false;
    Reset();
    return true;
  }
  if (key == VK_LEFT || key == VK_PRIOR) {
    ChangePage(-1);
    return true;
  }
  if (key == VK_RIGHT || key == VK_NEXT) {
    ChangePage(1);
    return true;
  }
  if (key == VK_RETURN || (key == VK_SPACE && candidates_.empty())) {
    if (SUCCEEDED(Commit(context, state_.input()))) {
      Reset();
      return true;
    }
    return false;
  }
  const bool candidate_key =
      !shifted && key >= '1' && key < '1' + kPageSize;
  const std::size_t slot =
      candidate_key ? static_cast<std::size_t>(key - '1') : 0;
  const auto index =
      state_.CandidateIndex(slot, candidates_.size(), kPageSize);
  if ((key == VK_SPACE || candidate_key) && index.has_value()) {
    const std::string selected = candidates_[*index].text;
    if (SUCCEEDED(Commit(context, selected))) {
      if (LearnSelection(selected, state_.input(), *index)) {
        committed_context_.push_back(selected);
        if (committed_context_.size() > 4)
          committed_context_.erase(committed_context_.begin());
      }
      Reset();
      return true;
    }
    return false;
  }
  if (key == VK_SPACE || candidate_key)
    return true;
  return false;
}

bool TextService::LearnSelection(const std::string& selected,
                                 const std::string& input,
                                 std::size_t candidate_index) {
  if (IsPrivateInput())
    return false;
  LearningEvent event;
  event.text = selected;
  event.input = input;
  event.application = application_;
  event.context = committed_context_;
  event.timestamp = static_cast<std::int64_t>(time(nullptr));
  if (candidate_index != 0 && !candidates_.empty()) {
    auto rejected = event;
    rejected.text = candidates_.front().text;
    engine_.memory().Reject(rejected);
  }
  engine_.memory().Accept(event);
  if (!memory_path_.empty())
    engine_.memory().Save(memory_path_);
  return true;
}

void TextService::RefreshCandidates() {
  if (state_.empty()) {
    candidates_.clear();
    state_.CandidatesChanged();
    candidate_window_.Hide();
    return;
  }
  LearningEvent request;
  request.input = state_.input();
  request.application = application_;
  request.context = committed_context_;
  request.timestamp = static_cast<std::int64_t>(time(nullptr));
  candidates_ = engine_.Query(request, 50);
  state_.CandidatesChanged();
  candidate_window_.Show(candidates_, state_.page(), kPageSize);
}

void TextService::ChangePage(int delta) {
  state_.ChangePage(delta, candidates_.size(), kPageSize);
  candidate_window_.Show(candidates_, state_.page(), kPageSize);
}

HRESULT TextService::Commit(ITfContext* context,
                            const std::string& utf8_text) {
  const std::wstring text = Utf8ToWide(utf8_text);
  if (text.empty())
    return E_INVALIDARG;
  return UpdateComposition(context, text, true);
}

HRESULT TextService::UpdateComposition(ITfContext* context,
                                       const std::wstring& text,
                                       bool end_composition) {
  if (!context)
    return E_INVALIDARG;

  const std::uint64_t previous_generation = edit_generation_;
  const std::uint64_t generation = previous_generation + 1;
  edit_generation_ = generation;
  auto* edit_session = new (std::nothrow) CompositionEditSession(
      context, &composition_, &composition_context_, this, &candidate_window_,
      &ending_composition_, &edit_generation_, generation, text,
      end_composition);
  if (!edit_session) {
    edit_generation_ = previous_generation;
    return E_OUTOFMEMORY;
  }

  HRESULT session_result = E_FAIL;
  HRESULT request_result = context->RequestEditSession(
      client_id_, edit_session, TF_ES_SYNC | TF_ES_READWRITE, &session_result);
  if (SUCCEEDED(request_result) && SUCCEEDED(session_result)) {
    edit_session->Release();
    return session_result;
  }

  const HRESULT synchronous_failure =
      FAILED(request_result) ? request_result : session_result;
  if (!end_composition) {
    session_result = E_FAIL;
    request_result = context->RequestEditSession(
        client_id_, edit_session, TF_ES_ASYNC | TF_ES_READWRITE,
        &session_result);
    if (SUCCEEDED(request_result) && SUCCEEDED(session_result)) {
      if (composition_context_ != context) {
        SafeRelease(composition_context_);
        composition_context_ = context;
        composition_context_->AddRef();
      }
      edit_session->Release();
      return S_OK;
    }
  }

  edit_generation_ = previous_generation;
  edit_session->Release();
  return synchronous_failure;
}

HRESULT TextService::CancelComposition() {
  if (composition_context_) {
    const HRESULT result = UpdateComposition(composition_context_, L"", true);
    if (SUCCEEDED(result))
      SafeRelease(composition_context_);
    return result;
  }
  return S_OK;
}

void TextService::Reset() {
  ++edit_generation_;
  state_.Reset();
  candidates_.clear();
  candidate_window_.Hide();
  candidate_window_.ClearAnchor();
}

}  // namespace zrinput::windows
