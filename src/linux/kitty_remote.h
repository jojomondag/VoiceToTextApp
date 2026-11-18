#pragma once
// kitty's remote control, spoken over its unix socket. A kitty that listens
// on one (Omarchy's /etc/xdg/kitty/kitty.conf sets allow_remote_control
// socket-only and listen_on for every kitty) tells the programs it runs where
// it is (KITTY_LISTEN_ON). Text sent there goes into one particular kitty
// window whether or not it has keyboard focus, as the exact characters: no
// key presses, keymaps or held modifiers are involved.

#include <string>

namespace vtt {
namespace kitty_remote {

struct Target {
    std::string socket;   // path of the unix socket
    int window_id = 0;    // kitty's id of the window inside that process
    bool valid() const { return !socket.empty() && window_id > 0; }
};

// For the kitty process `pid`: its remote-control socket and the window that is
// active in it right now (the one the user is typing in when it has focus).
// False when the process is not kitty or offers no socket.
bool find(long pid, Target& target);

// Writes text to the program in the window, as if typed. False when kitty
// could not be reached (the window was closed, remote control is off).
bool send_text(const Target& target, const std::string& utf8);

// Presses a key `count` times (kitty's key names: "backspace", "enter"),
// encoded the way the program in the window asked for.
bool send_key(const Target& target, const std::string& key, int count = 1);

} // namespace kitty_remote
} // namespace vtt
