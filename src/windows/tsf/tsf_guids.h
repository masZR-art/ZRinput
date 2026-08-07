#pragma once

#include <guiddef.h>

namespace zrinput::windows::tsf {

// Clean-room identifiers allocated for ZRinput. They are part of the public
// installation identity and must remain stable across upgrades.
inline constexpr CLSID kTextServiceClsid = {
    0xa22d9b55,
    0x29d0,
    0x4d34,
    {0x97, 0x8c, 0x3e, 0xf4, 0x8c, 0x6f, 0xa8, 0x14}};

inline constexpr GUID kSimplifiedChineseProfileGuid = {
    0xe7f1d624,
    0x7df4,
    0x45c8,
    {0x9b, 0xa6, 0xb1, 0x31, 0x0e, 0x0d, 0xc5, 0x18}};

inline constexpr wchar_t kTextServiceName[] = L"ZRinput";

}  // namespace zrinput::windows::tsf
