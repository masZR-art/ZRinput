#pragma once

#include "core/pinyin_engine.h"

#include <msctf.h>
#include <atomic>
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
  HRESULT Commit(ITfContext* context, const std::string& utf8_text);
  void Reset();

  std::atomic<ULONG> reference_count_{1};
  ITfThreadMgr* thread_manager_ = nullptr;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  PinyinEngine engine_;
  std::string input_;
  std::vector<Candidate> candidates_;
  std::vector<std::string> committed_context_;
};

}  // namespace zrinput::windows
