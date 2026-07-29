#pragma once

#include "core/pinyin_engine.h"
#include "windows/theme.h"

#include <windows.h>
#include <cstddef>
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

 private:
  static LRESULT CALLBACK WindowProcedure(HWND window,
                                           UINT message,
                                           WPARAM wparam,
                                           LPARAM lparam);
  bool EnsureWindow();
  int CalculateWidth() const;
  void UpdateWindowRegion();
  void Paint();
  void Position();

  HWND window_ = nullptr;
  std::vector<Candidate> candidates_;
  std::size_t page_ = 0;
  std::size_t page_size_ = 5;
  bool preview_mode_ = false;
  int window_width_ = 320;
  Theme theme_;
  RECT anchor_{};
  bool has_anchor_ = false;
};

}  // namespace zrinput::windows
