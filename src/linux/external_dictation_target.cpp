#include "external_dictation_target.h"

#include "../dictation_diagnostics.h"
#include "../live_dictation_buffer.h"
#include "../live_text_format.h"
#include "hyprland_ipc.h"
#include "kitty_remote.h"
#include "log.h"
#include "platform.h"
#include "virtual_keyboard.h"
#include "wayland_input.h"

#include <QGuiApplication>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vtt {
namespace {

// Longer spans are left alone: selecting them key by key takes too long.
constexpr std::size_t kSpanLimit = 3000;
constexpr int kKeysPerBatch = 150;
// The longest text selected forwards (see select_and_verify).
constexpr std::size_t kForwardSelectLimit = 400;
// Keys wait this long after the window got focus back: when the Alt+Tab
// switcher closes, Hyprland gives focus to the previous window for about
// 100 ms before the chosen one gets it.
constexpr std::uint64_t kSettleMs = 300;
// A modifier key reported held for longer is taken for a release the
// Hyprland config missed: keys are typed anyway rather than never.
constexpr std::uint64_t kModifierLimitMs = 5000;
// A layer that holds the keyboard for longer is no passing menu or switcher:
// keys are typed anyway rather than never.
constexpr std::uint64_t kGrabLimitMs = 30000;
// How often waiting text is retried while no dictation runs.
constexpr int kIdleRetryMs = 100;
// How long a language-mode result for a terminal typed into by keys may wait
// for its window (it deletes by length, blind; see retry_pending_revision).
constexpr std::uint64_t kBlindRevisionWaitMs = 10000;
// A waiting text's window is looked up (has it been closed?) at most this often.
constexpr std::uint64_t kParcelCheckMs = 1000;
// The longest window name the overlay shows.
constexpr std::size_t kLabelChars = 28;

std::uint64_t steady_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

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

// The text for a terminal written through kitty's socket: every character
// goes in as it is, but a line break would be Enter there, so it becomes a
// space, and other control characters are left out.
std::wstring terminal_text(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t value : text) {
        if (value == L'\r') continue;
        if (value == L'\n' || value == L'\t') value = L' ';
        if (value < 0x20 || value == 0x7F) continue;
        result.push_back(value);
    }
    return result;
}

