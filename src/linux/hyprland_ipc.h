#pragma once
// Hyprland IPC: the request socket hyprctl uses
// ($XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock), spoken to
// directly so no process has to be spawned per request.

#include <cstdint>
#include <string>
#include <vector>

namespace vtt {
namespace hyprland {

// True when running inside a Hyprland session.
bool available();

// Sends one request (e.g. "j/activewindow", "eval <lua>") and returns the
// reply. Returns false when Hyprland cannot be reached.
bool request(const std::string& command, std::string& reply);

// True when Hyprland runs a Lua config (0.5x+): "keyword" is refused there and
// binds are added with "eval hl.bind(...)".
bool uses_lua();

// Path of the event socket (.socket2.sock), empty when not under Hyprland.
std::string event_socket_path();

// A Lua string literal.
std::string lua_quote(const std::string& text);

struct Window {
    std::uintptr_t address = 0;   // Hyprland's window address
    long pid = 0;
    std::string window_class;     // lower case
    std::string title;
};

// What decides whether keys typed now reach the focused window as typed.
struct KeyboardState {
    // The Hyprland config tracks held modifier keys (~/.config/hypr/voicetotext.lua).
    bool tracked = false;
    // Modifier keys held right now on any keyboard. Hyprland adds them to every
    // key the app types, which turns words into shortcuts.
    int held_modifiers = 0;
    // A layer (Alt+Tab switcher, launcher, menu) holds the keyboard: keys go to
    // it, although the active window stays the same.
    bool grabbed = false;
    std::string grabbed_by;       // that layer's namespace
};

// One request. False when Hyprland could not be asked or gave no usable answer.
bool keyboard_state(KeyboardState& state);

// The focused window. False when there is none or Hyprland is not running.
bool active_window(Window& window);
// Looks a window up by address.
bool find_window(std::uintptr_t address, Window& window);
// The same, but tells "no such window" (true, `exists` false) apart from
// "Hyprland could not be asked" (false).
bool lookup_window(std::uintptr_t address, Window& window, bool& exists);
// Gives a window keyboard focus.
bool focus_window(std::uintptr_t address);
// Whether a window of process `pid` whose title starts with `title_prefix` is
// on a workspace that a monitor is showing right now. Returns false, and
// leaves `shown` alone, when Hyprland cannot be asked.
bool window_shown(long pid, const std::string& title_prefix, bool& shown);
// When that window has keyboard focus: gives focus back to the window that
// had it before (going to its workspace) and leaves the mouse pointer where it
// is. Returns false when nothing was done.
bool focus_previous_window(long pid, const std::string& title_prefix);


} // namespace hyprland
} // namespace vtt
