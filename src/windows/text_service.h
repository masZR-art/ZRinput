#pragma once

#include "core/pinyin_engine.h"
#include "windows/candidate_window.h"

#include <msctf.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace zrinput::windows {

extern std::atomic<long> g_object_count;
extern std::atomic<long> g_server_locks;

class TextService final : public ITfTextInputProcessor,
                          public ITfKeyEventSink {
 public:
  TextService();

  STDMETHODIMP QueryInterface(REFIID interface_id, void** object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP Activate(ITfThreadMgr* thread_manager,
                        TfClientId client_id) override;
  STDMETHODIMP Deactivate() override;

  STDMETHODIMP OnSetFocus(BOOL foreground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext* context,
                             WPARAM key,
                             LPARAM flags,
                             BOOL* eaten) override;
  STDMETHODIMP OnKeyDown(ITfContext* context,
                         WPARAM key,
                         LPARAM flags,
                         BOOL* eaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext* context,
                           WPARAM key,
                           LPARAM flags,
                           BOOL* eaten) override;
  STDMETHODIMP OnKeyUp(ITfContext* context,
                       WPARAM key,
                       LPARAM flags,
                       BOOL* eaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext* context,
                              REFGUID guid,
                              BOOL* eaten) override;

 private:
  ~TextService();

  bool ShouldHandle(WPARAM key) const;
  bool HandleKey(ITfContext* context, WPARAM key);
  void RefreshCandidates();
  void ChangePage(int delta);
  HRESULT UpdateComposition(ITfContext* context,
                            const std::wstring& text,
                            bool end_composition);
  void CancelComposition();
  HRESULT Commit(ITfContext* context, const std::string& utf8_text);
  void Reset();

  std::atomic<ULONG> reference_count_{1};
  ITfThreadMgr* thread_manager_ = nullptr;
  ITfContext* composition_context_ = nullptr;
  ITfComposition* composition_ = nullptr;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  PinyinEngine engine_;
  std::filesystem::path memory_path_;
  std::string input_;
  std::vector<Candidate> candidates_;
  CandidateWindow candidate_window_;
  std::size_t page_ = 0;
  static constexpr std::size_t kPageSize = 5;
  std::vector<std::string> committed_context_;
};

}  // namespace zrinput::windows
