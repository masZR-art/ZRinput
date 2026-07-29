#pragma once

#include "core/pinyin_engine.h"
#include "windows/candidate_layout.h"
#include "windows/theme.h"

#include <windows.h>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace zrinput::windows {

class CandidateWindow {
 public:
  explicit CandidateWindow(bool preview_mode = false)
      : preview_mode_(preview_mode) {}
  ~CandidateWindow();

  CandidateWindow(const CandidateWindow&) = delete;
  CandidateWindow& operator=(const CandidateWindow&) = delete;

  void Show(const std::vector<Candidate>& candidates,
            std::size_t page,
            std::size_t page_size);
  void Hide();
  void SetTheme(const Theme& theme);
  void SetAnchor(const RECT& anchor);
  void ClearAnchor();
  bool RenderToBitmap(const std::vector<Candidate>& candidates,
                      std::size_t page,
                      std::size_t page_size,
                      const std::filesystem::path& path);
  static bool TestHiddenWindowClassLifecycle();

 public:
  static LRESULT CALLBACK WindowProcedure(HWND window,
                                           UINT message,
                                           WPARAM wparam,
                                           LPARAM lparam);

 private:
  bool EnsureWindow();
  CandidateLayout CalculateLayout(HDC device) const;
  void UpdateWindowRegion();
  void Paint();
  void PaintSurface(HDC device, const CandidateLayout& layout) const;
  void Position();

  HWND window_ = nullptr;
  bool class_acquired_ = false;
  std::vector<Candidate> candidates_;
  std::size_t page_ = 0;
  std::size_t page_size_ = kMicrosoftCandidatePageSize;
  bool preview_mode_ = false;
  int window_width_ = 320;
  CandidateLayout layout_;
  Theme theme_;
  RECT anchor_{};
  bool has_anchor_ = false;
};

}  // namespace zrinput::windows
