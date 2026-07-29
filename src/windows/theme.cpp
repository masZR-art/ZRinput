#include "windows/theme.h"

#include <algorithm>
#include <array>
#include <string>

namespace zrinput::windows {
namespace {

COLORREF ReadColor(const std::filesystem::path& path,
                   const wchar_t* key,
                   COLORREF fallback) {
  wchar_t buffer[32] = {};
  GetPrivateProfileStringW(L"colors", key, L"", buffer,
                           static_cast<DWORD>(std::size(buffer)),
                           path.c_str());
  unsigned long value = 0;
  if (swscanf_s(buffer, L"#%06lx", &value) != 1)
    return fallback;
  return RGB((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

bool WriteColor(const std::filesystem::path& path,
                const wchar_t* key,
                COLORREF color) {
  wchar_t value[8] = {};
  swprintf_s(value, L"#%02X%02X%02X", GetRValue(color), GetGValue(color),
             GetBValue(color));
  return WritePrivateProfileStringW(L"colors", key, value, path.c_str());
}

}  // namespace

bool Theme::Load(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path))
    return false;
  background = ReadColor(path, L"background", background);
  selected = ReadColor(path, L"selected", selected);
  accent = ReadColor(path, L"accent", accent);
  text = ReadColor(path, L"text", text);
  secondary_text = ReadColor(path, L"secondary_text", secondary_text);
  font_size = GetPrivateProfileIntW(L"metrics", L"font_size", font_size,
                                    path.c_str());
  window_height = GetPrivateProfileIntW(
      L"metrics", L"window_height", window_height, path.c_str());
  font_size = std::clamp(font_size, 12, 30);
  window_height = std::clamp(window_height, 32, 72);
  return true;
}

bool Theme::Save(const std::filesystem::path& path) const {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;
  wchar_t number[16] = {};
  swprintf_s(number, L"%d", font_size);
  bool result = WritePrivateProfileStringW(L"theme", L"name",
                                            L"Microsoft Dark", path.c_str());
  result = result && WriteColor(path, L"background", background);
  result = result && WriteColor(path, L"selected", selected);
  result = result && WriteColor(path, L"accent", accent);
  result = result && WriteColor(path, L"text", text);
  result = result && WriteColor(path, L"secondary_text", secondary_text);
  result = result && WritePrivateProfileStringW(L"metrics", L"font_size",
                                                 number, path.c_str());
  swprintf_s(number, L"%d", window_height);
  result = result && WritePrivateProfileStringW(
                         L"metrics", L"window_height", number, path.c_str());
  return result;
}

}  // namespace zrinput::windows
