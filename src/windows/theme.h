#pragma once

#include <windows.h>
#include <filesystem>

namespace zrinput::windows {

struct Theme {
  COLORREF background = RGB(32, 32, 32);
  COLORREF selected = RGB(62, 62, 62);
  COLORREF accent = RGB(0, 120, 212);
  COLORREF text = RGB(245, 245, 245);
  COLORREF secondary_text = RGB(190, 190, 190);
  int font_size = 19;
  int window_height = 44;

  bool Load(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path) const;
};

}  // namespace zrinput::windows
