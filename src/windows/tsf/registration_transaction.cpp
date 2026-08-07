#include "windows/tsf/registration_transaction.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace zrinput::windows::tsf {
namespace {

constexpr wchar_t kThreadingModel[] = L"Apartment";

HRESULT ValidateModulePath(std::wstring_view module_path) noexcept {
  if (module_path.empty() ||
      module_path.size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      std::find(module_path.begin(), module_path.end(), L'\0') !=
          module_path.end()) {
    return E_INVALIDARG;
  }
  return S_OK;
}

template <typename Callable>
HRESULT InvokeBackend(Callable&& callable) noexcept {
  try {
    return std::forward<Callable>(callable)();
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

void CaptureFirstFailure(HRESULT result, HRESULT* first_failure) noexcept {
  if (SUCCEEDED(*first_failure) && FAILED(result)) {
    *first_failure = result;
  }
}

bool HasTsfRegistration(const RegistrationState& state) noexcept {
  return state.processor || state.profile ||
         std::ranges::any_of(state.categories,
                             [](bool present) { return present; });
}

bool HasConsistentShape(const ComRegistrationSnapshot& snapshot) noexcept {
  if ((!snapshot.clsid_key_existed && snapshot.inproc_key_existed) ||
      (!snapshot.inproc_key_existed &&
       (snapshot.default_value.existed ||
        snapshot.threading_model.existed))) {
    return false;
  }
  const auto absent_value_is_empty = [](const RawRegistryValue& value) {
    return value.existed ||
           (value.type == REG_NONE && value.bytes.empty());
  };
  return absent_value_is_empty(snapshot.default_value) &&
         absent_value_is_empty(snapshot.threading_model);
}

HRESULT MakeRegistryString(std::wstring_view value,
                           RawRegistryValue* result) {
  if (result == nullptr) {
    return E_POINTER;
  }
  constexpr std::size_t kMaximumDword =
      (std::numeric_limits<DWORD>::max)();
  if (value.size() > kMaximumDword / sizeof(wchar_t) - 1) {
    return E_INVALIDARG;
  }
  result->existed = true;
  result->type = REG_SZ;
  result->bytes.assign((value.size() + 1) * sizeof(wchar_t), std::byte{0});
  if (!value.empty()) {
    std::memcpy(result->bytes.data(), value.data(),
                value.size() * sizeof(wchar_t));
  }
  return S_OK;
}

HRESULT DesiredComRegistration(std::wstring_view module_path,
                               ComRegistrationSnapshot* desired) {
  if (desired == nullptr) {
    return E_POINTER;
  }
  const HRESULT validation_result = ValidateModulePath(module_path);
  if (FAILED(validation_result)) {
    return validation_result;
  }
  ComRegistrationSnapshot value;
  value.clsid_key_existed = true;
  value.inproc_key_existed = true;
  HRESULT result = MakeRegistryString(module_path, &value.default_value);
  if (SUCCEEDED(result)) {
    result = MakeRegistryString(kThreadingModel, &value.threading_model);
  }
  if (FAILED(result)) {
    return result;
  }
  *desired = std::move(value);
  return S_OK;
}

HRESULT DecodeRegistryString(const RawRegistryValue& value,
                             std::wstring* decoded) {
  if (decoded == nullptr) {
    return E_POINTER;
  }
  if (!value.existed || value.type != REG_SZ ||
      value.bytes.size() < sizeof(wchar_t) ||
      value.bytes.size() % sizeof(wchar_t) != 0) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  const std::size_t character_count = value.bytes.size() / sizeof(wchar_t);
  if (character_count - 1 >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  std::wstring text(character_count, L'\0');
  std::memcpy(text.data(), value.bytes.data(), value.bytes.size());
  if (text.back() != L'\0' ||
      std::find(text.begin(), text.end() - 1, L'\0') != text.end() - 1) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  text.pop_back();
  if (text.empty()) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  *decoded = std::move(text);
  return S_OK;
}

bool DesiredStateMatches(const RegistrationState& state,
                         const ComRegistrationSnapshot& desired) noexcept {
  return state.com == desired && state.processor && state.profile &&
         std::ranges::all_of(state.categories,
                             [](bool present) { return present; });
}

struct RegistrationUndo {
  bool restore_com = false;
  bool processor = false;
  bool profile = false;
  std::array<bool, kRegistrationCategoryCount> categories{};
};

HRESULT RollBackRegistration(RegistrationBackend& backend,
                             const ComRegistrationSnapshot& original_com,
                             const RegistrationUndo& undo,
                             HRESULT primary_failure) noexcept {
  HRESULT tsf_failure = S_OK;
  for (std::size_t index = kRegistrationCategoryCount; index > 0; --index) {
    const std::size_t category = index - 1;
    if (undo.categories[category]) {
      CaptureFirstFailure(
          InvokeBackend([&] { return backend.RemoveCategory(category); }),
          &tsf_failure);
    }
  }
  bool profile_removed = !undo.profile;
  if (undo.profile) {
    const HRESULT profile_result =
        InvokeBackend([&] { return backend.RemoveProfile(); });
    profile_removed = SUCCEEDED(profile_result);
    CaptureFirstFailure(profile_result, &tsf_failure);
  }
  if (undo.processor && profile_removed) {
    CaptureFirstFailure(
        InvokeBackend([&] { return backend.RemoveProcessor(); }),
        &tsf_failure);
  }
  if (FAILED(tsf_failure)) {
    // Keep the new COM path reachable while any TSF undo may still exist so
    // the installer can retain the DLL and retry cleanup safely.
    return tsf_failure;
  }
  if (!undo.restore_com) {
    return primary_failure;
  }
  const HRESULT com_restore_result = InvokeBackend(
      [&] { return backend.RestoreComRegistration(original_com); });
  return FAILED(com_restore_result) ? com_restore_result : primary_failure;
}

}  // namespace

HRESULT IsComRegistrationOwnedBy(
    const ComRegistrationSnapshot& snapshot,
    std::wstring_view module_path,
    bool* owned) {
  try {
    if (owned == nullptr) {
      return E_POINTER;
    }
    *owned = false;
    const HRESULT validation_result = ValidateModulePath(module_path);
    if (FAILED(validation_result)) {
      return validation_result;
    }
    if (!HasConsistentShape(snapshot)) {
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    if (!snapshot.inproc_key_existed || !snapshot.default_value.existed) {
      return S_OK;
    }
    std::wstring registered_path;
    const HRESULT decode_result =
        DecodeRegistryString(snapshot.default_value, &registered_path);
    if (FAILED(decode_result)) {
      return decode_result;
    }
    if (registered_path.size() >
        static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    SetLastError(ERROR_SUCCESS);
    const int comparison = CompareStringOrdinal(
        registered_path.data(), static_cast<int>(registered_path.size()),
        module_path.data(), static_cast<int>(module_path.size()), TRUE);
    if (comparison == 0) {
      const DWORD error = GetLastError();
      return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
    }
    *owned = comparison == CSTR_EQUAL;
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

HRESULT RegisterWithBackend(RegistrationBackend& backend,
                            std::wstring_view module_path) {
  try {
    ComRegistrationSnapshot desired_com;
    HRESULT result = DesiredComRegistration(module_path, &desired_com);
    if (FAILED(result)) {
      return result;
    }

    RegistrationState original;
    result = InvokeBackend([&] { return backend.CaptureState(&original); });
    if (FAILED(result)) {
      return result;
    }
    if (!HasConsistentShape(original.com)) {
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    RegistrationUndo undo;
    ComRegistrationSnapshot replaced_com = original.com;
    bool com_changed = false;
    result = InvokeBackend([&] {
      return backend.WriteComRegistration(module_path, &replaced_com,
                                          &com_changed);
    });
    undo.restore_com = com_changed;
    if (FAILED(result)) {
      return RollBackRegistration(backend, replaced_com, undo, result);
    }

    if (!original.processor) {
      bool created = false;
      result =
          InvokeBackend([&] { return backend.AddProcessor(&created); });
      undo.processor = created;
    }
    if (SUCCEEDED(result) && !original.profile) {
      bool created = false;
      result = InvokeBackend(
          [&] { return backend.AddProfile(module_path, &created); });
      undo.profile = created;
    }
    if (SUCCEEDED(result)) {
      for (std::size_t index = 0; index < kRegistrationCategoryCount; ++index) {
        if (original.categories[index]) {
          continue;
        }
        bool created = false;
        result = InvokeBackend(
            [&] { return backend.AddCategory(index, &created); });
        undo.categories[index] = created;
        if (FAILED(result)) {
          break;
        }
      }
    }
    if (FAILED(result)) {
      return RollBackRegistration(backend, replaced_com, undo, result);
    }

    RegistrationState verified;
    result = InvokeBackend([&] { return backend.CaptureState(&verified); });
    if (SUCCEEDED(result) && !DesiredStateMatches(verified, desired_com)) {
      result = E_FAIL;
    }
    return FAILED(result)
               ? RollBackRegistration(backend, replaced_com, undo, result)
               : S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

HRESULT UnregisterWithBackend(RegistrationBackend& backend,
                              std::wstring_view module_path) {
  try {
    const HRESULT validation_result = ValidateModulePath(module_path);
    if (FAILED(validation_result)) {
      return validation_result;
    }
    RegistrationState initial;
    HRESULT result =
        InvokeBackend([&] { return backend.CaptureState(&initial); });
    if (FAILED(result)) {
      return result;
    }
    if (!HasConsistentShape(initial.com)) {
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    if (!initial.com.inproc_key_existed) {
      return HasTsfRegistration(initial)
                 ? HRESULT_FROM_WIN32(ERROR_NOT_OWNER)
                 : S_OK;
    }
    bool owned = false;
    result = IsComRegistrationOwnedBy(initial.com, module_path, &owned);
    if (FAILED(result)) {
      return result;
    }
    if (!owned) {
      return HRESULT_FROM_WIN32(ERROR_NOT_OWNER);
    }

    HRESULT first_failure = S_OK;
    for (std::size_t index = 0; index < kRegistrationCategoryCount; ++index) {
      if (initial.categories[index]) {
        CaptureFirstFailure(
            InvokeBackend([&] { return backend.RemoveCategory(index); }),
            &first_failure);
      }
    }

    bool profile_removed = !initial.profile;
    if (initial.profile) {
      const HRESULT profile_result =
          InvokeBackend([&] { return backend.RemoveProfile(); });
      profile_removed = SUCCEEDED(profile_result);
      CaptureFirstFailure(profile_result, &first_failure);
    }
    if (initial.processor && profile_removed) {
      CaptureFirstFailure(
          InvokeBackend([&] { return backend.RemoveProcessor(); }),
          &first_failure);
    }
    if (FAILED(first_failure)) {
      return first_failure;
    }

    RegistrationState cleaned;
    result = InvokeBackend([&] { return backend.CaptureState(&cleaned); });
    if (FAILED(result)) {
      return result;
    }
    if (HasTsfRegistration(cleaned)) {
      return E_FAIL;
    }
    return InvokeBackend(
        [&] { return backend.RemoveComRegistrationIfOwned(module_path); });
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

}  // namespace zrinput::windows::tsf
