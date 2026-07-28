#include "windows/candidate_window.h"

#include <windows.h>
#include <vector>

namespace zrinput::windows {
HMODULE g_module = nullptr;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  zrinput::windows::g_module = instance;
  zrinput::windows::CandidateWindow window(true);
  std::vector<zrinput::Candidate> candidates = {
      {.text = "现在"}, {.text = "现状"}, {.text = "先在"},
      {.text = "县在"}, {.text = "现代"}, {.text = "显示"},
  };
  window.Show(candidates, 0, 5);
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
