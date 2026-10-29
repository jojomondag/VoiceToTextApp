#pragma once
#include <string>
#include <windows.h>

namespace vtt {

class TextInjector {
public:
    // Inject text into the currently focused application
    bool inject_text(const std::string& text, HWND expected_window = nullptr);

    // Configuration
    void set_method(const std::string& method);  // "auto", "clipboard", "keystrokes"
    void set_keystroke_delay_ms(int ms);
    void set_owner_window(HWND owner);

private:
    std::string method_ = "auto";
    int keystroke_delay_ms_ = 5;
    HWND owner_window_ = nullptr;

    bool inject_via_clipboard(const std::string& text, HWND expected_window);
    bool inject_via_keystrokes(const std::string& text, HWND expected_window);
};

} // namespace vtt
