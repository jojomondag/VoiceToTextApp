#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "external_dictation_target.h"
#include "mode_switch_policy.h"
#include "clipboard_test_fixture.h"

// Clipboard preparation is production code, with every clipboard access
// redirected to local test memory before the editor adapter is compiled.
#define OpenClipboard clipboard_fixture::open
#define CloseClipboard clipboard_fixture::close
#define EmptyClipboard clipboard_fixture::empty
#define EnumClipboardFormats clipboard_fixture::enumerate
#define GetClipboardData clipboard_fixture::get
#define SetClipboardData clipboard_fixture::set
#define GetClipboardSequenceNumber clipboard_fixture::current_sequence
#define GetClipboardOwner clipboard_fixture::current_owner
#define GlobalSize clipboard_fixture::memory_size
#define GlobalFlags clipboard_fixture::memory_flags
#include "../src/clipboard_text_transaction.cpp"
#undef OpenClipboard
#undef CloseClipboard
#undef EmptyClipboard
#undef EnumClipboardFormats
#undef GetClipboardData
#undef SetClipboardData
#undef GetClipboardSequenceNumber
#undef GetClipboardOwner
#undef GlobalSize
#undef GlobalFlags

namespace fixture {
std::atomic<HWND> foreground{nullptr}, focus{nullptr};
HWND editors[2]{};
std::atomic<int> selects{0}, replacements{0};
std::atomic<unsigned> selection_delay_ms{0}, restoration_delay_ms{0};
std::atomic<unsigned> replacement_delay_ms{0};
std::atomic<unsigned> raw_character_delay_ms{0}, raw_caret_delay_ms{0};
std::atomic<int> rejected_replacements{0};
std::atomic<bool> automation_editor{false};
std::atomic<int> paste_attempts{0};
bool capture_paste_shortcut = false;
std::vector<std::vector<INPUT>> paste_batches;
std::deque<UINT> paste_acceptance;
constexpr UINT schedule_selection = WM_APP + 72;
constexpr UINT clear_scheduled_selections = WM_APP + 73;
constexpr ULONG_PTR delayed_replacement_copy = 74;
constexpr ULONG_PTR progressive_input_copy = 75;
constexpr UINT append_unrelated_edit = WM_APP + 76;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool owned(HWND window) {
    return window && (window == editors[0] || window == editors[1]);
}

LRESULT send(HWND window, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    require(owned(window), "fixture refuses access outside its own editors");
    DWORD_PTR result = 0;
    require(SendMessageTimeoutW(window, message, wparam, lparam,
        SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &result) != 0, "hidden editor did not answer");
    return static_cast<LRESULT>(result);
}

std::wstring document() {
    const auto count = send(focus, WM_GETTEXTLENGTH);
    std::wstring text(static_cast<std::size_t>(count) + 1, L'\0');
    text.resize(static_cast<std::size_t>(send(focus, WM_GETTEXT, text.size(),
        reinterpret_cast<LPARAM>(text.data()))));
    return text;
}

std::wstring wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

HWND WINAPI redirected_foreground() { return foreground.load(); }
BOOL WINAPI redirected_thread_info(DWORD, PGUITHREADINFO value) {
    if (!value || value->cbSize != sizeof(*value)) return FALSE;
    value->hwndActive = foreground;
    value->hwndFocus = focus;
    value->hwndCaret = focus;
    return TRUE;
}
SHORT WINAPI no_modifiers(int) { return 0; }
HHOOK WINAPI no_hook(int, HOOKPROC, HINSTANCE, DWORD) { return nullptr; }
HRESULT WINAPI confined_automation(REFCLSID clsid, LPUNKNOWN outer, DWORD context,
                                  REFIID iid, LPVOID* result) {
    return CoCreateInstance(clsid, outer, context, iid, result);
}
int WINAPI fixture_class_name(HWND window, LPWSTR name, int count) {
    if (automation_editor && owned(window)) {
        const std::wstring value = L"VttHiddenAutomationFixture";
        require(count > static_cast<int>(value.size()), "class name fixture buffer is too small");
        std::copy(value.begin(), value.end(), name);
        name[value.size()] = L'\0';
        return static_cast<int>(value.size());
    }
    return GetClassNameW(window, name, count);
}
LRESULT WINAPI confined_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT flags, UINT timeout, PDWORD_PTR result) {
    require(owned(window), "production adapter accessed a non-fixture window");
    if (message == EM_SETSEL) {
        ++selects;
        const auto delay = wparam == static_cast<WPARAM>(lparam)
            ? restoration_delay_ms.load() : selection_delay_ms.load();
        if (delay) {
            require(wparam < 65536 && lparam >= 0 && lparam < 65536,
                "delayed fixture selection exceeds packed bounds");
            return SendMessageTimeoutW(window, schedule_selection,
                MAKELONG(static_cast<WORD>(wparam), static_cast<WORD>(lparam)),
                delay, flags, timeout, result);
        }
    }
    if (message == EM_REPLACESEL) {
        ++replacements;
        if (rejected_replacements.load() > 0) {
            --rejected_replacements;
            if (result) *result = 0;
            return 0;
        }
        if (const auto delay = replacement_delay_ms.load()) {
            const std::wstring text = reinterpret_cast<const wchar_t*>(lparam);
            COPYDATASTRUCT data{delayed_replacement_copy,
                static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)),
                const_cast<wchar_t*>(text.c_str())};
            return SendMessageTimeoutW(window, WM_COPYDATA, delay,
                reinterpret_cast<LPARAM>(&data), flags, timeout, result);
        }
    }
    return SendMessageTimeoutW(window, message, wparam, lparam, flags, timeout, result);
}
UINT WINAPI confined_input(UINT count, LPINPUT events, int size) {
    require(size == sizeof(INPUT), "invalid input size");
    if (capture_paste_shortcut) {
        paste_batches.emplace_back(events, events + count);
        require(!paste_acceptance.empty(), "unexpected repeated paste input batch");
        const auto accepted = paste_acceptance.front();
        paste_acceptance.pop_front();
        require(accepted <= count, "fixture acceptance exceeds input batch");
        return accepted;
    }
    if (count == 4 && events[0].ki.wVk == VK_CONTROL) {
        require(automation_editor && events[1].ki.wVk == 'V' && events[1].ki.dwFlags == 0 &&
            events[2].ki.wVk == 'V' && events[2].ki.dwFlags == KEYEVENTF_KEYUP &&
            events[3].ki.wVk == VK_CONTROL && events[3].ki.dwFlags == KEYEVENTF_KEYUP,
            "fixture refuses any shortcut except a balanced formatted paste");
        ++paste_attempts;
        const auto pasted = clipboard_fixture::text();
        send(focus, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(pasted.c_str()));
        return count;
    }
    std::wstring characters;
    for (UINT index = 0; index < count; ++index) {
        const auto& event = events[index];
        require(event.type == INPUT_KEYBOARD && event.ki.wVk == 0 &&
            (event.ki.dwFlags & KEYEVENTF_UNICODE), "fixture refuses navigation or deletion input");
        if (!(event.ki.dwFlags & KEYEVENTF_KEYUP)) characters += event.ki.wScan;
    }
    if (raw_character_delay_ms || raw_caret_delay_ms) {
        COPYDATASTRUCT data{progressive_input_copy,
            static_cast<DWORD>((characters.size() + 1) * sizeof(wchar_t)), characters.data()};
        require(send(focus, WM_COPYDATA,
            MAKELONG(raw_character_delay_ms.load(), raw_caret_delay_ms.load()),
            reinterpret_cast<LPARAM>(&data)) != 0, "queue progressive hidden input");
    } else {
        for (const auto character : characters) send(focus, WM_CHAR, character);
    }
    return count;
}
} // namespace fixture