// A window as the overlay names it: its title without the status marks some
// programs put first (Claude Code's spinner), cut short; else its class.
std::string window_label(const hyprland::Window& window) {
    std::wstring title = to_wide(window.title);
    std::size_t begin = 0;
    while (begin < title.size() && !std::iswalnum(static_cast<wint_t>(title[begin]))) ++begin;
    title = trim(title.substr(begin));
    if (title.empty()) return window.window_class;
    if (title.size() > kLabelChars) title = trim(title.substr(0, kLabelChars - 1)) + L"…";
    return to_utf8(title);
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

    // Words a finished dictation could not write, waiting for their window.
    struct Parcel {
        hyprland::Window window;
        bool terminal = false;
        std::wstring text;              // exactly what will be typed
        std::uint64_t token = 0;        // the ownership the words were added to
        std::uint64_t focused_since = 0; // 0: its window lacked focus at the last look
        std::uint64_t checked_at = 0;   // when it was last looked up
    };

    // A language-mode result that waits for its window (retried, never a
    // reason to keep the worker busy).
    struct PendingRevision {
        std::uint64_t token = 0;
        std::wstring source;
        std::wstring transformed;
    };

    // How text reaches the target right now.
    enum class Route { Hold, Keys, Kitty };

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
    // The agent window's kitty, written to through its socket: no focus or
    // key presses needed. Invalid for every other target.
    kitty_remote::Target kitty;
    // The target window has keyboard focus. While it has not, words are held
    // and typed when it gets focus back.
    bool target_focused = true;
    // When the target last got focus back; 0 while it has had focus since it
    // was chosen.
    std::uint64_t focused_since = 0;
    // Words are being held, and why (for the log when it changes).
    bool holding = false;
    std::string hold_reason;
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
    // The speech typed this session, exactly as typed. Revisions leave it
    // alone, so it is what the user said that landed in the window.
    std::wstring typed_speech;
    // Words were left untyped (dropped for a window without focus, or a
    // failed write): typed_speech is then not the whole dictation.
    bool speech_untyped = false;
    // Text of finished dictations waiting for its window (see park_held).
    std::deque<Parcel> parcels;
    std::function<void(const std::string&)> on_parcel_dropped;
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
    std::optional<PendingRevision> pending_revision;
    // When the waiting language-mode result began to wait (0: none waits).
    std::uint64_t pending_revision_since = 0;
    // Since when the keyboard check has seen a modifier held / a layer
    // holding the keyboard, and whether the limit was logged.
    std::uint64_t modifiers_since = 0;
    std::uint64_t grab_since = 0;
    bool modifier_limit_logged = false;
    bool grab_limit_logged = false;
    bool keyboard_unknown_logged = false;
    bool modifiers_untracked_logged = false;

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
            // Asked before the queue lock is taken (it takes `mutex`).
            const bool idle_work = idle_work_pending();
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                const auto has_job = [this] { return stopping || !jobs.empty(); };
                if (idle_work) {
                    queue_changed.wait_for(lock, std::chrono::milliseconds(kIdleRetryMs), has_job);
                } else {
                    queue_changed.wait(lock, has_job);
                }
                if (stopping && jobs.empty()) return;
                if (!jobs.empty()) {
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
            }
            if (job) {
                job();
            } else {
                // Nothing queued for a while: try the waiting text again.
                deliver_parcels();
                retry_pending_revision();
            }
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

    bool idle_work_pending() {
        if (pending_revision) return true;
        std::lock_guard<std::mutex> lock(mutex);
        return !parcels.empty();
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
    // then held and typed when that window has focus again (keys cannot be
    // delivered to a window without focus: Hyprland can address one, but
    // Chromium drops such keys), or, for an agent window, still written
    // through kitty's socket. Only when it is closed does dictation follow
    // the focus to a new window. Returns true when the target has focus now.
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
            kitty = {};
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
        hyprland::Window still;
        if (chosen && !(found && focused.address == current)) {
            alive = hyprland::find_window(current, still);
        }
        // A window about to become the target: a terminal, and if it is a
        // kitty with remote control, the window inside it to write to.
        bool new_terminal = false;
        kitty_remote::Target new_kitty;
        if ((!chosen || !alive) && found && !own_focused) {
            new_terminal = wayland_input::is_terminal_class(focused.window_class) ||
                           wayland_input::is_terminal_class(process_name(focused.pid));
            if (new_terminal) kitty_remote::find(focused.pid, new_kitty);
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (chosen && !alive) {
            vtlog("dictation target: the window being written to was closed");
            revoke_locked("target window closed");
            have_target = false;
            kitty = {};
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
            target_is_terminal = new_terminal;
            kitty = new_kitty;
            target_focused = true;
            focused_since = 0;
            if (kitty.valid()) {
                vtlog("dictation target: writing to %s (terminal) through kitty window %d; focus not needed",
                      focused.window_class.c_str(), kitty.window_id);
            } else {
                vtlog("dictation target: writing to %s%s", focused.window_class.c_str(),
                      target_is_terminal ? " (terminal)" : "");
            }
            return true;
        }
        const bool now_focused = found && focused.address == target.address;
        // Titles change (an agent's spinner, a browser tab): keep the
        // overlay's name of the window current.
        if (now_focused) target.title = focused.title;
        else if (!still.title.empty()) target.title = still.title;
        if (now_focused != target_focused) {
            if (now_focused) {
                focused_since = steady_ms();
                vtlog("dictation target: %s has focus again", target.window_class.c_str());
            } else if (kitty.valid()) {
                vtlog("dictation target: focus moved to %s; still writing to %s through kitty",
                      found ? (own_focused ? "VoiceToText" : focused.window_class.c_str())
                            : "nothing",
                      target.window_class.c_str());
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

    // The window dictation writes to has keyboard focus at this moment.
    bool target_focused_now() {
        if (!hyprland::available()) return !own_window_focused.load();
        std::uintptr_t address = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!have_target || target_is_own) return false;
            address = target.address;
        }
        hyprland::Window focused;
        return hyprland::active_window(focused) && focused.address == address;
    }

    // Keys typed now arrive as typed: no layer (Alt+Tab switcher, launcher,
    // menu) holds the keyboard, and no modifier key is down, which Hyprland
    // would add to every key the app types. Fails open: when Hyprland cannot
    // tell, or a block lasts far longer than a menu or a key press does, keys
    // are typed anyway and the log says so.
    bool keyboard_free(std::string& reason) {
        if (!hyprland::available()) return true;
        hyprland::KeyboardState state;
        if (!hyprland::keyboard_state(state)) {
            if (!keyboard_unknown_logged) {
                keyboard_unknown_logged = true;
                vtlog("dictation target: Hyprland did not report the keyboard state; typing without that check");
            }
            modifiers_since = grab_since = 0;
            return true;
        }
        keyboard_unknown_logged = false;
        const std::uint64_t now = steady_ms();
        if (state.grabbed) {
            if (!grab_since) grab_since = now;
            if (now - grab_since < kGrabLimitMs) {
                reason = "keyboard held by " +
                    (state.grabbed_by.empty() ? std::string("a layer") : state.grabbed_by);
                return false;
            }
            if (!grab_limit_logged) {
                grab_limit_logged = true;
                vtlog("dictation target: %s has held the keyboard for %llu s; typing anyway",
                      state.grabbed_by.c_str(),
                      static_cast<unsigned long long>((now - grab_since) / 1000));
            }
        } else {
            grab_since = 0;
            grab_limit_logged = false;
        }
        if (!state.tracked && !modifiers_untracked_logged) {
            modifiers_untracked_logged = true;
            vtlog("dictation target: held modifier keys are not tracked "
                  "(~/.config/hypr/voicetotext.lua is not loaded); typing without that check");
        }
        if (state.tracked && state.held_modifiers > 0) {
            if (!modifiers_since) modifiers_since = now;
            if (now - modifiers_since < kModifierLimitMs) {
                reason = "a modifier key is held";
                return false;
            }
            if (!modifier_limit_logged) {
                modifier_limit_logged = true;
                vtlog("dictation target: a modifier key has been reported held for %llu s; "
                      "taking it for a lost release and typing anyway",
                      static_cast<unsigned long long>((now - modifiers_since) / 1000));
            }
        } else {
            modifiers_since = 0;
            modifier_limit_logged = false;
        }
        return true;
    }

    // Records why words are held (logged once per reason).
    void note_hold(const std::string& reason) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            holding = true;
            changed = reason != hold_reason;
            hold_reason = reason;
        }
        if (changed) {
            vtlog("dictation target: holding words (%s)", reason.c_str());
            trace("output_held", {{"reason", reason}});
        }
    }

    void clear_hold() {
        std::lock_guard<std::mutex> lock(mutex);
        holding = false;
        hold_reason.clear();
    }

    // Decides how text reaches the target now: through kitty, as keys, or not
    // yet (held).
    Route route_now() {
        const bool focused = refresh_target();
        std::uint64_t since = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (have_target && !target_is_own && kitty.valid()) return Route::Kitty;
            since = focused_since;
        }
        if (!focused) {
            note_hold("the window lacks focus");
            return Route::Hold;
        }
        if (since && steady_ms() - since < kSettleMs) {
            note_hold("focus just came back");
            return Route::Hold;
        }
        std::string reason;
        if (!keyboard_free(reason)) {
            note_hold(reason);
            return Route::Hold;
        }
        return Route::Keys;
    }

    // kitty did not take the text: the window is probably gone. Keys from now
    // on; refresh_target() notices a closed window.
    void drop_kitty() {
        std::lock_guard<std::mutex> lock(mutex);
        if (kitty.valid()) vtlog("dictation target: kitty did not take the text; using keys from now on");
        kitty = {};
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

    // Writes on the route chosen: through kitty, or as keys at the caret.
    bool write_on(Route route, const std::wstring& text) {
        if (route != Route::Kitty) return write_text(text);
        kitty_remote::Target channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = kitty;
        }
        return kitty_remote::send_text(channel, to_utf8(text));
    }

    // Text that waited for the target (an earlier dictation's last words)
    // goes in before anything new. False when it could not be written; the
    // new text then waits too.
    bool flush_parcel_for_target(Route route) {
        std::wstring text;
        std::uint64_t parcel_token = 0;
        std::string window_class;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = std::find_if(parcels.begin(), parcels.end(), [&](const Parcel& p) {
                return p.window.address == target.address;
            });
            if (found == parcels.end()) return true;
            text = found->text;
            parcel_token = found->token;
            window_class = found->window.window_class;
        }
        if (!write_on(route, text)) return false;
        bool emptied = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            parcels.erase(std::remove_if(parcels.begin(), parcels.end(), [&](const Parcel& p) {
                return p.window.address == target.address;
            }), parcels.end());
            emptied = parcels.empty();
            // Words parked by an earlier dictation are text before the caret
            // for this one (its first words are spaced from them). Words of
            // this session's ownership were counted when they were parked.
            if (parcel_token != token) delivered_text += text;
            hold_reason.clear(); // written: the next hold is logged again
        }
        vtlog("dictation target: wrote the %zu characters that waited for %s", text.size(),
              window_class.c_str());
        trace("parked_output_written", {{"chars", text.size()}, {"target_class", window_class}});
        if (emptied) vtlog("dictation target: no text waits for a window any more");
        return true;
    }

    // Types the next stable words of an utterance. True when it is done with
    // this hypothesis (typed, or nothing new in it).
    bool deliver(const Deferred& update) {
        const auto prepare = [&](LiveDictationBuffer& next) {
            std::wstring document;
            {
                std::lock_guard<std::mutex> lock(mutex);
                document = delivered_text;
            }
            next = buffer;
            // Only newly spoken stable words go to an external application.
            next.seal_current_insertion();
            return next.apply(update.utterance, update.hypothesis, update.final,
                              document, document.size(), document.size());
        };
        LiveDictationBuffer next;
        auto replacement = prepare(next);
        if (!replacement || replacement->text.empty()) {
            // Nothing to type (e.g. the empty last utterance after Stop): never
            // a reason to hold the session for a window without focus.
            buffer = std::move(next);
            buffer.seal_current_insertion();
            return true;
        }
        const Route route = route_now();
        if (route == Route::Hold) return false;
        bool had_parcel = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            had_parcel = std::any_of(parcels.begin(), parcels.end(), [&](const Parcel& p) {
                return p.window.address == target.address;
            });
        }
        if (had_parcel) {
            if (!flush_parcel_for_target(route)) {
                if (route == Route::Kitty) drop_kitty();
                else note_hold("the text waiting for this window could not be written");
                return false;
            }
            // The text before the caret changed: space the new words from it.
            replacement = prepare(next);
            if (!replacement || replacement->text.empty()) {
                buffer = std::move(next);
                buffer.seal_current_insertion();
                return true;
            }
        }

        // Pending only while keys are actually being sent, so the status does
        // not flicker between every preview.
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++operations;
            if (route == Route::Kitty) replacement->text = terminal_text(replacement->text);
            else if (target_is_terminal) replacement->text = typable(replacement->text);
        }
        const bool typed = write_on(route, replacement->text);
        {
            std::lock_guard<std::mutex> lock(mutex);
            --operations;
        }
        if (!typed && route == Route::Kitty) {
            // Not lost: the words stay queued and go by keys (or wait).
            drop_kitty();
            return false;
        }
        if (!typed) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                speech_untyped = true;
            }
            trace("output_failed", {{"utterance_id", update.utterance}});
            return true; // do not retry forever; the transcript window has it
        }
        clear_hold();

        buffer = std::move(next);
        buffer.seal_current_insertion();
        {
            std::lock_guard<std::mutex> lock(mutex);
            delivered_text += replacement->text;
            typed_speech += replacement->text;
            delivered = true;
            if (span_typed.size() + replacement->text.size() > kSpanLimit) {
                revoke_locked("span limit reached");
            }
            span_raw += replacement->text;
            span_typed += replacement->text;
        }
        trace("output_dispatched", {{"utterance_id", update.utterance},
                                    {"is_final", update.final},
                                    {"action", route == Route::Kitty ? "kitty_send_text"
                                                                     : "type_at_caret"},
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
        // No words held. The reason stays as logged, so a language-mode
        // result still waiting does not log it again on every poll; it is
        // forgotten once something is written (clear_hold).
        if (deferred.empty()) holding = false;
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

    // The session's held words become text waiting for the target window
    // (see park_held). They count as typed: they are added to the owned span
    // and to History's text, and go in at the caret before anything else.
    void park() {
        std::wstring document;
        {
            std::lock_guard<std::mutex> lock(mutex);
            document = delivered_text;
        }
        // The held words as deliver() would have typed them, one after another.
        std::wstring text;
        for (const auto& held : deferred) {
            auto next = buffer;
            next.seal_current_insertion();
            const auto replacement = next.apply(held.utterance, held.hypothesis, held.final,
                                                document, document.size(), document.size());
            buffer = std::move(next);
            buffer.seal_current_insertion();
            if (replacement && !replacement->text.empty()) {
                document += replacement->text;
                text += replacement->text;
            }
        }
        deferred.clear();

        std::string window_class;
        {
            std::lock_guard<std::mutex> lock(mutex);
            held_updates = 0;
            holding = false;
            hold_reason.clear();
            if (text.empty()) return;
            if (!have_target || target_is_own) {
                // No window to wait for: they stay in History only.
                speech_untyped = true;
                return;
            }
            if (target_is_terminal) text = kitty.valid() ? terminal_text(text) : typable(text);
            if (text.empty()) return;
            const auto found = std::find_if(parcels.begin(), parcels.end(), [&](const Parcel& p) {
                return p.window.address == target.address;
            });
            if (found == parcels.end()) {
                Parcel parcel;
                parcel.window = target;
                parcel.terminal = target_is_terminal;
                parcel.text = text;
                parcel.token = token;
                parcels.push_back(std::move(parcel));
            } else {
                found->text += text;
                found->token = token;
            }
            delivered_text += text;
            typed_speech += text;
            delivered = true;
            if (span_typed.size() + text.size() > kSpanLimit) revoke_locked("span limit reached");
            span_raw += text;
            span_typed += text;
            window_class = target.window_class;
        }
        vtlog("dictation target: %zu characters wait for %s; written when it can take keys again (no time limit)",
              text.size(), window_class.c_str());
        // trace() takes the lock itself.
        trace("output_parked", {{"chars", text.size()}, {"target_class", window_class}});
    }

    // Writes waiting text into windows that can take keys now; drops text
    // whose window was closed.
    void deliver_parcels() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (parcels.empty()) return;
        }
        hyprland::Window focused;
        const bool found = hyprland::active_window(focused);
        const std::uint64_t now = steady_ms();

        std::vector<Parcel> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.assign(parcels.begin(), parcels.end());
        }
        for (auto& parcel : snapshot) {
            // The window dictation writes to (also after its session ended)
            // takes the text the way its words go: through kitty, or as keys
            // once it has focus and the keyboard is free.
            bool is_target = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                is_target = have_target && !target_is_own &&
                            target.address == parcel.window.address;
            }
            if (is_target) {
                const Route route = route_now();
                if (route != Route::Hold && !flush_parcel_for_target(route) &&
                    route == Route::Kitty) {
                    drop_kitty();
                }
                continue;
            }

            const bool focused_now = found && focused.address == parcel.window.address;
            if (!focused_now) {
                parcel.focused_since = 0;
                bool closed = false;
                if (now - parcel.checked_at >= kParcelCheckMs) {
                    parcel.checked_at = now;
                    hyprland::Window still;
                    bool exists = true;
                    // Only a lookup that worked can say the window is gone.
                    closed = hyprland::lookup_window(parcel.window.address, still, exists) &&
                             !exists;
                }
                std::size_t dropped_chars = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto it = std::find_if(parcels.begin(), parcels.end(), [&](const Parcel& p) {
                        return p.window.address == parcel.window.address;
                    });
                    if (it == parcels.end()) continue;
                    it->focused_since = 0;
                    it->checked_at = parcel.checked_at;
                    if (closed) {
                        const std::string label = window_label(it->window);
                        dropped_chars = it->text.size();
                        parcels.erase(it);
                        vtlog("dictation target: %s was closed; its %zu waiting characters are only in History",
                              parcel.window.window_class.c_str(), dropped_chars);
                        // Under the lock, so it cannot run once it was cleared.
                        if (on_parcel_dropped) on_parcel_dropped(label);
                    }
                }
                if (closed && dropped_chars) {
                    trace("parked_output_dropped", {{"chars", dropped_chars},
                                                    {"target_class", parcel.window.window_class}});
                }
                continue;
            }
            if (!parcel.focused_since) {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& p : parcels) {
                    if (p.window.address == parcel.window.address) p.focused_since = now;
                }
                continue; // settle first
            }
            if (now - parcel.focused_since < kSettleMs) continue;
            std::string reason;
            if (!keyboard_free(reason)) continue;

            const bool written = write_text_as(parcel.text, parcel.terminal);
            if (!written) continue; // tried again later
            {
                std::lock_guard<std::mutex> lock(mutex);
                parcels.erase(std::remove_if(parcels.begin(), parcels.end(), [&](const Parcel& p) {
                    return p.window.address == parcel.window.address;
                }), parcels.end());
            }
            vtlog("dictation target: wrote the %zu characters that waited for %s",
                  parcel.text.size(), parcel.window.window_class.c_str());
            trace("parked_output_written", {{"chars", parcel.text.size()},
                                            {"target_class", parcel.window.window_class}});
        }
    }

    void retry_pending_revision() {
        if (!pending_revision) return;
        // A terminal typed into by keys is revised blind: BackSpace by the
        // length typed. Long after the dictation that would delete whatever
        // is at the prompt by then, so such a result waits only a short while.
        // (kitty's socket never waits; other fields prove the text first.)
        bool blind = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            blind = target_is_terminal && !kitty.valid();
        }
        if (blind && pending_revision_since &&
            steady_ms() - pending_revision_since > kBlindRevisionWaitMs) {
            pending_revision.reset();
            pending_revision_since = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                revision_waiting = false;
            }
            vtlog("dictation target: a language-mode result for a terminal waited over %llu s; "
                  "not applied, the text stays as typed (History has the result)",
                  static_cast<unsigned long long>(kBlindRevisionWaitMs / 1000));
            trace("revision_dropped", {{"reason", "terminal_wait_limit"}});
            return;
        }
        const PendingRevision revision = *pending_revision;
        pending_revision.reset();
        revise(revision.token, revision.source, revision.transformed);
    }

    // Selects the `expected.size()` characters before the caret and waits
    // until the application reports exactly `expected` as selected. The
    // caret stays at the end of the selection. `stage` names the last step
    // (on failure the one that failed; a "_forward" stage means the field
    // was selected forwards) and `seen` describes the last selection read.
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

        const auto clear_selection = []() {
            wayland_input::run({"wl-copy", "--primary", "--clear"});
        };
        const auto press = [&](const char* key, const std::vector<std::string>& modifiers,
                               std::size_t count) {
            while (count > 0) {
                const int batch = static_cast<int>((std::min)(count,
                                                   static_cast<std::size_t>(kKeysPerBatch)));
                if (!send_key(key, modifiers, batch)) return false;
                count -= static_cast<std::size_t>(batch);
            }
            return true;
        };
        const std::wstring last = wanted.substr(wanted.size() - 1);
        const int span_wait = 400 + static_cast<int>(expected.size());

        // An empty primary selection first, so a stale one cannot pass.
        clear_selection();

        // Probe with one character: a field that does not select this way
        // (a terminal inside an editor, a game) is left after a single key.
        stage = "key";
        if (!send_key("Left", {"shift"}, 1)) return false;
        stage = "probe";
        if (selection_is(last, 250)) {
            stage = "key";
            if (!press("Left", {"shift"}, expected.size() - 1)) return false;
            stage = "span";
            return selection_is(wanted, span_wait);
        }

        // GTK 4 single-line entries publish a selection made backwards (caret
        // before the anchor) as an empty primary selection, and one made
        // forwards with its text. Such a field claims the selection but
        // serves nothing; select the same characters forwards there. Any
        // other failure (nothing claimed, other text) is refused as before.
        if (!(seen.value("readable", false) && seen.value("selected_chars", 1) == 0)) {
            return false;
        }
        // Such an entry takes longer per cursor key the longer its line is
        // (measured: about 3 ms at 700 characters, 22 ms at 2100), and this
        // needs twice the keys; past this length the application would stall
        // for many seconds, so the rewrite is refused instead.
        if (expected.size() > kForwardSelectLimit) {
            stage = "forward_limit";
            return false;
        }
        stage = "key";
        if (!send_key("Right")) return false; // back to the caret
        clear_selection();
        if (!send_key("Left") || !send_key("Right", {"shift"})) return false;
        stage = "probe_forward";
        if (!selection_is(last, 250)) return false;
        if (expected.size() == 1) return true;

        stage = "key";
        if (!send_key("Right")) return false; // back to the caret
        clear_selection();
        if (!press("Left", {}, expected.size()) ||
            !press("Right", {"shift"}, expected.size())) {
            return false;
        }
        // Twice the keys, and such an entry handles a long line slowly (about
        // 1 s for 190 characters of a 700-character line): wait longer.
        stage = "span_forward";
        return selection_is(wanted, 400 + 8 * static_cast<int>(expected.size()));
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
            if (terminal) new_typed = kitty.valid() ? terminal_text(new_typed) : typable(new_typed);
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
            // The text is replaced in its own window only. While it cannot
            // take keys the result waits (retried later, without keeping this
            // worker busy); a new session, a cancel or a closed window ends
            // the wait.
            const Route route = route_now();
            if (route == Route::Hold) {
                std::lock_guard<std::mutex> lock(mutex);
                if (request_token != token || !have_target || abandoned) {
                    revision_waiting = false;
                    pending_revision.reset();
                    pending_revision_since = 0;
                    return;
                }
                revision_waiting = true;
                pending_revision = PendingRevision{request_token, source, transformed};
                if (!pending_revision_since) pending_revision_since = steady_ms();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                revision_waiting = false;
                if (request_token != token || !can_replace_locked() || abandoned) return;
            }
            pending_revision.reset();
            pending_revision_since = 0;
            // Words that waited for this window go in first: the span ends
            // with them.
            if (!flush_parcel_for_target(route)) {
                if (route == Route::Kitty) drop_kitty();
                std::lock_guard<std::mutex> lock(mutex);
                revision_waiting = true;
                pending_revision = PendingRevision{request_token, source, transformed};
                return;
            }
            written = old_tail.empty() ? write_on(route, new_tail)
                    : terminal ? replace_in_terminal(route, old_tail, new_tail)
                               : replace_selected(old_tail, new_tail);
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (request_token != token) return;
        if (!written) {
            revoke_locked("the written text is no longer what dictation typed");
            return;
        }
        hold_reason.clear(); // written: the next hold is logged again
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
    bool replace_in_terminal(Route route, const std::wstring& old_text,
                             const std::wstring& new_text) {
        if (route == Route::Kitty) {
            kitty_remote::Target channel;
            {
                std::lock_guard<std::mutex> lock(mutex);
                channel = kitty;
            }
            const bool done = kitty_remote::send_key(channel, "backspace",
                                                     static_cast<int>(old_text.size())) &&
                (new_text.empty() || kitty_remote::send_text(channel, to_utf8(new_text)));
            trace("terminal_revision", {{"deleted_chars", old_text.size()},
                                        {"typed_chars", new_text.size()}, {"via", "kitty"}});
            return done;
        }
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
        const bool forward = std::strstr(stage, "forward") != nullptr;
        trace("selection_revision", {{"replaced_chars", old_text.size()},
                                     {"typed_chars", new_text.size()}, {"written", written},
                                     {"selected", forward ? "forward" : "backward"}});
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
        return write_text_as(text, in_terminal);
    }

    bool write_text_as(const std::wstring& text, bool terminal) {
        if (terminal) {
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
        const bool pasted = wayland_input::send_paste(false);
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
        impl_->holding = false;
        impl_->hold_reason.clear();
        impl_->revoke_locked("new session");
        impl_->delivered_text.clear();
        impl_->typed_speech.clear();
        impl_->speech_untyped = false;
        impl_->delivered = false;
        impl_->have_target = false;
        impl_->kitty = {};
        impl_->focused_since = 0;
    }
    impl_->enqueue([this]() {
        impl_->buffer.reset();
        impl_->deferred.clear();
        impl_->preview_utterance = -1;
        impl_->previous_hypothesis.clear();
        // A result still waiting belonged to the ownership this session ended.
        impl_->pending_revision.reset();
        impl_->pending_revision_since = 0;
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
        impl_->deliver_parcels();
        impl_->retry_pending_revision();
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
        impl_->revision_waiting = false;
        impl_->revoke_locked("session cancelled");
    }
    impl_->enqueue([this]() {
        impl_->deferred.clear();
        impl_->pending_revision.reset();
        impl_->pending_revision_since = 0;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->held_updates = 0;
        impl_->holding = false;
        impl_->hold_reason.clear();
    });
}

void ExternalDictationTarget::drop_held() {
    impl_->abandoned = true;
    impl_->enqueue([this]() {
        // Would any of the held words have been typed?
        bool untyped = false;
        auto next = impl_->buffer;
        std::wstring document;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            document = impl_->delivered_text;
        }
        for (const auto& held : impl_->deferred) {
            next.seal_current_insertion();
            const auto replacement = next.apply(held.utterance, held.hypothesis, held.final,
                                                document, document.size(), document.size());
            if (replacement && !replacement->text.empty()) {
                untyped = true;
                break;
            }
        }
        impl_->deferred.clear();
        impl_->pending_revision.reset();
        impl_->pending_revision_since = 0;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->held_updates = 0;
        impl_->revision_waiting = false;
        impl_->holding = false;
        impl_->hold_reason.clear();
        if (untyped) impl_->speech_untyped = true;
    });
}

