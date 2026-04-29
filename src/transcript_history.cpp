#include "transcript_history.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vtt {
namespace {

std::string now_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buffer;
}

int estimate_token_count(const std::string& text) {
    int tokens = 0;
    int current_word_chars = 0;

    auto finish_word = [&]() {
        if (current_word_chars == 0) return;
        tokens += std::max(1, (current_word_chars + 5) / 6);
        current_word_chars = 0;
    };

    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '_' || c >= 128) {
            ++current_word_chars;
        } else {
            finish_word();
            if (std::ispunct(c)) {
                ++tokens;
            }
        }
    }

    finish_word();
    return tokens;
}

void append_entry_text(std::ostringstream& oss,
                       const TranscriptHistoryEntry& entry,
                       const std::string& text,
                       int token_count) {
    oss << "[" << entry.timestamp << "]    Profile: " << entry.intent_profile
        << "    Tokens: " << token_count << "\r\n";
    oss << text << "\r\n\r\n";
}

} // namespace

bool TranscriptHistory::initialize(const std::string& path, int max_entries) {
    path_ = path;
    max_entries_ = std::max(10, max_entries);
    return load();
}

void TranscriptHistory::append(const std::string& recorded,
                               const std::string& transcribed,
                               const std::string& ai_language,
                               const std::string& intent_profile) {
    if (recorded.empty() && transcribed.empty() && ai_language.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({
        now_timestamp(),
        recorded,
        transcribed,
        ai_language,
        intent_profile.empty() ? "general" : intent_profile,
        estimate_token_count(recorded),
        estimate_token_count(transcribed),
        estimate_token_count(ai_language),
    });

    if (static_cast<int>(entries_.size()) > max_entries_) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + (entries_.size() - max_entries_));
    }

    save_locked();
}

std::string TranscriptHistory::format_recorded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        append_entry_text(oss, *it, it->recorded, it->recorded_tokens);
    }
    return oss.str();
}

std::string TranscriptHistory::format_transcribed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        append_entry_text(oss, *it, it->transcribed, it->transcribed_tokens);
    }
    return oss.str();
}

std::string TranscriptHistory::format_ai_language() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        append_entry_text(oss, *it, it->ai_language, it->ai_language_tokens);
    }
    return oss.str();
}

bool TranscriptHistory::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();

    std::ifstream file(path_);
    if (!file.is_open()) return true;

    try {
        nlohmann::json json;
        file >> json;
        if (!json.is_array()) return false;

        for (const auto& item : json) {
            TranscriptHistoryEntry entry;
            entry.timestamp = item.value("timestamp", "");
            entry.recorded = item.value("recorded", "");
            entry.transcribed = item.value("transcribed", "");
            entry.ai_language = item.value("ai_language", "");
            entry.intent_profile = item.value("intent_profile", "general");
            entry.recorded_tokens = item.value("recorded_tokens", 0);
            entry.transcribed_tokens = item.value("transcribed_tokens", 0);
            entry.ai_language_tokens = item.value("ai_language_tokens", 0);
            if (entry.recorded_tokens <= 0) {
                entry.recorded_tokens = estimate_token_count(entry.recorded);
            }
            if (entry.transcribed_tokens <= 0) {
                entry.transcribed_tokens = estimate_token_count(entry.transcribed);
            }
            if (entry.ai_language_tokens <= 0) {
                entry.ai_language_tokens = estimate_token_count(entry.ai_language);
            }
            if (!entry.timestamp.empty()) entries_.push_back(entry);
        }
        return true;
    } catch (const nlohmann::json::exception&) {
        entries_.clear();
        return false;
    }
}

bool TranscriptHistory::save_locked() const {
    std::filesystem::path path(path_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    nlohmann::json json = nlohmann::json::array();
    for (const auto& entry : entries_) {
        json.push_back({
            {"timestamp", entry.timestamp},
            {"recorded", entry.recorded},
            {"transcribed", entry.transcribed},
            {"ai_language", entry.ai_language},
            {"intent_profile", entry.intent_profile},
            {"recorded_tokens", entry.recorded_tokens},
            {"transcribed_tokens", entry.transcribed_tokens},
            {"ai_language_tokens", entry.ai_language_tokens},
        });
    }

    std::ofstream file(path_);
    if (!file.is_open()) return false;
    file << json.dump(4) << std::endl;
    return true;
}

} // namespace vtt
