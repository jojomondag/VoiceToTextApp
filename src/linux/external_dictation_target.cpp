#include "external_dictation_target.h"

#include "../dictation_diagnostics.h"
#include "../live_dictation_buffer.h"
#include "../live_text_format.h"
#include "hyprland_ipc.h"
#include "log.h"
#include "platform.h"
#include "virtual_keyboard.h"
#include "wayland_input.h"

#include <QGuiApplication>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vtt {
namespace {

// Longer spans are left alone: selecting them key by key takes too long.
constexpr std::size_t kSpanLimit = 3000;
constexpr int kKeysPerBatch = 150;

std::wstring to_wide(const std::string& text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2
                         : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + static_cast<std::size_t>(length) > text.size()) return {};
        wchar_t value = length == 1 ? lead : lead & (0xFF >> (length + 1));
        for (int k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((next & 0xC0) != 0x80) return {};
            value = static_cast<wchar_t>((value << 6) | (next & 0x3F));
        }
        result.push_back(value);
        i += static_cast<std::size_t>(length);
    }
    return result;
}

std::string to_utf8(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t wide : text) {
        const auto value = static_cast<std::uint32_t>(wide);
        if (value < 0x80) {
            result.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (value >> 6)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (value >> 12)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (value >> 18)));
            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
    }
    return result;
}

bool speech_space(wchar_t value) {
    return std::iswspace(static_cast<wint_t>(value)) != 0 || value == L' ' ||
        (value >= L' ' && value <= L' ') ||
        value == L' ' || value == L' ' || value == L' ';
}

struct SpeechWord { std::size_t end; std::wstring key; };

std::vector<SpeechWord> speech_words(const std::wstring& text) {
    std::vector<SpeechWord> words;
    for (std::size_t index = 0; index < text.size();) {
        while (index < text.size() && speech_space(text[index])) ++index;
        if (index == text.size()) break;
        SpeechWord word{};
        while (index < text.size() && !speech_space(text[index])) {
            wchar_t value = text[index++];
            if (std::iswpunct(static_cast<wint_t>(value)) ||
                (value >= L' ' && value <= L'⁯')) continue;
            if (value >= L'À' && value <= L'Þ' && value != L'×') value += 0x20;
            else value = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(value)));
            word.key += value;
        }
        word.end = index;
        words.push_back(std::move(word));
    }
    return words;
}

// Agree across consecutive previews and withhold the still-growing final word.
std::wstring stable_prefix(const std::wstring& previous, const std::wstring& current) {
    const auto before = speech_words(previous);
    const auto now = speech_words(current);
    if (now.empty()) return {};
    std::size_t count = 0;
    const auto limit = (std::min)(before.size(), now.size() - 1);
    while (count < limit && before[count].key == now[count].key) ++count;
    return count == 0 ? std::wstring{} : current.substr(0, now[count - 1].end);
}

// What an application reports as selected, made comparable with what was
// typed: one kind of line break, one kind of space.
std::wstring comparable(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        wchar_t value = text[index];
        if (value == L'\r') {
            if (index + 1 < text.size() && text[index + 1] == L'\n') continue;
            value = L'\n';
        }
        if (value != L'\n' && speech_space(value)) value = L' ';
        result.push_back(value);
    }
    return result;
}

std::wstring leading_space(const std::wstring& text) {
    std::size_t count = 0;
    while (count < text.size() && speech_space(text[count])) ++count;
    return text.substr(0, count);
}

