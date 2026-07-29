#include "windows/candidate_window.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace zrinput::windows {
extern HMODULE g_module;
namespace {

constexpr wchar_t kWindowClass[] = L"ZRinput.CandidateWindow";
constexpr wchar_t kTextFont[] = L"Microsoft YaHei UI";
constexpr int kWindowRadius = 8;
constexpr int kSelectionRadius = 5;
std::mutex g_window_class_mutex;
std::size_t g_window_class_users = 0;

bool AcquireWindowClass() {
  std::lock_guard lock(g_window_class_mutex);
  if (g_window_class_users == 0) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = CandidateWindow::WindowProcedure;
    window_class.hInstance = g_module;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }
  }
  ++g_window_class_users;
  return true;
}

void ReleaseWindowClass() {
  std::lock_guard lock(g_window_class_mutex);
  if (g_window_class_users == 0)
    return;
  --g_window_class_users;
  if (g_window_class_users == 0)
    UnregisterClassW(kWindowClass, g_module);
}

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

RECT ToWin32Rect(const CandidateRect& rect) {
  return {rect.left, rect.top, rect.right, rect.bottom};
}

COLORREF Blend(COLORREF first, COLORREF second, int second_percent) {
  const int first_percent = 100 - second_percent;
  return RGB((GetRValue(first) * first_percent +
              GetRValue(second) * second_percent) / 100,
             (GetGValue(first) * first_percent +
              GetGValue(second) * second_percent) / 100,
             (GetBValue(first) * first_percent +
              GetBValue(second) * second_percent) / 100);
}

void FillRoundedRect(HDC device,
                     const CandidateRect& rect,
                     COLORREF color,
                     int radius) {
  HBRUSH brush = CreateSolidBrush(color);
  HGDIOBJ old_brush = SelectObject(device, brush);
  HGDIOBJ old_pen = SelectObject(device, GetStockObject(NULL_PEN));
  RoundRect(device, rect.left, rect.top, rect.right, rect.bottom,
            radius * 2, radius * 2);
  SelectObject(device, old_pen);
  SelectObject(device, old_brush);
  DeleteObject(brush);
}

void FillRectColor(HDC device, const CandidateRect& rect, COLORREF color) {
  RECT native_rect = ToWin32Rect(rect);
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(device, &native_rect, brush);
  DeleteObject(brush);
}

void DrawTriangle(HDC device,
                  const CandidateRect& bounds,
                  bool points_right,
                  COLORREF color) {
  const int center_x =
      (bounds.left + bounds.right) / 2 + (points_right ? -2 : 0);
  const int center_y = (bounds.top + bounds.bottom) / 2 + 2;
  POINT points[3]{};
  if (points_right) {
    points[0] = {center_x - 4, center_y - 6};
    points[1] = {center_x - 4, center_y + 5};
    points[2] = {center_x + 5, center_y};
  } else {
    points[0] = {center_x + 4, center_y - 6};
    points[1] = {center_x + 4, center_y + 5};
    points[2] = {center_x - 3, center_y};
  }
  HBRUSH brush = CreateSolidBrush(color);
  HGDIOBJ old_brush = SelectObject(device, brush);
  HGDIOBJ old_pen = SelectObject(device, GetStockObject(NULL_PEN));
  Polygon(device, points, static_cast<int>(std::size(points)));
  SelectObject(device, old_pen);
  SelectObject(device, old_brush);
  DeleteObject(brush);
}

void DrawHeart(HDC device, const CandidateRect& bounds, COLORREF color) {
  const int center_x = (bounds.left + bounds.right) / 2;
  const int center_y = (bounds.top + bounds.bottom) / 2 + 1;
  HPEN outline = CreatePen(PS_SOLID, 1, color);
  HGDIOBJ old_pen = SelectObject(device, outline);
  HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
  RoundRect(device, center_x - 10, center_y - 8,
            center_x + 10, center_y + 10, 5, 5);
  SelectObject(device, old_brush);
  SelectObject(device, old_pen);
  DeleteObject(outline);

  HBRUSH fill = CreateSolidBrush(color);
  old_brush = SelectObject(device, fill);
  old_pen = SelectObject(device, GetStockObject(NULL_PEN));
  Ellipse(device, center_x - 2, center_y - 10,
          center_x + 5, center_y - 3);
  Ellipse(device, center_x + 3, center_y - 10,
          center_x + 10, center_y - 3);
  POINT heart_tip[] = {
      {center_x - 2, center_y - 7},
      {center_x + 10, center_y - 7},
      {center_x + 4, center_y + 3},
  };
  Polygon(device, heart_tip, static_cast<int>(std::size(heart_tip)));
  SelectObject(device, old_pen);
  SelectObject(device, old_brush);
  DeleteObject(fill);
}

