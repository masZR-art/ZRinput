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

class CommitEditSession final : public ITfEditSession {
 public:
  CommitEditSession(ITfContext* context, std::wstring text)
      : context_(context), text_(std::move(text)) {
    context_->AddRef();
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
    ITfInsertAtSelection* insert = nullptr;
    HRESULT result = context_->QueryInterface(
        IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert));
    if (FAILED(result))
      return result;

    ITfRange* range = nullptr;
    result = insert->InsertTextAtSelection(
        cookie, TF_IAS_NOQUERY, text_.c_str(), static_cast<LONG>(text_.size()),
        &range);
    insert->Release();
    SafeRelease(range);
    return result;
  }

 private:
  ~CommitEditSession() { SafeRelease(context_); }
  std::atomic<ULONG> reference_count_{1};
  ITfContext* context_;
  std::wstring text_;
};

}  // namespace

TextService::TextService() {
  ++g_object_count;
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
  const auto module_directory = ModuleDirectory();
  if (!module_directory.empty())
    engine_.LoadDictionary(module_directory / L"data" /
                           L"default_lexicon.tsv", false);
  const auto data_path = UserDataPath();
  if (!data_path.empty()) {
    memory_path_ = data_path / L"personal-model.zrim";
    engine_.memory().Load(memory_path_);
  }
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
  client_id_ = TF_CLIENTID_NULL;
  Reset();
  return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
  if (!foreground)
    Reset();
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext*,
                                        WPARAM key,
                                        LPARAM,
                                        BOOL* eaten) {
  if (!eaten)
    return E_INVALIDARG;
  *eaten = ShouldHandle(key);
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

bool TextService::ShouldHandle(WPARAM key) const {
  return (key >= 'A' && key <= 'Z') ||
         (!input_.empty() && (key == VK_BACK || key == VK_ESCAPE ||
                             key == VK_SPACE || key == VK_LEFT ||
                             key == VK_RIGHT || key == VK_PRIOR ||
                             key == VK_NEXT || (key >= '1' && key <= '5')));
}

bool TextService::HandleKey(ITfContext* context, WPARAM key) {
  if (key >= 'A' && key <= 'Z') {
    input_.push_back(static_cast<char>('a' + key - 'A'));
    RefreshCandidates();
    return true;
  }
  if (input_.empty())
    return false;
  if (key == VK_BACK) {
    input_.pop_back();
    RefreshCandidates();
    return true;
  }
  if (key == VK_ESCAPE) {
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
  std::size_t index = 0;
  if (key >= '1' && key <= '5')
    index = static_cast<std::size_t>(key - '1');
  index += page_ * kPageSize;
  if ((key == VK_SPACE || (key >= '1' && key <= '5')) &&
      index < candidates_.size()) {
    const std::string selected = candidates_[index].text;
    if (SUCCEEDED(Commit(context, selected))) {
      LearningEvent event;
      event.text = selected;
      event.input = input_;
      event.context = committed_context_;
      event.timestamp = static_cast<std::int64_t>(time(nullptr));
      engine_.memory().Accept(event);
      if (!memory_path_.empty())
        engine_.memory().Save(memory_path_);
      committed_context_.push_back(selected);
      if (committed_context_.size() > 4)
        committed_context_.erase(committed_context_.begin());
      Reset();
    }
    return true;
  }
  return false;
}

void TextService::RefreshCandidates() {
  if (input_.empty()) {
    candidates_.clear();
    page_ = 0;
    candidate_window_.Hide();
    return;
  }
  LearningEvent request;
  request.input = input_;
  request.context = committed_context_;
  request.timestamp = static_cast<std::int64_t>(time(nullptr));
  candidates_ = engine_.Query(request, 50);
  page_ = 0;
  candidate_window_.Show(candidates_, page_, kPageSize);
}

void TextService::ChangePage(int delta) {
  if (candidates_.empty())
    return;
  const std::size_t page_count =
      (candidates_.size() + kPageSize - 1) / kPageSize;
  if (delta < 0 && page_ > 0)
    --page_;
  else if (delta > 0 && page_ + 1 < page_count)
    ++page_;
  candidate_window_.Show(candidates_, page_, kPageSize);
}

HRESULT TextService::Commit(ITfContext* context,
                            const std::string& utf8_text) {
  const std::wstring text = Utf8ToWide(utf8_text);
  if (text.empty())
    return E_INVALIDARG;
  auto* edit_session = new (std::nothrow) CommitEditSession(context, text);
  if (!edit_session)
    return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT request_result = context->RequestEditSession(
      client_id_, edit_session, TF_ES_SYNC | TF_ES_READWRITE, &session_result);
  edit_session->Release();
  return FAILED(request_result) ? request_result : session_result;
}

void TextService::Reset() {
  input_.clear();
  candidates_.clear();
  page_ = 0;
  candidate_window_.Hide();
}

}  // namespace zrinput::windows