std::wstring trim(const std::wstring& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && speech_space(text[begin])) ++begin;
    while (end > begin && speech_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// Lower-case name of a process (/proc/<pid>/comm); empty when unknown.
std::string process_name(long pid) {
    if (pid <= 0) return {};
    std::ifstream file("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(file, name);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

// The text with line breaks as spaces and without the characters the virtual
// keyboard cannot type. Used where text must go in as plain key presses.
std::wstring typable(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t value : text) {
        if (value == L'\r') continue;
        if (value == L'\n' || value == L'\t') value = L' ';
        if (wayland_input::can_type(to_utf8(std::wstring(1, value)))) result.push_back(value);
    }
    return result;
}

bool get_primary_selection(std::wstring& text) {
    std::string utf8;
    if (!wayland_input::run({"wl-paste", "--primary", "--no-newline"}, nullptr, &utf8)) {
        text.clear();
        return false;
    }
    text = to_wide(utf8);
    return true;
}

} // namespace

struct ExternalDictationTarget::Impl {
    struct Deferred {
        int utterance = 0;
        std::wstring hypothesis;
        bool final = false;
    };

    // --- Worker ------------------------------------------------------------
    std::thread worker;
    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;

    // --- State (guarded by `mutex`) ----------------------------------------
    mutable std::mutex mutex;
    bool active = false;
    std::uint64_t token = 1;
    bool have_target = false;
    hyprland::Window target;
    bool target_is_terminal = false;
    bool target_is_own = false;
    // The target window has keyboard focus. While it has not, words are held
    // and typed when it gets focus back.
    bool target_focused = true;
    // Words recognised but not typed yet (the target lacked focus).
    std::size_t held_updates = 0;
    // A language-mode result is waiting for the target to get focus back.
    bool revision_waiting = false;
    // The owned span: the speech it came from and what the editor holds for
    // it now. A revision rewrites a prefix of it; the rest is still as typed,
    // so span_raw.substr(revised_raw) == span_typed.substr(revised_typed).
    std::wstring span_raw;
    std::wstring span_typed;
    std::size_t revised_raw = 0;
    std::size_t revised_typed = 0;
    std::wstring delivered_text;        // everything typed this session
    bool delivered = false;
    int operations = 0;                 // queued or running output jobs
    bool focus_job_queued = false;
    // One of this application's own windows has keyboard focus (set on the
    // UI thread; used where the compositor cannot tell us).
    std::atomic<bool> own_window_focused{false};
    // The session stopped waiting for its window (drop_held): nothing more is
    // written for it. Cleared when the next session begins.
    std::atomic<bool> abandoned{false};

    // --- Worker-only state --------------------------------------------------
    LiveDictationBuffer buffer;
    std::deque<Deferred> deferred;
    int preview_utterance = -1;
    std::wstring previous_hypothesis;

    std::shared_ptr<DictationDiagnostics> diagnostics;

    void trace(const std::string& event, nlohmann::json fields = nlohmann::json::object()) {
        std::shared_ptr<DictationDiagnostics> sink;
        {
            std::lock_guard<std::mutex> lock(mutex);
            sink = diagnostics;
        }
        if (sink) sink->write(event, fields);
    }

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_changed.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }

    void enqueue(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (stopping) return;
            jobs.push_back(std::move(job));
        }
        queue_changed.notify_one();
    }

    // An output job counts as pending from the moment it is queued.
    void enqueue_output(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++operations;
        }
        enqueue([this, job = std::move(job)]() {
            job();
            std::lock_guard<std::mutex> lock(mutex);
            --operations;
        });
    }

    // Dictation gives up its claim on the text written so far. The text stays
    // where it is; later speech starts a new span.
    void revoke_locked(const char* reason) {
        if (!span_raw.empty()) vtlog("dictation target: ownership ended (%s)", reason);
        span_raw.clear();
        span_typed.clear();
        revised_raw = revised_typed = 0;
        ++token;
    }

    // Keeps track of the window dictation writes to: the one that had focus
    // when the session started (or the first window of another application
    // focused after that). Moving focus elsewhere does not change it: words are
    // then held and typed when that window has focus again. (Keys cannot be
    // delivered to a window without focus: Hyprland can address one, but
    // Chromium drops such keys.) Only when it is closed does dictation follow
    // the focus to a new window. Returns true when the target can be typed into
    // now.
    bool refresh_target() {
        if (!hyprland::available()) {
            // No focus information from the compositor: type at whatever has
            // focus. A revision is still safe, it proves the span first.
            std::lock_guard<std::mutex> lock(mutex);
            const bool own = own_window_focused.load();
            if (have_target && own != target_is_own) revoke_locked("focus changed");
            have_target = true;
            target = {};
            target_is_terminal = false;
            target_is_own = own;
            target_focused = !own;
            return !own;
        }

        hyprland::Window focused;
        const bool found = hyprland::active_window(focused);
        const bool own_focused = found && focused.pid == static_cast<long>(getpid());

        bool chosen = false;
        std::uintptr_t current = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            chosen = have_target;
            current = target.address;
        }
        bool alive = true;
        if (chosen && !(found && focused.address == current)) {
            hyprland::Window still;
            alive = hyprland::find_window(current, still);
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (chosen && !alive) {
            vtlog("dictation target: the window being written to was closed");
            revoke_locked("target window closed");
            have_target = false;
            chosen = false;
        }
        if (!chosen) {
            if (!found || own_focused) {
                target_focused = false;
                return false;
            }
            have_target = true;
            target = focused;
            target_is_own = false;
            // A terminal started with its own window class (an agent window,
            // a scratchpad) is still recognised by the program that owns it.
            target_is_terminal =
                wayland_input::is_terminal_class(focused.window_class) ||
                wayland_input::is_terminal_class(process_name(focused.pid));
            target_focused = true;
            vtlog("dictation target: writing to %s%s", focused.window_class.c_str(),
                  target_is_terminal ? " (terminal)" : "");
            return true;
        }
        const bool now_focused = found && focused.address == target.address;
        if (now_focused != target_focused) {
            if (now_focused) {
                vtlog("dictation target: %s has focus again", target.window_class.c_str());
            } else {
                vtlog("dictation target: focus moved to %s; holding words for %s",
                      found ? (own_focused ? "VoiceToText" : focused.window_class.c_str())
                            : "nothing",
                      target.window_class.c_str());
            }
            target_focused = now_focused;
        }
        return now_focused;
    }

    bool send_key(const std::string& key, const std::vector<std::string>& modifiers = {},
                  int count = 1) {
        return wayland_input::press_key(key, modifiers, count);
    }

    bool can_replace_locked() const {
        return have_target && !target_is_own;
    }

    std::wstring owned_raw_locked() const {
        return trim(span_raw);
    }

    // Types the next stable words of an utterance. True when it is done with
    // this hypothesis (typed, or nothing new in it).
    bool deliver(const Deferred& update) {
        std::wstring document;
        {
            std::lock_guard<std::mutex> lock(mutex);
            document = delivered_text;
        }
        auto next = buffer;
        // Only newly spoken stable words go to an external application.
        next.seal_current_insertion();
        auto replacement = next.apply(update.utterance, update.hypothesis, update.final,
                                      document, document.size(), document.size());
        if (!replacement || replacement->text.empty()) {
            // Nothing to type (e.g. the empty last utterance after Stop): never
            // a reason to hold the session for a window without focus.
            buffer = std::move(next);
            buffer.seal_current_insertion();
            return true;
        }
        if (!refresh_target()) return false;

        // Pending only while keys are actually being sent, so the status does
        // not flicker between every preview.
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++operations;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (target_is_terminal) replacement->text = typable(replacement->text);
        }
        const bool typed = write_text(replacement->text);
        {
            std::lock_guard<std::mutex> lock(mutex);
            --operations;
        }
        if (!typed) {
            trace("output_failed", {{"utterance_id", update.utterance}});
            return true; // do not retry forever; the transcript window has it
        }

        buffer = std::move(next);
        buffer.seal_current_insertion();
        {
            std::lock_guard<std::mutex> lock(mutex);
            delivered_text += replacement->text;
            delivered = true;
            if (span_typed.size() + replacement->text.size() > kSpanLimit) {
                revoke_locked("span limit reached");
            }
            span_raw += replacement->text;
            span_typed += replacement->text;
        }
        trace("output_dispatched", {{"utterance_id", update.utterance},
                                    {"is_final", update.final},
                                    {"action", "type_at_caret"},
                                    {"inserted_text", to_utf8(replacement->text)}});
        return true;
    }

    void drain_deferred() {
        while (!deferred.empty()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!active) break;
            }
            if (!deliver(deferred.front())) break; // target without focus; keep it
            deferred.pop_front();
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!active) deferred.clear();
        held_updates = deferred.size();
    }

    void accept(int utterance, std::wstring hypothesis, bool final) {
        if (utterance < preview_utterance) return;
        if (utterance != preview_utterance) {
            preview_utterance = utterance;
            previous_hypothesis.clear();
        }
        // A word is typed once the recogniser has heard the next one. When two
        // previews in a row agree on the whole text (the speaker paused), the
        // last word is typed too instead of waiting for the end of the phrase.
        auto stable = final || (!previous_hypothesis.empty() && hypothesis == previous_hypothesis)
            ? hypothesis : stable_prefix(previous_hypothesis, hypothesis);
        previous_hypothesis = std::move(hypothesis);
        if (!deferred.empty() && deferred.back().utterance == utterance) {
            if (!deferred.back().final) deferred.back() = {utterance, std::move(stable), final};
        } else {
            deferred.push_back({utterance, std::move(stable), final});
        }
        drain_deferred();
    }

    // Selects `count` characters backwards from the caret and waits until the
    // application reports exactly `expected` as selected. On failure `stage`
    // says which step failed and `seen` describes the last selection read.
    bool select_and_verify(const std::wstring& expected, const char*& stage,
                           nlohmann::json& seen) {
        const std::wstring wanted = comparable(expected);
        const auto selection_is = [&](const std::wstring& text, int wait_ms) {
            for (int waited = 0;; waited += 20) {
                std::wstring selected;
                const bool read = get_primary_selection(selected);
                if (read && comparable(selected) == text) return true;
                if (waited >= wait_ms) {
                    // Lengths only: the selection holds the user's text.
                    seen = {{"readable", read}, {"selected_chars", selected.size()}};
                    return false;
                }
                platform::sleep_ms(20);
            }
        };

        // An empty primary selection first, so a stale one cannot pass.
        wayland_input::run({"wl-copy", "--primary", "--clear"});

        // Probe with one character: a field that does not select this way
        // (a terminal inside an editor, a game) is left after a single key.
        stage = "key";
        if (!send_key("Left", {"shift"}, 1)) return false;
        stage = "probe";
        if (!selection_is(wanted.substr(wanted.size() - 1), 250)) return false;

        stage = "key";
        std::size_t remaining = expected.size() - 1;
        while (remaining > 0) {
            const int batch = static_cast<int>((std::min)(remaining,
                                               static_cast<std::size_t>(kKeysPerBatch)));
            if (!send_key("Left", {"shift"}, batch)) return false;
            remaining -= static_cast<std::size_t>(batch);
        }
        stage = "span";
        return selection_is(wanted, 400 + static_cast<int>(expected.size()));
    }

    // Replaces the text of the owned span that came from `source` (a prefix
    // of its speech) with `transformed`. Later speech is kept after it.
    void revise(std::uint64_t request_token, const std::wstring& source,
                const std::wstring& transformed) {
        std::wstring old_typed;
        std::wstring new_typed;
        std::size_t cut = 0;
        std::size_t new_revised_typed = 0;
        bool terminal = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request_token != token || !can_replace_locked() || span_raw.empty()) return;

            const std::wstring lead = leading_space(span_raw);
            if (source.empty() || span_raw.compare(lead.size(), source.size(), source) != 0) {
                vtlog("dictation target: revision source no longer matches the owned span");
                return;
            }
            cut = lead.size() + source.size();
            // A result for less speech than was already revised is out of date.
            if (cut < revised_raw) return;

            const std::wstring tail = span_raw.substr(cut);
            old_typed = span_typed;
            terminal = target_is_terminal;
            // A terminal gets the text on one line: a line break there is Enter.
            new_typed = lead + (terminal
                ? trim(to_wide(prompt_for_direct_typing(to_utf8(transformed))))
                : trim(transformed));
            if (!tail.empty() && !speech_space(tail.front()) && !new_typed.empty() &&
                !speech_space(new_typed.back())) {
                new_typed += L' ';
            }
            if (terminal) new_typed = typable(new_typed);
            new_revised_typed = new_typed.size();
            new_typed += tail;
        }

        // Only the part after the common beginning has to be rewritten.
        std::size_t common = 0;
        const auto shortest = (std::min)(old_typed.size(), new_typed.size());
        while (common < shortest && old_typed[common] == new_typed[common]) ++common;
        const std::wstring old_tail = old_typed.substr(common);
        const std::wstring new_tail = new_typed.substr(common);

        bool written = old_tail.empty() && new_tail.empty();
        if (!written) {
            // The text is replaced in its own window only. While another one has
            // focus, wait for the user to come back (a new session, a cancel or
            // a closed window ends the wait).
            while (!refresh_target()) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (request_token != token || !have_target || abandoned) {
                        revision_waiting = false;
                        return;
                    }
                    revision_waiting = true;
                }
                platform::sleep_ms(100);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                revision_waiting = false;
                if (request_token != token || !can_replace_locked() || abandoned) return;
            }
            written = old_tail.empty() ? write_text(new_tail)
                    : terminal ? replace_in_terminal(old_tail, new_tail)
                               : replace_selected(old_tail, new_tail);
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (request_token != token) return;
        if (!written) {
            revoke_locked("the written text is no longer what dictation typed");
            return;
        }
        span_typed = new_typed;
        revised_raw = cut;
        revised_typed = new_revised_typed;
        if (delivered_text.size() >= old_typed.size()) {
            delivered_text.replace(delivered_text.size() - old_typed.size(), old_typed.size(),
                                   new_typed);
        }
        delivered = true;
    }

    // A terminal has no selection to prove the span with. What dictation typed
    // is deleted by its length and the new text is typed in its place; this is
    // right as long as the user has not typed or moved the cursor on that line
    // in the meantime (the user chose this trade-off over never revising).
    bool replace_in_terminal(const std::wstring& old_text, const std::wstring& new_text) {
        std::size_t remaining = old_text.size();
        while (remaining > 0) {
            const int batch = static_cast<int>((std::min)(remaining,
                                               static_cast<std::size_t>(kKeysPerBatch)));
            if (!send_key("BackSpace", {}, batch)) return false;
            remaining -= static_cast<std::size_t>(batch);
        }
        trace("terminal_revision", {{"deleted_chars", old_text.size()},
                                    {"typed_chars", new_text.size()}});
        return new_text.empty() || write_text(new_text);
    }

    bool replace_selected(const std::wstring& old_text, const std::wstring& new_text) {
        const char* stage = "";
        nlohmann::json seen = nlohmann::json::object();
        if (!select_and_verify(old_text, stage, seen)) {
            // Collapse whatever got selected back to the caret.
            send_key("Right");
            std::string window_class;
            {
                std::lock_guard<std::mutex> lock(mutex);
                window_class = target.window_class;
            }
            seen["reason"] = "selection_did_not_match";
            seen["expected_chars"] = old_text.size();
            seen["stage"] = stage;
            seen["target_class"] = window_class;
            trace("revision_rejected", seen);
            return false;
        }
        const bool written = new_text.empty() ? send_key("BackSpace") : write_text(new_text);
        trace("selection_revision", {{"replaced_chars", old_text.size()},
                                     {"typed_chars", new_text.size()}, {"written", written}});
        return written;
    }

    // Writes at the caret, over the selection if there is one. Ordinary text
    // is typed. Text with line breaks is pasted (a typed Enter could send a
    // chat message), and so is text with characters the keyboard layout lacks.
    bool write_text(const std::wstring& text) {
        bool in_terminal = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            in_terminal = target_is_terminal;
        }
        if (in_terminal) {
            // Typed only, so that what is on the line is exactly what was
            // sent: a paste can be shown differently, and Enter would run it.
            return wayland_input::type_text(to_utf8(typable(text)));
        }

        const std::string utf8 = to_utf8(text);
        if (text.find(L'\n') == std::wstring::npos && wayland_input::can_type(utf8)) {
            return wayland_input::type_text(utf8);
        }

        std::string previous;
        const bool had_text = wayland_input::get_clipboard(previous);
        if (!wayland_input::set_clipboard(utf8)) return false;
        platform::sleep_ms(40);
        bool terminal = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            terminal = target_is_terminal;
        }
        const bool pasted = wayland_input::send_paste(terminal);
        // Let the application read the paste before the old content returns.
        platform::sleep_ms(250);
        if (had_text) wayland_input::set_clipboard(previous);
        else wayland_input::clear_clipboard();
        return pasted;
    }
};

