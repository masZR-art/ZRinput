#include "windows/tsf/module_state.h"

#include <atomic>

namespace zrinput::windows::tsf {
namespace {

std::atomic<HMODULE> g_module = nullptr;
std::atomic<unsigned long> g_live_objects = 0;
std::atomic<unsigned long> g_server_locks = 0;

}  // namespace

void SetModuleHandle(HMODULE module) noexcept { g_module.store(module); }

HMODULE ModuleInstance() noexcept { return g_module.load(); }

void AddLiveObject() noexcept { g_live_objects.fetch_add(1); }

void RemoveLiveObject() noexcept { g_live_objects.fetch_sub(1); }

void LockServer() noexcept { g_server_locks.fetch_add(1); }

void UnlockServer() noexcept {
  unsigned long value = g_server_locks.load();
  while (value != 0 &&
         !g_server_locks.compare_exchange_weak(value, value - 1)) {
  }
}

bool CanUnload() noexcept {
  return g_live_objects.load() == 0 && g_server_locks.load() == 0;
}

}  // namespace zrinput::windows::tsf
