#include "windows/theme.h"

#include <windows.h>
#include <filesystem>
#include <iostream>

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    L"zrinput-theme-test" / L"active.ini";
  zrinput::windows::Theme expected;
  expected.background = RGB(12, 34, 56);
  expected.selected = RGB(65, 43, 21);
  expected.accent = RGB(1, 120, 240);
  expected.text = RGB(250, 249, 248);
  expected.secondary_text = RGB(140, 141, 142);
  expected.font_size = 18;
  expected.window_height = 42;
  if (!expected.Save(path)) {
    std::cerr << "theme save failed\n";
    return 1;
  }
  zrinput::windows::Theme actual;
  if (!actual.Load(path) || actual.background != expected.background ||
      actual.selected != expected.selected || actual.accent != expected.accent ||
      actual.text != expected.text ||
      actual.secondary_text != expected.secondary_text ||
      actual.font_size != expected.font_size ||
      actual.window_height != expected.window_height) {
    std::cerr << "theme round trip mismatch\n";
    return 1;
  }
  std::error_code error;
  std::filesystem::remove_all(path.parent_path(), error);
  return 0;
}
