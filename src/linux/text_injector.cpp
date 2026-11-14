#include "text_injector.h"

#include "hyprland_ipc.h"
#include "log.h"
#include "platform.h"
#include "wayland_input.h"

#include <fstream>

namespace vtt {

void TextInjector::set_method(const std::string& method) {
    method_ = method;
}

void TextInjector::set_keystroke_delay_ms(int ms) {
    keystroke_delay_ms_ = ms;
}

void TextInjector::set_owner_window(HWND /*owner*/) {}

bool TextInjector::inject_text(const std::string& text, HWND expected_window) {
    if (text.empty()) return false;
    if (expected_window && GetForegroundWindow() != expected_window) return false;

    // Text with characters the keyboard layout lacks can only be pasted.
    if (method_ == "keystrokes" && wayland_input::can_type(text)) {
        return inject_via_keystrokes(text);
    }
    // "clipboard" and "auto" -- prefer clipboard as it handles Unicode better
    return inject_via_clipboard(text);
}

bool TextInjector::inject_via_clipboard(const std::string& text) {
    if (!wayland_input::set_clipboard(text)) {
        vtlog("inject: wl-copy failed (is wl-clipboard installed?)");
        return false;
    }

    // Give the compositor a moment to announce the new selection.
    platform::sleep_ms(40);

    // A terminal with a custom window class is recognised by its process.
    hyprland::Window focused;
    bool terminal = false;
    if (hyprland::active_window(focused)) {
        std::string process;
        std::ifstream comm("/proc/" + std::to_string(focused.pid) + "/comm");
        std::getline(comm, process);
        terminal = wayland_input::is_terminal_class(focused.window_class) ||
                   wayland_input::is_terminal_class(process);
    }
    if (!wayland_input::send_paste(terminal)) {
        vtlog("inject: could not send the paste shortcut; the text is on the clipboard");
        return false;
    }
    return true;
}

bool TextInjector::inject_via_keystrokes(const std::string& text) {
    if (!wayland_input::type_text(text, keystroke_delay_ms_)) {
        vtlog("inject: the virtual keyboard could not type the text");
        return false;
    }
    return true;
}

} // namespace vtt
