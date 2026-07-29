#include "windows/registration.h"
#include "windows/text_service.h"
#include "windows/tsf_guids.h"

#include <windows.h>

namespace zrinput::windows {

std::atomic<long> g_object_count{0};
std::atomic<long> g_server_locks{0};
HMODULE g_module = nullptr;

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() { ++g_object_count; }

  STDMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
    if (!object)
      return E_INVALIDARG;
    *object = nullptr;
    if (interface_id == IID_IUnknown || interface_id == IID_IClassFactory)
      *object = static_cast<IClassFactory*>(this);
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
  STDMETHODIMP CreateInstance(IUnknown* outer,
                              REFIID interface_id,
                              void** object) override {
    if (outer)
      return CLASS_E_NOAGGREGATION;
    auto* service = new (std::nothrow) TextService();
    if (!service)
      return E_OUTOFMEMORY;
    const HRESULT result = service->QueryInterface(interface_id, object);
    service->Release();
    return result;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    lock ? ++g_server_locks : --g_server_locks;
    return S_OK;
  }

 private:
  ~ClassFactory() { --g_object_count; }
  std::atomic<ULONG> reference_count_{1};
};

}  // namespace zrinput::windows

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    zrinput::windows::g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

HRESULT __stdcall DllCanUnloadNow() {
  return zrinput::windows::g_object_count == 0 &&
                 zrinput::windows::g_server_locks == 0
             ? S_OK
             : S_FALSE;
}

HRESULT __stdcall DllGetClassObject(
    REFCLSID class_id,
    REFIID interface_id,
    void** object) {
  if (class_id != zrinput::windows::kTextServiceClsid)
    return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new (std::nothrow) zrinput::windows::ClassFactory();
  if (!factory)
    return E_OUTOFMEMORY;
  const HRESULT result = factory->QueryInterface(interface_id, object);
  factory->Release();
  return result;
}

HRESULT __stdcall DllRegisterServer() {
  return zrinput::windows::RegisterTextService(zrinput::windows::g_module);
}

HRESULT __stdcall DllUnregisterServer() {
  return zrinput::windows::UnregisterTextService();
}
