#include "windows/registration.h"

#include "windows/tsf_guids.h"

#include <msctf.h>
#include <string>

namespace zrinput::windows {
namespace {

std::wstring GuidString(REFGUID guid) {
  wchar_t buffer[40] = {};
  return StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)))
             ? buffer
             : L"";
}

HRESULT RegisterComServer(HMODULE module) {
  wchar_t module_path[MAX_PATH] = {};
  if (!GetModuleFileNameW(module, module_path, MAX_PATH))
    return HRESULT_FROM_WIN32(GetLastError());
  const std::wstring key_path = L"Software\\Classes\\CLSID\\" +
                                GuidString(kTextServiceClsid) +
                                L"\\InprocServer32";
  HKEY key = nullptr;
  LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr,
                                0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (result != ERROR_SUCCESS)
    return HRESULT_FROM_WIN32(result);
  const DWORD path_bytes =
      static_cast<DWORD>((wcslen(module_path) + 1) * sizeof(wchar_t));
  result = RegSetValueExW(key, nullptr, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(module_path),
                          path_bytes);
  const wchar_t threading_model[] = L"Apartment";
  if (result == ERROR_SUCCESS)
    result = RegSetValueExW(
        key, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(threading_model),
        static_cast<DWORD>(sizeof(threading_model)));
  RegCloseKey(key);
  return HRESULT_FROM_WIN32(result);
}

HRESULT RegisterProfile(HMODULE module) {
  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&profiles));
  if (FAILED(result))
    return result;
  result = profiles->Register(kTextServiceClsid);
  wchar_t module_path[MAX_PATH] = {};
  GetModuleFileNameW(module, module_path, MAX_PATH);
  const wchar_t description[] = L"ZRinput 中文输入法";
  if (SUCCEEDED(result))
    result = profiles->AddLanguageProfile(
        kTextServiceClsid,
        MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
        kLanguageProfileGuid, description,
        static_cast<ULONG>(std::size(description) - 1), module_path,
        static_cast<ULONG>(wcslen(module_path)), 0);
  profiles->Release();
  return result;
}

HRESULT RegisterCategories() {
  ITfCategoryMgr* categories = nullptr;
  HRESULT result = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                    reinterpret_cast<void**>(&categories));
  if (FAILED(result))
    return result;
  const GUID category_list[] = {
      GUID_TFCAT_TIP_KEYBOARD,
      GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
      GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
  };
  for (const auto& category : category_list) {
    result = categories->RegisterCategory(kTextServiceClsid, category,
                                          kTextServiceClsid);
    if (FAILED(result))
      break;
  }
  categories->Release();
  return result;
}

}  // namespace

HRESULT RegisterTextService(HMODULE module) {
  const HRESULT initialize_result =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(initialize_result);
  if (FAILED(initialize_result) && initialize_result != RPC_E_CHANGED_MODE)
    return initialize_result;

  HRESULT result = RegisterComServer(module);
  if (SUCCEEDED(result))
    result = RegisterProfile(module);
  if (SUCCEEDED(result))
    result = RegisterCategories();
  if (FAILED(result))
    UnregisterTextService();
  if (uninitialize)
    CoUninitialize();
  return result;
}

HRESULT UnregisterTextService() {
  const HRESULT initialize_result =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(initialize_result);
  if (FAILED(initialize_result) && initialize_result != RPC_E_CHANGED_MODE)
    return initialize_result;

  ITfCategoryMgr* categories = nullptr;
  if (SUCCEEDED(CoCreateInstance(
          CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
          IID_ITfCategoryMgr, reinterpret_cast<void**>(&categories)))) {
    const GUID category_list[] = {
        GUID_TFCAT_TIP_KEYBOARD,
        GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
        GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    };
    for (const auto& category : category_list)
      categories->UnregisterCategory(kTextServiceClsid, category,
                                     kTextServiceClsid);
    categories->Release();
  }
  ITfInputProcessorProfiles* profiles = nullptr;
  if (SUCCEEDED(CoCreateInstance(
          CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
          IID_ITfInputProcessorProfiles,
          reinterpret_cast<void**>(&profiles)))) {
    profiles->RemoveLanguageProfile(
        kTextServiceClsid,
        MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
        kLanguageProfileGuid);
    profiles->Unregister(kTextServiceClsid);
    profiles->Release();
  }
  const std::wstring key_path =
      L"Software\\Classes\\CLSID\\" + GuidString(kTextServiceClsid);
  const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, key_path.c_str());
  const HRESULT unregister_result =
      result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND
          ? S_OK
          : HRESULT_FROM_WIN32(result);
  if (uninitialize)
    CoUninitialize();
  return unregister_result;
}

}  // namespace zrinput::windows
