#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace vtt {

// A speech hypothesis is separate from the editable document. Only the latest
// untouched insertion belongs to the recognizer; user edits permanently seal it.
class LiveDictationBuffer {
public:
    struct Replacement {
        std::size_t start = 0;
        std::size_t length = 0;
        std::wstring text;
        std::size_t caret = 0;
    };

    void reset();
    void observe_editor(const std::wstring& document,
                        std::size_t selection_start, std::size_t selection_end);
    std::optional<Replacement> apply(int utterance_id,
                                    const std::wstring& hypothesis, bool is_final,
                                    const std::wstring& document,
                                    std::size_t selection_start,
                                    std::size_t selection_end);
    void finish();
    // External editors can change identity even with identical empty text.
    // Seal delivered words before speech follows a different field.
    void seal_current_insertion();

private:
    void seal_insertion();

    int utterance_id_ = -1;
    bool utterance_final_ = false;
    std::wstring hypothesis_;
    std::wstring sealed_hypothesis_;
    std::optional<std::wstring> owned_text_;
    std::size_t insertion_start_ = 0;
    std::size_t caret_after_insertion_ = 0;
    std::wstring observed_document_;
    std::size_t observed_selection_start_ = 0;
    std::size_t observed_selection_end_ = 0;
    bool has_observation_ = false;
};

} // namespace vtt
