#include "windows/tsf_guids.h"

#include <windows.h>
#include <msctf.h>

#include <iostream>
#include <string_view>

namespace {

constexpr LANGID kSimplifiedChinese =
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

HRESULT Activate(ITfInputProcessorProfileMgr* manager) {
  return manager->ActivateProfile(
      TF_PROFILETYPE_INPUTPROCESSOR, kSimplifiedChinese,
      zrinput::windows::kTextServiceClsid,
      zrinput::windows::kLanguageProfileGuid, nullptr,
      TF_IPPMF_FORSESSION | TF_IPPMF_ENABLEPROFILE);
}

HRESULT PrintStatus(ITfInputProcessorProfileMgr* manager) {
  TF_INPUTPROCESSORPROFILE profile{};
  const HRESULT result =
      manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile);
  if (FAILED(result))
    return result;
  const bool active = profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
                      profile.clsid == zrinput::windows::kTextServiceClsid &&
                      profile.guidProfile ==
                          zrinput::windows::kLanguageProfileGuid;
  std::wcout << (active ? L"active" : L"inactive") << L'\n';
  return active ? S_OK : S_FALSE;
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
  if (argument_count != 2) {
    std::wcerr << L"Usage: zrinput_profile_tool <activate|status>\n";
    return 2;
  }
  const HRESULT initialize = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialize)) {
    std::wcerr << L"COM initialization failed: 0x" << std::hex << initialize
               << L'\n';
    return 1;
  }
  ITfInputProcessorProfileMgr* manager = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&manager));
  if (SUCCEEDED(result)) {
    const std::wstring_view command(arguments[1]);
    if (command == L"activate")
      result = Activate(manager);
    else if (command == L"status")
      result = PrintStatus(manager);
    else {
      std::wcerr << L"Unknown command.\n";
      result = E_INVALIDARG;
    }
    manager->Release();
  }
  CoUninitialize();
  if (FAILED(result)) {
    std::wcerr << L"TSF operation failed: 0x" << std::hex << result << L'\n';
    return 1;
  }
  return result == S_OK ? 0 : 3;
}
