#pragma once

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
};

class TranscriptHistory {
public:
    bool initialize(const std::string& path, int max_entries);
    void append(const std::string& recorded,
                const std::string& transcribed,
                const std::string& ai_language,
                const std::string& intent_profile);

    std::string format_recorded() const;
    std::string format_transcribed() const;
    std::string format_ai_language() const;

private:
    std::string path_;
    int max_entries_ = 100;
    mutable std::mutex mutex_;
    std::vector<TranscriptHistoryEntry> entries_;

    bool load();
    bool save_locked() const;
};

} // namespace vtt
