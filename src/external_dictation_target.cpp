#include "external_dictation_target.h"

#include "live_dictation_buffer.h"
#include "dictation_diagnostics.h"
#include "clipboard_text_transaction.h"

#include <nlohmann/json.hpp>

#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace vtt {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::size_t kDocumentLimit = 1024 * 1024;
constexpr std::size_t kRevisionLimit = 3000;
constexpr UINT kMessageTimeoutMs = 100;

std::wstring to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

bool message(HWND window, UINT identifier, WPARAM wparam, LPARAM lparam,
             DWORD_PTR* result = nullptr) {
    DWORD_PTR ignored = 0;
    return SendMessageTimeoutW(window, identifier, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, kMessageTimeoutMs, result ? result : &ignored) != 0;
}

bool input_busy() {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
                    VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return true;
    }
    return false;
}

bool send_paste_shortcut(bool& complete) {
    INPUT input[4]{};
    for (auto& event : input) event.type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].ki.wVk = 'V';
    input[2].ki.wVk = 'V';
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].ki.wVk = VK_CONTROL;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(4, input, sizeof(INPUT));
    // Never replay V after a partial batch: paste may already be queued.
    if (sent > 0 && sent < 4) {
        INPUT release[2] = {input[2], input[3]};
        SendInput(2, release, sizeof(INPUT));
    }
    complete = sent == 4;
    return sent >= 2;
}

bool range_text(IUIAutomationTextRange* range, std::wstring& text) {
    if (!range) return false;
    BSTR value = nullptr;
    const HRESULT status = range->GetText(static_cast<int>(kDocumentLimit + 1), &value);
    if (FAILED(status)) return false;
    const auto size = static_cast<std::size_t>(SysStringLen(value));
    if (size > kDocumentLimit) {
        SysFreeString(value);
        return false;
    }
    text.assign(value ? value : L"", size);
    SysFreeString(value);
    return true;
}

// Physical input invalidates a captured caret; our injected Unicode events do not.
thread_local std::atomic<std::uint64_t>* monitored_epoch = nullptr;
thread_local std::atomic<ULONGLONG>* monitored_tick = nullptr;
thread_local std::atomic<std::uint64_t>* monitored_toggle = nullptr;

void note_user_input() {
    if (monitored_epoch) monitored_epoch->fetch_add(1, std::memory_order_relaxed);
    if (monitored_tick) monitored_tick->store(GetTickCount64(), std::memory_order_relaxed);
}