void DrawChevronDown(HDC device,
                     const CandidateRect& bounds,
                     COLORREF color) {
  const int center_x = (bounds.left + bounds.right) / 2;
  const int center_y = (bounds.top + bounds.bottom) / 2 + 2;
  HPEN pen = CreatePen(PS_SOLID, 2, color);
  HGDIOBJ old_pen = SelectObject(device, pen);
  MoveToEx(device, center_x - 7, center_y - 4, nullptr);
  LineTo(device, center_x, center_y + 4);
  LineTo(device, center_x + 7, center_y - 4);
  SelectObject(device, old_pen);
  DeleteObject(pen);
}

}  // namespace

CandidateWindow::~CandidateWindow() {
  if (window_) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
  if (class_acquired_)
    ReleaseWindowClass();
}

bool CandidateWindow::EnsureWindow() {
  if (window_)
    return true;
  if (!class_acquired_) {
    class_acquired_ = AcquireWindowClass();
    if (!class_acquired_)
      return false;
  }
  const DWORD extended_style = preview_mode_
      ? WS_EX_TOOLWINDOW
      : WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
  window_ = CreateWindowExW(
      extended_style, kWindowClass, L"ZRinput Candidate", WS_POPUP, 0, 0,
      window_width_, theme_.window_height, nullptr, nullptr, g_module, this);
  if (!window_) {
    ReleaseWindowClass();
    class_acquired_ = false;
    return false;
  }
  UpdateWindowRegion();
  return true;
}

void CandidateWindow::Show(const std::vector<Candidate>& candidates,
                           std::size_t page,
                           std::size_t page_size) {
  candidates_ = candidates;
  page_ = page;
  page_size_ = std::clamp<std::size_t>(page_size, 1,
                                      kMicrosoftCandidatePageSize);
  if (candidates_.empty() || !EnsureWindow()) {
    Hide();
    return;
  }

  HDC device = GetDC(window_);
  if (device) {
    layout_ = CalculateLayout(device);
    ReleaseDC(window_, device);
  }
  window_width_ = layout_.width;
  UpdateWindowRegion();
  Position();
  InvalidateRect(window_, nullptr, FALSE);
  ShowWindow(window_, preview_mode_ ? SW_SHOWNOACTIVATE : SW_SHOWNOACTIVATE);
}

void CandidateWindow::Hide() {
  if (window_)
    ShowWindow(window_, SW_HIDE);
}

void CandidateWindow::SetTheme(const Theme& theme) {
  theme_ = theme;
  if (!window_)
    return;
  HDC device = GetDC(window_);
  if (device) {
    layout_ = CalculateLayout(device);
    ReleaseDC(window_, device);
  }
  window_width_ = layout_.width;
  UpdateWindowRegion();
  Position();
  InvalidateRect(window_, nullptr, FALSE);
}

void CandidateWindow::SetAnchor(const RECT& anchor) {
  anchor_ = anchor;
  has_anchor_ = true;
}

void CandidateWindow::ClearAnchor() {
  has_anchor_ = false;
}

CandidateLayout CandidateWindow::CalculateLayout(HDC device) const {
  HFONT font = CreateFontW(-theme_.font_size, 0, 0, 0, FW_LIGHT, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH, kTextFont);
  HGDIOBJ old_font = SelectObject(device, font);
  std::vector<int> label_widths;
  const std::size_t begin = page_ * page_size_;
  if (begin >= candidates_.size()) {
    SelectObject(device, old_font);
    DeleteObject(font);
    return CalculateCandidateLayout({}, theme_.window_height);
  }
  const std::size_t end = std::min(begin + page_size_, candidates_.size());
  label_widths.reserve(end - begin);
  for (std::size_t index = begin; index < end; ++index) {
    const std::wstring label = std::to_wstring(index - begin + 1) + L" " +
                               Utf8ToWide(candidates_[index].text);
    SIZE extent{};
    GetTextExtentPoint32W(device, label.c_str(), static_cast<int>(label.size()),
                          &extent);
    label_widths.push_back(extent.cx);
  }
  SelectObject(device, old_font);
  DeleteObject(font);
  return CalculateCandidateLayout(label_widths, theme_.window_height);
}

