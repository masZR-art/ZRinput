#include "windows/candidate_window.h"

#include <algorithm>
#include <string>

namespace zrinput::windows {
extern HMODULE g_module;
namespace {

constexpr wchar_t kWindowClass[] = L"ZRinput.CandidateWindow";
constexpr int kWindowHeight = 44;
constexpr int kWindowWidth = 620;

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty())
    return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
  if (length <= 0)
    return {};
  std::wstring result(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

}  // namespace

CandidateWindow::~CandidateWindow() {
  if (window_)
    DestroyWindow(window_);
}

bool CandidateWindow::EnsureWindow() {
  if (window_)
    return true;
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = g_module;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  RegisterClassExW(&window_class);
  const DWORD extended_style = preview_mode_
      ? WS_EX_TOPMOST
      : WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
  window_ = CreateWindowExW(
      extended_style,
      kWindowClass, L"ZRinput Candidate", WS_POPUP, 0, 0,
      kWindowWidth, kWindowHeight,
      nullptr, nullptr, g_module, this);
  if (!window_)
    return false;
  SetWindowRgn(window_, CreateRoundRectRgn(0, 0, kWindowWidth + 1,
                                           kWindowHeight + 1, 8, 8), TRUE);
  return true;
}

void CandidateWindow::Show(const std::vector<Candidate>& candidates,
                           std::size_t page,
                           std::size_t page_size) {
  candidates_ = candidates;
  page_ = page;
  page_size_ = page_size;
  if (candidates_.empty() || !EnsureWindow()) {
    Hide();
    return;
  }
  Position();
  InvalidateRect(window_, nullptr, FALSE);
  ShowWindow(window_, preview_mode_ ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
}

void CandidateWindow::Hide() {
  if (window_)
    ShowWindow(window_, SW_HIDE);
}

void CandidateWindow::Position() {
  POINT point{};
  GUITHREADINFO info{sizeof(info)};
  if (GetGUIThreadInfo(0, &info) && info.hwndCaret) {
    point = {info.rcCaret.left, info.rcCaret.bottom};
    ClientToScreen(info.hwndCaret, &point);
  } else {
    GetCursorPos(&point);
  }
  HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  GetMonitorInfoW(monitor, &monitor_info);
  point.x = std::clamp(point.x, monitor_info.rcWork.left,
                       monitor_info.rcWork.right - kWindowWidth);
  if (point.y + kWindowHeight > monitor_info.rcWork.bottom)
    point.y -= kWindowHeight + 24;
  SetWindowPos(window_, HWND_TOPMOST, point.x, point.y + 4,
               kWindowWidth, kWindowHeight,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CandidateWindow::Paint() {
  PAINTSTRUCT paint{};
  HDC device = BeginPaint(window_, &paint);
  RECT bounds{};
  GetClientRect(window_, &bounds);
  HBRUSH background = CreateSolidBrush(RGB(32, 32, 32));
  FillRect(device, &bounds, background);
  DeleteObject(background);

  SetBkMode(device, TRANSPARENT);
  SetTextColor(device, RGB(245, 245, 245));
  HFONT font = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Microsoft YaHei UI");
  HFONT old_font = static_cast<HFONT>(SelectObject(device, font));

  const std::size_t begin = page_ * page_size_;
  const std::size_t end = std::min(begin + page_size_, candidates_.size());
  int x = 9;
  for (std::size_t index = begin; index < end; ++index) {
    const std::wstring text = Utf8ToWide(candidates_[index].text);
    const std::wstring label = std::to_wstring(index - begin + 1) + L" " + text;
    SIZE extent{};
    GetTextExtentPoint32W(device, label.c_str(), static_cast<int>(label.size()),
                          &extent);
    const int item_width = extent.cx + 22;
    RECT item{x, 4, x + item_width, kWindowHeight - 4};
    if (index == begin) {
      HBRUSH selected = CreateSolidBrush(RGB(62, 62, 62));
      FillRect(device, &item, selected);
      DeleteObject(selected);
      RECT accent{x, 9, x + 2, kWindowHeight - 9};
      HBRUSH blue = CreateSolidBrush(RGB(0, 120, 212));
      FillRect(device, &accent, blue);
      DeleteObject(blue);
    }
    DrawTextW(device, label.c_str(), static_cast<int>(label.size()), &item,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x += item_width + 3;
  }

  SetTextColor(device, RGB(190, 190, 190));
  RECT pager{kWindowWidth - 55, 4, kWindowWidth - 8, kWindowHeight - 4};
  DrawTextW(device, L"‹  ›", -1, &pager,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(device, old_font);
  DeleteObject(font);
  EndPaint(window_, &paint);
}

LRESULT CALLBACK CandidateWindow::WindowProcedure(HWND window,
                                                   UINT message,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  CandidateWindow* self = reinterpret_cast<CandidateWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<CandidateWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
  }
  if (message == WM_PAINT && self) {
    self->Paint();
    return 0;
  }
  if (message == WM_ERASEBKGND)
    return 1;
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace zrinput::windows
