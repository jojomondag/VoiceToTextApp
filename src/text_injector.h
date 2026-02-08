#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace vtt {

class TextInjector {
public:
    TextInjector();
    ~TextInjector();

    bool initialize();
    void shutdown();

    // Inject text into the currently focused application
    bool inject_text(const std::string& text);

    // Configuration
    void set_method(const std::string& method);  // "auto", "clipboard", "keystrokes"
    void set_keystroke_delay_ms(int ms);

private:
    std::string method_ = "auto";
    int keystroke_delay_ms_ = 5;

    bool inject_via_clipboard(const std::string& text);
    bool inject_via_keystrokes(const std::string& text);
};

} // namespace vtt