ExternalDictationTarget::ExternalDictationTarget() : impl_(std::make_unique<Impl>()) {}

ExternalDictationTarget::~ExternalDictationTarget() {
    {
        std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->stopping = true;
        impl_->jobs.clear();
    }
    impl_->queue_changed.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool ExternalDictationTarget::initialize(HWND /*owner*/) {
    if (!impl_->worker.joinable()) {
        impl_->worker = std::thread([this]() { impl_->run(); });
        // Bring the virtual keyboard up now, so the first word is not delayed
        // and the compositor already knows the device.
        impl_->enqueue([]() { virtual_keyboard::available(); });
    }
    return true;
}

void ExternalDictationTarget::set_toggle_hotkey(UINT /*modifiers*/, UINT /*key*/) {}

void ExternalDictationTarget::set_diagnostics(std::shared_ptr<DictationDiagnostics> diagnostics) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->diagnostics = std::move(diagnostics);
}

void ExternalDictationTarget::begin_session() {
    impl_->own_window_focused = QGuiApplication::focusWindow() != nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->active = true;
        impl_->abandoned = false;
        impl_->revision_waiting = false;
        impl_->revoke_locked("new session");
        impl_->delivered_text.clear();
        impl_->delivered = false;
        impl_->have_target = false;
    }
    impl_->enqueue([this]() {
        impl_->buffer.reset();
        impl_->deferred.clear();
        impl_->preview_utterance = -1;
        impl_->previous_hypothesis.clear();
        impl_->refresh_target();
        impl_->trace("session_begin", {{"delivery_mode", "stable_words_at_user_caret"}});
    });
}

