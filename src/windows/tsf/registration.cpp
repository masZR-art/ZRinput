#include "windows/tsf/registration.h"

#include "windows/tsf/com_support.h"
#include "windows/tsf/module_state.h"
#include "windows/tsf/registration_transaction.h"
#include "windows/tsf/tsf_guids.h"

#include <msctf.h>
#include <sddl.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace zrinput::windows::tsf {
namespace {

constexpr LANGID kSimplifiedChinese =
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
constexpr std::size_t kMaximumRegistryValueBytes = 1024 * 1024;
constexpr DWORD kRegistrationLockTimeoutMilliseconds = 120'000;
constexpr wchar_t kRegistrationLockPrefix[] =
    L"Global\\ZRinput.TsfRegistration.A22D9B55-29D0-4D34-978C-3EF48C6FA814.";

constexpr std::array<const GUID*, kRegistrationCategoryCount> kCategories = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_TIPCAP_SECUREMODE,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
};

class RegistrationOperationLock final {
 public:
  RegistrationOperationLock() = default;
  RegistrationOperationLock(const RegistrationOperationLock&) = delete;
  RegistrationOperationLock& operator=(const RegistrationOperationLock&) =
      delete;

  ~RegistrationOperationLock() noexcept {
    if (acquired_) {
      (void)ReleaseMutex(mutex_);
    }
    if (mutex_ != nullptr) {
      (void)CloseHandle(mutex_);
    }
  }

  HRESULT Acquire() {
    if (mutex_ != nullptr) {
      return E_UNEXPECTED;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    struct TokenGuard {
      HANDLE value;
      ~TokenGuard() noexcept { (void)CloseHandle(value); }
    } token_guard{token};

    DWORD token_bytes = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL size_query =
        GetTokenInformation(token, TokenUser, nullptr, 0, &token_bytes);
    const DWORD size_query_error = GetLastError();
    if (size_query != FALSE || size_query_error != ERROR_INSUFFICIENT_BUFFER ||
        token_bytes < sizeof(TOKEN_USER)) {
      return HRESULT_FROM_WIN32(size_query_error == ERROR_SUCCESS
                                    ? ERROR_INVALID_DATA
                                    : size_query_error);
    }
    std::vector<std::byte> token_buffer(token_bytes);
    if (!GetTokenInformation(token, TokenUser, token_buffer.data(), token_bytes,
                             &token_bytes)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }

    const auto* token_user =
        reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
    if (!IsValidSid(token_user->User.Sid)) {
      return HRESULT_FROM_WIN32(ERROR_INVALID_SID);
    }
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    std::wstring mutex_name;
    try {
      mutex_name.assign(kRegistrationLockPrefix);
      mutex_name.append(sid_text);
    } catch (...) {
      (void)LocalFree(sid_text);
      throw;
    }
    (void)LocalFree(sid_text);

    mutex_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex_ == nullptr) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    const DWORD wait_result =
        WaitForSingleObject(mutex_, kRegistrationLockTimeoutMilliseconds);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
      acquired_ = true;
      return S_OK;
    }
    if (wait_result == WAIT_TIMEOUT) {
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return HRESULT_FROM_WIN32(wait_result == WAIT_FAILED ? GetLastError()
                                                         : ERROR_GEN_FAILURE);
  }

 private:
  HANDLE mutex_ = nullptr;
  bool acquired_ = false;
};

class ComInitialization final {
 public:
  ComInitialization() noexcept {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    should_uninitialize_ = result == S_OK || result == S_FALSE;
    result_ = result == RPC_E_CHANGED_MODE ? S_OK : result;
  }

  ~ComInitialization() noexcept {
    if (should_uninitialize_) {
      CoUninitialize();
    }
  }

  [[nodiscard]] HRESULT result() const noexcept { return result_; }

 private:
  HRESULT result_ = E_FAIL;
  bool should_uninitialize_ = false;
};

class RegistryKey final {
 public:
  RegistryKey() = default;
  ~RegistryKey() noexcept {
    if (key_ != nullptr) {
      RegCloseKey(key_);
    }
  }

  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;