// Exercise the actual production adapter and Windows EDIT implementation.
// All focus/input access is confined to hidden editors in a child process;
// UIA resolves only the owned child EDIT handle. Its keyboard-focus property
// is mapped to its enabled state because this test editor remains hidden.
// No real user field can be reached and all keyboard delivery is intercepted.
#define GetForegroundWindow fixture::redirected_foreground
#define GetGUIThreadInfo fixture::redirected_thread_info
#define GetAsyncKeyState fixture::no_modifiers
#define SendInput fixture::confined_input
#define SendMessageTimeoutW fixture::confined_message
#define SetWindowsHookExW fixture::no_hook
#define CoCreateInstance fixture::confined_automation
#define GetClassNameW fixture::fixture_class_name
#define GetFocusedElement(output) ElementFromHandle(fixture::focus.load(), output)
#define get_CurrentHasKeyboardFocus(output) get_CurrentIsEnabled(output)
#include "../src/external_dictation_target.cpp"
#undef GetForegroundWindow
#undef GetGUIThreadInfo
#undef GetAsyncKeyState
#undef SendInput
#undef SendMessageTimeoutW
#undef SetWindowsHookExW
#undef CoCreateInstance
#undef GetClassNameW
#undef GetFocusedElement
#undef get_CurrentHasKeyboardFocus

namespace fixture {
struct DelayedSelection {
    HWND editor;
    UINT_PTR timer;
    WORD start;
    WORD end;
};
std::vector<DelayedSelection> delayed_selections;
struct DelayedReplacement {
    HWND editor;
    UINT_PTR timer;
    std::wstring text;
};
std::vector<DelayedReplacement> delayed_replacements;
struct ProgressiveInput {
    HWND editor;
    UINT_PTR timer;
    std::wstring text;
    std::size_t written;
    unsigned character_delay;
    unsigned caret_delay;
    DWORD initial_start;
    DWORD initial_end;
};
std::vector<ProgressiveInput> progressive_inputs;
WNDPROC original_editor_proc = nullptr;
UINT_PTR next_selection_timer = 100;

// Emulate an accessibility provider that acknowledges Select() before its
// editor applies it. Timer callbacks run exclusively inside the hidden host.
LRESULT CALLBACK delayed_editor_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == append_unrelated_edit) {
        DWORD start = 0, end = 0;
        CallWindowProcW(original_editor_proc, window, EM_GETSEL,
            reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        const auto length = CallWindowProcW(original_editor_proc, window, WM_GETTEXTLENGTH, 0, 0);
        CallWindowProcW(original_editor_proc, window, EM_SETSEL, length, length);
        CallWindowProcW(original_editor_proc, window, EM_REPLACESEL, TRUE,
            reinterpret_cast<LPARAM>(L" USER EDIT"));
        CallWindowProcW(original_editor_proc, window, EM_SETSEL, start, end);
        return 1;
    }
    if (message == WM_COPYDATA) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (data && data->dwData == progressive_input_copy && data->cbData >= sizeof(wchar_t)) {
            const auto timer = ++next_selection_timer;
            const unsigned character_delay = LOWORD(wparam), caret_delay = HIWORD(wparam);
            DWORD start = 0, end = 0;
            CallWindowProcW(original_editor_proc, window, EM_GETSEL,
                reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            if (!SetTimer(window, timer, character_delay ? character_delay : 1, nullptr)) return 0;
            const auto* text = static_cast<const wchar_t*>(data->lpData);
            progressive_inputs.push_back({window, timer,
                std::wstring(text, data->cbData / sizeof(wchar_t) - 1), 0,
                character_delay, caret_delay, start, end});
            return 1;
        }
        if (data && data->dwData == delayed_replacement_copy && data->cbData >= sizeof(wchar_t)) {
            const auto timer = ++next_selection_timer;
            if (!SetTimer(window, timer, static_cast<UINT>(wparam), nullptr)) return 0;
            const auto* text = static_cast<const wchar_t*>(data->lpData);
            delayed_replacements.push_back({window, timer,
                std::wstring(text, data->cbData / sizeof(wchar_t) - 1)});
            return 1;
        }
    }
    if (message == schedule_selection) {
        const auto timer = ++next_selection_timer;
        if (!SetTimer(window, timer, static_cast<UINT>(lparam), nullptr)) return 0;
        delayed_selections.push_back({window, timer, LOWORD(wparam), HIWORD(wparam)});
        return 1;
    }
    if (message == clear_scheduled_selections) {
        for (auto it = progressive_inputs.begin(); it != progressive_inputs.end();) {
            if (it->editor == window) {
                KillTimer(window, it->timer);
                it = progressive_inputs.erase(it);
            } else ++it;
        }
        for (auto it = delayed_selections.begin(); it != delayed_selections.end();) {
            if (it->editor == window) {
                KillTimer(window, it->timer);
                it = delayed_selections.erase(it);
            } else ++it;
        }
        for (auto it = delayed_replacements.begin(); it != delayed_replacements.end();) {
            if (it->editor == window) {
                KillTimer(window, it->timer);
                it = delayed_replacements.erase(it);
            } else ++it;
        }
        return 0;
    }
    if (message == EM_GETSEL) {
        const auto progress = std::find_if(progressive_inputs.begin(), progressive_inputs.end(),
            [&](const ProgressiveInput& pending) { return pending.editor == window && pending.caret_delay; });
        if (progress != progressive_inputs.end()) {
            if (wparam) *reinterpret_cast<DWORD*>(wparam) = progress->initial_start;
            if (lparam) *reinterpret_cast<DWORD*>(lparam) = progress->initial_end;
            return MAKELONG(progress->initial_start, progress->initial_end);
        }
    }
    if (message == WM_TIMER) {
        const auto progress = std::find_if(progressive_inputs.begin(), progressive_inputs.end(),
            [&](const ProgressiveInput& pending) {
                return pending.editor == window && pending.timer == wparam;
            });
        if (progress != progressive_inputs.end()) {
            if (progress->written < progress->text.size()) {
                const auto batch_end = progress->character_delay
                    ? (std::min)(progress->written + 2, progress->text.size()) : progress->text.size();
                do {
                    CallWindowProcW(original_editor_proc, window, WM_CHAR,
                        progress->text[progress->written++], 0);
                } while (progress->written < batch_end);
                if (progress->written < progress->text.size()) return 0;
                if (progress->caret_delay) {
                    SetTimer(window, progress->timer, progress->caret_delay, nullptr);
                    return 0;
                }
            }
            KillTimer(window, progress->timer);
            progressive_inputs.erase(progress);
            return 0;
        }
        const auto found = std::find_if(delayed_selections.begin(), delayed_selections.end(),
            [&](const DelayedSelection& pending) {
                return pending.editor == window && pending.timer == wparam;
            });
        if (found != delayed_selections.end()) {
            const auto pending = *found;
            delayed_selections.erase(found);
            KillTimer(window, pending.timer);
            return CallWindowProcW(original_editor_proc, window, EM_SETSEL,
                pending.start, pending.end);
        }
        const auto replacement = std::find_if(delayed_replacements.begin(), delayed_replacements.end(),
            [&](const DelayedReplacement& pending) {
                return pending.editor == window && pending.timer == wparam;
            });
        if (replacement != delayed_replacements.end()) {
            const auto pending = *replacement;
            delayed_replacements.erase(replacement);
            KillTimer(window, pending.timer);
            return CallWindowProcW(original_editor_proc, window, EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(pending.text.c_str()));
        }
    }
    return CallWindowProcW(original_editor_proc, window, message, wparam, lparam);
}

