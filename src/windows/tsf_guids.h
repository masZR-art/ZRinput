#pragma once

#include <guiddef.h>

namespace zrinput::windows {

// {BFD6C220-320C-46F4-94D0-78C4779AE70C}
inline constexpr GUID kTextServiceClsid = {
    0xbfd6c220, 0x320c, 0x46f4, {0x94, 0xd0, 0x78, 0xc4, 0x77, 0x9a, 0xe7, 0x0c}};

// {97313B73-4F48-48E4-BC7E-10DF2538892C}
inline constexpr GUID kLanguageProfileGuid = {
    0x97313b73, 0x4f48, 0x48e4, {0xbc, 0x7e, 0x10, 0xdf, 0x25, 0x38, 0x89, 0x2c}};

}  // namespace zrinput::windows