void CandidateWindow::UpdateWindowRegion() {
  if (!window_)
    return;
  const int height = std::max(layout_.height, theme_.window_height);
  SetWindowRgn(window_, CreateRoundRectRgn(0, 0, window_width_ + 1,
                                           height + 1,
                                           kWindowRadius * 2,
                                           kWindowRadius * 2),
               TRUE);
}

void CandidateWindow::Position() {
  POINT point{};
  GUITHREADINFO info{sizeof(info)};
  if (has_anchor_) {
    point = {anchor_.left, anchor_.bottom};
  } else if (GetGUIThreadInfo(0, &info) && info.hwndCaret) {
    point = {info.rcCaret.left, info.rcCaret.bottom};
    ClientToScreen(info.hwndCaret, &point);
  } else {
    GetCursorPos(&point);
  }
  HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  GetMonitorInfoW(monitor, &monitor_info);
  const LONG maximum_x = std::max(
      monitor_info.rcWork.left,
      monitor_info.rcWork.right - static_cast<LONG>(window_width_));
  point.x = std::clamp<LONG>(point.x, monitor_info.rcWork.left, maximum_x);
  if (point.y + layout_.height > monitor_info.rcWork.bottom)
    point.y -= layout_.height + 24;
  SetWindowPos(window_, HWND_TOPMOST, point.x, point.y + 4,
               window_width_, layout_.height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CandidateWindow::PaintSurface(HDC device,
                                   const CandidateLayout& layout) const {
  RECT bounds{0, 0, layout.width, layout.height};
  HBRUSH background = CreateSolidBrush(theme_.background);
  FillRect(device, &bounds, background);
  DeleteObject(background);

  SetBkMode(device, TRANSPARENT);
  SetTextColor(device, theme_.text);
  HFONT font = CreateFontW(-theme_.font_size, 0, 0, 0, FW_LIGHT, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH, kTextFont);
  HGDIOBJ old_font = SelectObject(device, font);

  const std::size_t begin = page_ * page_size_;
  const std::size_t end = std::min(begin + page_size_, candidates_.size());
  for (std::size_t index = begin; index < end; ++index) {
    const std::size_t visible_index = index - begin;
    if (visible_index >= layout.candidate_items.size())
      break;
    const CandidateRect& item = layout.candidate_items[visible_index];
    if (visible_index == 0) {
      CandidateRect selection = item;
      selection.left += 6;
      selection.right += 2;
      selection.top += 2;
      selection.bottom += 1;
      FillRoundedRect(device, selection, theme_.selected, kSelectionRadius);
      CandidateRect accent = layout.accent;
      accent.right += 2;
      accent.top += 1;
      accent.bottom += 1;
      FillRoundedRect(device, accent, theme_.accent, 2);
    }
    const std::wstring label = std::to_wstring(visible_index + 1) + L" " +
                               Utf8ToWide(candidates_[index].text);
    RECT text_rect = ToWin32Rect(item);
    text_rect.left -= 4;
    text_rect.right -= 4;
    ++text_rect.top;
    ++text_rect.bottom;
    DrawTextW(device, label.c_str(), static_cast<int>(label.size()), &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
  }
  SelectObject(device, old_font);
  DeleteObject(font);

  const COLORREF separator_base = Blend(theme_.background, theme_.text, 8);
  const COLORREF separator = RGB(
      std::min(255, static_cast<int>(GetRValue(separator_base)) + 1),
      std::min(255, static_cast<int>(GetGValue(separator_base)) + 1),
      std::min(255, static_cast<int>(GetBValue(separator_base)) + 1));
  FillRectColor(device, layout.candidate_separator, separator);
  FillRectColor(device, layout.tools_separator, separator);
  FillRectColor(device, layout.menu_separator, separator);

  const bool has_previous = page_ > 0;
  const bool has_next = (page_ + 1) * page_size_ < candidates_.size();
  const COLORREF disabled = Blend(theme_.background, theme_.secondary_text, 62);
  DrawTriangle(device, layout.previous_page, false,
               has_previous ? theme_.text : disabled);
  DrawTriangle(device, layout.next_page, true,
               has_next ? theme_.text : disabled);
  DrawHeart(device, layout.symbols, theme_.text);

  CandidateRect menu_background = layout.menu;
  menu_background.left += 5;
  menu_background.top += 9;
  menu_background.bottom -= 6;
  menu_background.right -= 4;
  FillRoundedRect(device, menu_background, theme_.selected,
                  kSelectionRadius);
  DrawChevronDown(device, layout.menu, theme_.text);

  HPEN border_pen =
      CreatePen(PS_SOLID, 1, Blend(theme_.background, theme_.text, 1));
  HGDIOBJ old_pen = SelectObject(device, border_pen);
  HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
  RoundRect(device, 0, 0, layout.width, layout.height,
            kWindowRadius * 2, kWindowRadius * 2);
  SelectObject(device, old_brush);
  SelectObject(device, old_pen);
  DeleteObject(border_pen);
}

void CandidateWindow::Paint() {
  PAINTSTRUCT paint{};
  HDC device = BeginPaint(window_, &paint);
  HDC memory = CreateCompatibleDC(device);
  HBITMAP bitmap =
      CreateCompatibleBitmap(device, layout_.width, layout_.height);
  if (!memory || !bitmap) {
    if (bitmap)
      DeleteObject(bitmap);
    if (memory)
      DeleteDC(memory);
    EndPaint(window_, &paint);
    return;
  }
  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
  PaintSurface(memory, layout_);
  BitBlt(device, 0, 0, layout_.width, layout_.height, memory, 0, 0, SRCCOPY);
  SelectObject(memory, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
  EndPaint(window_, &paint);
}

bool CandidateWindow::RenderToBitmap(
    const std::vector<Candidate>& candidates,
    std::size_t page,
    std::size_t page_size,
    const std::filesystem::path& path) {
  if (candidates.empty())
    return false;
  candidates_ = candidates;
  page_ = page;
  page_size_ = std::clamp<std::size_t>(page_size, 1,
                                      kMicrosoftCandidatePageSize);

  HDC screen = GetDC(nullptr);
  if (!screen)
    return false;
  layout_ = CalculateLayout(screen);
  window_width_ = layout_.width;

  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = layout_.width;
  bitmap_info.bmiHeader.biHeight = -layout_.height;
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS,
                                    &pixels, nullptr, 0);
  HDC memory = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  if (!bitmap || !memory || !pixels) {
    if (bitmap)
      DeleteObject(bitmap);
    if (memory)
      DeleteDC(memory);
    return false;
  }

  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
  PaintSurface(memory, layout_);
  GdiFlush();

  const std::uint32_t image_size = static_cast<std::uint32_t>(
      layout_.width * layout_.height * sizeof(std::uint32_t));
  BITMAPFILEHEADER file_header{};
  file_header.bfType = 0x4d42;
  file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file_header.bfSize = file_header.bfOffBits + image_size;

  std::error_code error;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (output) {
    output.write(reinterpret_cast<const char*>(&file_header),
                 sizeof(file_header));
    output.write(reinterpret_cast<const char*>(&bitmap_info.bmiHeader),
                 sizeof(bitmap_info.bmiHeader));
    output.write(static_cast<const char*>(pixels), image_size);
  }
  const bool written = output.good();

  SelectObject(memory, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
  return !error && written;
}

bool CandidateWindow::TestHiddenWindowClassLifecycle() {
  {
    CandidateWindow first(true);
    CandidateWindow second(true);
    if (!first.EnsureWindow() || !second.EnsureWindow())
      return false;
  }
  WNDCLASSEXW window_class{sizeof(window_class)};
  return GetClassInfoExW(g_module, kWindowClass, &window_class) == FALSE;
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