LRESULT CALLBACK keyboard_monitor(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const auto* event = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        const DWORD key = event->vkCode;
        const bool modifier = key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
            key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_MENU ||
            key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN;
        UINT held = 0;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) held |= MOD_CONTROL;
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) held |= MOD_SHIFT;
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) held |= MOD_ALT;
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) held |= MOD_WIN;
        const auto toggle = monitored_toggle ? monitored_toggle->load(std::memory_order_relaxed) : 0;
        const bool recording_toggle = key == static_cast<UINT>(toggle) &&
            held == static_cast<UINT>(toggle >> 32);
        // Modifiers alone do not alter the editor. The registered recording
        // hotkey is consumed by this app, so its stop event can retain a final
        // pending revision. Any actual edit/navigation key still revokes it.
        if ((event->flags & LLKHF_INJECTED) == 0 && !modifier && !recording_toggle) note_user_input();
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK mouse_monitor(int code, WPARAM wparam, LPARAM lparam) {
    const auto* event = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
    if (code >= 0 && (event->flags & LLMHF_INJECTED) == 0 &&
        (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN ||
         wparam == WM_MBUTTONDOWN || wparam == WM_XBUTTONDOWN ||
         wparam == WM_MOUSEWHEEL || wparam == WM_MOUSEHWHEEL)) {
        // The nonactivating mode overlay does not move the external caret.
        // Its clicks must leave a verified dictation span available to switch
        // profiles. Every external click still revokes that ownership.
        DWORD process = 0;
        const HWND hit = WindowFromPoint(event->pt);
        if (hit) GetWindowThreadProcessId(hit, &process);
        if (process != GetCurrentProcessId()) note_user_input();
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

struct SpeechWord { std::size_t end; std::wstring key; };

bool speech_space(wchar_t value) {
    return std::iswspace(value) != 0 || value == L'\u00a0' ||
        (value >= L'\u2000' && value <= L'\u200a') ||
        value == L'\u2028' || value == L'\u2029' || value == L'\u202f';
}

// Normalize the replacement only: changing the surrounding document would
// invalidate its verified UTF-16 selection offsets and ownership checks.
std::wstring editor_line_breaks(const std::wstring& text, bool windows) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto value = text[index];
        if (value == L'\r' || value == L'\n') {
            if (value == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') ++index;
            if (windows) result += L'\r';
            result += L'\n';
        } else result += value;
    }
    return result;
}

std::vector<SpeechWord> speech_words(const std::wstring& text) {
    std::vector<SpeechWord> words;
    for (std::size_t index = 0; index < text.size();) {
        while (index < text.size() && speech_space(text[index])) ++index;
        if (index == text.size()) break;
        SpeechWord word{};
        while (index < text.size() && !speech_space(text[index])) {
            wchar_t value = text[index++];
            if (std::iswpunct(value) || (value >= L'\u2000' && value <= L'\u206f')) continue;
            if (value >= L'\u00c0' && value <= L'\u00de' && value != L'\u00d7') value += 0x20;
            else value = static_cast<wchar_t>(std::towlower(value));
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

} // namespace

struct ExternalDictationTarget::Impl {
    enum class Kind { Native, Automation, Append };
    struct Snapshot {
        Kind kind = Kind::Append;
        HWND foreground = nullptr;
        HWND focus = nullptr;
        HWND caret_window = nullptr;
        RECT caret = {};
        std::uint64_t input_epoch = 0;
        std::wstring document;
        std::size_t start = 0;
        std::size_t end = 0;
        ComPtr<IUIAutomationElement> element;
        ComPtr<IUIAutomationTextRange> document_range;
    };
    struct Pending {
        Snapshot before;
        std::wstring expected;
        std::size_t caret = 0;
        LiveDictationBuffer buffer;
        LiveDictationBuffer::Replacement replacement;
        int utterance = -1;
        bool final = false;
        std::uint64_t input_epoch = 0;
        std::chrono::steady_clock::time_point dispatched_at;
        std::chrono::steady_clock::time_point progress_at;
        std::size_t observed_prefix_length = 0;
    };
    struct Deferred {
        int utterance;
        std::wstring hypothesis;
        bool final;
    };
    struct OwnedRevision {
        Snapshot current;
        std::size_t start = 0;
        std::wstring raw;
        std::wstring rendered;
        std::size_t revised_source_length = 0;
    };
    struct TemporarySelection {
        Snapshot before;
        std::size_t start = 0;
        std::size_t end = 0;
        ComPtr<IUIAutomationTextRange> original_range;
        bool restore_sent = false;
        bool selection_observed = false;
        bool waiting_for_user = false;
        std::uint64_t resume_after_epoch = 0;
        std::size_t resume_start = 0;
        std::size_t resume_end = 0;
        std::optional<std::wstring> expected_document;
        std::size_t expected_caret = 0;
        std::uint64_t revision_token = 0;
        std::wstring replacement;
        std::size_t source_length = 0;
        std::unique_ptr<ClipboardTextTransaction> clipboard;
    };

    HWND owner = nullptr;
    HWND clipboard_owner = nullptr;
    std::thread input_monitor;
    DWORD input_monitor_thread = 0;
    std::atomic<std::uint64_t> user_input_epoch{0};
    std::atomic<ULONGLONG> last_user_input_tick{0};
    std::atomic<std::uint64_t> toggle_hotkey{
        (static_cast<std::uint64_t>(MOD_CONTROL | MOD_SHIFT) << 32) | VK_SPACE};
    std::shared_ptr<DictationDiagnostics> diagnostics;
    std::thread worker;
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    std::atomic<bool> poll_queued{false};
    mutable std::mutex published_mutex;
    std::string published_text;
    bool published_delivered = false;
    bool published_available = false;

    // Everything below is accessed exclusively on the UIA worker apartment.
    ComPtr<IUIAutomation> automation;
    LiveDictationBuffer buffer;
    bool active = false;
    bool finishing = false;
    std::optional<Snapshot> previous;
    std::optional<Pending> pending;
    std::deque<Deferred> deferred;
    bool retry_update = false;
    std::wstring fallback_document;
    std::wstring transcript;
    int preview_utterance = -1;
    std::wstring previous_hypothesis;
    std::chrono::steady_clock::time_point last_poll{};
    std::optional<OwnedRevision> owned_revision;
    // UIA Select can finish before Chromium publishes the selection. Keep a
    // fence until that selection is consumed or its original caret is restored;
    // otherwise the next spoken word would replace our own delayed selection.
    std::optional<TemporarySelection> temporary_selection;
    std::unique_ptr<ClipboardTextTransaction> clipboard_restore_pending;
    bool selection_blocks_output = false;
    std::uint64_t revision_token = 1;
    std::uint64_t target_token = 1;

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            stopping = true;
        }
        queue_ready.notify_one();
        if (worker.joinable()) worker.join();
        if (input_monitor_thread) PostThreadMessageW(input_monitor_thread, WM_QUIT, 0, 0);
        if (input_monitor.joinable()) input_monitor.join();
    }

    void enqueue(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (stopping) return;
            jobs.push_back(std::move(job));
        }
        queue_ready.notify_one();
    }

    template<class Function>
    auto call(Function function) -> decltype(function()) {
        using Result = decltype(function());
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(function));
        auto result = task->get_future();
        enqueue([task] { (*task)(); });
        return result.get();
    }

    void publish(bool available) {
        std::lock_guard<std::mutex> lock(published_mutex);
        published_available = available;
        published_text = to_utf8(transcript);
        published_delivered = !transcript.empty();
    }

    void trace(const std::string& event, nlohmann::json fields = nlohmann::json::object()) {
        if (diagnostics) diagnostics->write(event, fields);
    }

    nlohmann::json target_fields(const Snapshot& snapshot) const {
        DWORD process = 0;
        GetWindowThreadProcessId(snapshot.foreground, &process);
        wchar_t name[128] = {};
        GetClassNameW(snapshot.focus, name, static_cast<int>(std::size(name)));
        return {{"target_kind", snapshot.kind == Kind::Native ? "native" :
            snapshot.kind == Kind::Automation ? "uia" : "append"},
            {"process_id", process}, {"window_class", to_utf8(name)},
            {"selection_start", snapshot.start}, {"selection_end", snapshot.end},
            {"document_length", snapshot.document.size()}, {"input_epoch", snapshot.input_epoch},
            {"revision_owned", false}};
    }

    bool input_settled() const {
        const auto tick = last_user_input_tick.load(std::memory_order_relaxed);
        return !input_busy() && (tick == 0 || GetTickCount64() - tick >= 80);
    }

    bool initialize(HWND window) {
        owner = window;
        auto monitoring = std::make_shared<std::promise<void>>();
        auto monitor_ready = monitoring->get_future();
        input_monitor = std::thread([this, monitoring] {
            input_monitor_thread = GetCurrentThreadId();
            MSG event{};
            PeekMessageW(&event, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            monitored_epoch = &user_input_epoch;
            monitored_tick = &last_user_input_tick;
            monitored_toggle = &toggle_hotkey;
            const auto module = GetModuleHandleW(nullptr);
            // Clipboard ownership notifications must reach a pumping thread.
            // The main UI is waiting synchronously for this worker's result.
            clipboard_owner = CreateWindowExW(0, L"STATIC", L"VoiceToTextClipboard", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
            const HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_monitor, module, 0);
            const HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_monitor, module, 0);
            monitoring->set_value();
            while (GetMessageW(&event, nullptr, 0, 0) > 0) {
                TranslateMessage(&event);
                DispatchMessageW(&event);
            }
            if (keyboard) UnhookWindowsHookEx(keyboard);
            if (mouse) UnhookWindowsHookEx(mouse);
            if (clipboard_owner) DestroyWindow(clipboard_owner);
            clipboard_owner = nullptr;
            monitored_epoch = nullptr;
            monitored_tick = nullptr;
            monitored_toggle = nullptr;
        });
        monitor_ready.get();
        auto initialized = std::make_shared<std::promise<bool>>();
        auto ready = initialized->get_future();
        worker = std::thread([this, initialized] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(apartment)) {
                CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&automation));
                if (!automation) CoCreateInstance(CLSID_CUIAutomation, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
                ComPtr<IUIAutomation2> configured;
                if (automation && SUCCEEDED(automation.As(&configured))) {
                    configured->put_ConnectionTimeout(300);
                    configured->put_TransactionTimeout(200);
                }
            }
            // Native EDIT and caret-based Unicode append remain available even
            // when the optional UI Automation COM provider is unavailable.
            initialized->set_value(true);
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    if (temporary_selection || clipboard_restore_pending) {
                        queue_ready.wait_for(lock, std::chrono::milliseconds(80),
                            [this] { return stopping || !jobs.empty(); });
                    } else {
                        queue_ready.wait(lock, [this] { return stopping || !jobs.empty(); });
                    }
                    if (stopping && jobs.empty()) break;
                    if (!jobs.empty()) {
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                }
                if (job) job();
                // Cancel/Stop can remove the app's polling timer while Select
                // is still queued in another process. Recover the selection
                // when idle, but never emit queued speech from this path.
                else observe(capture());
            }
            // Give an already dispatched write a final chance to acknowledge
            // consumption before restoring its temporary clipboard payload.
            restore_revision_selection(1000);
            confirm_background_paste();
            if (temporary_selection && temporary_selection->clipboard) {
                // A stalled target can still process Ctrl+V after we exit.
                // Leave the AI text available rather than exposing the user's
                // previous clipboard contents to that unconfirmed paste.
                temporary_selection->clipboard->abandon_without_restore();
                trace("revision_clipboard_retained_on_exit");
            }
            pending.reset();
            previous.reset();
            owned_revision.reset();
            temporary_selection.reset();
            retry_clipboard_restore();
            clipboard_restore_pending.reset();
            automation.Reset();
            if (SUCCEEDED(apartment)) CoUninitialize();
        });
        return ready.get();
    }

    bool same_target(const Snapshot& left, const Snapshot& right) const {
        if (left.foreground != right.foreground || left.focus != right.focus ||
            left.kind != right.kind) return false;
        if (left.element || right.element) {
            if (!left.element || !right.element || !automation) return false;
            BOOL same = FALSE;
            return SUCCEEDED(automation->CompareElements(left.element.Get(),
                right.element.Get(), &same)) && same;
        }
        return true;
    }

    bool owned_matches(const Snapshot& snapshot) const {
        if (!owned_revision || snapshot.kind == Kind::Append) return false;
        const auto& owned = *owned_revision;
        const auto end = owned.start + owned.rendered.size();
        return same_target(owned.current, snapshot) &&
            owned.current.document == snapshot.document &&
            owned.current.input_epoch == snapshot.input_epoch &&
            snapshot.start == end && snapshot.end == end &&
            owned.start <= snapshot.document.size() &&
            owned.rendered.size() <= snapshot.document.size() - owned.start &&
            snapshot.document.compare(owned.start, owned.rendered.size(), owned.rendered) == 0;
    }

    void revoke_revision(const char* reason) {
        if (!owned_revision) return;
        trace("revision_skipped", {{"reason", reason}, {"revision_token", revision_token},
            {"owned_length", owned_revision->rendered.size()}});
        owned_revision.reset();
        ++revision_token;
    }

    void track_confirmed(const Pending& operation, const Snapshot& observed) {
        if (observed.kind == Kind::Append || operation.replacement.text.empty()) {
            revoke_revision("unreadable_or_empty_output");
            return;
        }
        if (operation.replacement.text.size() > kRevisionLimit) {
            revoke_revision("revision_chunk_too_large");
            return;
        }
        const bool extends = owned_matches(operation.before) &&
            operation.replacement.length == 0 &&
            operation.replacement.start == owned_revision->start + owned_revision->rendered.size() &&
            owned_revision->raw.size() + operation.replacement.text.size() <= kRevisionLimit;
        if (!extends) {
            revoke_revision("new_insertion_span");
            ++revision_token;
            owned_revision.emplace();
            owned_revision->start = operation.replacement.start;
        }
        owned_revision->raw += operation.replacement.text;
        owned_revision->rendered += operation.replacement.text;
        owned_revision->current = observed;
    }

    bool native_snapshot(Snapshot& result, bool& rejected) {
        wchar_t name[128] = {};
        GetClassNameW(result.focus, name, static_cast<int>(std::size(name)));
        std::wstring kind = name;
        std::transform(kind.begin(), kind.end(), kind.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        // RichEdit's WM_GETTEXT and EM_GETSEL use different paragraph units.
        // Use UIA from the first character so adding a paragraph never changes
        // the coordinate model in the middle of an owned replacement.
        if (kind != L"edit") return false;
        const LONG_PTR style = GetWindowLongPtrW(result.focus, GWL_STYLE);
        if ((style & (ES_READONLY | ES_PASSWORD)) != 0 || !IsWindowEnabled(result.focus)) {
            rejected = true;
            return false;
        }
        if (!IsWindowUnicode(result.focus)) return false;
        DWORD_PTR length = 0;
        if (!message(result.focus, WM_GETTEXTLENGTH, 0, 0, &length) ||
            length > kDocumentLimit) return false;
        std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
        DWORD_PTR copied = 0;
        if (!message(result.focus, WM_GETTEXT, value.size(),
                reinterpret_cast<LPARAM>(value.data()), &copied) || copied > length) return false;
        value.resize(static_cast<std::size_t>(copied));
        DWORD start = 0;
        DWORD end = 0;
        if (!message(result.focus, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                reinterpret_cast<LPARAM>(&end))) return false;
        DWORD_PTR length_after = 0;
        if (!message(result.focus, WM_GETTEXTLENGTH, 0, 0, &length_after) ||
            copied != length || length_after != length ||
            start > value.size() || end > value.size()) {
            // A growing control can truncate WM_GETTEXT to the buffer sized by
            // the earlier length read. That mixed snapshot falsely looks like
            // an unrelated suffix edit. Retry it as unreadable instead of
            // falling back to an opaque target or discarding owned context.
            rejected = true;
            return false;
        }
        result.kind = Kind::Native;
        result.document = std::move(value);
        result.start = (std::min)(start, end);
        result.end = (std::max)(start, end);
        return true;
    }

    bool automation_snapshot(Snapshot& result, bool& rejected) {
        if (!automation) return false;
        ComPtr<IUIAutomationElement> element;
        if (FAILED(automation->GetFocusedElement(&element)) || !element) return false;
        BOOL focused = FALSE;
        BOOL enabled = FALSE;
        BOOL password = FALSE;
        int process = 0;
        CONTROLTYPEID type = 0;
        if (FAILED(element->get_CurrentHasKeyboardFocus(&focused)) || !focused ||
            FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled ||
            FAILED(element->get_CurrentIsPassword(&password)) || password ||
            FAILED(element->get_CurrentProcessId(&process)) ||
            static_cast<DWORD>(process) == GetCurrentProcessId()) {
            rejected = true;
            return false;
        }
        element->get_CurrentControlType(&type);
        bool writable = false;
        ComPtr<IUIAutomationValuePattern> value;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,
                IID_PPV_ARGS(&value))) && value) {
            BOOL readonly = TRUE;
            if (SUCCEEDED(value->get_CurrentIsReadOnly(&readonly))) {
                if (readonly) {
                    rejected = true;
                    return false;
                }
                writable = true;
            }
        }
        ComPtr<IUIAutomationTextPattern> text;
        if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId,
                IID_PPV_ARGS(&text))) || !text) {
            if (writable && type == UIA_EditControlTypeId) {
                result.kind = Kind::Append;
                result.element = std::move(element);
                return true;
            }
            return false;
        }
        ComPtr<IUIAutomationTextRange> document_range;
        if (FAILED(text->get_DocumentRange(&document_range)) || !document_range) return false;
        VARIANT readonly;
        VariantInit(&readonly);
        if (SUCCEEDED(document_range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readonly)) &&
            readonly.vt == VT_BOOL) {
            if (readonly.boolVal == VARIANT_TRUE) {
                VariantClear(&readonly);
                rejected = true;
                return false;
            }
            writable = true;
        }
        VariantClear(&readonly);
        if (!writable && type != UIA_EditControlTypeId) return false;
        ComPtr<IUIAutomationTextRangeArray> ranges;
        int count = 0;
        ComPtr<IUIAutomationTextRange> selection;
        if (FAILED(text->GetSelection(&ranges)) || !ranges ||
            FAILED(ranges->get_Length(&count)) || count != 1 ||
            FAILED(ranges->GetElement(0, &selection)) || !selection) return false;
        std::wstring document;
        if (!range_text(document_range.Get(), document)) return false;
        const auto offset = [&](TextPatternRangeEndpoint endpoint,
                                std::size_t& result_offset) {
            ComPtr<IUIAutomationTextRange> prefix;
            std::wstring prefix_text;
            if (FAILED(document_range->Clone(&prefix)) ||
                FAILED(prefix->MoveEndpointByRange(TextPatternRangeEndpoint_End,
                    selection.Get(), endpoint)) || !range_text(prefix.Get(), prefix_text) ||
                prefix_text.size() > document.size() ||
                document.compare(0, prefix_text.size(), prefix_text) != 0) return false;
            result_offset = prefix_text.size();
            return true;
        };
        if (!offset(TextPatternRangeEndpoint_Start, result.start) ||
            !offset(TextPatternRangeEndpoint_End, result.end)) return false;
        result.kind = Kind::Automation;
        result.element = std::move(element);
        result.document_range = std::move(document_range);
        result.document = std::move(document);
        return true;
    }

    std::optional<Snapshot> capture() {
        Snapshot result;
        result.input_epoch = user_input_epoch.load(std::memory_order_relaxed);
        result.foreground = GetForegroundWindow();
        if (!result.foreground) return std::nullopt;
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(result.foreground, &process);
        // Never ask UIA to call back into our own window-owning UI thread.
        if (!thread || process == GetCurrentProcessId() ||
            result.foreground == owner) return std::nullopt;
        GUITHREADINFO gui = {};
        gui.cbSize = sizeof(gui);
        if (!GetGUIThreadInfo(thread, &gui)) return std::nullopt;
        result.focus = gui.hwndFocus;
        result.caret_window = gui.hwndCaret;
        result.caret = gui.rcCaret;
        if (!result.focus || !IsWindow(result.focus)) return std::nullopt;
        bool rejected = false;
        const auto current = [&]() -> std::optional<Snapshot> {
            if (result.input_epoch != user_input_epoch.load(std::memory_order_relaxed) ||
                GetForegroundWindow() != result.foreground) return std::nullopt;
            return result;
        };
        if (native_snapshot(result, rejected)) return current();
        if (rejected) return std::nullopt;
        if (automation_snapshot(result, rejected)) {
            if (result.kind == Kind::Append) {
                result.document = fallback_document;
                result.start = result.end = fallback_document.size();
            }
            return current();
        }
        if (rejected || !gui.hwndCaret ||
            !(gui.hwndCaret == gui.hwndFocus || IsChild(gui.hwndFocus, gui.hwndCaret))) {
            return std::nullopt;
        }
        // A native text caret is evidence that Unicode text can be entered.
        // With no readable selection/document, we never revise existing text.
        result.kind = Kind::Append;
        result.document = fallback_document;
        result.start = result.end = fallback_document.size();
        return current();
    }

    void record(const Pending& delivered) {
        if (!transcript.empty() && !delivered.replacement.text.empty() &&
            !std::iswspace(transcript.back()) &&
            !std::iswspace(delivered.replacement.text.front())) transcript += L' ';
        transcript += delivered.replacement.text;
    }

    void consume_pending(const std::string& outcome, const Snapshot* observed = nullptr) {
        if (!pending) return;
        auto fields = target_fields(pending->before);
        fields["utterance_id"] = pending->utterance;
        fields["readback"] = outcome;
        fields["dispatched_text"] = to_utf8(pending->replacement.text);
        if (observed) {
            fields["selection_after_start"] = observed->start;
            fields["selection_after_end"] = observed->end;
            fields["input_epoch_after"] = observed->input_epoch;
            fields["document_length_after"] = observed->document.size();
            // Infer a short delivery only when the unchanged surrounding text
            // and the middle's exact match to our own output prove its origin.
            // Never record an arbitrary substring of the user's document.
            if (same_target(pending->before, *observed) &&
                observed->input_epoch == pending->input_epoch &&
                (observed->document != pending->before.document ||
                    outcome == "confirmed")) {
                const auto start = pending->replacement.start;
                const auto old_end = start + pending->replacement.length;
                const auto suffix = pending->before.document.size() - old_end;
                if (observed->document.size() >= start + suffix &&
                    observed->document.compare(0, start, pending->before.document, 0, start) == 0 &&
                    observed->document.compare(observed->document.size() - suffix, suffix,
                        pending->before.document, old_end, suffix) == 0) {
                    const auto length = observed->document.size() - start - suffix;
                    if (length <= pending->replacement.text.size() &&
                        observed->document.compare(start, length, pending->replacement.text, 0, length) == 0) {
                        pending->replacement.text.resize(length);
                        fields["confirmed_written_text"] = to_utf8(pending->replacement.text);
                    }
                }
            }
        }
        trace("output_readback", std::move(fields));
        if (outcome == "confirmed" && observed) track_confirmed(*pending, *observed);
        else revoke_revision("output_not_verified");
        // Never replay a dispatched batch, including a short/unconfirmed one.
        buffer = pending->buffer;
        buffer.seal_current_insertion();
        record(*pending);
        pending.reset();
    }

    bool pending_readback_window() const {
        if (!pending) return false;
        const auto now = std::chrono::steady_clock::now();
        return now - pending->progress_at < std::chrono::milliseconds(500) &&
            now - pending->dispatched_at < std::chrono::seconds(5);
    }

    bool pending_target_unchanged() const {
        if (!pending || pending->input_epoch != user_input_epoch.load(std::memory_order_relaxed) ||
            pending->before.foreground != GetForegroundWindow()) return false;
        const DWORD thread = GetWindowThreadProcessId(pending->before.foreground, nullptr);
        GUITHREADINFO gui = {};
        gui.cbSize = sizeof(gui);
        return thread && GetGUIThreadInfo(thread, &gui) && gui.hwndFocus == pending->before.focus;
    }

    bool empty_automation_paragraph(const Pending& operation) const {
        // Chromium can expose an empty editor's placeholder paragraph as a
        // newline which disappears on the first keystroke. Only this empty
        // document case may omit a surrounding character during readback.
        return operation.before.kind == Kind::Automation &&
            operation.before.start == 0 && operation.before.end == 0 &&
            operation.replacement.start == 0 && operation.replacement.length == 0 &&
            (operation.before.document == L"\n" || operation.before.document == L"\r\n");
    }

    bool pending_document_complete(const Snapshot& snapshot) const {
        return snapshot.document == pending->expected ||
            (empty_automation_paragraph(*pending) &&
                snapshot.document == pending->replacement.text);
    }

    std::optional<std::size_t> pending_document_progress(const Snapshot& snapshot) const {
        const auto& operation = *pending;
        if (snapshot.document == operation.before.document) return 0;
        const auto start = operation.replacement.start;
        const auto old_end = start + operation.replacement.length;
        const auto suffix = operation.before.document.size() - old_end;
        // SendInput and UIA readback are asynchronous. A document containing
        // only a prefix of our batch, with both surrounding spans unchanged,
        // is progress, not an unrelated edit or a permanently short write.
        if (snapshot.document.size() >= start + suffix &&
            snapshot.document.compare(0, start, operation.before.document, 0, start) == 0 &&
            snapshot.document.compare(snapshot.document.size() - suffix, suffix,
                operation.before.document, old_end, suffix) == 0) {
            const auto length = snapshot.document.size() - start - suffix;
            if (length <= operation.replacement.text.size() &&
                snapshot.document.compare(start, length, operation.replacement.text, 0, length) == 0)
                return length;
        }
        if (empty_automation_paragraph(operation) &&
            snapshot.document.size() <= operation.replacement.text.size() &&
            operation.replacement.text.compare(0, snapshot.document.size(), snapshot.document) == 0)
            return snapshot.document.size();
        return std::nullopt;
    }

    void observe(std::optional<Snapshot> snapshot) {
        retry_clipboard_restore();
        if (temporary_selection && temporary_selection->clipboard &&
            (!snapshot || !same_target(temporary_selection->before, *snapshot)))
            confirm_background_paste();
        selection_blocks_output = temporary_selection && !recover_revision_selection(snapshot);
        if (selection_blocks_output) {
            // Never publish our own selection as a user-selected replacement
            // range, including when the provider has not exposed it yet.
            publish(false);
            return;
        }
        if (!snapshot) {
            // A UIA provider may briefly fail while its document and selection
            // snapshots catch up with injected keystrokes. Keep the in-flight
            // batch fenced; only an exact final readback can retain ownership.
            if (pending_readback_window() && pending_target_unchanged()) {
                publish(false);
                return;
            }
            consume_pending("target_unavailable");
            revoke_revision("target_unavailable");
            buffer.seal_current_insertion();
            if (previous) ++target_token;
            previous.reset();
            publish(false);
            return;
        }
        bool own_write_observed = false;
        if (pending) {
            const bool same = same_target(pending->before, *snapshot);
            const auto progress = same && snapshot->input_epoch == pending->input_epoch
                ? pending_document_progress(*snapshot) : std::nullopt;
            if (progress && *progress > pending->observed_prefix_length) {
                pending->observed_prefix_length = *progress;
                pending->progress_at = std::chrono::steady_clock::now();
            }
            if (same && pending_document_complete(*snapshot) &&
                snapshot->start == pending->caret && snapshot->end == pending->caret) {
                own_write_observed = snapshot->input_epoch == pending->input_epoch;
                consume_pending(snapshot->input_epoch == pending->input_epoch
                    ? "confirmed" : "confirmed_after_user_input", &*snapshot);
            } else if (pending_readback_window() && pending_target_unchanged() &&
                       snapshot->input_epoch == pending->input_epoch &&
                       (progress.has_value() ||
                           (pending->before.kind == Kind::Automation && snapshot->kind == Kind::Append &&
                               snapshot->foreground == pending->before.foreground &&
                               snapshot->focus == pending->before.focus))) {
                publish(true);
                return;
            } else {
                consume_pending(same ? "changed_or_unconfirmed" : "focus_changed", &*snapshot);
            }
        }
        if (owned_revision && !owned_matches(*snapshot)) {
            revoke_revision("caret_input_or_document_changed");
        }
        if (!previous || !same_target(*previous, *snapshot)) {
            ++target_token;
            buffer.seal_current_insertion();
            if (snapshot->kind == Kind::Append) {
                fallback_document.clear();
                snapshot->document.clear();
                snapshot->start = snapshot->end = 0;
            }
            trace("target_changed", target_fields(*snapshot));
        } else if (previous->document != snapshot->document ||
                   previous->start != snapshot->start || previous->end != snapshot->end ||
                   previous->input_epoch != snapshot->input_epoch ||
                   (snapshot->kind == Kind::Append &&
                    !EqualRect(&previous->caret, &snapshot->caret))) {
            if (!own_write_observed) ++target_token;
            buffer.seal_current_insertion();
            auto fields = target_fields(*snapshot);
            fields["previous_selection_start"] = previous->start;
            fields["previous_selection_end"] = previous->end;
            trace("editor_caret_or_text_changed", std::move(fields));
            if (snapshot->kind == Kind::Append) {
                // We cannot read this editor. After user movement, previous
                // synthetic text says nothing about the new insertion context.
                fallback_document.clear();
                snapshot->document.clear();
                snapshot->start = snapshot->end = 0;
            }
        }
        buffer.observe_editor(snapshot->document, snapshot->start, snapshot->end);
        previous = std::move(snapshot);
        publish(true);
    }

    bool verify(const Snapshot& before) {
        if (!input_settled() || before.input_epoch != user_input_epoch.load(std::memory_order_relaxed)) {
            return false;
        }
        auto current = capture();
        return current && same_target(before, *current) &&
            before.document == current->document && before.start == current->start &&
            before.end == current->end && before.input_epoch == current->input_epoch;
    }

    bool send_unicode(const Snapshot& snapshot, const LiveDictationBuffer::Replacement& replacement,
                      bool& complete) {
        complete = false;
        if (replacement.text.empty() || replacement.text.find_first_of(L"\r\n\t") != std::wstring::npos ||
            !input_settled() ||
            snapshot.input_epoch != user_input_epoch.load(std::memory_order_relaxed) ||
            GetForegroundWindow() != snapshot.foreground) return false;
        std::vector<INPUT> input;
        input.reserve(replacement.text.size() * 2);
        for (wchar_t value : replacement.text) {
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wScan = static_cast<WORD>(value);
            event.ki.dwFlags = KEYEVENTF_UNICODE;
            input.push_back(event);
            event.ki.dwFlags |= KEYEVENTF_KEYUP;
            input.push_back(event);
        }
        // Windows types at the current caret and replaces only a user selection.
        // No selection calls, backspaces or synthetic caret movement are needed.
        if (!input_settled() || snapshot.input_epoch != user_input_epoch.load(std::memory_order_relaxed) ||
            GetForegroundWindow() != snapshot.foreground) return false;
        const auto sent = SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
        complete = sent == input.size();
        return sent != 0;
    }

    bool send_paste(const Snapshot& snapshot, ClipboardTextTransaction& clipboard, bool& complete) {
        complete = false;
        if (!verify(snapshot)) {
            trace("revision_paste_rejected", {{"reason", "selection_or_input_changed"}});
            return false;
        }
        if (!clipboard.owns_clipboard()) {
            trace("revision_paste_rejected", {{"reason", "clipboard_ownership_changed"},
                {"clipboard_status", clipboard.status()}});
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        const bool dispatched = send_paste_shortcut(complete);
        trace(dispatched ? "revision_paste_dispatched" : "revision_paste_rejected",
            {{"reason", dispatched ? "clipboard_text" : "send_input_rejected"},
                {"complete", complete}, {"win32_error", GetLastError()}});
        return dispatched;
    }

    void retry_clipboard_restore() {
        if (!clipboard_restore_pending) return;
        clipboard_restore_pending->restore();
        if (!clipboard_restore_pending->owns_clipboard()) clipboard_restore_pending.reset();
    }

    void retire_clipboard(std::unique_ptr<ClipboardTextTransaction> clipboard) {
        if (!clipboard) return;
        clipboard_restore_pending = std::move(clipboard);
        retry_clipboard_restore();
    }

    bool revision_write_matches(const Snapshot& observed, bool require_caret = true) {
        if (!temporary_selection || !temporary_selection->expected_document) return false;
        auto& selection = *temporary_selection;
        if (!same_target(selection.before, observed)) return false;
        if (observed.document == *selection.expected_document &&
            (!require_caret || (observed.start == selection.expected_caret && observed.end == selection.expected_caret)))
            return true;
        if (selection.before.kind != Kind::Automation) return false;
        // UIA providers can expose pasted CRLF as LF or CR. Accept only that
        // representation difference inside our range; surrounding text and
        // the collapsed final caret must still match exactly.
        const auto& before = selection.before.document;
        const auto suffix = before.size() - selection.end;
        if (observed.document.size() < selection.start + suffix ||
            observed.document.compare(0, selection.start, before, 0, selection.start) != 0 ||
            observed.document.compare(observed.document.size() - suffix, suffix,
                before, selection.end, suffix) != 0) return false;
        const auto length = observed.document.size() - selection.start - suffix;
        const auto caret = selection.start + length;
        if (require_caret && (observed.start != caret || observed.end != caret)) return false;
        auto replacement = observed.document.substr(selection.start, length);
        if (editor_line_breaks(replacement, false) != editor_line_breaks(selection.replacement, false))
            return false;
        selection.replacement = std::move(replacement);
        selection.expected_document = observed.document;
        selection.expected_caret = caret;
        return true;
    }

    void confirm_background_paste() {
        if (!temporary_selection || !temporary_selection->clipboard ||
            !temporary_selection->before.element) return;
        ComPtr<IUIAutomationTextPattern> text;
        ComPtr<IUIAutomationTextRange> document;
        const auto& before = temporary_selection->before;
        if (FAILED(before.element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) ||
            !text || FAILED(text->get_DocumentRange(&document)) || !document) return;
        Snapshot observed = before;
        if (range_text(document.Get(), observed.document) && revision_write_matches(observed, false)) {
            // Reading the exact complete document proves consumption even
            // after a focus change. It does not authorize further rewriting.
            retire_clipboard(std::move(temporary_selection->clipboard));
        }
    }

    // UIA character units are provider-defined (and need not be UTF-16 code
    // units). Validate both the prefix and selected text before trusting them.
    ComPtr<IUIAutomationTextRange> exact_range(const Snapshot& snapshot,
                                              std::size_t start, std::size_t end) {
        ComPtr<IUIAutomationTextRange> range;
        if (!snapshot.document_range || start > end || end > snapshot.document.size() ||
            FAILED(snapshot.document_range->Clone(&range)) || !range ||
            FAILED(range->MoveEndpointByRange(TextPatternRangeEndpoint_End,
                snapshot.document_range.Get(), TextPatternRangeEndpoint_Start))) return {};
        int moved = 0;
        if (start > 0 && FAILED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_End,
                TextUnit_Character, static_cast<int>(start), &moved))) return {};
        std::wstring prefix;
        if (!range_text(range.Get(), prefix) || prefix != snapshot.document.substr(0, start) ||
            FAILED(range->MoveEndpointByRange(TextPatternRangeEndpoint_Start,
                range.Get(), TextPatternRangeEndpoint_End))) return {};
        if (end > start && FAILED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_End,
                TextUnit_Character, static_cast<int>(end - start), &moved))) return {};
        std::wstring selected;
        if (!range_text(range.Get(), selected) ||
            selected != snapshot.document.substr(start, end - start)) return {};
        return range;
    }

    bool selection_is(const Snapshot& before, const Snapshot& selected,
                      std::size_t start, std::size_t end) const {
        return same_target(before, selected) && before.document == selected.document &&
            before.input_epoch == selected.input_epoch && selected.start == start && selected.end == end;
    }

    bool finish_revision_write(const Snapshot& observed) {
        if (!revision_write_matches(observed)) return false;
        const auto& selection = *temporary_selection;
        if (!same_target(selection.before, observed) ||
            observed.document != *selection.expected_document ||
            observed.start != selection.expected_caret || observed.end != selection.expected_caret)
            return false;
        const bool retains_ownership = owned_revision && revision_token == selection.revision_token &&
            observed.input_epoch == selection.before.input_epoch &&
            observed.input_epoch == user_input_epoch.load(std::memory_order_relaxed);
        if (retains_ownership) {
            owned_revision->rendered = selection.replacement;
            owned_revision->current = observed;
            owned_revision->revised_source_length = selection.source_length;
            buffer.seal_current_insertion();
            buffer.observe_editor(observed.document, observed.start, observed.end);
            previous = observed;
            publish(true);
            trace("revision_applied", {{"revision_token", selection.revision_token},
                {"source_characters", selection.source_length},
                {"rendered_characters", owned_revision->rendered.size()},
                {"preserved_tail_characters", owned_revision->raw.size() - selection.source_length},
                {"target_kind", selection.before.kind == Kind::Native ? "native" : "uia"}});
        }
        // The exact complete write consumed our temporary selection even if a
        // subsequent real edit or new session has revoked its ownership.
        retire_clipboard(std::move(temporary_selection->clipboard));
        temporary_selection.reset();
        selection_blocks_output = false;
        return retains_ownership;
    }

    bool recover_revision_selection(const std::optional<Snapshot>& observed) {
        if (!temporary_selection) return true;
        auto& selection = *temporary_selection;
        const auto& before = selection.before;
        bool different_target = observed && (before.foreground != observed->foreground ||
            before.focus != observed->focus);
        if (observed && before.element && observed->element && automation) {
            BOOL same = TRUE;
            if (SUCCEEDED(automation->CompareElements(before.element.Get(), observed->element.Get(), &same)) &&
                !same) different_target = true;
        }
        // A switch to another field does not cancel a queued Select in the
        // original field. Retain its fence for a later return while allowing
        // ordinary speech at the new target; additional revisions wait.
        if (different_target) return true;
        if (!observed || !same_target(before, *observed) ||
            GetForegroundWindow() != before.foreground) return false;
        const auto epoch = user_input_epoch.load(std::memory_order_relaxed);
        if (epoch != observed->input_epoch) return false;
        if (revision_write_matches(*observed, false)) {
            retire_clipboard(std::move(selection.clipboard));
            if (epoch != before.input_epoch) {
                revoke_revision("user_caret_after_paste");
                temporary_selection.reset();
                return true;
            }
        }
        if (revision_write_matches(*observed)) {
            // Send may also be asynchronous. A late, exact full readback is
            // proof that the selection was consumed; never replay the write.
            trace("revision_late_readback_confirmed");
            finish_revision_write(*observed);
            return true;
        }
        const bool original_document = before.document == observed->document;
        const bool own_range = original_document && observed->start == selection.start &&
            observed->end == selection.end;
        const bool restored_range = original_document && observed->start == before.start &&
            observed->end == before.end;
        if (epoch != before.input_epoch) {
            // New physical input is not proof that an asynchronous Select was
            // cancelled. Do not restore the old caret over a newer user edit.
            if (selection.waiting_for_user) {
                if (epoch > selection.resume_after_epoch &&
                    (observed->start != selection.resume_start ||
                        observed->end != selection.resume_end || !original_document)) {
                    trace("revision_selection_released", {{"reason", "fresh_user_caret"}});
                    temporary_selection.reset();
                    return true;
                }
                return false;
            }
            if (!selection.expected_document && selection.selection_observed &&
                !selection.restore_sent && !own_range) {
                trace("revision_selection_released", {{"reason", "user_caret_after_selection"}});
                temporary_selection.reset();
                return true;
            }
            if ((!selection.expected_document && !selection.restore_sent && own_range) ||
                (!selection.expected_document && selection.restore_sent && restored_range)) {
                selection.selection_observed = true;
                selection.waiting_for_user = true;
                selection.resume_after_epoch = epoch;
                selection.resume_start = observed->start;
                selection.resume_end = observed->end;
                trace("revision_selection_waiting_for_user", {{"reason", "input_during_pending_selection"}});
            }
            return false;
        }
        // Once a replacement was dispatched, restoring the caret could race
        // that queued write and make it insert at the wrong location. Keep
        // output fenced until its exact expected result is observed instead.
        if (selection.expected_document || !original_document) return false;
        if (selection.restore_sent &&
            selection_is(before, *observed, before.start, before.end)) {
            trace("revision_selection_restored");
            temporary_selection.reset();
            return true;
        }
        if (!selection.restore_sent &&
            selection_is(before, *observed, selection.start, selection.end)) {
            selection.selection_observed = true;
            if (!input_settled() ||
                before.input_epoch != user_input_epoch.load(std::memory_order_relaxed)) return false;
            selection.restore_sent = before.kind == Kind::Native
                ? message(before.focus, EM_SETSEL, before.start, before.end)
                : selection.original_range && SUCCEEDED(selection.original_range->Select());
            if (selection.restore_sent) trace("revision_selection_restore_requested");
        }
        // Seeing the original caret before the delayed Select arrives is not
        // proof of restoration. Keep fencing output until our selection has
        // appeared and the subsequent restore has itself been read back.
        return false;
    }

    void restore_revision_selection(unsigned milliseconds = 250) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(milliseconds);
        while (temporary_selection) {
            if (recover_revision_selection(capture())) return;
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        if (temporary_selection) trace("revision_selection_restore_pending");
    }

    std::optional<Snapshot> wait_for_revision_selection(const Snapshot& before,
                                                       std::size_t start, std::size_t end) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        do {
            auto selected = capture();
            if (before.input_epoch != user_input_epoch.load(std::memory_order_relaxed) ||
                (selected && (!same_target(before, *selected) ||
                    before.document != selected->document))) return std::nullopt;
            if (selected && selection_is(before, *selected, start, end) && input_settled()) {
                if (temporary_selection) temporary_selection->selection_observed = true;
                return selected;
            }
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        } while (true);
        return std::nullopt;
    }

    ExternalDictationTarget::DictationRevision revision_context() {
        settle_pending(120);
        observe(capture());
        if (finishing) {
            drain_deferred();
            complete_finishing();
        }
        ExternalDictationTarget::DictationRevision result;
        result.token = revision_token;
        result.target_token = target_token;
        result.available = previous.has_value() && !selection_blocks_output;
        result.readable_target = previous && previous->kind != Kind::Append;
        result.pending_output = selection_blocks_output || pending.has_value() || !deferred.empty() ||
            clipboard_restore_pending != nullptr;
        if (owned_revision && !pending && !temporary_selection) {
            result.raw_text = to_utf8(owned_revision->raw);
            result.can_replace = true;
        }
        return result;
    }

    bool revise(std::uint64_t token, const std::optional<std::wstring>& source,
                const std::wstring& transformed) {
        settle_pending(120);
        observe(capture());
        const auto skip = [&](const char* reason) {
            trace("revision_skipped", {{"reason", reason}, {"revision_token", token}});
            return false;
        };
        const auto terminal_skip = [&](const char* reason) {
            revoke_revision(reason);
            return skip(reason);
        };
        if (temporary_selection) return skip("selection_restore_pending");
        if (clipboard_restore_pending) return skip("clipboard_restore_pending");
        if (token != revision_token || !owned_revision || pending || !previous) {
            return skip("ownership_expired");
        }
        const auto source_raw = source ? *source : owned_revision->raw;
        if (source_raw.empty() || source_raw.size() > owned_revision->raw.size() ||
            owned_revision->raw.compare(0, source_raw.size(), source_raw) != 0) {
            return terminal_skip("raw_prefix_changed");
        }
        if (source_raw.size() < owned_revision->revised_source_length) {
            return skip("older_prefix_result");
        }
        std::size_t first = 0;
        std::size_t last = transformed.size();
        while (first < last && speech_space(transformed[first])) ++first;
        while (last > first && speech_space(transformed[last - 1])) --last;
        if (first == last) return terminal_skip("empty_revision");
        std::size_t prefix_end = 0;
        std::size_t suffix_start = source_raw.size();
        while (prefix_end < source_raw.size() && speech_space(source_raw[prefix_end])) ++prefix_end;
        while (suffix_start > prefix_end && speech_space(source_raw[suffix_start - 1])) --suffix_start;
        std::wstring replacement = source_raw.substr(0, prefix_end);
        replacement += transformed.substr(first, last - first);
        replacement += source_raw.substr(suffix_start);
        replacement += owned_revision->raw.substr(source_raw.size());
        replacement = editor_line_breaks(replacement, previous->kind == Kind::Native ||
            previous->document.find(L"\r\n") != std::wstring::npos);
        if (replacement.size() > kDocumentLimit) return terminal_skip("revision_too_large");
        if (replacement == owned_revision->rendered ||
            (previous->kind == Kind::Automation &&
                editor_line_breaks(replacement, false) == editor_line_breaks(owned_revision->rendered, false))) {
            // A provider's CR/LF representation alone must never trigger a
            // paste: its unchanged document could otherwise look like early
            // acknowledgement of a replacement that is still queued.
            owned_revision->revised_source_length = source_raw.size();
            return true;
        }
        Snapshot before = *previous;
        if (!owned_matches(before) || !verify(before)) return skip("caret_or_input_changed");
        const auto start = owned_revision->start;
        const auto end = start + owned_revision->rendered.size();
        ComPtr<IUIAutomationTextRange> range;
        ComPtr<IUIAutomationTextRange> original_range;
        if (before.kind == Kind::Automation) {
            range = exact_range(before, start, end);
            original_range = exact_range(before, before.start, before.end);
            if (!range || !original_range) return terminal_skip("uia_exact_range_unavailable");
        }
        std::unique_ptr<ClipboardTextTransaction> clipboard;
        if (before.kind == Kind::Automation && replacement.find_first_of(L"\r\n\t") != std::wstring::npos) {
            clipboard = std::make_unique<ClipboardTextTransaction>();
            // Clipboard text preserves line breaks and indentation without
            // sending Enter (submit) or Tab (change focus) to a chat editor.
            if (!clipboard->publish(clipboard_owner, editor_line_breaks(replacement, true))) {
                const auto failure = clipboard->failure_info();
                trace("revision_clipboard_unavailable", {{"reason", clipboard->status()},
                    {"format_id", clipboard->failure_format()},
                    {"format_reason", clipboard->failure_reason()}, {"format_bytes", failure.bytes},
                    {"format_flags", failure.global_flags}, {"data_error", failure.data_error},
                    {"size_error", failure.size_error}, {"flags_error", failure.flags_error}});
                retire_clipboard(std::move(clipboard));
                return skip("formatted_paste_unavailable");
            }
        }
        if (!verify(before)) {
            retire_clipboard(std::move(clipboard));
            return skip("caret_or_input_changed");
        }
        temporary_selection = TemporarySelection{before, start, end, original_range};
        selection_blocks_output = true;
        const bool selected = before.kind == Kind::Native
            ? message(before.focus, EM_SETSEL, start, end)
            : SUCCEEDED(range->Select());
        if (!selected) {
            retire_clipboard(std::move(clipboard));
            restore_revision_selection();
            observe(capture());
            return skip("selection_rejected");
        }
        auto selected_snapshot = wait_for_revision_selection(before, start, end);
        if (!selected_snapshot) {
            retire_clipboard(std::move(clipboard));
            restore_revision_selection();
            observe(capture());
            return skip("selection_or_input_changed");
        }
        std::wstring expected = before.document;
        expected.replace(start, end - start, replacement);
        const auto caret = start + replacement.size();
        bool complete = false;
        bool dispatched = false;
        if (before.kind == Kind::Native) {
            // EM_REPLACESEL replaces this exact owned range and preserves the
            // surrounding document and the editor's undo history.
            if (verify(*selected_snapshot)) {
                dispatched = message(before.focus, EM_REPLACESEL, TRUE,
                    reinterpret_cast<LPARAM>(replacement.c_str()));
                complete = dispatched;
            }
        } else if (clipboard) {
            dispatched = send_paste(*selected_snapshot, *clipboard, complete);
        } else {
            LiveDictationBuffer::Replacement change{start, end - start, replacement, caret};
            if (verify(*selected_snapshot)) dispatched = send_unicode(*selected_snapshot, change, complete);
        }
        if (!dispatched) {
            retire_clipboard(std::move(clipboard));
            restore_revision_selection();
            observe(capture());
            return skip("revision_send_rejected");
        }
        temporary_selection->expected_document = expected;
        temporary_selection->clipboard = std::move(clipboard);
        temporary_selection->expected_caret = caret;
        temporary_selection->revision_token = token;
        temporary_selection->replacement = replacement;
        temporary_selection->source_length = source_raw.size();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        std::optional<Snapshot> observed;
        do {
            observed = capture();
            if (!observed || !same_target(before, *observed) ||
                observed->input_epoch != before.input_epoch) break;
            if (revision_write_matches(*observed)) break;
            if (!complete) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!observed || !revision_write_matches(*observed) ||
            observed->input_epoch != before.input_epoch) {
            restore_revision_selection();
            // Recovery may have confirmed the complete replacement after the
            // first readback failed. Keep its raw source rather than throwing
            // away context and transforming the next spoken word in isolation.
            const auto recovered = [&] {
                return !temporary_selection && owned_revision && revision_token == token &&
                    editor_line_breaks(owned_revision->rendered, false) == editor_line_breaks(replacement, false) &&
                    owned_revision->revised_source_length == source_raw.size() &&
                    previous && owned_matches(*previous);
            };
            if (recovered()) return true;
            observe(capture());
            if (recovered()) return true;
            if (before.input_epoch != user_input_epoch.load(std::memory_order_relaxed))
                revoke_revision("revision_input_changed");
            return skip("revision_readback_pending");
        }
        const bool committed = finish_revision_write(*observed);
        publish(true);
        return committed;
    }

    void settle_pending(unsigned milliseconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (pending && std::chrono::steady_clock::now() < deadline) {
            observe(capture());
            if (pending) std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }

    bool update(int utterance, const std::wstring& hypothesis, bool final,
                std::optional<std::uint64_t> required_target_token = std::nullopt) {
        retry_update = false;
        if (!active) return false;
        retry_update = true;
        settle_pending(120);
        observe(capture());
        if (required_target_token && *required_target_token != target_token) return false;
        if (!previous || pending || selection_blocks_output || !input_settled()) return false;
        Snapshot snapshot = *previous;
        auto next = buffer;
        // Only newly spoken stable words go to an external application.
        next.seal_current_insertion();
        auto replacement = next.apply(utterance, hypothesis, final,
            snapshot.document, snapshot.start, snapshot.end);
        if (!replacement || replacement->text.empty()) {
            buffer = std::move(next);
            buffer.seal_current_insertion();
            retry_update = false;
            return false;
        }
        // Ordinary typing leaves the caret after separators as well.
        replacement->caret = replacement->start + replacement->text.size();
        Pending operation;
        operation.before = snapshot;
        operation.expected = snapshot.document;
        operation.expected.replace(replacement->start, replacement->length, replacement->text);
        operation.caret = replacement->caret;
        operation.buffer = std::move(next);
        operation.replacement = *replacement;
        operation.utterance = utterance;
        operation.final = final;
        operation.input_epoch = snapshot.input_epoch;
        if (!verify(snapshot)) {
            trace("output_deferred", {{"reason", "caret_or_input_changed"}, {"utterance_id", utterance}});
            observe(capture());
            return false;
        }
        bool complete = false;
        if (!send_unicode(snapshot, *replacement, complete)) {
            trace("output_deferred", {{"reason", "input_busy_or_send_rejected"}, {"utterance_id", utterance}});
            return false;
        }
        operation.dispatched_at = std::chrono::steady_clock::now();
        operation.progress_at = operation.dispatched_at;
        auto fields = target_fields(snapshot);
        fields["utterance_id"] = utterance;
        fields["is_final"] = final;
        fields["action"] = replacement->length ? "replace_user_selection" : "type_at_caret";
        fields["inserted_text"] = to_utf8(replacement->text);
        fields["replaced_length"] = replacement->length;
        fields["send_complete"] = complete;
        trace("output_dispatched", std::move(fields));
        if (snapshot.kind == Kind::Append) {
            fallback_document = operation.expected;
            buffer = operation.buffer;
            buffer.seal_current_insertion();
            record(operation);
            previous->document = fallback_document;
            previous->start = previous->end = fallback_document.size();
            // Capture the caret resulting from our own input so that it does
            // not look like a user move when the next processed chunk arrives.
            auto after = capture();
            if (after && same_target(snapshot, *after) && after->input_epoch == snapshot.input_epoch) {
                previous = std::move(after);
            }
            publish(true);
        } else {
            pending = std::move(operation);
            settle_pending(120);
        }
        retry_update = false;
        return true;
    }

    bool drain_deferred() {
        bool delivered = false;
        while (active && !deferred.empty() && input_settled()) {
            const auto& next = deferred.front();
            delivered = update(next.utterance, next.hypothesis, next.final) || delivered;
            if (retry_update) break;
            deferred.pop_front();
        }
        return delivered;
    }

    void complete_finishing() {
        if (!finishing || selection_blocks_output || pending || !deferred.empty()) return;
        buffer.finish();
        active = false;
        finishing = false;
        trace("session_end", {{"delivered_characters", transcript.size()}});
        publish(previous.has_value());
    }

    bool accept(int utterance, std::wstring hypothesis, bool final) {
        if (!active) return false;
        if (utterance < preview_utterance) return false;
        if (utterance != preview_utterance) {
            preview_utterance = utterance;
            previous_hypothesis.clear();
        }
        auto stable = final ? hypothesis : stable_prefix(previous_hypothesis, hypothesis);
        previous_hypothesis = std::move(hypothesis);
        trace("hypothesis_stability", {{"utterance_id", utterance}, {"is_final", final},
            {"stable_text", to_utf8(stable)}});
        hypothesis = std::move(stable);
        if (!deferred.empty() && deferred.back().utterance == utterance) {
            if (!deferred.back().final) deferred.back() = {utterance, std::move(hypothesis), final};
        } else {
            deferred.push_back({utterance, std::move(hypothesis), final});
        }
        return drain_deferred();
    }
};

