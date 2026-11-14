#include "mode_switch_policy.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace vtt {
namespace {

std::size_t word_count(const std::string& text, std::size_t begin = 0) {
    std::size_t count = 0;
    bool in_word = false;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool letter = std::isalnum(c) != 0 || c >= 128;
        if (letter && !in_word) ++count;
        in_word = letter || (in_word && (c == '\'' || c == '-'));
    }
    return count;
}

std::size_t completed_sentence_prefix(const std::string& text) {
    std::size_t completed = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto punctuation = text[i];
        if (punctuation != '.' && punctuation != '!' && punctuation != '?') continue;
        // A decimal, extension or version dot is not followed by a separator.
        // Ellipses explicitly leave a thought open, including their final dot.
        if (punctuation == '.' && ((i > 0 && text[i - 1] == '.') ||
            (i + 1 < text.size() && text[i + 1] == '.'))) continue;
        auto end = i + 1;
        // Include a closing quote/bracket in the exact source prefix, without
        // consuming the whitespace that separates the unfinished spoken tail.
        for (;;) {
            if (end < text.size() && std::string("\"')]}").find(text[end]) != std::string::npos) ++end;
            else if (text.compare(end, 3, u8"”") == 0 || text.compare(end, 3, u8"’") == 0) end += 3;
            else if (text.compare(end, 2, u8"»") == 0) end += 2;
            else break;
        }
        if (end == text.size() || std::isspace(static_cast<unsigned char>(text[end])))
            completed = end;
    }
    return completed;
}

} // namespace

bool ModeSwitchPolicy::same_target(const Context& left, const Context& right) {
    return left.session == right.session &&
           left.target_token == right.target_token;
}

bool ModeSwitchPolicy::same_source(const Context& left, const Context& right) {
    return same_target(left, right) && left.raw_text == right.raw_text;
}

