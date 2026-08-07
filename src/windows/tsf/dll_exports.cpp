#include "windows/tsf/class_factory.h"
#include "windows/tsf/com_support.h"
#include "windows/tsf/module_state.h"
#include "windows/tsf/registration.h"
#include "windows/tsf/tsf_guids.h"

#include <windows.h>

#include <new>

using zrinput::windows::tsf::CanUnload;
using zrinput::windows::tsf::ClassFactory;
using zrinput::windows::tsf::ComBoundary;
using zrinput::windows::tsf::RegisterServer;
using zrinput::windows::tsf::SetModuleHandle;
using zrinput::windows::tsf::UnregisterServer;
using zrinput::windows::tsf::kTextServiceClsid;

extern "C" BOOL WINAPI DllMain(HINSTANCE instance,
                               DWORD reason,
                               LPVOID) noexcept {
  if (reason == DLL_PROCESS_ATTACH) {
    SetModuleHandle(instance);
    (void)DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid,
                                        _In_ REFIID iid,
                                        _Outptr_ LPVOID FAR* object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (!IsEqualCLSID(clsid, kTextServiceClsid)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  ClassFactory* factory = new (std::nothrow) ClassFactory();
  if (factory == nullptr) {
    return E_OUTOFMEMORY;
  }
  const HRESULT result = factory->QueryInterface(iid, object);
  factory->Release();
  return result;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() {
  return CanUnload() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
  return ComBoundary([]() { return RegisterServer(); });
}

STDAPI DllUnregisterServer() {
  return ComBoundary([]() { return UnregisterServer(); });
}
