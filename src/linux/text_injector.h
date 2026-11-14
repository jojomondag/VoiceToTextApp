#pragma once
#include <string>

#include "win_compat.h"

namespace vtt {

// Puts a finished text into the focused application on Wayland ("Infoga
// text" in the dictation window).
//
//   clipboard  - copy with wl-copy, then send the paste shortcut
//                (Ctrl+V, or Ctrl+Shift+V when a terminal has focus).
//   keystrokes - type the text with the virtual keyboard.
//   auto       - clipboard, as on Windows.
class TextInjector {
public:
    // Inject text into the currently focused application. When
    // `expected_window` is given, nothing is sent unless it still has focus.
    bool inject_text(const std::string& text, HWND expected_window = nullptr);

    // Configuration
    void set_method(const std::string& method);  // "auto", "clipboard", "keystrokes"
    void set_keystroke_delay_ms(int ms);
    void set_owner_window(HWND owner);

private:
    std::string method_ = "auto";
    int keystroke_delay_ms_ = 5;

    bool inject_via_clipboard(const std::string& text);
    bool inject_via_keystrokes(const std::string& text);
};

} // namespace vtt