  [[nodiscard]] HKEY Get() const noexcept { return key_; }
  [[nodiscard]] HKEY* Put() noexcept {
    if (key_ != nullptr) {
      RegCloseKey(key_);
      key_ = nullptr;
    }
    return &key_;
  }

 private:
  HKEY key_ = nullptr;
};

void CaptureFirstFailure(HRESULT result, HRESULT* first_failure) noexcept {
  if (SUCCEEDED(*first_failure) && FAILED(result)) {
    *first_failure = result;
  }
}

HRESULT ModulePath(std::wstring* path) {
  if (path == nullptr) {
    return E_POINTER;
  }
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(ModuleInstance(), buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  if (length == buffer.size()) {
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  }
  buffer.resize(length);
  *path = std::move(buffer);
  return S_OK;
}

HRESULT ClsidRegistryPath(std::wstring* path) {
  if (path == nullptr) {
    return E_POINTER;
  }
  wchar_t guid[64]{};
  if (StringFromGUID2(kTextServiceClsid, guid,
                      static_cast<int>(std::size(guid))) == 0) {
    return E_FAIL;
  }
  *path = L"Software\\Classes\\CLSID\\";
  path->append(guid);
  return S_OK;
}

HRESULT InprocRegistryPath(std::wstring* path) {
  const HRESULT result = ClsidRegistryPath(path);
  if (SUCCEEDED(result)) {
    path->append(L"\\InprocServer32");
  }
  return result;
}

HRESULT OpenOptionalKey(const std::wstring& path,
                        REGSAM access,
                        RegistryKey* key,
                        bool* existed) {
  if (key == nullptr || existed == nullptr) {
    return E_POINTER;
  }
  *existed = false;
  const LSTATUS status =
      RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, key->Put());
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  if (status != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(static_cast<DWORD>(status));
  }
  *existed = true;
  return S_OK;
}

HRESULT ReadRegistryValue(HKEY key,
                          const wchar_t* name,
                          RawRegistryValue* value) {
  if (key == nullptr || value == nullptr) {
    return E_POINTER;
  }
  *value = {};
  DWORD type = REG_NONE;
  DWORD size = 0;
  LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
  if (status == ERROR_FILE_NOT_FOUND) {
    return S_OK;
  }
  if (status != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(static_cast<DWORD>(status));
  }
  if (size > kMaximumRegistryValueBytes) {
    return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    value->bytes.resize(size);
    DWORD actual_size = size;
    status = RegQueryValueExW(
        key, name, nullptr, &type,
        value->bytes.empty()
            ? nullptr
            : reinterpret_cast<BYTE*>(value->bytes.data()),
        &actual_size);
    if (status == ERROR_MORE_DATA) {
      if (actual_size > kMaximumRegistryValueBytes) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
      }
      size = actual_size;
      continue;
    }
    if (status == ERROR_FILE_NOT_FOUND) {
      *value = {};
      return S_OK;
    }
    if (status != ERROR_SUCCESS) {
      return HRESULT_FROM_WIN32(static_cast<DWORD>(status));
    }
    if (actual_size > kMaximumRegistryValueBytes) {
      return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    if (actual_size > value->bytes.size()) {
      size = actual_size;
      continue;
    }
    value->bytes.resize(actual_size);
    value->existed = true;
    value->type = type;
    return S_OK;
  }
  return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
}

HRESULT CaptureComRegistration(ComRegistrationSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return E_POINTER;
  }
  *snapshot = {};
  std::wstring clsid_path;
  HRESULT result = ClsidRegistryPath(&clsid_path);
  if (FAILED(result)) {
    return result;
  }
  RegistryKey clsid_key;
  result = OpenOptionalKey(clsid_path, KEY_READ, &clsid_key,
                           &snapshot->clsid_key_existed);
  if (FAILED(result) || !snapshot->clsid_key_existed) {
    return result;
  }

  std::wstring inproc_path;
  result = InprocRegistryPath(&inproc_path);
  if (FAILED(result)) {
    return result;
  }
  RegistryKey inproc_key;
  result = OpenOptionalKey(inproc_path, KEY_QUERY_VALUE, &inproc_key,
                           &snapshot->inproc_key_existed);
  if (FAILED(result) || !snapshot->inproc_key_existed) {
    return result;
  }
  result = ReadRegistryValue(inproc_key.Get(), nullptr,
                             &snapshot->default_value);
  return FAILED(result)
             ? result
             : ReadRegistryValue(inproc_key.Get(), L"ThreadingModel",
                                 &snapshot->threading_model);
}

