#include "hotkey_manager.h"

#include "hyprland_ipc.h"
#include "log.h"
#include "platform.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vtt {
namespace {

// Lua globals in Hyprland: the handles of the binds this app created, keyed by
// command name, and what each one was bound to. Only these handles are ever
// switched on or off, so a bind the user made on the same keys is never
// touched. A bind that is no longer wanted is disabled, not removed, and the
// same handle is enabled again the next time (the cancel key every recording,
// the others every start), so binds do not pile up in Hyprland.
const char* const LUA_TABLE = "__voicetotext_binds";
const char* const LUA_SPECS = "__voicetotext_bind_specs";

// The binds must not react to the keys this app sends itself: Hyprland
// matches binds by keycode against the user's layout, whatever the virtual
// keyboard's own keymap says. (wtype, used by earlier versions, typed its
// first character on the keycode of Escape, which cancelled the recording.)
const char* const LUA_IGNORED_DEVICES =
    "device = { inclusive = false, list = { \"hl-virtual-keyboard-voicetotext\", "
    "\"hl-virtual-keyboard-voicetotext-1\", \"hl-virtual-keyboard-wtype\" } }";

const char* command_name(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::ToggleRecording: return "toggle";
        case HotkeyAction::CancelRecording: return "cancel";
        case HotkeyAction::SwitchLanguage:  return "language";
        case HotkeyAction::OpenSettings:    return "settings";
    }
    return "toggle";
}

const char* command_description(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::ToggleRecording: return "VoiceToText: start/stop recording";
        case HotkeyAction::CancelRecording: return "VoiceToText: cancel recording";
        case HotkeyAction::SwitchLanguage:  return "VoiceToText: switch language";
        case HotkeyAction::OpenSettings:    return "VoiceToText: open settings";
    }
    return "VoiceToText";
}

std::string vk_to_string(unsigned int vk) {
    switch (vk) {
        case VK_SPACE:  return "Space";
        case VK_ESCAPE: return "Escape";
        case VK_RETURN: return "Enter";
        case VK_TAB:    return "Tab";
        case VK_BACK:   return "Backspace";
        case VK_DELETE: return "Delete";
        default: {
            if (vk >= 'A' && vk <= 'Z')
                return std::string(1, static_cast<char>(vk));
            if (vk >= '0' && vk <= '9')
                return std::string(1, static_cast<char>(vk));
            if (vk >= VK_F1 && vk <= VK_F24)
                return "F" + std::to_string(vk - VK_F1 + 1);
            return "Key(" + std::to_string(vk) + ")";
        }
    }
}

// Hyprland's name for a Windows virtual key; empty when there is none.
std::string hyprland_key(unsigned int vk) {
    switch (vk) {
        case VK_SPACE:  return "SPACE";
        case VK_ESCAPE: return "ESCAPE";
        case VK_RETURN: return "RETURN";
        case VK_TAB:    return "TAB";
        case VK_BACK:   return "BACKSPACE";
        case VK_DELETE: return "DELETE";
        default: break;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    return {};
}

// "CTRL + SHIFT + SPACE" (Lua config) or "CTRL SHIFT,SPACE" (keyword config).
std::string key_spec(unsigned int modifiers, const std::string& key, bool lua) {
    std::string mods;
    auto add = [&](const char* name) {
        if (!mods.empty()) mods += lua ? " + " : " ";
        mods += name;
    };
    if (modifiers & MOD_CONTROL) add("CTRL");
    if (modifiers & MOD_SHIFT)   add("SHIFT");
    if (modifiers & MOD_ALT)     add("ALT");
    if (modifiers & MOD_WIN)     add("SUPER");

    if (lua) return mods.empty() ? key : mods + " + " + key;
    return mods + "," + key;
}

// The command a bind runs. Hyprland passes it to `sh -c`, so the path is
// single-quoted and the rest holds nothing a shell would expand.
std::string exec_command(HotkeyAction action) {
    std::string exe = platform::exe_path().string();
    std::string quoted = "'";
    for (char c : exe) {
        if (c == '\'') quoted += "'\\''";
        else quoted.push_back(c);
    }
    quoted += "' --";
    quoted += command_name(action);
    return quoted;
}

size_t count_occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

// Hyprland 0.56 segfaults (and takes the session with it) when one `eval`
// binds the same key more than once. Every request this file builds holds at
// most one bind call; this refuses anything else before it is sent.
bool send(const std::string& command) {
    if (count_occurrences(command, "hl.bind(") +
        count_occurrences(command, "hl.unbind(") > 1) {
        vtlog("hotkeys: refused a request with more than one bind call");
        return false;
    }

    std::string reply;
    if (!hyprland::request(command, reply)) return false;

    size_t end = reply.find_last_not_of(" \t\r\n");
    reply = end == std::string::npos ? std::string() : reply.substr(0, end + 1);
    if (reply != "ok") {
        vtlog("hotkeys: Hyprland answered \"%s\"", reply.c_str());
        return false;
    }
    return true;
}

} // namespace

