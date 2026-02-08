#pragma once
#include <functional>
#include <unordered_map>
#include <string>
#include <windows.h>

namespace vtt {

enum class HotkeyAction {
    ToggleRecording,
    CancelRecording,
    SwitchLanguage,
    OpenSettings
};

using HotkeyCallback = std::function<void(HotkeyAction)>;

class HotkeyManager {
public:
    HotkeyManager();
    ~HotkeyManager();

    bool initialize(HWND hwnd);
    void shutdown();

    // Register default hotkeys
    bool register_defaults();

    // Custom hotkey registration
    bool register_hotkey(HotkeyAction action, UINT modifiers, UINT vk);
    void unregister_hotkey(HotkeyAction action);

    // Set callback
    void set_callback(HotkeyCallback callback);

    // Call this from WndProc when WM_HOTKEY is received
    void handle_hotkey(int id);

    // Get description of current bindings
    std::string get_binding_description(HotkeyAction action) const;

private:
    struct HotkeyBinding {
        HotkeyAction action;
        UINT modifiers;
        UINT vk;
    };

    HWND hwnd_ = nullptr;
    HotkeyCallback callback_;
    std::unordered_map<int, HotkeyBinding> hotkey_map_;
    int next_id_ = 1;
};

} // namespace vtt