ExternalDictationTarget::ExternalDictationTarget() : impl_(std::make_unique<Impl>()) {}
ExternalDictationTarget::~ExternalDictationTarget() = default;

bool ExternalDictationTarget::initialize(HWND owner) {
    if (impl_->worker.joinable()) return true;
    return impl_->initialize(owner);
}

void ExternalDictationTarget::set_toggle_hotkey(UINT modifiers, UINT key) {
    constexpr UINT valid_modifiers = MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN;
    impl_->toggle_hotkey.store((static_cast<std::uint64_t>(modifiers & valid_modifiers) << 32) | key,
        std::memory_order_relaxed);
}

void ExternalDictationTarget::set_diagnostics(std::shared_ptr<DictationDiagnostics> diagnostics) {
    if (impl_->worker.joinable()) {
        impl_->call([this, diagnostics = std::move(diagnostics)] { impl_->diagnostics = diagnostics; });
    } else {
        impl_->diagnostics = std::move(diagnostics);
    }
}

void ExternalDictationTarget::begin_session() {
    if (!impl_->worker.joinable()) return;
    impl_->call([this] {
        impl_->buffer.reset();
        impl_->finishing = false;
        impl_->previous.reset();
        impl_->pending.reset();
        impl_->deferred.clear();
        impl_->transcript.clear();
        impl_->fallback_document.clear();
        impl_->preview_utterance = -1;
        impl_->previous_hypothesis.clear();
        impl_->revoke_revision("new_session");
        ++impl_->revision_token;
        ++impl_->target_token;
        impl_->trace("session_begin", {{"delivery_mode", "stable_words_at_user_caret"}});
        impl_->active = true;
        impl_->observe(impl_->capture());
    });
}

