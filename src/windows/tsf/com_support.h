#pragma once

#include <unknwn.h>
#include <windows.h>

#include <new>
#include <utility>

namespace zrinput::windows::tsf {

template <typename Interface>
class ComPtr final {
 public:
  ComPtr() noexcept = default;
  explicit ComPtr(Interface* value) noexcept : value_(value) {
    if (value_ != nullptr) {
      value_->AddRef();
    }
  }

  ComPtr(const ComPtr& other) noexcept : ComPtr(other.value_) {}
  ComPtr(ComPtr&& other) noexcept : value_(other.Detach()) {}

  ~ComPtr() noexcept { Reset(); }

  ComPtr& operator=(const ComPtr& other) noexcept {
    if (this != &other) {
      Assign(other.value_);
    }
    return *this;
  }

  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = other.Detach();
    }
    return *this;
  }

  void Assign(Interface* value) noexcept {
    if (value != nullptr) {
      value->AddRef();
    }
    Reset();
    value_ = value;
  }

  void Attach(Interface* value) noexcept {
    Reset();
    value_ = value;
  }

  [[nodiscard]] Interface* Detach() noexcept {
    Interface* result = value_;
    value_ = nullptr;
    return result;
  }

  void Reset() noexcept {
    Interface* value = value_;
    value_ = nullptr;
    if (value != nullptr) {
      value->Release();
    }
  }

  [[nodiscard]] Interface* Get() const noexcept { return value_; }
  [[nodiscard]] Interface** Put() noexcept {
    Reset();
    return &value_;
  }
  [[nodiscard]] Interface* operator->() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }

 private:
  Interface* value_ = nullptr;
};

template <typename Callable>
HRESULT ComBoundary(Callable&& callable) noexcept {
  try {
    return std::forward<Callable>(callable)();
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

inline bool SameComIdentity(IUnknown* left, IUnknown* right) noexcept {
  if (left == right) {
    return true;
  }
  if (left == nullptr || right == nullptr) {
    return false;
  }
  ComPtr<IUnknown> left_identity;
  ComPtr<IUnknown> right_identity;
  if (FAILED(left->QueryInterface(IID_IUnknown,
                                  reinterpret_cast<void**>(left_identity.Put()))) ||
      FAILED(right->QueryInterface(
          IID_IUnknown, reinterpret_cast<void**>(right_identity.Put())))) {
    return false;
  }
  return left_identity.Get() == right_identity.Get();
}

}  // namespace zrinput::windows::tsf
