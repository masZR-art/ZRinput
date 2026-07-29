#include "windows/tsf_guids.h"

#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <filesystem>
#include <iostream>

namespace {

using DllCanUnloadNowFunction = HRESULT(__stdcall*)();
using DllGetClassObjectFunction = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);

class RejectingContext final : public ITfContext {
 public:
  STDMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
    if (!object)
      return E_INVALIDARG;
    *object = nullptr;
    if (interface_id == IID_IUnknown || interface_id == IID_ITfContext)
      *object = static_cast<ITfContext*>(this);
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
  STDMETHODIMP RequestEditSession(TfClientId,
                                  ITfEditSession*,
                                  DWORD flags,
                                  HRESULT* session_result) override {
    if (!session_result)
      return E_INVALIDARG;
    ++request_count;
    *session_result = (flags & TF_ES_ASYNC) ? E_FAIL : TF_E_SYNCHRONOUS;
    return (flags & TF_ES_ASYNC) ? E_FAIL : S_OK;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetSelection(TfEditCookie,
                            ULONG,
                            ULONG,
                            TF_SELECTION*,
                            ULONG*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP SetSelection(TfEditCookie,
                            ULONG,
                            const TF_SELECTION*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
  STDMETHODIMP GetStatus(TF_STATUS*) override { return E_NOTIMPL; }
  STDMETHODIMP GetProperty(REFGUID, ITfProperty**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP TrackProperties(const GUID**,
                               ULONG,
                               const GUID**,
                               ULONG,
                               ITfReadOnlyProperty**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumProperties(IEnumTfProperties**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP CreateRangeBackup(TfEditCookie,
                                 ITfRange*,
                                 ITfRangeBackup**) override {
    return E_NOTIMPL;
  }

  int request_count = 0;

 private:
  ~RejectingContext() = default;
  std::atomic<ULONG> reference_count_{1};
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::cerr << "usage: zrinput_tsf_dll_smoke_tests DLL\n";
    return 2;
  }
  SetEnvironmentVariableW(L"ZRINPUT_TEST_MODE", L"1");
  const auto installed_dictionary =
      std::filesystem::path(argv[1]).parent_path() / L"data" /
      L"default_lexicon.tsv";
  const auto source_dictionary =
      std::filesystem::path(argv[1]).parent_path().parent_path().parent_path() /
      L"data" / L"default_lexicon.tsv";
  const auto runtime_dictionary = std::filesystem::exists(installed_dictionary)
                                      ? installed_dictionary
                                      : source_dictionary;
  const bool can_test_keys = std::filesystem::exists(runtime_dictionary);
  const bool can_test_missing_dictionary =
      !std::filesystem::exists(installed_dictionary);
  if (can_test_keys) {
    SetEnvironmentVariableW(L"ZRINPUT_TEST_DICTIONARY",
                            runtime_dictionary.c_str());
  }
  HMODULE module = LoadLibraryW(argv[1]);
  if (!module) {
    std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
    return 1;
  }
  const auto can_unload = reinterpret_cast<DllCanUnloadNowFunction>(
      GetProcAddress(module, "DllCanUnloadNow"));
  const auto get_class_object = reinterpret_cast<DllGetClassObjectFunction>(
      GetProcAddress(module, "DllGetClassObject"));
  if (!can_unload || !get_class_object || can_unload() != S_OK) {
    std::cerr << "required COM exports are missing or initially busy\n";
    FreeLibrary(module);
    return 1;
  }

  IClassFactory* factory = nullptr;
  HRESULT result = get_class_object(
      zrinput::windows::kTextServiceClsid, IID_IClassFactory,
      reinterpret_cast<void**>(&factory));
  if (SUCCEEDED(result) && can_unload() != S_FALSE) {
    std::cerr << "DLL reported unloadable while its class factory was alive\n";
    result = E_FAIL;
  }
  ITfTextInputProcessor* service = nullptr;
  if (SUCCEEDED(result))
    result = factory->CreateInstance(
        nullptr, IID_ITfTextInputProcessor,
        reinterpret_cast<void**>(&service));
  if (SUCCEEDED(result) && can_unload() != S_FALSE) {
    std::cerr << "DLL reported unloadable while its text service was alive\n";
    result = E_FAIL;
  }
  ITfKeyEventSink* key_sink = nullptr;
  if (SUCCEEDED(result) && can_test_keys)
    result = service->QueryInterface(IID_ITfKeyEventSink,
                                     reinterpret_cast<void**>(&key_sink));
  if (SUCCEEDED(result) && key_sink) {
    auto* context = new RejectingContext();
    BOOL tested_eaten = FALSE;
    BOOL key_eaten = TRUE;
    BOOL backspace_eaten = TRUE;
    const HRESULT tested =
        key_sink->OnTestKeyDown(context, 'A', 0, &tested_eaten);
    const HRESULT handled = key_sink->OnKeyDown(context, 'A', 0, &key_eaten);
    const HRESULT rollback_test =
        key_sink->OnTestKeyDown(context, VK_BACK, 0, &backspace_eaten);
    if (FAILED(tested) || FAILED(handled) || FAILED(rollback_test) ||
        !tested_eaten || key_eaten || backspace_eaten ||
        context->request_count != 2) {
      std::cerr << "rejected edit sessions swallowed a key or kept state\n";
      result = E_FAIL;
    }
    context->Release();
  }
  if (key_sink)
    key_sink->Release();
  key_sink = nullptr;
  if (service)
    service->Release();
  service = nullptr;

  if (SUCCEEDED(result) && can_test_missing_dictionary) {
    SetEnvironmentVariableW(L"ZRINPUT_TEST_DICTIONARY", nullptr);
    ITfTextInputProcessor* disabled_service = nullptr;
    result = factory->CreateInstance(
        nullptr, IID_ITfTextInputProcessor,
        reinterpret_cast<void**>(&disabled_service));
    ITfKeyEventSink* disabled_keys = nullptr;
    if (SUCCEEDED(result)) {
      result = disabled_service->QueryInterface(
          IID_ITfKeyEventSink, reinterpret_cast<void**>(&disabled_keys));
    }
    if (SUCCEEDED(result)) {
      auto* context = new RejectingContext();
      BOOL eaten = TRUE;
      const HRESULT tested =
          disabled_keys->OnTestKeyDown(context, 'A', 0, &eaten);
      if (FAILED(tested) || eaten || context->request_count != 0) {
        std::cerr << "missing dictionary did not fail safe to pass-through\n";
        result = E_FAIL;
      }
      context->Release();
    }
    if (disabled_keys)
      disabled_keys->Release();
    if (disabled_service)
      disabled_service->Release();
  }
  if (service)
    service->Release();
  if (factory)
    factory->Release();
  const bool unloaded = can_unload() == S_OK;
  FreeLibrary(module);
  if (FAILED(result) || !unloaded) {
    std::cerr << "TSF class factory smoke test failed\n";
    return 1;
  }
  std::cout << "TSF DLL exports and class factory passed.\n";
  return 0;
}
