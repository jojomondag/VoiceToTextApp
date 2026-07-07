#include "config_manager.h"
#include <cctype>
#include <fstream>
#include <filesystem>
#include <set>

namespace vtt {
namespace {

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0);
    if (length <= 0) return {};

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string utf8_from_wide(const std::wstring& text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

std::string windows_1252_from_wide(const std::wstring& text) {
    if (text.empty()) return {};

    BOOL used_default = FALSE;
    int length = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, &used_default);
    if (length <= 0 || used_default) return {};

    std::string bytes(static_cast<size_t>(length), '\0');
    used_default = FALSE;
    WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, text.data(),
                        static_cast<int>(text.size()), bytes.data(), length,
                        nullptr, &used_default);
    return used_default ? std::string() : bytes;
}

bool looks_like_mojibake(const std::string& text) {
    return text.find("\xC3\x83") != std::string::npos ||
           text.find("\xC3\x82") != std::string::npos ||
           text.find("\xC2\xA5") != std::string::npos;
}

bool repair_mojibake(std::string& text) {
    bool changed = false;

    for (int pass = 0; pass < 3 && looks_like_mojibake(text); ++pass) {
        std::wstring wide = wide_from_utf8(text);
        std::string bytes = windows_1252_from_wide(wide);
        std::wstring repaired_wide = wide_from_utf8(bytes);
        std::string repaired = utf8_from_wide(repaired_wide);

        if (repaired.empty() || repaired == text) break;
        text = repaired;
        changed = true;
    }

    return changed;
}

bool repair_config_text(AppConfig& config) {
    bool changed = false;
    changed |= repair_mojibake(config.llm_pre_prompt);
    changed |= repair_mojibake(config.llm_ai_language_prompt);
    changed |= repair_mojibake(config.llm_intent_detection_prompt);
    changed |= repair_mojibake(config.llm_profile_transform_prompt);
    return changed;
}

std::string trim_ascii(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool normalize_profiles(AppConfig& config) {
    bool changed = false;
    std::set<std::string> seen;
    std::vector<ProfileDefinition> normalized;

    for (auto profile : config.llm_profiles) {
        profile.name = trim_ascii(profile.name);
        if (profile.name.empty() || seen.count(profile.name) != 0) {
            changed = true;
            continue;
        }
        if (profile.instructions.empty()) {
            profile.instructions = u8"Beskriv hur den h\u00E4r profilen ska skriva om texten.";
            changed = true;
        }
        seen.insert(profile.name);
        normalized.push_back(std::move(profile));
    }

    AppConfig defaults;
    for (const auto& profile : defaults.llm_profiles) {
        if (seen.count(profile.name) == 0) {
            normalized.push_back(profile);
            seen.insert(profile.name);
            changed = true;
        }
    }

    if (normalized.empty()) {
        normalized = defaults.llm_profiles;
        changed = true;
    }

    if (changed) {
        config.llm_profiles = std::move(normalized);
    }
    return changed;
}

} // namespace

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

        bool changed = repair_config_text(config_);
        changed |= normalize_profiles(config_);
        if (changed) {
            save();
        }

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