HRESULT SetRegistryValue(HKEY key,
                         const wchar_t* name,
                         const RawRegistryValue& value) {
  if (key == nullptr) {
    return E_POINTER;
  }
  if (value.bytes.size() >
      static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
    return E_INVALIDARG;
  }
  const LSTATUS status = RegSetValueExW(
      key, name, 0, value.type,
      value.bytes.empty()
          ? nullptr
          : reinterpret_cast<const BYTE*>(value.bytes.data()),
      static_cast<DWORD>(value.bytes.size()));
  return status == ERROR_SUCCESS
             ? S_OK
             : HRESULT_FROM_WIN32(static_cast<DWORD>(status));
}

HRESULT RestoreRegistryValue(HKEY key,
                             const wchar_t* name,
                             const RawRegistryValue& value) {
  if (value.existed) {
    return SetRegistryValue(key, name, value);
  }
  const LSTATUS status = RegDeleteValueW(key, name);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND
             ? S_OK
             : HRESULT_FROM_WIN32(static_cast<DWORD>(status));
}

HRESULT DeleteRegistryTree(const std::wstring& path) {
  const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
                 status == ERROR_PATH_NOT_FOUND
             ? S_OK
             : HRESULT_FROM_WIN32(static_cast<DWORD>(status));
}

HRESULT CreateRegistryKey(const std::wstring& path,
                          REGSAM access,
                          RegistryKey* key) {
  if (key == nullptr) {
    return E_POINTER;
  }
  const LSTATUS status = RegCreateKeyExW(
      HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
      access, nullptr, key->Put(), nullptr);
  return status == ERROR_SUCCESS
             ? S_OK
             : HRESULT_FROM_WIN32(static_cast<DWORD>(status));
}

HRESULT MakeRegistryString(std::wstring_view text, RawRegistryValue* value) {
  if (value == nullptr) {
    return E_POINTER;
  }
  if (text.size() >
      static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) /
                  sizeof(wchar_t) -
              1) {
    return E_INVALIDARG;
  }
  RawRegistryValue result;
  result.existed = true;
  result.type = REG_SZ;
  result.bytes.assign((text.size() + 1) * sizeof(wchar_t), std::byte{0});
  if (!text.empty()) {
    std::memcpy(result.bytes.data(), text.data(),
                text.size() * sizeof(wchar_t));
  }
  *value = std::move(result);
  return S_OK;
}

HRESULT DesiredComRegistration(std::wstring_view module_path,
                               ComRegistrationSnapshot* desired) {
  if (desired == nullptr) {
    return E_POINTER;
  }
  ComRegistrationSnapshot result;
  result.clsid_key_existed = true;
  result.inproc_key_existed = true;
  HRESULT status = MakeRegistryString(module_path, &result.default_value);
  if (SUCCEEDED(status)) {
    status = MakeRegistryString(L"Apartment", &result.threading_model);
  }
  if (SUCCEEDED(status)) {
    *desired = std::move(result);
  }
  return status;
}

HRESULT WriteComRegistrationToRegistry(std::wstring_view module_path) {
  ComRegistrationSnapshot desired;
  HRESULT result = DesiredComRegistration(module_path, &desired);
  if (FAILED(result)) {
    return result;
  }
  std::wstring inproc_path;
  result = InprocRegistryPath(&inproc_path);
  if (FAILED(result)) {
    return result;
  }
  RegistryKey key;
  result = CreateRegistryKey(inproc_path, KEY_SET_VALUE, &key);
  if (FAILED(result)) {
    return result;
  }
  result = SetRegistryValue(key.Get(), nullptr, desired.default_value);
  if (FAILED(result)) {
    return result;
  }

  return SetRegistryValue(key.Get(), L"ThreadingModel",
                          desired.threading_model);
}

