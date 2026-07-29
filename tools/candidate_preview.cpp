#include "windows/candidate_window.h"

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string_view>
#include <vector>

namespace zrinput::windows {
HMODULE g_module = nullptr;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  zrinput::windows::g_module = instance;
  zrinput::windows::CandidateWindow window(true);
  zrinput::windows::Theme theme;
  theme.background = RGB(44, 44, 44);
  theme.selected = RGB(56, 56, 56);
  theme.accent = RGB(179, 193, 224);
  theme.text = RGB(255, 255, 255);
  theme.secondary_text = RGB(190, 190, 190);
  theme.font_size = 17;
  theme.window_height = 41;
  window.SetTheme(theme);
  std::vector<zrinput::Candidate> candidates = {
      {.text = "和"}, {.text = "好"}, {.text = "还"}, {.text = "会"},
      {.text = "后"}, {.text = "或"}, {.text = "很"}, {.text = "话"},
  };

  int argument_count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  const bool show = arguments && argument_count >= 2 &&
                    std::wstring_view(arguments[1]) == L"--show";
  if (!show) {
    std::filesystem::path output =
        std::filesystem::current_path() / L"candidate-preview.bmp";
    if (arguments && argument_count >= 3 &&
        std::wstring_view(arguments[1]) == L"--render") {
      output = arguments[2];
    }
    const bool lifecycle_ok =
        zrinput::windows::CandidateWindow::TestHiddenWindowClassLifecycle();
    const bool rendered = lifecycle_ok && window.RenderToBitmap(
        candidates, 0, zrinput::windows::kMicrosoftCandidatePageSize, output);
    if (arguments)
      LocalFree(arguments);
    return rendered ? 0 : 1;
  }
  if (arguments)
    LocalFree(arguments);

  window.Show(candidates, 0, zrinput::windows::kMicrosoftCandidatePageSize);
  const DWORD deadline = GetTickCount() + 30000;
  MSG message{};
  while (GetTickCount() < deadline) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT)
        return 0;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  return 0;
}
