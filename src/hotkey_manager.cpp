#include "hotkey_manager.h"

namespace vtt {

HotkeyManager::~HotkeyManager() {
    shutdown();
}

bool HotkeyManager::initialize(HWND hwnd) {
    hwnd_ = hwnd;
    return hwnd_ != nullptr;
}

void HotkeyManager::shutdown() {
    if (!hwnd_) return;

    for (auto& [id, binding] : hotkey_map_) {
        ::UnregisterHotKey(hwnd_, id);
    }
    hotkey_map_.clear();
    hwnd_ = nullptr;
}

bool HotkeyManager::register_hotkey(HotkeyAction action, UINT modifiers, UINT vk) {
    if (!hwnd_) return false;

    // Remove any existing binding for this action
    unregister_hotkey(action);

    int id = next_id_++;
    if (!::RegisterHotKey(hwnd_, id, modifiers, vk)) {
        return false;
    }

    hotkey_map_[id] = HotkeyBinding{action, modifiers, vk};
    return true;
}

void HotkeyManager::unregister_hotkey(HotkeyAction action) {
    if (!hwnd_) return;

    for (auto it = hotkey_map_.begin(); it != hotkey_map_.end(); ++it) {
        if (it->second.action == action) {
            ::UnregisterHotKey(hwnd_, it->first);
            hotkey_map_.erase(it);
            return;
        }
    }
}

void HotkeyManager::set_callback(HotkeyCallback callback) {
    callback_ = std::move(callback);
}

void HotkeyManager::handle_hotkey(int id) {
    auto it = hotkey_map_.find(id);
    if (it != hotkey_map_.end() && callback_) {
        callback_(it->second.action);
    }
}

static std::string vk_to_string(UINT vk) {
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

std::string HotkeyManager::get_binding_description(HotkeyAction action) const {
    for (auto& [id, binding] : hotkey_map_) {
        if (binding.action == action) {
            std::string desc;
            if (binding.modifiers & MOD_CONTROL) desc += "Ctrl+";
            if (binding.modifiers & MOD_SHIFT)   desc += "Shift+";
            if (binding.modifiers & MOD_ALT)     desc += "Alt+";
            if (binding.modifiers & MOD_WIN)     desc += "Win+";
            desc += vk_to_string(binding.vk);
            return desc;
        }
    }
    return "Unbound";
}

} // namespace vtt