HotkeyManager::~HotkeyManager() {
    shutdown();
}

bool HotkeyManager::initialize(HWND /*hwnd*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;
    initialized_ = true;

    if (!hyprland::available()) {
        vtlog("hotkeys: not running under Hyprland; bind `VoiceToText --toggle` "
              "in your compositor to get a global hotkey");
        return true;
    }

    if (hyprland::uses_lua()) {
        // Binds a previous run left switched on (it crashed or was killed).
        std::string table = LUA_TABLE;
        send("eval local t = " + table + "; "
             "if t then for _, b in pairs(t) do "
             "pcall(function() b:set_enabled(false) end) end end");
    }

    stop_ = false;
    reload_thread_ = std::thread([this]() { watch_reloads(); });
    return true;
}

void HotkeyManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return;
        initialized_ = false;
        for (auto& [action, binding] : active_) {
            remove_locked(action, binding);
        }
        active_.clear();
    }

    stop_ = true;
    if (reload_thread_.joinable()) {
        reload_thread_.join();
    }
}

bool HotkeyManager::register_hotkey(HotkeyAction action, unsigned int modifiers,
                                    unsigned int vk) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    // Remove any existing binding for this action
    auto existing = active_.find(action);
    if (existing != active_.end()) {
        if (existing->second.modifiers == modifiers && existing->second.vk == vk) {
            return true;
        }
        remove_locked(action, existing->second);
        active_.erase(existing);
    }

    HotkeyBinding binding;
    binding.modifiers = modifiers;
    binding.vk = vk;

    bool ok = apply_locked(action, binding);
    active_[action] = binding;
    return ok;
}

void HotkeyManager::unregister_hotkey(HotkeyAction action) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_.find(action);
    if (it == active_.end()) return;

    remove_locked(action, it->second);
    active_.erase(it);
}

void HotkeyManager::set_callback(HotkeyCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

std::string HotkeyManager::get_binding_description(HotkeyAction action) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_.find(action);
    if (it == active_.end()) return "Unbound";

    std::string desc;
    if (it->second.modifiers & MOD_CONTROL) desc += "Ctrl+";
    if (it->second.modifiers & MOD_SHIFT)   desc += "Shift+";
    if (it->second.modifiers & MOD_ALT)     desc += "Alt+";
    if (it->second.modifiers & MOD_WIN)     desc += "Super+";
    desc += vk_to_string(it->second.vk);
    return desc;
}

bool HotkeyManager::apply_locked(HotkeyAction action, HotkeyBinding& binding) {
    if (!hyprland::available()) return false;

    std::string key = hyprland_key(binding.vk);
    if (key.empty()) {
        vtlog("hotkeys: no Hyprland key name for virtual key %u", binding.vk);
        return false;
    }

    std::string name = command_name(action);
    bool ok = false;

    if (hyprland::uses_lua()) {
        std::string table = LUA_TABLE;
        std::string specs = LUA_SPECS;
        std::string id = hyprland::lua_quote(name);
        std::string keys = key_spec(binding.modifiers, key, true);
        std::string command = exec_command(action);
        std::string spec = hyprland::lua_quote(keys + " -> " + command + " | " +
                                               LUA_IGNORED_DEVICES);

        // One request, at most one bind call: switch the existing bind back on
        // when it is for the same keys and command, otherwise retire it and
        // bind anew.
        ok = send(
            "eval local t = " + table + " or {}; " + table + " = t; "
            "local s = " + specs + " or {}; " + specs + " = s; "
            "local old = t[" + id + "]; "
            "if old and s[" + id + "] == " + spec + " then old:set_enabled(true) "
            "else "
            "if old then pcall(function() old:set_enabled(false) end) end; "
            "t[" + id + "] = hl.bind(" + hyprland::lua_quote(keys) +
            ", hl.dsp.exec_cmd(" + hyprland::lua_quote(command) + "), "
            "{ description = " +
            hyprland::lua_quote(command_description(action)) + ", " +
            LUA_IGNORED_DEVICES + " }); "
            "s[" + id + "] = " + spec + " end");
        binding.applied = ok;
    } else {
        ok = send("keyword bind " + key_spec(binding.modifiers, key, false) +
                  ",exec," + exec_command(action));
        binding.applied = ok;
    }

    vtlog("hotkeys: %s on %s %s", name.c_str(),
          key_spec(binding.modifiers, key, true).c_str(),
          ok ? "registered" : "FAILED");
    return ok;
}

void HotkeyManager::remove_locked(HotkeyAction action, HotkeyBinding& binding) {
    if (!hyprland::available()) return;

    std::string key = hyprland_key(binding.vk);
    if (key.empty()) return;

    if (!binding.applied) return;
    binding.applied = false;

    if (hyprland::uses_lua()) {
        std::string table = LUA_TABLE;
        std::string id = hyprland::lua_quote(command_name(action));
        send("eval local t = " + table + "; if t and t[" + id + "] then "
             "pcall(function() t[" + id + "]:set_enabled(false) end) end");
    } else {
        send("keyword unbind " + key_spec(binding.modifiers, key, false));
    }
}

void HotkeyManager::reapply_after_reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;

    // The reload replaced Hyprland's Lua state: every bind object is gone.
    vtlog("hotkeys: Hyprland config reloaded, binding again");
    for (auto& [action, binding] : active_) {
        binding.applied = false;
        apply_locked(action, binding);
    }
}

// Follows Hyprland's event socket. A config reload drops every runtime bind,
// so they are applied again when `configreloaded` shows up.
void HotkeyManager::watch_reloads() {
    std::string path = hyprland::event_socket_path();
    auto wait = [this](int milliseconds) {
        for (int waited = 0; waited < milliseconds && !stop_.load(); waited += 50) {
            platform::sleep_ms(50);
        }
    };

    while (!stop_.load()) {
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        if (path.empty() || path.size() >= sizeof(address.sun_path)) return;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0 &&
            connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            std::string pending;
            char buffer[4096];
            while (!stop_.load()) {
                pollfd poll_fd = {fd, POLLIN, 0};
                int ready = poll(&poll_fd, 1, 200);
                if (ready < 0) break;
                if (ready == 0) continue;

                ssize_t n = read(fd, buffer, sizeof(buffer));
                if (n <= 0) break;
                pending.append(buffer, static_cast<size_t>(n));

                size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (line.rfind("configreloaded>>", 0) == 0) {
                        // Let the reload settle before binding again.
                        wait(250);
                        if (!stop_.load()) reapply_after_reload();
                    }
                }
            }
        }
        if (fd >= 0) close(fd);
        wait(2000);
    }
}

} // namespace vtt