HRESULT RestoreComRegistrationToRegistry(
    const ComRegistrationSnapshot& snapshot) {
  std::wstring clsid_path;
  HRESULT result = ClsidRegistryPath(&clsid_path);
  if (FAILED(result)) {
    return result;
  }
  if (!snapshot.clsid_key_existed) {
    return DeleteRegistryTree(clsid_path);
  }

  RegistryKey clsid_key;
  result = CreateRegistryKey(clsid_path, KEY_SET_VALUE, &clsid_key);
  if (FAILED(result)) {
    return result;
  }
  std::wstring inproc_path;
  result = InprocRegistryPath(&inproc_path);
  if (FAILED(result)) {
    return result;
  }
  if (!snapshot.inproc_key_existed) {
    return DeleteRegistryTree(inproc_path);
  }

  RegistryKey inproc_key;
  result = CreateRegistryKey(inproc_path, KEY_SET_VALUE, &inproc_key);
  if (FAILED(result)) {
    return result;
  }
  HRESULT first_failure = S_OK;
  CaptureFirstFailure(
      RestoreRegistryValue(inproc_key.Get(), nullptr, snapshot.default_value),
      &first_failure);
  CaptureFirstFailure(RestoreRegistryValue(inproc_key.Get(), L"ThreadingModel",
                                           snapshot.threading_model),
                      &first_failure);
  return first_failure;
}

HRESULT RemoveOwnedComRegistrationFromRegistry(std::wstring_view module_path) {
  ComRegistrationSnapshot snapshot;
  HRESULT result = CaptureComRegistration(&snapshot);
  if (FAILED(result)) {
    return result;
  }
  if (!snapshot.clsid_key_existed && !snapshot.inproc_key_existed) {
    return S_OK;
  }
  bool owned = false;
  result = IsComRegistrationOwnedBy(snapshot, module_path, &owned);
  if (FAILED(result)) {
    return result;
  }
  if (!owned) {
    return HRESULT_FROM_WIN32(ERROR_NOT_OWNER);
  }
  std::wstring clsid_path;
  result = ClsidRegistryPath(&clsid_path);
  return FAILED(result) ? result : DeleteRegistryTree(clsid_path);
}

HRESULT CreateProfiles(ComPtr<ITfInputProcessorProfiles>* profiles) {
  if (profiles == nullptr) {
    return E_POINTER;
  }
  return CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                          CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                          reinterpret_cast<void**>(profiles->Put()));
}

HRESULT CreateCategoryManager(ComPtr<ITfCategoryMgr>* manager) {
  if (manager == nullptr) {
    return E_POINTER;
  }
  return CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr,
                          reinterpret_cast<void**>(manager->Put()));
}

HRESULT EnumeratorContains(IEnumGUID* enumerator,
                           REFGUID expected,
                           bool* contains) {
  if (enumerator == nullptr || contains == nullptr) {
    return E_POINTER;
  }
  *contains = false;
  for (;;) {
    GUID value{};
    ULONG fetched = 0;
    const HRESULT result = enumerator->Next(1, &value, &fetched);
    if (FAILED(result)) {
      return result;
    }
    if (fetched == 0) {
      return S_OK;
    }
    if (IsEqualGUID(value, expected)) {
      *contains = true;
      return S_OK;
    }
  }
}

HRESULT ProcessorExists(ITfInputProcessorProfiles* profiles, bool* exists) {
  if (profiles == nullptr || exists == nullptr) {
    return E_POINTER;
  }
  ComPtr<IEnumGUID> enumerator;
  const HRESULT result = profiles->EnumInputProcessorInfo(enumerator.Put());
  return FAILED(result)
             ? result
             : EnumeratorContains(enumerator.Get(), kTextServiceClsid, exists);
}

