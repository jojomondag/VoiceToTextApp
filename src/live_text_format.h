#pragma once

#include <string>

namespace vtt {

// Keep structured prompts readable in arbitrary editors without injecting an
// Enter key, which may submit the user's message instead of inserting a line.
inline std::string prompt_for_direct_typing(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    bool line_break = false;
    for (const char ch : text) {
        if (ch == '\r' || ch == '\n') {
            line_break = true;
            continue;
        }
        if (line_break) {
            if (ch == ' ' || ch == '\t') continue;
            if (!result.empty() && result.back() != ' ' && result.back() != '\t')
                result += ' ';
            line_break = false;
        }
        result += ch;
    }
    return result;
}

} // namespace vtt