bool ExternalDictationTarget::update_transcript(int utterance_id, const std::string& text,
                                                bool is_final) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->active) return false;
    }
    auto wide = to_wide(text);
    if (!text.empty() && wide.empty()) return false;
    impl_->enqueue([this, utterance_id, wide = std::move(wide), is_final]() mutable {
        impl_->accept(utterance_id, std::move(wide), is_final);
    });
    return true;
}

void ExternalDictationTarget::poll_focus() {
    impl_->own_window_focused = QGuiApplication::focusWindow() != nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->active || impl_->focus_job_queued) return;
        impl_->focus_job_queued = true;
    }
    impl_->enqueue([this]() {
        impl_->refresh_target();
        impl_->drain_deferred();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->focus_job_queued = false;
    });
}

void ExternalDictationTarget::finish_session() {
    // Queued words are still written; ownership of the span is kept so the
    // selected language mode can be applied to it after Stop.
    impl_->enqueue_output([this]() { impl_->drain_deferred(); });
}

void ExternalDictationTarget::cancel_session() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->active = false;
        impl_->revoke_locked("session cancelled");
    }
    impl_->enqueue([this]() {
        impl_->deferred.clear();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->held_updates = 0;
    });
}

void ExternalDictationTarget::drop_held() {
    impl_->abandoned = true;
    impl_->enqueue([this]() {
        impl_->deferred.clear();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->held_updates = 0;
    });
}

