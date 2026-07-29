#pragma once

#include <windows.h>
#include <filesystem>

namespace zrinput::windows {

struct Theme {
  COLORREF background = RGB(44, 44, 44);
  COLORREF selected = RGB(56, 56, 56);
  COLORREF accent = RGB(179, 193, 224);
  COLORREF text = RGB(255, 255, 255);
  COLORREF secondary_text = RGB(190, 190, 190);
  int font_size = 17;
  int window_height = 41;

  bool Load(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path) const;
};

}  // namespace zrinput::windows
