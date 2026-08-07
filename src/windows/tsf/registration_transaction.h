#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace zrinput::windows::tsf {

inline constexpr std::size_t kRegistrationCategoryCount = 3;

struct RawRegistryValue {
  bool existed = false;
  DWORD type = REG_NONE;
  std::vector<std::byte> bytes;

  bool operator==(const RawRegistryValue&) const = default;
};

struct ComRegistrationSnapshot {
  bool clsid_key_existed = false;
  bool inproc_key_existed = false;
  RawRegistryValue default_value;
  RawRegistryValue threading_model;

  bool operator==(const ComRegistrationSnapshot&) const = default;
};

struct RegistrationState {
  ComRegistrationSnapshot com;
  bool processor = false;
  bool profile = false;
  std::array<bool, kRegistrationCategoryCount> categories{};

  bool operator==(const RegistrationState&) const = default;
};

class RegistrationBackend {
 public:
  virtual ~RegistrationBackend() = default;

  virtual HRESULT CaptureState(RegistrationState* state) = 0;

  // `previous` is the state observed immediately before this call's write.
  // `changed` follows the same rollback-ownership contract as `created` below.
  virtual HRESULT WriteComRegistration(
      std::wstring_view module_path,
      ComRegistrationSnapshot* previous,
      bool* changed) = 0;
  virtual HRESULT RestoreComRegistration(
      const ComRegistrationSnapshot& snapshot) = 0;
  virtual HRESULT RemoveComRegistrationIfOwned(
      std::wstring_view module_path) = 0;

  // `created` reports whether this call may have created the resource and
  // therefore owns its removal during rollback, even when HRESULT is a
  // failure. Implementations must set it before returning.
  virtual HRESULT AddProcessor(bool* created) = 0;
  virtual HRESULT RemoveProcessor() = 0;
  virtual HRESULT AddProfile(std::wstring_view module_path, bool* created) = 0;
  virtual HRESULT RemoveProfile() = 0;
  virtual HRESULT AddCategory(std::size_t index, bool* created) = 0;
  virtual HRESULT RemoveCategory(std::size_t index) = 0;
};

HRESULT IsComRegistrationOwnedBy(
    const ComRegistrationSnapshot& snapshot,
    std::wstring_view module_path,
    bool* owned);

HRESULT RegisterWithBackend(RegistrationBackend& backend,
                            std::wstring_view module_path);
HRESULT UnregisterWithBackend(RegistrationBackend& backend,
                              std::wstring_view module_path);

}  // namespace zrinput::windows::tsf
