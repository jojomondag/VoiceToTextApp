#include "config_manager.h"
#include <fstream>
#include <filesystem>

namespace vtt {

ConfigManager::ConfigManager() {
    config_path_ = get_default_path();
}

bool ConfigManager::load(const std::string& path) {
    if (!path.empty()) {
        config_path_ = path;
    }

    std::ifstream file(config_path_);
    if (!file.is_open()) {
        // File doesn't exist -- create default config
        config_ = AppConfig{};
        return save();
    }

    try {
        nlohmann::json j;
        file >> j;

        // Use the NLOHMANN macro for most fields
        config_ = j.get<AppConfig>();

        // Handle modifier fields separately (not in NLOHMANN macro)
        if (j.contains("toggle_modifiers"))
            config_.toggle_modifiers = j["toggle_modifiers"].get<unsigned int>();
        if (j.contains("cancel_modifiers"))
            config_.cancel_modifiers = j["cancel_modifiers"].get<unsigned int>();

        return true;
    } catch (const nlohmann::json::exception&) {
        // Malformed JSON -- reset to defaults and overwrite
        config_ = AppConfig{};
        save();
        return false;
    }
}

bool ConfigManager::save(const std::string& path) {
    if (!path.empty()) {
        config_path_ = path;
    }

    // Make sure the parent directory exists
    std::filesystem::path p(config_path_);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    try {
        // Serialize with the NLOHMANN macro
        nlohmann::json j = config_;

        // Add modifier fields that are not part of the macro
        j["toggle_modifiers"] = config_.toggle_modifiers;
        j["cancel_modifiers"] = config_.cancel_modifiers;

        std::ofstream file(config_path_);
        if (!file.is_open()) {
            return false;
        }

        file << j.dump(4) << std::endl;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

AppConfig& ConfigManager::config() {
    return config_;
}

const AppConfig& ConfigManager::config() const {
    return config_;
}

std::string ConfigManager::get_config_path() const {
    return config_path_;
}

std::string ConfigManager::get_default_path() const {
    // Place config.json next to the executable
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    std::filesystem::path p(exe_path);
    p = p.parent_path() / "config.json";
    return p.string();
}

} // namespace vtt
