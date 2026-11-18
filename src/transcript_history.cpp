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

const std::string kPendingText = u8"(V\u00e4ntar p\u00e5 spr\u00e5kmodellen...)";

std::string now_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

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
                       const std::string& entry_text,
                       int display_index,
                       int token_count,
                       bool include_profile) {
    // The language-model columns of an entry still being processed.
    const std::string& text = include_profile && entry.llm_pending && entry_text.empty()
        ? kPendingText : entry_text;
    oss << "[[VTT_ENTRY_BEGIN:" << display_index << "]]";
    oss << "[" << entry.timestamp << "]            ";
    if (include_profile) {
        oss << "Profile: " << entry.intent_profile << "    ";
    }
    oss << "Tokens: " << token_count << "\r\n";
    oss << text << "[[VTT_ENTRY_END:" << display_index << "]]\r\n";
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
    trim_locked();
    save_locked();
}

std::uint64_t TranscriptHistory::append_pending(const std::string& recorded) {
    if (recorded.empty()) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    TranscriptHistoryEntry entry;
    entry.timestamp = now_timestamp();
    entry.recorded = recorded;
    entry.recorded_tokens = estimate_token_count(recorded);
    entry.id = next_id_++;
    entry.llm_pending = true;
    entries_.push_back(std::move(entry));
    const std::uint64_t id = entries_.back().id;
    trim_locked();
    save_locked();
    return id;
}

bool TranscriptHistory::complete(std::uint64_t id,
                                 const std::string& recorded,
                                 const std::string& transcribed,
                                 const std::string& ai_language,
                                 const std::string& intent_profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    TranscriptHistoryEntry* entry = find_locked(id);
    if (!entry) return false;
    if (!recorded.empty()) {
        entry->recorded = recorded;
        entry->recorded_tokens = estimate_token_count(recorded);
    }
    entry->transcribed = transcribed;
    entry->ai_language = ai_language;
    entry->intent_profile = intent_profile.empty() ? "general" : intent_profile;
    entry->transcribed_tokens = estimate_token_count(transcribed);
    entry->ai_language_tokens = estimate_token_count(ai_language);
    entry->llm_pending = false;
    save_locked();
    return true;
}

int TranscriptHistory::mark_failed(std::uint64_t id, int max_attempts) {
    std::lock_guard<std::mutex> lock(mutex_);
    TranscriptHistoryEntry* entry = find_locked(id);
    if (!entry || !entry->llm_pending) return -1;
    ++entry->llm_attempts;
    if (entry->llm_attempts >= max_attempts) {
        // Give up: keep what the user said, as a failed cleanup always did.
        entry->llm_pending = false;
        if (entry->transcribed.empty()) {
            entry->transcribed = entry->recorded;
            entry->transcribed_tokens = entry->recorded_tokens;
        }
    }
    save_locked();
    return entry->llm_attempts;
}

bool TranscriptHistory::replace_recorded(std::uint64_t id, const std::string& recorded) {
    if (recorded.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    TranscriptHistoryEntry* entry = find_locked(id);
    if (!entry || entry->recorded == recorded) return false;
    if (!entry->llm_pending && entry->ai_language.empty() && entry->transcribed == entry->recorded) {
        entry->transcribed = recorded;
        entry->transcribed_tokens = estimate_token_count(recorded);
    }
    entry->recorded = recorded;
    entry->recorded_tokens = estimate_token_count(recorded);
    save_locked();
    return true;
}

void TranscriptHistory::trim_locked() {
    if (static_cast<int>(entries_.size()) > max_entries_) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + (entries_.size() - max_entries_));
    }
}

TranscriptHistoryEntry* TranscriptHistory::find_locked(std::uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& entry : entries_) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

void TranscriptHistory::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    save_locked();
}

std::vector<TranscriptHistoryEntry> TranscriptHistory::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::string TranscriptHistory::format_recorded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    int display_index = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        append_entry_text(oss, *it, it->recorded, display_index++,
                          it->recorded_tokens, false);
    }
    return oss.str();
}

std::string TranscriptHistory::format_transcribed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    int display_index = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        append_entry_text(oss, *it, it->transcribed, display_index++,
                          it->transcribed_tokens, true);
    }
    return oss.str();
}

std::string TranscriptHistory::format_ai_language() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    int display_index = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        append_entry_text(oss, *it, it->ai_language, display_index++,
                          it->ai_language_tokens, true);
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
            entry.id = item.value("id", static_cast<std::uint64_t>(0));
            entry.llm_pending = item.value("llm_pending", false);
            entry.llm_attempts = item.value("llm_attempts", 0);
            if (entry.id >= next_id_) next_id_ = entry.id + 1;
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
        nlohmann::json item = {
            {"timestamp", entry.timestamp},
            {"recorded", entry.recorded},
            {"transcribed", entry.transcribed},
            {"ai_language", entry.ai_language},
            {"intent_profile", entry.intent_profile},
            {"recorded_tokens", entry.recorded_tokens},
            {"transcribed_tokens", entry.transcribed_tokens},
            {"ai_language_tokens", entry.ai_language_tokens},
        };
        // Only entries made by the background language-model step carry
        // these keys, so older entries are written exactly as before.
        if (entry.id != 0) item["id"] = entry.id;
        if (entry.llm_pending) item["llm_pending"] = true;
        if (entry.llm_attempts > 0) item["llm_attempts"] = entry.llm_attempts;
        json.push_back(std::move(item));
    }

    // Write a temporary file and rename it over the old one, so an exit in
    // the middle of a save never leaves a torn file (which load() would
    // reject, and the next save would then replace with an empty history).
    const std::string temp_path = path_ + ".tmp";
    {
        std::ofstream file(temp_path);
        if (!file.is_open()) return false;
        file << json.dump(4) << std::endl;
        file.flush();
        if (!file.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp_path, path_, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }
    return true;
}

} // namespace vtt
