#include "live_dictation_buffer.h"

#include <algorithm>
#include <cwctype>
#include <numeric>
#include <utility>
#include <vector>

namespace vtt {
namespace {

bool is_space(wchar_t value) {
    return std::iswspace(value) != 0 || value == L'\u00a0' ||
           (value >= L'\u2000' && value <= L'\u200a') ||
           value == L'\u2028' || value == L'\u2029' || value == L'\u202f';
}

bool is_punctuation(wchar_t value) {
    return std::iswpunct(value) != 0 ||
           (value >= L'\u2000' && value <= L'\u206f') ||
           (value >= L'\u3001' && value <= L'\u303f');
}

std::wstring trim(const std::wstring& value) {
    std::size_t start = 0;
    std::size_t end = value.size();
    while (start < end && is_space(value[start])) ++start;
    while (end > start && is_space(value[end - 1])) --end;
    return value.substr(start, end - start);
}

struct Word {
    std::size_t start;
    std::wstring key;
};

std::vector<Word> words(const std::wstring& text) {
    std::vector<Word> result;
    for (std::size_t index = 0; index < text.size();) {
        while (index < text.size() && is_space(text[index])) ++index;
        if (index == text.size()) break;
        Word word{index, {}};
        while (index < text.size() && !is_space(text[index])) {
            wchar_t value = text[index++];
            if (is_punctuation(value)) continue;
            // Swedish casing must also work in the default C locale.
            if (value >= L'\u00c0' && value <= L'\u00de' && value != L'\u00d7') {
                value = static_cast<wchar_t>(value + 0x20);
            } else {
                value = static_cast<wchar_t>(std::towlower(value));
            }
            word.key.push_back(value);
        }
        result.push_back(std::move(word));
    }
    return result;
}

// Match already delivered words to the revised hypothesis. This reproduces
// TeachersLittleHelper's DictationTextInsertion alignment with O(n) memory.
std::size_t map_sealed_words(const std::wstring& sealed,
                             const std::vector<Word>& current) {
    const auto previous = words(sealed);
    if (previous.empty()) return 0;

    std::vector<std::size_t> costs(current.size() + 1);
    std::iota(costs.begin(), costs.end(), std::size_t{0});
    for (std::size_t old = 1; old <= previous.size(); ++old) {
        std::size_t diagonal = costs[0];
        costs[0] = old;
        for (std::size_t now = 1; now <= current.size(); ++now) {
            const auto above = costs[now];
            const auto substitution = diagonal +
                (previous[old - 1].key == current[now - 1].key ? 0 : 1);
            costs[now] = (std::min)({substitution, above + 1, costs[now - 1] + 1});
            diagonal = above;
        }
    }

    // Prefer appended words on ties: a newly spoken repeated word remains new.
    auto consumed = current.size();
    while (consumed > 0 && costs[consumed] == costs[consumed - 1] + 1) --consumed;
    return consumed;
}

} // namespace

void LiveDictationBuffer::reset() {
    *this = LiveDictationBuffer{};
}

void LiveDictationBuffer::seal_insertion() {
    if (!owned_text_) return;
    sealed_hypothesis_ = hypothesis_;
    owned_text_.reset();
}

void LiveDictationBuffer::observe_editor(const std::wstring& document,
                                        std::size_t selection_start,
                                        std::size_t selection_end) {
    if (has_observation_ &&
        (document != observed_document_ || selection_start != observed_selection_start_ ||
         selection_end != observed_selection_end_)) {
        seal_insertion();
    }
    observed_document_ = document;
    observed_selection_start_ = selection_start;
    observed_selection_end_ = selection_end;
    has_observation_ = true;
}

std::optional<LiveDictationBuffer::Replacement> LiveDictationBuffer::apply(
    int utterance_id, const std::wstring& hypothesis, bool is_final,
    const std::wstring& document, std::size_t selection_start,
    std::size_t selection_end) {
    observe_editor(document, selection_start, selection_end);
    if (utterance_id < utterance_id_ ||
        (utterance_id == utterance_id_ && utterance_final_)) return std::nullopt;

    if (utterance_id != utterance_id_) {
        utterance_id_ = utterance_id;
        utterance_final_ = false;
        hypothesis_.clear();
        sealed_hypothesis_.clear();
        owned_text_.reset();
    }

    const auto text = trim(hypothesis);
    std::optional<Replacement> replacement;
    if (text != hypothesis_) {
        if (owned_text_ &&
            (selection_start != caret_after_insertion_ ||
             selection_end != caret_after_insertion_ ||
             insertion_start_ > document.size() ||
             owned_text_->size() > document.size() - insertion_start_ ||
             document.compare(insertion_start_, owned_text_->size(), *owned_text_) != 0)) {
            seal_insertion();
        }

        const auto current_words = words(text);
        const auto consumed = map_sealed_words(sealed_hypothesis_, current_words);
        const auto remaining = consumed < current_words.size()
            ? text.substr(current_words[consumed].start) : std::wstring{};
        const auto selected_start = (std::min)((std::min)(selection_start, selection_end), document.size());
        const auto selected_end = (std::min)((std::max)(selection_start, selection_end), document.size());
        const bool replace_selection = !owned_text_ && !remaining.empty() && selected_end > selected_start;
        const auto start = owned_text_ ? insertion_start_ : replace_selection
            ? selected_start : (std::min)(selection_end, document.size());
        const auto length = owned_text_ ? owned_text_->size()
            : replace_selection ? selected_end - selected_start : 0;
        const bool prefix_space = !remaining.empty() && start > 0 &&
            !is_space(document[start - 1]) && !is_punctuation(remaining[0]);
        const bool suffix_space = !remaining.empty() && start + length < document.size() &&
            !is_space(document[start + length]) && !is_punctuation(document[start + length]);
        std::wstring insertion = prefix_space ? L" " : L"";
        insertion += remaining;
        if (suffix_space) insertion += L' ';

        if (length > 0 || !insertion.empty()) {
            const auto caret = start + (prefix_space ? 1 : 0) + remaining.size();
            replacement = Replacement{start, length, insertion, caret};
            insertion_start_ = start;
            owned_text_ = insertion.empty() ? std::nullopt
                : std::optional<std::wstring>{insertion};
            caret_after_insertion_ = caret;
            observed_document_ = document;
            observed_document_.replace(start, length, insertion);
            observed_selection_start_ = observed_selection_end_ = caret;
        }
        hypothesis_ = text;
    }

    if (is_final) {
        utterance_final_ = true;
        owned_text_.reset();
    }
    return replacement;
}

void LiveDictationBuffer::finish() {
    utterance_final_ = true;
    owned_text_.reset();
}

void LiveDictationBuffer::seal_current_insertion() {
    seal_insertion();
}

} // namespace vtt