ExternalDictationTarget::DictationRevision ExternalDictationTarget::revision_context() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DictationRevision revision;
    revision.token = impl_->token;
    revision.target_token = impl_->token;
    revision.readable_target = impl_->can_replace_locked();
    revision.can_replace = revision.readable_target;
    revision.available = impl_->have_target && !impl_->target_is_own;
    revision.pending_output = impl_->operations > 0 || impl_->held_updates > 0;
    revision.waiting_for_focus =
        (impl_->held_updates > 0 || impl_->revision_waiting) && !impl_->target_focused;
    if (revision.can_replace) revision.raw_text = to_utf8(impl_->owned_raw_locked());
    return revision;
}

bool ExternalDictationTarget::apply_revision(std::uint64_t token,
                                             const std::string& source_raw_text,
                                             const std::string& transformed_text) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (token != impl_->token || !impl_->can_replace_locked() || impl_->span_raw.empty() ||
            impl_->abandoned) {
            return false;
        }
    }
    impl_->enqueue_output([this, token, source = trim(to_wide(source_raw_text)),
                           transformed = to_wide(transformed_text)]() {
        impl_->revise(token, source, transformed);
    });
    return true;
}

bool ExternalDictationTarget::apply_revision(std::uint64_t token,
                                             const std::string& transformed_text) {
    std::string source;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        source = to_utf8(impl_->owned_raw_locked());
    }
    return apply_revision(token, source, transformed_text);
}

std::string ExternalDictationTarget::text_utf8() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return to_utf8(impl_->delivered_text);
}

bool ExternalDictationTarget::has_delivered() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->delivered;
}

} // namespace vtt