HRESULT LanguageProfileExists(ITfInputProcessorProfiles* profiles,
                              bool* exists) {
  if (profiles == nullptr || exists == nullptr) {
    return E_POINTER;
  }
  *exists = false;
  ComPtr<IEnumTfLanguageProfiles> enumerator;
  HRESULT result =
      profiles->EnumLanguageProfiles(kSimplifiedChinese, enumerator.Put());
  if (FAILED(result) || !enumerator) {
    return FAILED(result) ? result : E_FAIL;
  }
  for (;;) {
    TF_LANGUAGEPROFILE profile{};
    ULONG fetched = 0;
    result = enumerator->Next(1, &profile, &fetched);
    if (FAILED(result)) {
      return result;
    }
    if (fetched == 0) {
      return S_OK;
    }
    if (IsEqualCLSID(profile.clsid, kTextServiceClsid) &&
        IsEqualGUID(profile.guidProfile, kSimplifiedChineseProfileGuid)) {
      *exists = true;
      return S_OK;
    }
  }
}

HRESULT CategoryExists(ITfCategoryMgr* manager,
                       std::size_t index,
                       bool* exists) {
  if (manager == nullptr || exists == nullptr) {
    return E_POINTER;
  }
  if (index >= kCategories.size()) {
    return E_INVALIDARG;
  }
  ComPtr<IEnumGUID> enumerator;
  const HRESULT result =
      manager->EnumItemsInCategory(*kCategories[index], enumerator.Put());
  return FAILED(result)
             ? result
             : EnumeratorContains(enumerator.Get(), kTextServiceClsid, exists);
}

template <typename Probe, typename Mutation>
HRESULT AddIfMissing(Probe&& probe, Mutation&& mutation, bool* created) {
  if (created == nullptr) {
    return E_POINTER;
  }
  *created = false;
  bool exists = false;
  HRESULT result = std::forward<Probe>(probe)(&exists);
  if (FAILED(result) || exists) {
    return result;
  }

  result = std::forward<Mutation>(mutation)();
  if (SUCCEEDED(result)) {
    *created = true;
    return result;
  }

  // A COM registration call can report failure after applying its side effect.
  // Re-probe so rollback is conservative without claiming ownership of no-op
  // calls that observed an existing resource.
  bool exists_after_failure = false;
  const HRESULT probe_result =
      std::forward<Probe>(probe)(&exists_after_failure);
  *created = FAILED(probe_result) || exists_after_failure;
  return result;
}

class WindowsRegistrationBackend final : public RegistrationBackend {
 public:
  HRESULT Initialize() {
    HRESULT result = CreateProfiles(&profiles_);
    if (SUCCEEDED(result)) {
      result = CreateCategoryManager(&category_manager_);
    }
    return result;
  }

  HRESULT CaptureState(RegistrationState* state) override {
    if (state == nullptr) {
      return E_POINTER;
    }
    RegistrationState captured;
    HRESULT result = CaptureComRegistration(&captured.com);
    if (SUCCEEDED(result)) {
      result = ProcessorExists(profiles_.Get(), &captured.processor);
    }
    if (SUCCEEDED(result)) {
      result = LanguageProfileExists(profiles_.Get(), &captured.profile);
    }
    if (SUCCEEDED(result)) {
      for (std::size_t index = 0; index < kCategories.size(); ++index) {
        result = CategoryExists(category_manager_.Get(), index,
                                &captured.categories[index]);
        if (FAILED(result)) {
          break;
        }
      }
    }
    if (SUCCEEDED(result)) {
      *state = std::move(captured);
    }
    return result;
  }

  HRESULT WriteComRegistration(
      std::wstring_view module_path,
      ComRegistrationSnapshot* previous,
      bool* changed) override {
    if (previous == nullptr || changed == nullptr) {
      return E_POINTER;
    }
    *changed = false;
    ComRegistrationSnapshot before;
    HRESULT result = CaptureComRegistration(&before);
    if (FAILED(result)) {
      return result;
    }
    *previous = before;

    ComRegistrationSnapshot desired;
    result = DesiredComRegistration(module_path, &desired);
    if (FAILED(result) || before == desired) {
      return result;
    }
    *changed = true;
    result = WriteComRegistrationToRegistry(module_path);
    if (SUCCEEDED(result)) {
      return result;
    }

    ComRegistrationSnapshot after;
    const HRESULT probe_result = CaptureComRegistration(&after);
    if (SUCCEEDED(probe_result) && after == before) {
      *changed = false;
    }
    return result;
  }