bool ExternalDictationTarget::park_held() {
    auto done = std::make_shared<std::promise<void>>();
    auto taken = done->get_future();
    impl_->enqueue([this, done]() {
        impl_->park();
        done->set_value();
    });
    return taken.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
}

std::string ExternalDictationTarget::waiting_for() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->parcels.empty()) return window_label(impl_->parcels.back().window);
    if (impl_->revision_waiting && impl_->have_target && !impl_->target_is_own) {
        return window_label(impl_->target);
    }
    return {};
}

void ExternalDictationTarget::set_parcel_dropped_callback(
    std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_parcel_dropped = std::move(callback);
}

bool ExternalDictationTarget::target_has_focus() {
    return impl_->target_focused_now();
}

void ExternalDictationTarget::press_return(bool into_target) {
    impl_->enqueue_output([this, into_target]() {
        if (into_target) {
            if (impl_->abandoned.load()) {
                vtlog("dictation target: Enter not pressed (its output was given up)");
                impl_->trace("submit_key_dropped", {{"reason", "its output was given up"}});
                return;
            }
            // Through kitty for an agent window, wherever focus is now;
            // otherwise only into the window while it has focus, not into a
            // menu holding the keyboard and not as Shift+Enter (held keys get
            // a moment to come up).
            using Route = Impl::Route;
            Route route = Route::Hold;
            for (int waited = 0;; waited += 50) {
                route = impl_->route_now();
                if (route != Route::Hold || waited >= 1000) break;
                platform::sleep_ms(50);
            }
            std::string reason;
            if (route == Route::Hold) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                reason = impl_->hold_reason.empty() ? "the window lost focus" : impl_->hold_reason;
            } else if (!impl_->flush_parcel_for_target(route)) {
                // Never an Enter without the words it was meant to send.
                reason = "the last words could not be written";
            }
            if (!reason.empty()) {
                vtlog("dictation target: Enter not pressed (%s)", reason.c_str());
                impl_->trace("submit_key_dropped", {{"reason", reason}});
                return;
            }
            if (route == Route::Kitty) {
                kitty_remote::Target channel;
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    channel = impl_->kitty;
                }
                const bool pressed = kitty_remote::send_key(channel, "enter");
                vtlog("dictation target: Enter %s after the dictation (through kitty)",
                      pressed ? "pressed" : "could NOT be pressed");
                impl_->trace("submit_key", {{"pressed", pressed}, {"into_target", true},
                                            {"via", "kitty"}});
                return;
            }
        }
        const bool pressed = impl_->send_key("Return");
        vtlog("dictation target: Enter %s%s", pressed ? "pressed" : "could NOT be pressed",
              into_target ? " after the dictation" : " (passed on)");
        impl_->trace("submit_key", {{"pressed", pressed}, {"into_target", into_target}});
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
    revision.output_held =
        (impl_->held_updates > 0 && impl_->holding) || impl_->revision_waiting;
    revision.waiting_for_focus = revision.output_held && !impl_->target_focused;
    if (revision.waiting_for_focus && impl_->have_target) {
        revision.waiting_for = window_label(impl_->target);
    }
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

bool ExternalDictationTarget::typed_speech(std::string& text) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    text = to_utf8(impl_->typed_speech);
    return impl_->delivered && !impl_->speech_untyped;
}

bool ExternalDictationTarget::has_delivered() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->delivered;
}

} // namespace vtt
