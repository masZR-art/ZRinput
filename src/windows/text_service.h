#pragma once

#include "core/composition_state.h"
#include "core/pinyin_engine.h"
#include "windows/candidate_window.h"

#include <msctf.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zrinput::windows {

extern std::atomic<long> g_object_count;
extern std::atomic<long> g_server_locks;

class TextService final : public ITfTextInputProcessor,
                          public ITfKeyEventSink,
                          public ITfCompositionSink {
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
  STDMETHODIMP OnCompositionTerminated(
      TfEditCookie cookie,
      ITfComposition* composition) override;

 private:
  ~TextService();

  bool ShouldHandle(ITfContext* context, WPARAM key) const;
  bool HandleKey(ITfContext* context, WPARAM key);
  void RefreshCandidates();
  void ChangePage(int delta);
  bool LearnSelection(const std::string& selected,
                      const std::string& input,
                      std::size_t candidate_index);
  HRESULT UpdateComposition(ITfContext* context,
                            const std::wstring& text,
                            bool end_composition);
  HRESULT CancelComposition();
  HRESULT Commit(ITfContext* context, const std::string& utf8_text);
  void Reset();

  std::atomic<ULONG> reference_count_{1};
  ITfThreadMgr* thread_manager_ = nullptr;
  ITfContext* composition_context_ = nullptr;
  ITfComposition* composition_ = nullptr;
  bool ending_composition_ = false;
  std::uint64_t edit_generation_ = 0;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  PinyinEngine engine_;
  std::filesystem::path memory_path_;
  std::string application_;
  bool dictionary_ready_ = false;
  CompositionState state_;
  std::vector<Candidate> candidates_;
  CandidateWindow candidate_window_;
  static constexpr std::size_t kPageSize = 7;
  std::vector<std::string> committed_context_;
};

}  // namespace zrinput::windows