LRESULT CALLBACK host_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

int run_host() {
    WNDCLASSW klass{};
    klass.lpfnWndProc = host_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"VttLiveModeRegressionHost";
    require(RegisterClassW(&klass) != 0, "register hidden host");
    const HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        klass.lpszClassName, L"VTT isolated regression host", WS_OVERLAPPED,
        0, 0, 400, 200, nullptr, nullptr, klass.hInstance, nullptr);
    HWND handles[3]{host};
    for (int index = 1; index < 3; ++index) {
        handles[index] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE,
            0, (index - 1) * 100, 400, 100, host, nullptr, klass.hInstance, nullptr);
        require(handles[index] != nullptr, "create hidden editor");
        original_editor_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(handles[index],
            GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(delayed_editor_proc)));
        require(original_editor_proc != nullptr, "subclass hidden editor");
    }
    DWORD written = 0;
    require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), handles, sizeof(handles), &written, nullptr) &&
        written == sizeof(handles), "publish hidden host handles");
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

struct Host {
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    PROCESS_INFORMATION process{};
    HWND window = nullptr;

    ~Host() {
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
        if (read_pipe) CloseHandle(read_pipe);
        if (write_pipe) CloseHandle(write_pipe);
    }

    void start() {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        require(CreatePipe(&read_pipe, &write_pipe, &security, 0) != 0, "create host pipe");
        require(SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0) != 0, "configure host pipe");
        wchar_t executable[MAX_PATH]{};
        require(GetModuleFileNameW(nullptr, executable, MAX_PATH) > 0, "find test executable");
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --host";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = startup.hStdError = write_pipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != 0, "launch hidden host");
        CloseHandle(write_pipe);
        write_pipe = nullptr;
        HWND handles[3]{};
        DWORD read = 0;
        require(ReadFile(read_pipe, handles, sizeof(handles), &read, nullptr) && read == sizeof(handles),
            "hidden host readiness");
        window = handles[0];
        editors[0] = handles[1];
        editors[1] = handles[2];
        foreground = window;
        focus = editors[0];
    }
};
} // namespace fixture

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--host") return fixture::run_host();
    try {
        const HWND real_foreground = GetForegroundWindow();
        fixture::Host host;
        host.start();
        vtt::ExternalDictationTarget target;
        fixture::require(target.initialize(nullptr), "initialize external writer");
        const std::wstring left = L"MANUAL LEFT|", right = L"|MANUAL RIGHT";
        int utterance = 0;
        int passed = 0;
        const auto reset = [&] {
            target.cancel_session();
            fixture::automation_editor = false;
            fixture::paste_attempts = 0;
            clipboard_fixture::reset();
            fixture::selection_delay_ms = fixture::restoration_delay_ms = 0;
            fixture::replacement_delay_ms = 0;
            fixture::raw_character_delay_ms = fixture::raw_caret_delay_ms = 0;
            fixture::rejected_replacements = 0;
            for (const auto editor : fixture::editors)
                fixture::send(editor, fixture::clear_scheduled_selections);
            fixture::focus = fixture::editors[0];
            const auto manual = left + right;
            fixture::send(fixture::focus, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(manual.c_str()));
            fixture::send(fixture::focus, EM_SETSEL, left.size(), left.size());
            target.begin_session();
            fixture::selects = fixture::replacements = 0;
            utterance = 0;
        };
        const auto speech = [&](const std::string& text) {
            fixture::require(target.update_transcript(utterance++, text, true), "speech was not delivered");
            const auto source = target.revision_context();
            fixture::require(source.can_replace && !source.raw_text.empty(), "speech range is not owned");
            return source;
        };
        const auto preserved = [&] {
            const auto text = fixture::document();
            fixture::require(text.size() >= left.size() + right.size() &&
                text.compare(0, left.size(), left) == 0 &&
                text.compare(text.size() - right.size(), right.size(), right) == 0,
                "manual surrounding text changed");
            DWORD start = 0, end = 0;
            fixture::send(fixture::focus, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            fixture::require(start == end, "temporary selection remains");
        };
        const auto run = [&](const char* name, const auto& test) {
            reset();
            test();
            ++passed;
            std::cout << "PASS: " << name << '\n';
        };
        const auto wait_for_output = [&](const std::wstring& expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            do {
                target.poll_focus();
                if (fixture::document().find(expected) != std::wstring::npos) return;
                Sleep(20);
            } while (std::chrono::steady_clock::now() < deadline);
            std::wcerr << L"expected pending output: " << expected << L"\nactual fixture document: "
                << fixture::document() << L'\n';
            fixture::require(false, "deferred speech did not resume after selection recovery");
        };
        const auto await_owned = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            do {
                const auto current = target.revision_context();
                if (current.can_replace && !current.pending_output) return current;
                target.poll_focus();
                Sleep(15);
            } while (std::chrono::steady_clock::now() < deadline);
            throw std::runtime_error("progressive output did not regain its complete owned source");
        };
        run("formatted paste never submits Enter or Tab and never replays a partially accepted paste", [&] {
            struct ResetCapture { ~ResetCapture() { fixture::capture_paste_shortcut = false; } } reset_capture;
            fixture::capture_paste_shortcut = true;
            for (UINT accepted = 0; accepted <= 4; ++accepted) {
                fixture::paste_batches.clear();
                fixture::paste_acceptance = {accepted, 2};
                bool complete = true;
                const bool dispatched = vtt::send_paste_shortcut(complete);
                fixture::require(dispatched == (accepted >= 2) && complete == (accepted == 4),
                    "paste dispatch certainty is incorrect for a partially accepted input batch");
                const bool releases_needed = accepted > 0 && accepted < 4;
                fixture::require(fixture::paste_batches.size() == (releases_needed ? 2 : 1),
                    "partially accepted paste did not release keys exactly once");
                const auto& keys = fixture::paste_batches.front();
                fixture::require(keys.size() == 4 && keys[0].ki.wVk == VK_CONTROL &&
                    keys[0].ki.dwFlags == 0 && keys[1].ki.wVk == 'V' && keys[1].ki.dwFlags == 0 &&
                    keys[2].ki.wVk == 'V' && keys[2].ki.dwFlags == KEYEVENTF_KEYUP &&
                    keys[3].ki.wVk == VK_CONTROL && keys[3].ki.dwFlags == KEYEVENTF_KEYUP,
                    "formatted paste did not use a balanced Ctrl+V shortcut");
                int paste_keydowns = 0;
                for (const auto& batch : fixture::paste_batches) {
                    for (const auto& key : batch) {
                        fixture::require(key.type == INPUT_KEYBOARD && key.ki.wScan == 0 &&
                            (key.ki.wVk == VK_CONTROL || key.ki.wVk == 'V'),
                            "formatted paste injected Enter, Tab, text or navigation keys");
                        paste_keydowns += key.ki.wVk == 'V' && !(key.ki.dwFlags & KEYEVENTF_KEYUP);
                    }
                }
                fixture::require(paste_keydowns == 1, "a partial paste was replayed");
                if (releases_needed) {
                    const auto& releases = fixture::paste_batches.back();
                    fixture::require(releases.size() == 2 && releases[0].ki.wVk == 'V' &&
                        releases[1].ki.wVk == VK_CONTROL && releases[0].ki.dwFlags == KEYEVENTF_KEYUP &&
                        releases[1].ki.dwFlags == KEYEVENTF_KEYUP, "partial paste left a modifier held down");
                }
            }
        });
        run("partial Unicode readback keeps cumulative original speech after rewrites", [&] {
            const auto initial = speech("bygg en implementation av Teachers Little Helper");
            fixture::require(target.apply_revision(initial.token, initial.raw_text,
                "Implementera Teachers Little Helper."), "initial AI rewrite failed");
            fixture::raw_character_delay_ms = 10;
            const std::string added = "och ratta alla uppgifter fran Google Classroom for samtliga elever "
                "samt berakna quizresultat och jamfor resultatet med den angivna gransen";
            fixture::require(target.update_transcript(utterance++, added, true), "progressive speech was not dispatched");
            const auto partial = target.revision_context();
            if (!partial.pending_output || partial.can_replace) {
                std::cerr << "partial readback: pending=" << partial.pending_output
                    << " owned=" << partial.can_replace << " token=" << partial.token
                    << " original_token=" << initial.token << '\n';
                std::wcerr << L"fixture partial document: " << fixture::document() << L'\n';
            }
            fixture::require(partial.pending_output && !partial.can_replace,
                "partial own output was finalized before the complete readback");
            const auto accumulated = await_owned();
            fixture::require(accumulated.token == initial.token &&
                accumulated.raw_text == initial.raw_text + " " + added,
                "partial insertion split the AI context or lost original speech");
            fixture::require(fixture::document() == left + L" Implementera Teachers Little Helper. " +
                fixture::wide(added) + right, "progressive output lost or duplicated characters");
            fixture::raw_character_delay_ms = 0;
            fixture::require(target.apply_revision(accumulated.token, accumulated.raw_text,
                "Implementera Teachers Little Helper och ratta uppgifter fran Google Classroom."),
                "complete contextual rewrite failed");
            const auto final = speech("jamfor resultatet med en grans");
            fixture::require(final.token == initial.token &&
                final.raw_text == accumulated.raw_text + " jamfor resultatet med en grans",
                "later speech did not retain the complete original instruction");
            preserved();
        });
        run("delayed caret readback preserves ownership after all characters arrive", [&] {
            const auto initial = speech("samtliga elever i klassen");
            fixture::require(target.apply_revision(initial.token, initial.raw_text,
                "Hantera samtliga elever i klassen."), "initial contextual rewrite failed");
            fixture::raw_caret_delay_ms = 350;
            fixture::require(target.update_transcript(utterance++, "och varje quiz har flera fragor", true),
                "caret-delay speech was not dispatched");
            const auto delayed = target.revision_context();
            fixture::require(delayed.pending_output && !delayed.can_replace,
                "stale provider caret prematurely invalidated complete own output");
            const auto accumulated = await_owned();
            fixture::require(accumulated.token == initial.token &&
                accumulated.raw_text == initial.raw_text + " och varje quiz har flera fragor",
                "lagging provider caret discarded prior source context");
            fixture::require(fixture::document() == left +
                L" Hantera samtliga elever i klassen. och varje quiz har flera fragor" + right,
                "delayed caret input was lost or duplicated");
            preserved();
        });
        run("an unrelated edit during progressive output cannot reclaim prior ownership", [&] {
            const auto initial = speech("ursprunglig sammanhangande instruktion");
            fixture::raw_character_delay_ms = 10;
            const std::string added = "och detta ar fler ord som levereras under tiden ett externt program andrar faltet";
            fixture::require(target.update_transcript(utterance++, added, true), "progressive speech was not dispatched");
            fixture::send(fixture::focus, fixture::append_unrelated_edit);
            const auto invalidated = target.revision_context();
            fixture::require(!invalidated.can_replace, "unrelated edit retained permission to rewrite previous text");
            wait_for_output(fixture::wide(added));
            fixture::raw_character_delay_ms = 0;
            const auto before = fixture::document();
            fixture::require(!target.apply_revision(initial.token, initial.raw_text, "Must never replace edited text"),
                "partial-output recovery reclaimed a user-modified document");
            fixture::require(fixture::document() == before &&
                before.find(L" USER EDIT") != std::wstring::npos,
                "stale rewrite changed the unrelated edit");
        });
        run("delayed selection is confirmed before a revision writes", [&] {
            const auto source = speech("alla tidigare ord ska finnas kvar");
            fixture::selection_delay_ms = 150;
            fixture::require(target.apply_revision(source.token, source.raw_text,
                "Alla tidigare ord ska finnas kvar."), "asynchronous selection was not confirmed");
            speech("och texten fortsatter att vaxa");
            fixture::require(fixture::document().find(L"Alla tidigare ord ska finnas kvar.") != std::wstring::npos,
                "confirmed revision lost preceding text");
            fixture::require(fixture::document().find(L"och texten fortsatter att vaxa") != std::wstring::npos,
                "speech did not grow after delayed selection");
            preserved();
        });
        run("rejected revision restores a delayed selection before more speech", [&] {
            const auto source = speech("dessa tidigare ord far aldrig raderas");
            fixture::selection_delay_ms = 40;
            fixture::restoration_delay_ms = 120;
            fixture::rejected_replacements = 1;
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                "Dessa tidigare ord far aldrig raderas."), "fixture did not reject revision write");
            Sleep(180);
            target.update_transcript(utterance++, "och de nya orden laggs till", true);
            wait_for_output(L"och de nya orden laggs till");
            fixture::require(fixture::document().find(L"dessa tidigare ord far aldrig raderas") != std::wstring::npos,
                "delayed revision selection removed earlier speech");
            preserved();
        });
        run("selection arriving after timeout cannot consume queued dictation", [&] {
            const auto source = speech("bevara hela den har texten");
            fixture::selection_delay_ms = 1100;
            fixture::restoration_delay_ms = 140;
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                "Bevara hela den har texten."), "very late selection unexpectedly completed revision");
            fixture::require(!target.update_transcript(utterance++, "forsta nya delen", true),
                "speech dispatched while a delayed programmatic selection was unresolved");
            wait_for_output(L"forsta nya delen");
            speech("och andra nya delen");
            fixture::require(fixture::document().find(L"bevara hela den har texten") != std::wstring::npos &&
                fixture::document().find(L"forsta nya delen") != std::wstring::npos &&
                fixture::document().find(L"och andra nya delen") != std::wstring::npos,
                "late selection deleted earlier or deferred dictation");
            preserved();
        });
        run("caret restoration arriving after timeout resumes queued speech safely", [&] {
            const auto source = speech("original text must survive slow restoration");
            fixture::selection_delay_ms = 40;
            fixture::restoration_delay_ms = 650;
            fixture::rejected_replacements = 1;
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                "Original text must survive slow restoration."), "fixture did not reject revision write");
            fixture::require(!target.update_transcript(utterance++, "and the new speech remains", true),
                "speech dispatched before caret restoration was confirmed");
            wait_for_output(L"and the new speech remains");
            fixture::require(fixture::document().find(L"original text must survive slow restoration") != std::wstring::npos,
                "speech replaced the delayed restoration selection");
            preserved();
        });
        run("stopping during selection recovery drains the final spoken words", [&] {
            const auto source = speech("keep the text spoken before stop");
            const auto before = fixture::document();
            fixture::selection_delay_ms = 1900;
            fixture::restoration_delay_ms = 140;
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                "Keep the text spoken before stop."), "very late selection unexpectedly completed revision");
            fixture::require(!target.update_transcript(utterance++, "and preserve this final spoken suffix", true),
                "final words dispatched into an unresolved temporary selection");
            target.finish_session();
            const auto waiting = target.revision_context();
            fixture::require(waiting.pending_output && fixture::document() == before,
                "stop discarded pending words or wrote before selection recovery");
            wait_for_output(L"and preserve this final spoken suffix");
            const auto completed = target.revision_context();
            const auto final_text = fixture::document();
            const std::wstring suffix = L"and preserve this final spoken suffix";
            const auto suffix_position = final_text.find(suffix);
            fixture::require(!completed.pending_output &&
                final_text.find(L"keep the text spoken before stop") != std::wstring::npos &&
                suffix_position != std::wstring::npos &&
                final_text.find(suffix, suffix_position + suffix.size()) == std::wstring::npos,
                "stop did not preserve prior text and deliver the queued suffix exactly once");
            preserved();
        });
        run("late successful replacement readback resumes deferred speech", [&] {
            const auto source = speech("all previous words stay in the document");
            fixture::replacement_delay_ms = 950;
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                "All previous words stay in the document."), "very late write unexpectedly completed revision");
            fixture::require(!target.update_transcript(utterance++, "and later speech continues growing", true),
                "speech dispatched while an earlier replacement was unresolved");
            wait_for_output(L"and later speech continues growing");
            const auto context = target.revision_context();
            const auto final_text = fixture::document();
            fixture::require(!context.pending_output && context.can_replace && context.token == source.token &&
                context.raw_text == source.raw_text + " and later speech continues growing" &&
                final_text.find(L"All previous words stay in the document.") != std::wstring::npos,
                "late revision readback did not preserve cumulative source ownership");
            preserved();
        });
        run("cancel restores a late selection without application polling", [&] {
            const auto source = speech("keep this text when recording is cancelled");
            const auto before = fixture::document();
            fixture::selection_delay_ms = 1900;
            fixture::restoration_delay_ms = 140;
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                "Keep this text when recording is cancelled."), "very late selection unexpectedly completed revision");
            fixture::require(!target.update_transcript(utterance++, "cancelled suffix must not appear", true),
                "cancelled words dispatched into an unresolved temporary selection");
            target.cancel_session();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            bool restored = false;
            // Cancel stops the app's polling timer. Only the production
            // adapter's own recovery work may restore its outstanding selection.
            do {
                DWORD start = 0, end = 0;
                fixture::send(fixture::focus, EM_GETSEL,
                    reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                restored = fixture::selects >= 2 && start == end;
                if (restored) break;
                Sleep(20);
            } while (std::chrono::steady_clock::now() < deadline);
            fixture::require(restored, "cancel left a delayed temporary selection after application polling stopped");
            fixture::require(fixture::document() == before,
                "cancel changed original text or delivered cancelled speech");
            preserved();
        });
        run("a genuine user selection still accepts dictated replacement", [&] {
            speech("gammal diktering");
            const auto before = fixture::document();
            const auto selected_start = before.find(L"gammal diktering");
            fixture::require(selected_start != std::wstring::npos, "manual selection source missing");
            fixture::send(fixture::focus, EM_SETSEL, selected_start,
                selected_start + std::wstring(L"gammal diktering").size());
            speech("ersatt manuellt markerade ord");
            fixture::require(fixture::document().find(L"gammal diktering") == std::wstring::npos &&
                fixture::document().find(L"ersatt manuellt markerade ord") != std::wstring::npos,
                "genuine user selection was not replaced");
            preserved();
        });
        run("persistent policy drives ongoing revisions through final capture drain", [&] {
            vtt::ModeSwitchPolicy policy;
            const auto observe = [&] {
                const auto source = target.revision_context();
                vtt::ModeSwitchPolicy::Context context{1, source.can_replace ? source.token : 0, source.raw_text};
                policy.observe_context(context);
                return context;
            };
            const auto selection = policy.request_mode(observe(), "clean");
            fixture::require(selection != 0 && !policy.has_pending(), "mode before first speech was not retained");
            speech("de första orden");
            const auto first = observe();
            fixture::require(policy.pending_context() && policy.pending_context()->raw_text == first.raw_text,
                "first speech was not queued by selected mode");
            fixture::require(policy.mark_submitted(first), "first source could not be frozen for inference");
            speech("och nya ord medan modellen arbetar");
            const auto second = observe();
            policy.receive_versions({first, "De första orden.", {}, {}, {}});
            const auto initial = policy.take_ready();
            fixture::require(initial && initial->click_id == selection &&
                target.apply_revision(initial->context.target_token, initial->context.raw_text, initial->text),
                "policy result did not revise original prefix");
            const auto suffix = second.raw_text.substr(first.raw_text.size());
            fixture::require(fixture::document().find(fixture::wide(suffix)) != std::wstring::npos,
                "policy revision removed newer speech");
            fixture::require(policy.pending_context() && policy.pending_context()->raw_text == second.raw_text,
                "continued speech did not schedule automatically");
            policy.receive_versions({second, "De första orden och nya ord.", {}, {}, {}});
            const auto continued = policy.take_ready();
            fixture::require(continued && continued->click_id == selection &&
                target.apply_revision(continued->context.target_token, continued->context.raw_text, continued->text),
                "continued selected mode did not apply");
            speech("och slutorden");
            const auto final = observe();
            target.finish_session();
            fixture::require(policy.has_pending(), "capture completion lost final mode work");
            policy.receive_versions({final, "De första orden, nya ord och slutorden.", {}, {}, {}});
            const auto drained = policy.take_ready();
            fixture::require(drained && drained->click_id == selection &&
                target.apply_revision(drained->context.target_token, drained->context.raw_text, drained->text),
                "final mode work did not apply after capture ended");
            observe();
            fixture::require(!policy.has_pending() && !policy.take_ready(), "final source kept rescheduling");
            fixture::require(fixture::document().find(L"De första orden, nya ord och slutorden.") != std::wstring::npos,
                "final selected version is not visible");
            preserved();
        });
        run("clean and AI rewrite growing raw text repeatedly", [&] {
            auto source = speech(u8"hej jag vill boka möte");
            const auto token = source.token;
            fixture::require(target.apply_revision(token, source.raw_text, u8"Hej, jag vill boka ett möte."), "initial clean revision");
            source = speech(u8"det ska vara på fredag");
            fixture::require(source.token == token && source.raw_text.find("Hej,") == std::string::npos,
                "own revision changed raw source or ownership");
            fixture::require(target.apply_revision(token, source.raw_text, u8"Hej, jag vill boka ett möte på fredag."), "continuous clean revision");
            source = speech(u8"skriv ett kort svar");
            fixture::require(target.apply_revision(token, source.raw_text, u8"Boka ett möte på fredag och skriv ett kort svar."), "AI mode revision");
            source = speech(u8"ange tiden också");
            fixture::require(target.apply_revision(token, source.raw_text, u8"Boka ett möte på fredag. Svara kort och ange tiden."), "continuous AI revision");
            fixture::require(fixture::document().find(L"Svara kort och ange tiden.") != std::wstring::npos,
                "latest transformed speech is not visible");
            fixture::require(fixture::replacements == 4, "continuous revisions did not each apply once");
            preserved();
        });
        run("multiline AI revisions retain indentation and exact CRLF caret offsets", [&] {
            const auto source = speech(u8"bygg ett spel med fjorton karaktärer och olika förmågor");
            const std::string structured = u8"Mål: Bygg ett spel.\n\nKrav:\n- 14 karaktärer.\n  - Olika förmågor.\n\t- Bevara deras namn.";
            const std::wstring normalized = L"Mål: Bygg ett spel.\r\n\r\nKrav:\r\n- 14 karaktärer.\r\n  - Olika förmågor.\r\n\t- Bevara deras namn.";
            fixture::require(target.apply_revision(source.token, source.raw_text, structured),
                "multiline AI revision did not apply");
            fixture::require(fixture::document() == left + L" " + normalized + right,
                "multiline revision lost line breaks, blank lines, indentation or surrounding text");
            DWORD start = 0, end = 0;
            fixture::send(fixture::focus, EM_GETSEL,
                reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            fixture::require(start == left.size() + 1 + normalized.size() && start == end,
                "caret offset did not account for CRLF pairs");
            const auto current = target.revision_context();
            fixture::require(current.can_replace && current.token == source.token &&
                current.raw_text == source.raw_text, "formatting changed the original speech source");
            preserved();
        });
        run("speech grows after a multiline rewrite and remains available for the next full revision", [&] {
            const auto source = speech(u8"bygg ett spel med fjorton karaktärer");
            fixture::require(target.apply_revision(source.token, source.raw_text,
                u8"Mål: Bygg ett spel.\r\n\r\nKrav:\r\n  - 14 karaktärer."),
                "initial structured AI revision failed");
            const auto continued = speech(u8"och de ska kunna spawnas på olika sätt");
            fixture::require(continued.token == source.token &&
                continued.raw_text == source.raw_text + u8" och de ska kunna spawnas på olika sätt",
                "later dictation lost the original raw source or changed ownership");
            fixture::require(fixture::document() == left +
                L" Mål: Bygg ett spel.\r\n\r\nKrav:\r\n  - 14 karaktärer. och de ska kunna spawnas på olika sätt" + right,
                "later speech removed existing formatting or was inserted at the wrong caret");
            fixture::require(target.apply_revision(continued.token, continued.raw_text,
                u8"Mål: Bygg ett spel.\n\nKrav:\n  - 14 karaktärer.\n  - Karaktärerna ska kunna spawnas på olika sätt."),
                "full structured revision after later dictation failed");
            fixture::require(fixture::document() == left +
                L" Mål: Bygg ett spel.\r\n\r\nKrav:\r\n  - 14 karaktärer.\r\n  - Karaktärerna ska kunna spawnas på olika sätt." + right,
                "latest structured revision lost prior or newly spoken requirements");
            fixture::require(target.revision_context().raw_text == continued.raw_text,
                "full multiline rewrite changed original dictation");
            preserved();
        });
        run("a formatted replacement can be undone without touching manual surrounding text", [&] {
            const auto source = speech("keep every original spoken word");
            const auto before = fixture::document();
            fixture::send(fixture::focus, EM_EMPTYUNDOBUFFER);
            fixture::require(target.apply_revision(source.token, source.raw_text,
                "Goal: Keep the original words.\n\nRequirements:\n  - Preserve every word."),
                "formatted replacement did not apply before undo");
            fixture::require(fixture::send(fixture::focus, EM_CANUNDO) != 0,
                "formatted replacement is not undoable");
            const auto undo_result = fixture::send(fixture::focus, EM_UNDO);
            // A native EDIT may group the previous typed insertion with its
            // replacement. Either valid undo group must preserve manual text.
            fixture::require(undo_result != 0 &&
                (fixture::document() == before || fixture::document() == left + right),
                "undo changed manual surrounding text or left a partial formatted replacement");
            const auto after_undo = fixture::document();
            fixture::require(!target.apply_revision(source.token, source.raw_text, "Stale revision"),
                "a stale result overwrote the user's undo");
            fixture::require(fixture::document() == after_undo,
                "rejected stale revision changed the undone document");
        });
        run("delayed transform preserves later speech and transforms it next", [&] {
            const auto prefix = speech("tidigare ord");
            const auto current = speech("nytt tal medan modellen arbetar");
            const auto suffix = current.raw_text.substr(prefix.raw_text.size());
            fixture::require(target.apply_revision(prefix.token, prefix.raw_text, "Tidigare ord."), "delayed prefix revision");
            fixture::require(fixture::document().find(fixture::wide(suffix)) != std::wstring::npos, "later speech was lost");
            fixture::require(target.apply_revision(current.token, current.raw_text, "Tidigare ord och nytt tal."), "newest revision");
            fixture::require(fixture::document().find(L"Tidigare ord och nytt tal.") != std::wstring::npos, "latest version missing");
            preserved();
        });
        run("an older processed prefix does not revoke the newer owned range", [&] {
            const auto older = speech("tidigare ord");
            const auto current = speech("och fortsatt diktering");
            fixture::require(target.apply_revision(current.token, current.raw_text,
                "Tidigare ord och fortsatt diktering."), "current revision did not apply");
            const auto before = fixture::document();
            fixture::require(!target.apply_revision(older.token, older.raw_text, "For gammalt."),
                "an obsolete shorter revision was applied");
            const auto still_owned = target.revision_context();
            fixture::require(still_owned.can_replace && still_owned.token == current.token &&
                fixture::document() == before, "an obsolete result revoked valid ownership or changed text");
            speech("och annu fler ord");
            fixture::require(fixture::document().find(L"Tidigare ord och fortsatt diktering.") != std::wstring::npos &&
                fixture::document().find(L"och annu fler ord") != std::wstring::npos,
                "dictation did not grow after obsolete result was rejected");
            preserved();
        });
        run("raw mode restores the original complete dictation", [&] {
            speech("ursprunglig text");
            const auto source = speech("med fler ord");
            fixture::require(target.apply_revision(source.token, source.raw_text, "Bearbetad text."), "processed revision");
            fixture::require(target.apply_revision(source.token, source.raw_text, source.raw_text), "raw restoration");
            fixture::require(fixture::document() == left + fixture::wide(source.raw_text) + right, "raw restoration differs");
            preserved();
        });
        run("final transformation can finish after microphone capture", [&] {
            const auto source = speech("sista orden vid stopp");
            target.finish_session();
            fixture::require(target.apply_revision(source.token, source.raw_text, "Sista orden vid stopp."), "final post-stop revision");
            preserved();
        });
        run("manual edit rejects a stale transformation", [&] {
            const auto source = speech("min dikterade text");
            fixture::send(fixture::focus, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" MANUAL EDIT"));
            const auto before = fixture::document();
            fixture::require(!target.apply_revision(source.token, source.raw_text, "Should never appear"), "manual edit was overwritten");
            fixture::require(fixture::document() == before && fixture::selects == 0 && fixture::replacements == 0,
                "stale transformation changed user input");
            preserved();
        });
        run("caret movement revokes old range and new speech owns only its insertion", [&] {
            const auto stale = speech("gammal diktering");
            fixture::send(fixture::focus, EM_SETSEL, 0, 0);
            const auto before = fixture::document();
            fixture::require(!target.apply_revision(stale.token, stale.raw_text, "Should never appear"), "caret movement ignored");
            fixture::require(fixture::document() == before && fixture::selects == 0, "stale revision selected old text");
            const auto fresh = speech("nya ord");
            fixture::require(fresh.token != stale.token && fresh.raw_text.find("gammal") == std::string::npos,
                "new caret inherited revoked words");
            fixture::require(target.apply_revision(fresh.token, fresh.raw_text, "Nya ord."), "new insertion did not resume transformations");
            fixture::require(fixture::document().find(before) != std::wstring::npos, "new mode revision touched old document");
        });
        run("changing text fields rejects old results", [&] {
            const auto source = speech("text i det gamla fältet");
            fixture::focus = fixture::editors[1];
            fixture::send(fixture::focus, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"SECOND FIELD"));
            const auto before = fixture::document();
            fixture::require(!target.apply_revision(source.token, source.raw_text, "Should never appear"), "field change ignored");
            fixture::require(fixture::document() == before && fixture::selects == 0, "new field modified by stale revision");
        });
        run("real UIA retains raw speech after a blocked formatted paste and applies formatting once clipboard recovers", [&] {
            fixture::automation_editor = true;
            clipboard_fixture::seed_text(L"Unrelated clipboard content");
            const unsigned char opaque[] = {1, 2, 3};
            clipboard_fixture::seed(CF_PRIVATEFIRST, opaque, sizeof(opaque));
            const auto clipboard_before = clipboard_fixture::snapshot();
            const auto source = speech(u8"bygg ett spel med fjorton karaktärer");
            const auto before = fixture::document();
            fixture::require(!target.apply_revision(source.token, source.raw_text,
                u8"1. **Spel:** Bygg ett spel.\n\n2. **Karaktärer:**\n    - Använd 14 karaktärer."),
                "unsupported clipboard format unexpectedly allowed a formatted paste");
            fixture::require(fixture::paste_attempts == 0 && fixture::document() == before &&
                clipboard_fixture::snapshot() == clipboard_before,
                "failed formatted paste changed the editor or clipboard");
            const auto retained = target.revision_context();
            fixture::require(retained.can_replace && !retained.pending_output &&
                retained.token == source.token && retained.raw_text == source.raw_text,
                "clipboard preparation failure discarded the owned speech or left output fenced");
            preserved();
            const auto continued = speech(u8"och olika förmågor");
            fixture::require(continued.token == source.token &&
                continued.raw_text == source.raw_text + u8" och olika förmågor",
                "speech after a failed formatted paste lost the cumulative original source");
            clipboard_fixture::clear();
            clipboard_fixture::seed_text(L"New user clipboard content");
            clipboard_fixture::seed_empty_marker(clipboard_fixture::binary);
            const auto clipboard_recovered = clipboard_fixture::snapshot();
            fixture::require(target.apply_revision(continued.token, continued.raw_text,
                u8"1. **Spel:** Bygg ett spel.\n\n2. **Karaktärer:**\n    - Använd 14 karaktärer.\n    - Ge dem olika förmågor."),
                "formatted UIA revision did not resume after clipboard recovery");
            fixture::require(fixture::paste_attempts == 1 && fixture::document() == left +
                L" 1. **Spel:** Bygg ett spel.\r\n\r\n2. **Karaktärer:**\r\n    - Använd 14 karaktärer.\r\n    - Ge dem olika förmågor." + right,
                "formatted UIA paste lost content, numbering, bold markers or indentation");
            fixture::require(clipboard_fixture::snapshot() == clipboard_recovered,
                "acknowledged formatted UIA paste did not restore the latest clipboard content");
            const auto final = target.revision_context();
            fixture::require(final.can_replace && !final.pending_output &&
                final.raw_text == continued.raw_text, "successful formatted UIA rewrite lost original speech ownership");
            preserved();
        });
        target.finish_session();
        const HWND final_foreground = GetForegroundWindow();
        DWORD foreground_process = 0;
        if (final_foreground) GetWindowThreadProcessId(final_foreground, &foreground_process);
        fixture::require(final_foreground != host.window && !fixture::owned(final_foreground) &&
            foreground_process != host.process.dwProcessId && foreground_process != GetCurrentProcessId(),
            "test fixture became the foreground window");
        if (final_foreground != real_foreground)
            std::cout << "NOTE: Another application changed foreground during the isolated tests.\n";
        std::cout << passed << " checks passed; native Windows EDIT, no real keyboard input or fixture activation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
