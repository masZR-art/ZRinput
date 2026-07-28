#include "windows/theme.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace {

constexpr int kBackground = 101;
constexpr int kSelected = 102;
constexpr int kAccent = 103;
constexpr int kText = 104;
constexpr int kSecondaryText = 105;
constexpr int kFontSize = 201;
constexpr int kWindowHeight = 202;
constexpr int kSave = 301;

zrinput::windows::Theme g_theme;
HWND g_font_size = nullptr;
HWND g_window_height = nullptr;
std::array<COLORREF, 16> g_custom_colors{};

std::filesystem::path ActiveThemePath() {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &value)))
    return {};
  std::filesystem::path path(value);
  CoTaskMemFree(value);
  return path / L"ZRinput" / L"themes" / L"active.ini";
}

COLORREF* ColorForControl(int identifier) {
  switch (identifier) {
    case kBackground: return &g_theme.background;
    case kSelected: return &g_theme.selected;
    case kAccent: return &g_theme.accent;
    case kText: return &g_theme.text;
    case kSecondaryText: return &g_theme.secondary_text;
    default: return nullptr;
  }
}

void ChooseThemeColor(HWND owner, int identifier) {
  COLORREF* color = ColorForControl(identifier);
  if (!color)
    return;
  CHOOSECOLORW dialog{sizeof(dialog)};
  dialog.hwndOwner = owner;
  dialog.rgbResult = *color;
  dialog.lpCustColors = g_custom_colors.data();
  dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (ChooseColorW(&dialog)) {
    *color = dialog.rgbResult;
    InvalidateRect(owner, nullptr, TRUE);
    InvalidateRect(GetDlgItem(owner, identifier), nullptr, TRUE);
  }
}

void CreateLabelAndSwatch(HWND window,
                          const wchar_t* label,
                          int y,
                          int identifier) {
  CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE,
                  24, y + 5, 150, 24, window, nullptr, nullptr, nullptr);
  CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  180, y, 56, 30, window,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
                  nullptr, nullptr);
}

void ReadMetrics() {
  wchar_t buffer[16] = {};
  GetWindowTextW(g_font_size, buffer, static_cast<int>(std::size(buffer)));
  g_theme.font_size = std::clamp(_wtoi(buffer), 12, 30);
  GetWindowTextW(g_window_height, buffer,
                 static_cast<int>(std::size(buffer)));
  g_theme.window_height = std::clamp(_wtoi(buffer), 32, 72);
}

LRESULT CALLBACK WindowProcedure(HWND window,
                                 UINT message,
                                 WPARAM wparam,
                                 LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      CreateLabelAndSwatch(window, L"Background", 24, kBackground);
      CreateLabelAndSwatch(window, L"Selected candidate", 62, kSelected);
      CreateLabelAndSwatch(window, L"Accent line", 100, kAccent);
      CreateLabelAndSwatch(window, L"Candidate text", 138, kText);
      CreateLabelAndSwatch(window, L"Secondary text", 176, kSecondaryText);
      CreateWindowExW(0, L"STATIC", L"Font size", WS_CHILD | WS_VISIBLE,
                      270, 29, 100, 24, window, nullptr, nullptr, nullptr);
      g_font_size = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"19",
                                    WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                    380, 24, 70, 28, window,
                                    reinterpret_cast<HMENU>(
                                        static_cast<INT_PTR>(kFontSize)), nullptr,
                                    nullptr);
      CreateWindowExW(0, L"STATIC", L"Window height", WS_CHILD | WS_VISIBLE,
                      270, 67, 100, 24, window, nullptr, nullptr, nullptr);
      g_window_height = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"44",
          WS_CHILD | WS_VISIBLE | ES_NUMBER, 380, 62, 70, 28, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWindowHeight)),
          nullptr, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Save active theme",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      270, 166, 180, 38, window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSave)),
                      nullptr, nullptr);
      return 0;
    }
    case WM_COMMAND: {
      const int identifier = LOWORD(wparam);
      if (ColorForControl(identifier)) {
        ChooseThemeColor(window, identifier);
        return 0;
      }
      if (identifier == kSave) {
        ReadMetrics();
        const auto path = ActiveThemePath();
        const bool saved = !path.empty() && g_theme.Save(path);
        MessageBoxW(window,
                    saved ? L"Theme saved. Restart the active input method to apply it."
                          : L"The theme could not be saved.",
                    L"ZRinput Theme Editor",
                    saved ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
        return 0;
      }
      break;
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      COLORREF* color = ColorForControl(static_cast<int>(item->CtlID));
      if (!color)
        break;
      HBRUSH brush = CreateSolidBrush(*color);
      FillRect(item->hDC, &item->rcItem, brush);
      DeleteObject(brush);
      FrameRect(item->hDC, &item->rcItem,
                reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
      return TRUE;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC device = BeginPaint(window, &paint);
      RECT preview{270, 112, 590, 154};
      HBRUSH background = CreateSolidBrush(g_theme.background);
      FillRect(device, &preview, background);
      DeleteObject(background);
      RECT selected{278, 116, 365, 150};
      HBRUSH selected_brush = CreateSolidBrush(g_theme.selected);
      FillRect(device, &selected, selected_brush);
      DeleteObject(selected_brush);
      RECT accent{278, 122, 280, 144};
      HBRUSH accent_brush = CreateSolidBrush(g_theme.accent);
      FillRect(device, &accent, accent_brush);
      DeleteObject(accent_brush);
      SetBkMode(device, TRANSPARENT);
      SetTextColor(device, g_theme.text);
      DrawTextW(device, L"1 现在   2 现状   3 先在", -1, &preview,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  const auto active_theme = ActiveThemePath();
  if (!active_theme.empty())
    g_theme.Load(active_theme);
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = L"ZRinput.ThemeEditor";
  RegisterClassExW(&window_class);
  HWND window = CreateWindowExW(
      0, window_class.lpszClassName, L"ZRinput Theme Editor",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 630, 270, nullptr, nullptr, instance,
      nullptr);
  if (!window)
    return 1;
  wchar_t number[16] = {};
  swprintf_s(number, L"%d", g_theme.font_size);
  SetWindowTextW(g_font_size, number);
  swprintf_s(number, L"%d", g_theme.window_height);
  SetWindowTextW(g_window_height, number);
  ShowWindow(window, show_command);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
