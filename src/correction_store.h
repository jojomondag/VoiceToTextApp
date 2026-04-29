#pragma once

#include <string>
#include <vector>

namespace vtt {

struct CorrectionEntry {
    std::string wrong;
    std::string correct;
    int count = 0;
    std::string last_seen;
};

class CorrectionStore {
public:
    bool initialize(const std::string& path, int max_entries);

    void learn_from(const std::string& raw_text, const std::string& cleaned_text);
    std::string build_prompt_block(int max_prompt_entries, int min_count) const;

private:
    std::string path_;
    int max_entries_ = 200;
    std::vector<CorrectionEntry> entries_;

    bool load();
    bool save() const;
};

} // namespace vtt
