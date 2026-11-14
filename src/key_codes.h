#pragma once
// Hotkeys are stored in config.json as Windows virtual-key codes plus
// RegisterHotKey modifier flags. Other platforms use the same numbers so a
// config file means the same thing everywhere.

#ifdef _WIN32
#include <windows.h>
#else
enum : unsigned int {
    MOD_ALT     = 0x0001,
    MOD_CONTROL = 0x0002,
    MOD_SHIFT   = 0x0004,
    MOD_WIN     = 0x0008,
};

enum : unsigned int {
    VK_LBUTTON = 0x01,
    VK_RBUTTON = 0x02,
    VK_MBUTTON = 0x04,
    VK_SHIFT   = 0x10,
    VK_CONTROL = 0x11,
    VK_MENU    = 0x12,
    VK_LWIN    = 0x5B,
    VK_RWIN    = 0x5C,
    VK_BACK   = 0x08,
    VK_TAB    = 0x09,
    VK_RETURN = 0x0D,
    VK_ESCAPE = 0x1B,
    VK_SPACE  = 0x20,
    VK_DELETE = 0x2E,
    VK_F1     = 0x70,
    VK_F24    = 0x87,
};
#endif