bool ModeSwitchPolicy::is_prefix(const std::string& prefix,
                                 const std::string& text) {
    return prefix.size() <= text.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool ModeSwitchPolicy::source_is_owned(const Context& source) const {
    return current_ && source.target_token != 0 && !source.raw_text.empty() &&
           same_target(source, *current_) &&
           is_prefix(source.raw_text, current_->raw_text);
}

void ModeSwitchPolicy::observe_context(Context context) {
    if (current_ && (!same_target(*current_, context) ||
                     !is_prefix(current_->raw_text, context.raw_text))) {
        invalidate_work();
    }
    current_ = std::move(context);
    if (pending_ && !submitted_ && source_is_owned(pending_->context)) {
        pending_->context = *current_;
        resolve_pending();
    }
    schedule_current();
}

std::uint64_t ModeSwitchPolicy::request_mode(const Context& context,
                                            const std::string& mode) {
    if (!current_ || !same_source(context, *current_) ||
        (mode != "recorded" && mode != "clean" && mode != "ai")) {
        return 0;
    }

    const auto click_id = ++next_click_id_;
    selected_ = Selection{click_id, mode};
    pending_.reset();
    submitted_ = false;
    ready_.reset();
    completed_.reset();
    // A deliberate retry must not immediately replay a cached failure. Keep
    // successful versions so switching between modes can still be immediate.
    for (auto& cached : cache_) {
        if (mode == "clean" && !cached.clean_error.empty()) {
            cached.clean.clear();
            cached.clean_error.clear();
        } else if (mode == "ai" && !cached.ai_error.empty()) {
            cached.ai.clear();
            cached.ai_error.clear();
        }
    }
    if (mode == "recorded") {
        if (source_is_owned(context)) {
            pending_ = Click{click_id, context, mode};
            resolve_pending();
        }
    } else {
        schedule_current();
    }
    return click_id;
}

void ModeSwitchPolicy::schedule_current() {
    if (!selected_ || selected_->mode == "recorded" || pending_ || ready_ ||
        !current_ || !source_is_owned(*current_) ||
        (completed_ && same_source(*completed_, *current_))) {
        return;
    }
    pending_ = Click{selected_->id, *current_, selected_->mode};
    submitted_ = false;
    resolve_pending();
}

void ModeSwitchPolicy::receive_versions(Versions versions) {
    if (!source_is_owned(versions.source)) return;

    const auto existing = std::find_if(cache_.begin(), cache_.end(),
        [&](const Versions& cached) {
            return same_source(cached.source, versions.source);
        });
    if (existing != cache_.end()) {
        // Clean and AI can finish independently. Missing fields mean that a
        // version has not completed, rather than replacing a cached result.
        if (!versions.clean.empty() || !versions.clean_error.empty()) {
            existing->clean = std::move(versions.clean);
            existing->clean_error = std::move(versions.clean_error);
        }
        if (!versions.ai.empty() || !versions.ai_error.empty()) {
            existing->ai = std::move(versions.ai);
            existing->ai_error = std::move(versions.ai_error);
        }
        Versions merged = std::move(*existing);
        cache_.erase(existing);
        cache_.push_back(std::move(merged));
    } else {
        cache_.push_back(std::move(versions));
    }
    while (cache_.size() > cache_limit_) cache_.pop_front();
    resolve_pending();
}

void ModeSwitchPolicy::resolve_pending() {
    if (!pending_ || !source_is_owned(pending_->context)) return;

    Replacement replacement;
    replacement.click_id = pending_->id;
    replacement.context = pending_->context;
    replacement.mode = pending_->mode;
    if (pending_->mode == "recorded") {
        replacement.text = pending_->context.raw_text;
    } else {
        const auto found = std::find_if(cache_.begin(), cache_.end(),
            [&](const Versions& cached) {
                return same_source(cached.source, pending_->context);
            });
        if (found == cache_.end()) return;
        const bool clean = pending_->mode == "clean";
        replacement.text = clean ? found->clean : found->ai;
        replacement.error = clean ? found->clean_error : found->ai_error;
        if (replacement.text.empty() && replacement.error.empty()) return;
        if (!replacement.error.empty()) {
            replacement.text = pending_->context.raw_text;
        }
    }

    ready_ = std::move(replacement);
    // Keep this source frozen until the owner consumes its replacement. Only
    // then may the next, longer source be scheduled.
    pending_.reset();
    submitted_ = false;
}

std::optional<ModeSwitchPolicy::Replacement> ModeSwitchPolicy::take_ready() {
    if (ready_ && !source_is_owned(ready_->context)) ready_.reset();
    auto result = std::move(ready_);
    ready_.reset();
    if (result) completed_ = result->context;
    schedule_current();
    return result;
}

std::optional<ModeSwitchPolicy::Context> ModeSwitchPolicy::pending_context() const {
    if (!pending_) return std::nullopt;
    return pending_->context;
}

std::optional<ModeSwitchPolicy::Context> ModeSwitchPolicy::submission_context(bool flush) const {
    if (!pending_ || submitted_ || !source_is_owned(pending_->context)) return std::nullopt;
    const auto& text = pending_->context.raw_text;
    std::size_t offset = 0;
    if (completed_ && same_target(*completed_, pending_->context) &&
        is_prefix(completed_->raw_text, text)) offset = completed_->raw_text.size();
    if (text.size() <= offset) return std::nullopt;
    if (flush) return pending_->context;
    if (pending_->mode == "ai") {
        const auto end = completed_sentence_prefix(text);
        if (end <= offset) return std::nullopt;
        Context source = pending_->context;
        source.raw_text.resize(end);
        if (word_count(source.raw_text) < 2) return std::nullopt;
        return source;
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    if (last == std::string::npos || last < offset) return std::nullopt;
    // Cleanup keeps its existing phrase-sized batching; it does not turn an
    // unfinished thought into a prompt-level request for clarification.
    if (std::string(".!?;").find(text[last]) != std::string::npos &&
        word_count(text) >= 2) return pending_->context;
    if (last + 1 - offset >= 32 && word_count(text, offset) >= 4) return pending_->context;
    return std::nullopt;
}

bool ModeSwitchPolicy::ready_to_submit(bool flush) const {
    return submission_context(flush).has_value();
}

bool ModeSwitchPolicy::mark_submitted(const Context& context) {
    const auto full = submission_context(true);
    if (!full || !same_target(*full, context)) return false;
    const auto automatic = submission_context();
    if (!same_source(*full, context) && (!automatic || !same_source(*automatic, context))) return false;
    // The model receives this frozen sentence prefix. New speech continues to
    // coalesce in current_, and the adapter preserves its unprocessed raw tail.
    pending_->context = context;
    submitted_ = true;
    return true;
}

bool ModeSwitchPolicy::has_pending() const {
    return pending_.has_value() || ready_.has_value();
}

void ModeSwitchPolicy::invalidate_work() {
    pending_.reset();
    submitted_ = false;
    ready_.reset();
    completed_.reset();
    cache_.clear();
}

void ModeSwitchPolicy::revoke() {
    current_.reset();
    selected_.reset();
    invalidate_work();
}

void ModeSwitchPolicy::clear() {
    revoke();
}

} // namespace vtt
