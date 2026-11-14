#pragma once
// The handful of Win32 names the shared application code is written against,
// for builds that are not Windows. The application core (app.cpp) keeps its
// Win32 shape - one window procedure fed by posted messages and a timer - and
// this header maps that shape onto the Qt event loop (see win_compat.cpp).
// Nothing here pretends to be a general Win32 layer.

#include <cstddef>
#include <cstdint>

#include "key_codes.h"

using BOOL = int;
using UINT = unsigned int;
using DWORD = unsigned long;
using ULONGLONG = unsigned long long;
using UINT_PTR = std::uintptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using LONG_PTR = std::intptr_t;

// Opaque handles. A window handle is either the application's message target
// (created by vtt::compat::create_message_window) or a token for a window of
// another application (its compositor address); the two never mix.
using HWND = void*;
using HINSTANCE = void*;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef CALLBACK
#define CALLBACK
#endif

// --- Messages -------------------------------------------------------------

enum : UINT {
    WM_DESTROY  = 0x0002,
    WM_CLOSE    = 0x0010,
    WM_NCCREATE = 0x0081,
    WM_TIMER    = 0x0113,
    WM_HOTKEY   = 0x0312,
    WM_APP      = 0x8000,
};

enum : UINT { MB_ICONWARNING = 0x30 };

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

namespace vtt {
namespace compat {

using WindowProc = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);

// The application's message target. Messages posted to it from any thread are
// delivered to `proc` on the UI thread, in order.
HWND create_message_window(WindowProc proc, void* user_data);
void destroy_message_window(HWND window);
void* window_user_data(HWND window);

} // namespace compat
} // namespace vtt

// Queues a message for the window procedure. WM_CLOSE ends the application
// the way DefWindowProc would (the procedure then sees WM_DESTROY).
BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
inline BOOL PostMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return PostMessageW(window, message, wparam, lparam);
}
inline LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
void PostQuitMessage(int exit_code);
inline UINT RegisterWindowMessageW(const wchar_t*) { return 0; }

// Repeating UI-thread timer that posts WM_TIMER with `id`.
UINT_PTR SetTimer(HWND window, UINT_PTR id, UINT milliseconds, void* unused);
BOOL KillTimer(HWND window, UINT_PTR id);

ULONGLONG GetTickCount64();
void Sleep(DWORD milliseconds);
inline DWORD GetLastError() { return 0; }
DWORD GetCurrentProcessId();
int MessageBoxW(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type);

// Physical key state is not observable on Wayland; nothing reads as held.
inline short GetAsyncKeyState(int) { return 0; }

// Windows of other applications, identified by their compositor address
// (Hyprland). Without a supported compositor there is no foreground window.
HWND GetForegroundWindow();
BOOL IsWindow(HWND window);
DWORD GetWindowThreadProcessId(HWND window, DWORD* process_id);
BOOL SetForegroundWindow(HWND window);
