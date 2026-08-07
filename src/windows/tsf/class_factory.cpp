#include "windows/tsf/class_factory.h"

#include "windows/tsf/com_support.h"
#include "windows/tsf/module_state.h"
#include "windows/tsf/text_service.h"

#include <new>

namespace zrinput::windows::tsf {

ClassFactory::ClassFactory() noexcept { AddLiveObject(); }

ClassFactory::~ClassFactory() noexcept { RemoveLiveObject(); }

HRESULT ClassFactory::QueryInterface(REFIID iid, void** object) noexcept {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IClassFactory)) {
    return E_NOINTERFACE;
  }
  *object = static_cast<IClassFactory*>(this);
  AddRef();
  return S_OK;
}

ULONG ClassFactory::AddRef() noexcept {
  return reference_count_.fetch_add(1) + 1;
}

ULONG ClassFactory::Release() noexcept {
  const ULONG remaining = reference_count_.fetch_sub(1) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT ClassFactory::CreateInstance(IUnknown* outer,
                                     REFIID iid,
                                     void** object) noexcept {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (outer != nullptr) {
    return CLASS_E_NOAGGREGATION;
  }
  return ComBoundary([&]() -> HRESULT {
    TextService* service = new (std::nothrow) TextService();
    if (service == nullptr) {
      return E_OUTOFMEMORY;
    }
    const HRESULT result = service->QueryInterface(iid, object);
    service->Release();
    return result;
  });
}

HRESULT ClassFactory::LockServer(BOOL lock) noexcept {
  if (lock != FALSE) {
    zrinput::windows::tsf::LockServer();
  } else {
    UnlockServer();
  }
  return S_OK;
}

}  // namespace zrinput::windows::tsf
