#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "key_codes.h"
#include "win_compat.h"

namespace vtt {

enum class HotkeyAction {
    ToggleRecording,
    CancelRecording,
    SwitchLanguage,
    OpenSettings
};

using HotkeyCallback = std::function<void(HotkeyAction)>;

// Global hotkeys on Linux.
//
// Wayland gives no client a way to grab keys, so the compositor has to do it.
// Under Hyprland every hotkey is registered at runtime as a bind that runs
// `VoiceToText --<command>`; that second process hands the command to the
// running instance over its socket (see main.cpp) and App::handle_command
// acts on it. The binds consume the key, like RegisterHotKey does on Windows,
// and are disabled again when the hotkey is unregistered or the app exits.
//
// On other compositors nothing is registered: bind the same commands
// (`VoiceToText --toggle`, `--cancel`, `--language`) in the compositor config.
class HotkeyManager {
public:
    ~HotkeyManager();

    bool initialize(HWND hwnd);
    void shutdown();

    // Custom hotkey registration (Windows virtual-key code + MOD_* flags)
    bool register_hotkey(HotkeyAction action, unsigned int modifiers,
                         unsigned int vk);
    void unregister_hotkey(HotkeyAction action);

    // Set callback
    void set_callback(HotkeyCallback callback);

    // Hotkeys arrive as commands (App::handle_command), never as WM_HOTKEY.
    void handle_hotkey(int /*id*/) {}

    // Get description of current bindings
    std::string get_binding_description(HotkeyAction action) const;

private:
    struct HotkeyBinding {
        unsigned int modifiers = 0;
        unsigned int vk = 0;
        // Hyprland accepted the bind and it is switched on.
        bool applied = false;
    };

    bool apply_locked(HotkeyAction action, HotkeyBinding& binding);
    void remove_locked(HotkeyAction action, HotkeyBinding& binding);
    void reapply_after_reload();
    void watch_reloads();

    mutable std::mutex mutex_;
    HotkeyCallback callback_;
    // Hotkeys that are registered right now.
    std::map<HotkeyAction, HotkeyBinding> active_;
    bool initialized_ = false;

    std::thread reload_thread_;
    std::atomic<bool> stop_{false};
};

} // namespace vtt
