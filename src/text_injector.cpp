#include "text_injector.h"
#include <vector>
#include <windows.h>

namespace vtt {

void TextInjector::set_method(const std::string& method) {
    method_ = method;
}

void TextInjector::set_keystroke_delay_ms(int ms) {
    keystroke_delay_ms_ = ms;
}

void TextInjector::set_owner_window(HWND owner) {
    owner_window_ = owner;
}

bool TextInjector::inject_text(const std::string& text, HWND expected_window) {
    if (text.empty()) return false;
    if (expected_window && GetForegroundWindow() != expected_window) return false;

    if (method_ == "keystrokes") {
        return inject_via_keystrokes(text, expected_window);
    }

    // "auto" -- prefer clipboard as it handles Unicode better
    return inject_via_clipboard(text, expected_window);
}

bool TextInjector::inject_via_clipboard(const std::string& text, HWND expected_window) {
    // Convert UTF-8 to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(wchar_t));
    if (!hMem) return false;

    wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
    if (!pMem) {
        GlobalFree(hMem);
        return false;
    }

    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wlen);
    GlobalUnlock(hMem);

    // EmptyClipboard must assign ownership to our window before we can
    // publish CF_UNICODETEXT. A null owner prevents SetClipboardData.
    if (!owner_window_ || !OpenClipboard(owner_window_)) {
        GlobalFree(hMem);
        return false;
    }

    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, hMem)) {
        CloseClipboard();
        GlobalFree(hMem);
        return false;
    }
    CloseClipboard();
    // hMem is now owned by the clipboard

    // Simulate Ctrl+V to paste
    INPUT inputs[4] = {};

    // Key down: Ctrl
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    // Key down: V
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';

    // Key up: V
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    // Key up: Ctrl
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    if (expected_window && GetForegroundWindow() != expected_window) return false;
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

bool TextInjector::inject_via_keystrokes(const std::string& text, HWND expected_window) {
    // Convert to wide string for Unicode keystroke injection
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;

    std::vector<wchar_t> wtext(wlen);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), wlen);

    for (int i = 0; i < wlen - 1; ++i) { // -1 to skip null terminator
        if (expected_window && GetForegroundWindow() != expected_window) return false;
        INPUT inputs[2] = {};

        // Key down
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = wtext[i];
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

        // Key up
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = wtext[i];
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        if (SendInput(2, inputs, sizeof(INPUT)) != 2) return false;

        if (keystroke_delay_ms_ > 0) {
            Sleep(static_cast<DWORD>(keystroke_delay_ms_));
        }
    }

    return true;
}

} // namespace vtt