bool ExternalDictationTarget::update_transcript(int utterance_id,
                                               const std::string& text, bool is_final) {
    if (!impl_->worker.joinable()) return false;
    auto wide = to_wide(text);
    if (!text.empty() && wide.empty()) return false;
    return impl_->call([this, utterance_id, wide = std::move(wide), is_final] {
        return impl_->accept(utterance_id, wide, is_final);
    });
}

void ExternalDictationTarget::poll_focus() {
    if (!impl_->worker.joinable() || impl_->poll_queued.exchange(true)) return;
    impl_->enqueue([this] {
        const auto now = std::chrono::steady_clock::now();
        if ((impl_->active || impl_->temporary_selection || impl_->finishing) &&
            now - impl_->last_poll >= std::chrono::milliseconds(80)) {
            impl_->observe(impl_->capture());
            impl_->drain_deferred();
            impl_->complete_finishing();
            impl_->last_poll = now;
        }
        impl_->poll_queued = false;
    });
}

void ExternalDictationTarget::finish_session() {
    if (!impl_->worker.joinable()) return;
    impl_->call([this] {
        impl_->finishing = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while ((!impl_->deferred.empty() || impl_->pending || impl_->selection_blocks_output) &&
               std::chrono::steady_clock::now() < deadline) {
            impl_->observe(impl_->capture());
            impl_->drain_deferred();
            impl_->settle_pending(20);
            if (!impl_->deferred.empty() || impl_->selection_blocks_output)
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        // A timed-out selection is still capable of arriving later. Keep its
        // queued speech and the capture drain alive instead of dropping words
        // at Stop or typing them into that temporary selection.
        impl_->complete_finishing();
        if (impl_->finishing) impl_->trace("session_finish_waiting", {
            {"deferred_utterances", impl_->deferred.size()},
            {"selection_recovery", impl_->selection_blocks_output}});
    });
}

void ExternalDictationTarget::cancel_session() {
    if (!impl_->worker.joinable()) return;
    impl_->call([this] {
        impl_->deferred.clear();
        impl_->finishing = false;
        impl_->restore_revision_selection();
        impl_->settle_pending(120);
        impl_->consume_pending("session_cancelled_unconfirmed");
        impl_->buffer.finish();
        impl_->active = false;
        impl_->revoke_revision("session_cancelled");
        ++impl_->target_token;
        impl_->trace("session_cancelled", {{"delivered_characters", impl_->transcript.size()}});
        impl_->publish(impl_->previous.has_value());
    });
}

ExternalDictationTarget::DictationRevision ExternalDictationTarget::revision_context() {
    if (!impl_->worker.joinable()) return {};
    return impl_->call([this] { return impl_->revision_context(); });
}

bool ExternalDictationTarget::apply_revision(std::uint64_t token,
                                            const std::string& source_raw_text,
                                            const std::string& transformed_text) {
    if (!impl_->worker.joinable()) return false;
    auto source = to_wide(source_raw_text);
    auto transformed = to_wide(transformed_text);
    if (source.empty() || transformed.empty() || transformed.find(L'\0') != std::wstring::npos) return false;
    return impl_->call([this, token, source = std::move(source), transformed = std::move(transformed)] {
        return impl_->revise(token, source, transformed);
    });
}

bool ExternalDictationTarget::apply_revision(std::uint64_t token, const std::string& transformed_text) {
    if (!impl_->worker.joinable()) return false;
    auto transformed = to_wide(transformed_text);
    if (transformed.empty() || transformed.find(L'\0') != std::wstring::npos) return false;
    return impl_->call([this, token, transformed = std::move(transformed)] {
        return impl_->revise(token, std::nullopt, transformed);
    });
}

bool ExternalDictationTarget::append_processed(std::uint64_t target_token, int utterance_id,
                                              const std::string& transformed_text) {
    if (!impl_->worker.joinable()) return false;
    auto transformed = to_wide(transformed_text);
    if (transformed.empty() || transformed.find(L'\0') != std::wstring::npos) return false;
    return impl_->call([this, target_token, utterance_id, transformed = std::move(transformed)] {
        impl_->observe(impl_->capture());
        if (!impl_->previous || impl_->previous->kind != Impl::Kind::Append ||
            impl_->target_token != target_token || !impl_->input_settled()) {
            impl_->trace("revision_skipped", {{"reason", "processed_chunk_caret_changed"},
                {"target_token", target_token}});
            return false;
        }
        // A final background result may arrive after microphone capture ends.
        // It retains the same guarded caret until a new session or user action.
        const bool was_active = impl_->active;
        impl_->active = true;
        const bool delivered = impl_->update(utterance_id, transformed, true, target_token);
        impl_->active = was_active;
        if (delivered) impl_->trace("processed_chunk_appended", {{"target_token", target_token},
            {"utterance_id", utterance_id}, {"characters", transformed.size()}});
        return delivered;
    });
}

std::string ExternalDictationTarget::text_utf8() const {
    std::lock_guard<std::mutex> lock(impl_->published_mutex);
    return impl_->published_text;
}

bool ExternalDictationTarget::has_delivered() const {
    std::lock_guard<std::mutex> lock(impl_->published_mutex);
    return impl_->published_delivered;
}

bool ExternalDictationTarget::is_available() const {
    std::lock_guard<std::mutex> lock(impl_->published_mutex);
    return impl_->published_available;
}

} // namespace vtt
