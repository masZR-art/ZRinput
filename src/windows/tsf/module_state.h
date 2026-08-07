#pragma once

#include <windows.h>

namespace zrinput::windows::tsf {

void SetModuleHandle(HMODULE module) noexcept;
[[nodiscard]] HMODULE ModuleInstance() noexcept;

void AddLiveObject() noexcept;
void RemoveLiveObject() noexcept;
void LockServer() noexcept;
void UnlockServer() noexcept;
[[nodiscard]] bool CanUnload() noexcept;

}  // namespace zrinput::windows::tsf
