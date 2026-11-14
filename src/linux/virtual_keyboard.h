#pragma once
// A virtual keyboard of our own (zwp_virtual_keyboard_v1), used to type the
// dictated text and to send the few shortcuts the app needs.
//
// wtype is not used for this: it numbers the characters of a text from
// keycode 9 upwards, whatever keys those are on a real keyboard. Programs
// that also look at the physical key - Chromium and everything built on it,
// and the compositor's key binds - then see Escape, Backspace, Tab, Return or
// Control being pressed: characters vanish, the previous one is deleted, or a
// web page reacts to Escape. This keyboard has one fixed Swedish layout, with
// every character on the key it has on a real keyboard and named keys (Left,
// BackSpace, ...) on the keycode they really have.

#include <string>
#include <vector>

namespace vtt {
namespace virtual_keyboard {

// False when the compositor does not offer the virtual-keyboard protocol.
bool available();

// True when every character of the text is on the keyboard's layout: Swedish
// and English letters, digits and common punctuation. Line breaks are not.
bool can_type(const std::string& text);

// Types UTF-8 text at the keyboard focus. Fails without typing anything when
// the text has a character that is not on the layout.
bool type_text(const std::string& text, int delay_ms = 0);

// Presses a key `count` times with modifiers ("ctrl", "shift", "alt", "logo")
// held. `key` is a single ASCII letter or digit, or one of: Left, Right, Up,
// Down, Home, End, BackSpace, Delete, Return, Tab, Escape.
bool press_key(const std::string& key, const std::vector<std::string>& modifiers = {},
               int count = 1);

} // namespace virtual_keyboard
} // namespace vtt
