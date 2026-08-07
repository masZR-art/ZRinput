#pragma once

#include <unknwn.h>

#include <atomic>

namespace zrinput::windows::tsf {

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() noexcept;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** object) noexcept override;
  ULONG STDMETHODCALLTYPE AddRef() noexcept override;
  ULONG STDMETHODCALLTYPE Release() noexcept override;
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,
                                           REFIID iid,
                                           void** object) noexcept override;
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override;

 private:
  ~ClassFactory() noexcept;

  std::atomic<ULONG> reference_count_{1};
};

}  // namespace zrinput::windows::tsf
