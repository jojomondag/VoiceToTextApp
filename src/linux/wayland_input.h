#pragma once
// Synthetic input and clipboard access on Wayland: keys through the app's own
// virtual keyboard (virtual_keyboard.h), the clipboard through wl-copy and
// wl-paste (wl-clipboard). Every call blocks until it is done.

#include <string>
#include <vector>

namespace vtt {
namespace wayland_input {

// Runs a program and waits for it. `input`, when given, is written to its
// stdin; `output`, when given, receives its stdout. True on exit status 0.
bool run(const std::vector<std::string>& arguments, const std::string* input = nullptr,
         std::string* output = nullptr);

// True when the text can be typed key by key (see virtual_keyboard.h);
// anything else has to be pasted.
bool can_type(const std::string& text);

// Types text at the keyboard focus. `delay_ms` is the pause between keys.
bool type_text(const std::string& text, int delay_ms = 0);

// Presses a key `count` times, with modifiers ("ctrl", "shift", "alt",
// "logo") held. `key` is an XKB key name such as "v", "Left" or "BackSpace".
bool press_key(const std::string& key, const std::vector<std::string>& modifiers = {},
               int count = 1);

// Clipboard (the regular selection, text only).
bool set_clipboard(const std::string& text);
bool clear_clipboard();
// False when the clipboard holds no text.
bool get_clipboard(std::string& text);

// True for window classes of terminal emulators, which paste with
// Ctrl+Shift+V and where Ctrl+C / Shift+arrows do not edit text.
bool is_terminal_class(const std::string& window_class);

// Sends the paste shortcut that suits the focused window.
bool send_paste(bool terminal);

} // namespace wayland_input
} // namespace vtt
