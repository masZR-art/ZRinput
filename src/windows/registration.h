#pragma once

#include <windows.h>

namespace zrinput::windows {
HRESULT RegisterTextService(HMODULE module);
HRESULT UnregisterTextService();
}
