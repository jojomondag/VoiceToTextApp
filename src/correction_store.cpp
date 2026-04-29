#include "correction_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace vtt {
namespace {

std::string now_isoish() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buffer;
}

bool is_edge_punctuation(unsigned char c) {
    return std::ispunct(c) && c != '.' && c != '_' && c != '-' && c != '+';
}

std::string trim_token_edges(const std::string& token) {
    auto begin = token.begin();
    auto end = token.end();

    while (begin != end && is_edge_punctuation(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end && is_edge_punctuation(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::vector<std::string> tokenize(const std::string& text) {
    std::istringstream iss(text);
    std::vector<std::string> tokens;
    std::string token;

    while (iss >> token) {
        token = trim_token_edges(token);
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

std::string normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '_' || c == '+' || c == '#') {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

std::string join_tokens(const std::vector<std::string>& tokens,
                        size_t begin,
                        size_t end) {
    std::ostringstream oss;
    for (size_t i = begin; i < end; ++i) {
        if (i > begin) oss << ' ';
        oss << tokens[i];
    }
    return oss.str();
}

bool is_filler_token(const std::string& token) {
    static const std::unordered_set<std::string> fillers = {
        "alltsa", "assa", "assaa", "typ", "liksom", "eh", "ehm", "um", "uh",
        "bara"
    };
    return fillers.find(normalize(token)) != fillers.end() ||
           fillers.find(token) != fillers.end();
}

void trim_filler_edges(const std::vector<std::string>& tokens,
                       size_t& begin,
                       size_t& end) {
    while (begin < end && is_filler_token(tokens[begin])) ++begin;
    while (begin < end && is_filler_token(tokens[end - 1])) --end;
}

std::vector<std::pair<size_t, size_t>> lcs_matches(
    const std::vector<std::string>& raw,
    const std::vector<std::string>& cleaned) {
    const size_t n = raw.size();
    const size_t m = cleaned.size();
    std::vector<int> dp((n + 1) * (m + 1), 0);

    auto at = [&](size_t i, size_t j) -> int& {
        return dp[i * (m + 1) + j];
    };

    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            if (normalize(raw[i]) == normalize(cleaned[j])) {
                at(i, j) = at(i + 1, j + 1) + 1;
            } else {
                at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
            }
        }
    }

    std::vector<std::pair<size_t, size_t>> matches;
    size_t i = 0;
    size_t j = 0;
    while (i < n && j < m) {
        if (normalize(raw[i]) == normalize(cleaned[j])) {
            matches.emplace_back(i, j);
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;
        } else {
            ++j;
        }
    }
    return matches;
}

bool likely_useful_correction(const std::string& wrong,
                              const std::string& correct,
                              size_t wrong_tokens,
                              size_t correct_tokens) {
    if (wrong.empty() || correct.empty()) return false;
    if (wrong == correct || normalize(wrong) == normalize(correct)) return false;
    if (wrong_tokens > 6 || correct_tokens > 6) return false;
    if (wrong.size() > 90 || correct.size() > 90) return false;
    if (wrong.size() < 2 || correct.size() < 2) return false;
    return true;
}

} // namespace

bool CorrectionStore::initialize(const std::string& path, int max_entries) {
    path_ = path;
    max_entries_ = std::max(10, max_entries);
    return load();
}

void CorrectionStore::learn_from(const std::string& raw_text,
                                 const std::string& cleaned_text) {
    auto raw = tokenize(raw_text);
    auto cleaned = tokenize(cleaned_text);
    if (raw.empty() || cleaned.empty()) return;

    auto matches = lcs_matches(raw, cleaned);
    size_t raw_prev = 0;
    size_t clean_prev = 0;
    std::string seen_at = now_isoish();
    bool changed = false;

    matches.emplace_back(raw.size(), cleaned.size());
    for (const auto& match : matches) {
        size_t raw_begin = raw_prev;
        size_t raw_end = match.first;
        size_t clean_begin = clean_prev;
        size_t clean_end = match.second;

        trim_filler_edges(raw, raw_begin, raw_end);
        trim_filler_edges(cleaned, clean_begin, clean_end);

        std::string wrong = join_tokens(raw, raw_begin, raw_end);
        std::string correct = join_tokens(cleaned, clean_begin, clean_end);

        if (likely_useful_correction(wrong, correct,
                                     raw_end - raw_begin,
                                     clean_end - clean_begin)) {
            auto it = std::find_if(entries_.begin(), entries_.end(),
                                   [&](const CorrectionEntry& entry) {
                return normalize(entry.wrong) == normalize(wrong) &&
                       entry.correct == correct;
            });

            if (it == entries_.end()) {
                entries_.push_back({wrong, correct, 1, seen_at});
            } else {
                it->count += 1;
                it->last_seen = seen_at;
            }
            changed = true;
        }

        raw_prev = match.first + 1;
        clean_prev = match.second + 1;
    }

    if (!changed) return;

    std::sort(entries_.begin(), entries_.end(),
              [](const CorrectionEntry& a, const CorrectionEntry& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.last_seen > b.last_seen;
    });

    if (static_cast<int>(entries_.size()) > max_entries_) {
        entries_.resize(static_cast<size_t>(max_entries_));
    }

    save();
}

std::string CorrectionStore::build_prompt_block(int max_prompt_entries,
                                                int min_count) const {
    if (entries_.empty() || max_prompt_entries <= 0) return {};

    std::ostringstream oss;
    int added = 0;
    oss << "Kanda aterkommande transkriberingskorrigeringar. Anvand dem bara "
           "nar sammanhanget passar:\n";

    for (const auto& entry : entries_) {
        if (entry.count < min_count) continue;
        oss << "- \"" << entry.wrong << "\" -> \"" << entry.correct
            << "\" (seen " << entry.count << "x)\n";
        if (++added >= max_prompt_entries) break;
    }

    if (added == 0) return {};
    return oss.str();
}

bool CorrectionStore::load() {
    entries_.clear();
    if (path_.empty()) return false;

    std::ifstream file(path_);
    if (!file.is_open()) return true;

    try {
        nlohmann::json json;
        file >> json;
        if (!json.is_array()) return false;

        for (const auto& item : json) {
            CorrectionEntry entry;
            entry.wrong = item.value("wrong", "");
            entry.correct = item.value("correct", "");
            entry.count = item.value("count", 0);
            entry.last_seen = item.value("last_seen", "");

            if (!entry.wrong.empty() && !entry.correct.empty() && entry.count > 0) {
                entries_.push_back(entry);
            }
        }
        return true;
    } catch (const nlohmann::json::exception&) {
        entries_.clear();
        return false;
    }
}

bool CorrectionStore::save() const {
    if (path_.empty()) return false;

    std::filesystem::path path(path_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    nlohmann::json json = nlohmann::json::array();
    for (const auto& entry : entries_) {
        json.push_back({
            {"wrong", entry.wrong},
            {"correct", entry.correct},
            {"count", entry.count},
            {"last_seen", entry.last_seen},
        });
    }

    std::ofstream file(path_);
    if (!file.is_open()) return false;
    file << json.dump(4) << std::endl;
    return true;
}

} // namespace vtt
