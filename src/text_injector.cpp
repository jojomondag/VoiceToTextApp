#include "text_injector.h"

namespace vtt {

TextInjector::TextInjector() = default;
TextInjector::~TextInjector() { shutdown(); }

bool TextInjector::initialize() {
    return true;
}

void TextInjector::shutdown() {}

void TextInjector::set_method(const std::string& method) {
    method_ = method;
}

void TextInjector::set_keystroke_delay_ms(int ms) {
    keystroke_delay_ms_ = ms;
}

bool TextInjector::inject_text(const std::string& text) {
    if (text.empty()) return false;

    if (method_ == "clipboard") {
        return inject_via_clipboard(text);
    } else if (method_ == "keystrokes") {
        return inject_via_keystrokes(text);
    }

    // "auto" -- prefer clipboard as it handles Unicode better
    return inject_via_clipboard(text);
}

bool TextInjector::inject_via_clipboard(const std::string& text) {
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

    if (!OpenClipboard(nullptr)) {
        GlobalFree(hMem);
        return false;
    }

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hMem);
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

    SendInput(4, inputs, sizeof(INPUT));
    return true;
}

bool TextInjector::inject_via_keystrokes(const std::string& text) {
    // Convert to wide string for Unicode keystroke injection
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;

    std::vector<wchar_t> wtext(wlen);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), wlen);

    for (int i = 0; i < wlen - 1; ++i) { // -1 to skip null terminator
        INPUT inputs[2] = {};

        // Key down
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = wtext[i];
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

        // Key up
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = wtext[i];
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        SendInput(2, inputs, sizeof(INPUT));

        if (keystroke_delay_ms_ > 0) {
            Sleep(static_cast<DWORD>(keystroke_delay_ms_));
        }
    }

    return true;
}

} // namespace vtt
