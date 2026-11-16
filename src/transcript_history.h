#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vtt {

struct TranscriptHistoryEntry {
    std::string timestamp;
    std::string recorded;
    std::string transcribed;
    std::string ai_language;
    std::string intent_profile = "general";
    int recorded_tokens = 0;
    int transcribed_tokens = 0;
    int ai_language_tokens = 0;
    // Set only on entries saved before their language-model step finished.
    // Entries without them (all older ones) are written back unchanged.
    std::uint64_t id = 0;
    bool llm_pending = false;
    int llm_attempts = 0;
};

class TranscriptHistory {
public:
    bool initialize(const std::string& path, int max_entries);
    void append(const std::string& recorded,
                const std::string& transcribed,
                const std::string& ai_language,
                const std::string& intent_profile);
    // Saves the recorded text at once, marked as waiting for the language
    // model. Returns the entry id (0 when nothing was saved).
    std::uint64_t append_pending(const std::string& recorded);
    // Fills a pending entry with the language-model results and clears the
    // pending mark. False when the entry is gone (cleared or trimmed).
    bool complete(std::uint64_t id,
                  const std::string& recorded,
                  const std::string& transcribed,
                  const std::string& ai_language,
                  const std::string& intent_profile);
    // Counts a failed language-model attempt. At `max_attempts` the entry
    // stops waiting and keeps the recorded text as its transcript. Returns
    // the attempt count, or -1 when the entry is gone or not pending.
    int mark_failed(std::uint64_t id, int max_attempts);
    void clear();

    // Snapshot of all entries, oldest first.
    std::vector<TranscriptHistoryEntry> entries() const;

    std::string format_recorded() const;
    std::string format_transcribed() const;
    std::string format_ai_language() const;

private:
    std::string path_;
    int max_entries_ = 100;
    std::uint64_t next_id_ = 1;
    mutable std::mutex mutex_;
    std::vector<TranscriptHistoryEntry> entries_;

    bool load();
    bool save_locked() const;
    void trim_locked();
    TranscriptHistoryEntry* find_locked(std::uint64_t id);
};

} // namespace vtt