  HRESULT RestoreComRegistration(
      const ComRegistrationSnapshot& snapshot) override {
    return RestoreComRegistrationToRegistry(snapshot);
  }

  HRESULT RemoveComRegistrationIfOwned(
      std::wstring_view module_path) override {
    return RemoveOwnedComRegistrationFromRegistry(module_path);
  }

  HRESULT AddProcessor(bool* created) override {
    return AddIfMissing(
        [&](bool* exists) {
          return ProcessorExists(profiles_.Get(), exists);
        },
        [&] { return profiles_->Register(kTextServiceClsid); }, created);
  }

  HRESULT RemoveProcessor() override {
    bool exists = false;
    HRESULT result = ProcessorExists(profiles_.Get(), &exists);
    return FAILED(result) || !exists ? result
                                     : profiles_->Unregister(kTextServiceClsid);
  }

  HRESULT AddProfile(std::wstring_view module_path, bool* created) override {
    return AddIfMissing(
        [&](bool* exists) {
          return LanguageProfileExists(profiles_.Get(), exists);
        },
        [&]() -> HRESULT {
          if (module_path.size() >
              static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())) {
            return E_INVALIDARG;
          }
          constexpr std::size_t kDescriptionLength =
              std::size(kTextServiceName) - 1;
          const std::wstring path(module_path);
          return profiles_->AddLanguageProfile(
              kTextServiceClsid, kSimplifiedChinese,
              kSimplifiedChineseProfileGuid, kTextServiceName,
              static_cast<ULONG>(kDescriptionLength), path.c_str(),
              static_cast<ULONG>(path.size()), 0);
        },
        created);
  }

  HRESULT RemoveProfile() override {
    bool exists = false;
    HRESULT result = LanguageProfileExists(profiles_.Get(), &exists);
    return FAILED(result) || !exists
               ? result
               : profiles_->RemoveLanguageProfile(
                     kTextServiceClsid, kSimplifiedChinese,
                     kSimplifiedChineseProfileGuid);
  }

  HRESULT AddCategory(std::size_t index, bool* created) override {
    if (index >= kCategories.size()) {
      return E_INVALIDARG;
    }
    return AddIfMissing(
        [&](bool* exists) {
          return CategoryExists(category_manager_.Get(), index, exists);
        },
        [&] {
          return category_manager_->RegisterCategory(
              kTextServiceClsid, *kCategories[index], kTextServiceClsid);
        },
        created);
  }

  HRESULT RemoveCategory(std::size_t index) override {
    if (index >= kCategories.size()) {
      return E_INVALIDARG;
    }
    bool exists = false;
    HRESULT result = CategoryExists(category_manager_.Get(), index, &exists);
    return FAILED(result) || !exists
               ? result
               : category_manager_->UnregisterCategory(
                     kTextServiceClsid, *kCategories[index],
                     kTextServiceClsid);
  }

 private:
  ComPtr<ITfInputProcessorProfiles> profiles_;
  ComPtr<ITfCategoryMgr> category_manager_;
};

}  // namespace

HRESULT RegisterServer() {
  ComInitialization com;
  if (FAILED(com.result())) {
    return com.result();
  }
  std::wstring module_path;
  HRESULT result = ModulePath(&module_path);
  if (FAILED(result)) {
    return result;
  }
  RegistrationOperationLock operation_lock;
  result = operation_lock.Acquire();
  if (FAILED(result)) {
    return result;
  }
  WindowsRegistrationBackend backend;
  result = backend.Initialize();
  return FAILED(result) ? result
                        : RegisterWithBackend(backend, module_path);
}

HRESULT UnregisterServer() {
  ComInitialization com;
  if (FAILED(com.result())) {
    return com.result();
  }
  std::wstring module_path;
  HRESULT result = ModulePath(&module_path);
  if (FAILED(result)) {
    return result;
  }
  RegistrationOperationLock operation_lock;
  result = operation_lock.Acquire();
  if (FAILED(result)) {
    return result;
  }
  WindowsRegistrationBackend backend;
  result = backend.Initialize();
  return FAILED(result) ? result
                        : UnregisterWithBackend(backend, module_path);
}

}  // namespace zrinput::windows::tsf
