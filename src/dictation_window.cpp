#include "dictation_window.h"

#include "app_icon.h"

#include <algorithm>
#include <cstring>
#include <dwmapi.h>
#include <utility>

namespace vtt {
namespace {

constexpr wchar_t kClassName[] = L"VTTLiveDictationWindow";
constexpr wchar_t kOwnerProperty[] = L"VTTLiveDictationOwner";
constexpr int kStopId = 2101;
constexpr int kAcceptId = 2102;
constexpr int kCopyId = 2103;
constexpr int kEditorId = 2104;
constexpr COLORREF kBackground = RGB(17, 23, 31);
constexpr COLORREF kEditorBackground = RGB(25, 34, 45);
constexpr COLORREF kText = RGB(231, 237, 244);
constexpr COLORREF kMuted = RGB(165, 180, 195);

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

} // namespace

DictationWindow::~DictationWindow() {
    shutdown();
}

bool DictationWindow::initialize(HINSTANCE instance) {
    if (hwnd_) return true;
    instance_ = instance;
    background_brush_ = CreateSolidBrush(kBackground);
    editor_brush_ = CreateSolidBrush(kEditorBackground);
    font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    editor_font_ = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    icon_ = create_app_icon(32, RGB(0, 120, 215));
    small_icon_ = create_app_icon(16, RGB(0, 120, 215));

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance_;
    window_class.lpfnWndProc = WndProc;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = icon_;
    window_class.hIconSm = small_icon_;
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        shutdown();
        return false;
    }

    hwnd_ = CreateWindowExW(0, kClassName, L"Pianissimo \u2013 livediktering",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        800, 480, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        shutdown();
        return false;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    const auto make_control = [this](const wchar_t* kind, const wchar_t* title,
                                    DWORD style, int identifier) {
        return CreateWindowExW(0, kind, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            instance_, nullptr);
    };
    hint_ = make_control(L"STATIC",
        L"Texten visas medan du talar. Markera text och forts\u00e4tt tala f\u00f6r att ers\u00e4tta den. "
        L"Du kan ocks\u00e5 skriva eller flytta mark\u00f6ren.", SS_LEFT, 0);
    editor_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorId)),
        instance_, nullptr);
    status_ = make_control(L"STATIC", L"Redo f\u00f6r diktering.", SS_LEFT, 0);
    stop_button_ = make_control(L"BUTTON", L"Stoppa", BS_PUSHBUTTON | WS_TABSTOP, kStopId);
    copy_button_ = make_control(L"BUTTON", L"Kopiera", BS_PUSHBUTTON | WS_TABSTOP, kCopyId);
    accept_button_ = make_control(L"BUTTON", L"Infoga text", BS_DEFPUSHBUTTON | WS_TABSTOP, kAcceptId);
    if (!hint_ || !editor_ || !status_ || !stop_button_ || !copy_button_ || !accept_button_) {
        shutdown();
        return false;
    }
    for (HWND control : {hint_, status_, stop_button_, copy_button_, accept_button_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(editor_font_), TRUE);
    SendMessageW(editor_, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    SendMessageW(editor_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
    SetPropW(editor_, kOwnerProperty, this);
    original_edit_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(editor_,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditWndProc)));
    layout();
    update_buttons();
    return true;
}

void DictationWindow::shutdown() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = hint_ = editor_ = status_ = stop_button_ = accept_button_ = copy_button_ = nullptr;
    original_edit_proc_ = nullptr;
    if (font_) DeleteObject(font_);
    if (editor_font_) DeleteObject(editor_font_);
    if (background_brush_) DeleteObject(background_brush_);
    if (editor_brush_) DeleteObject(editor_brush_);
    if (icon_) DestroyIcon(icon_);
    if (small_icon_) DestroyIcon(small_icon_);
    font_ = editor_font_ = nullptr;
    background_brush_ = editor_brush_ = nullptr;
    icon_ = small_icon_ = nullptr;
}

void DictationWindow::show(bool activate) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (activate) {
        SetForegroundWindow(hwnd_);
        SetFocus(editor_);
    }
    UpdateWindow(hwnd_);
}

void DictationWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool DictationWindow::is_visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

bool DictationWindow::owns_window(HWND window) const {
    return hwnd_ && window && (window == hwnd_ || IsChild(hwnd_, window));
}

void DictationWindow::begin_session(bool clear) {
    if (!editor_) return;
    set_busy(false);
    applying_ = true;
    if (clear) SetWindowTextW(editor_, L"");
    insertion_.reset();
    applying_ = false;
    observe_editor();
    recording_ = true;
    processing_ = false;
    set_status(L"Lyssnar \u2013 texten uppdateras i realtid.");
    update_buttons();
}

void DictationWindow::update_transcript(int utterance_id, const std::string& text, bool is_final) {
    if (!editor_ || (!recording_ && !processing_)) return;
    const auto wide = to_wide(text);
    if (!text.empty() && wide.empty()) return;
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(editor_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    const auto replacement = insertion_.apply(utterance_id, wide, is_final, document(), start, end);
    if (!replacement) return;

    applying_ = true;
    SendMessageW(editor_, EM_SETSEL, replacement->start, replacement->start + replacement->length);
    SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(replacement->text.c_str()));
    SendMessageW(editor_, EM_SETSEL, replacement->caret, replacement->caret);
    SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
    applying_ = false;
    // No SetFocus/SetForegroundWindow here: recognition must not move focus.
    update_buttons();
}

void DictationWindow::finish_session() {
    insertion_.finish();
    recording_ = false;
    processing_ = false;
    set_status(L"Klart. Granska texten och v\u00e4lj Infoga text eller Kopiera.");
    update_buttons();
}

void DictationWindow::set_busy(bool busy) {
    busy_ = busy;
    if (editor_) SendMessageW(editor_, EM_SETREADONLY, busy ? TRUE : FALSE, 0);
    update_buttons();
}

void DictationWindow::set_status(const std::wstring& status) {
    if (status_) SetWindowTextW(status_, status.c_str());
}

std::wstring DictationWindow::document() const {
    if (!editor_) return {};
    const int length = GetWindowTextLengthW(editor_);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(editor_, text.data(), length + 1);
    text.resize(static_cast<std::size_t>((std::max)(copied, 0)));
    return text;
}

std::string DictationWindow::text_utf8() const {
    return to_utf8(document());
}

void DictationWindow::set_stop_callback(std::function<void()> callback) {
    stop_callback_ = std::move(callback);
}

void DictationWindow::set_accept_callback(std::function<void(const std::string&)> callback) {
    accept_callback_ = std::move(callback);
}

void DictationWindow::observe_editor() {
    if (applying_ || !editor_) return;
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(editor_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    insertion_.observe_editor(document(), start, end);
    update_buttons();
}

void DictationWindow::update_buttons() {
    if (stop_button_) EnableWindow(stop_button_, recording_);
    const bool has_text = editor_ && GetWindowTextLengthW(editor_) > 0;
    if (copy_button_) EnableWindow(copy_button_, has_text);
    if (accept_button_) EnableWindow(accept_button_, has_text && !recording_ && !processing_ && !busy_);
}

void DictationWindow::request_stop() {
    if (!recording_) return;
    recording_ = false;
    processing_ = true;
    set_status(L"Slutf\u00f6r de sista orden\u2026");
    update_buttons();
    if (stop_callback_) stop_callback_();
    else finish_session();
}

void DictationWindow::copy_text() {
    const auto text = document();
    if (text.empty() || !OpenClipboard(hwnd_)) return;
    const auto size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory) {
        if (void* destination = GlobalLock(memory)) {
            std::memcpy(destination, text.c_str(), size);
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
                memory = nullptr;
                set_status(L"Texten har kopierats.");
            }
        }
        if (memory) GlobalFree(memory);
    }
    CloseClipboard();
}

void DictationWindow::layout() {
    if (!hwnd_) return;
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = (std::max)(client.right, 400L);
    const int height = (std::max)(client.bottom, 260L);
    constexpr int inset = 20;
    MoveWindow(hint_, inset, 16, width - inset * 2, 46, TRUE);
    MoveWindow(editor_, inset, 70, width - inset * 2, height - 150, TRUE);
    MoveWindow(status_, inset, height - 66, (std::max)(100, width - 390), 44, TRUE);
    MoveWindow(stop_button_, width - 362, height - 61, 96, 36, TRUE);
    MoveWindow(copy_button_, width - 254, height - 61, 96, 36, TRUE);
    MoveWindow(accept_button_, width - 146, height - 61, 126, 36, TRUE);
}

LRESULT CALLBACK DictationWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<DictationWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<DictationWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);

    switch (message) {
        case WM_SIZE:
            self->layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {640, 360};
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, self->background_brush_);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC context = reinterpret_cast<HDC>(wparam);
            if (reinterpret_cast<HWND>(lparam) == self->editor_) {
                SetBkColor(context, kEditorBackground);
                SetTextColor(context, kText);
                return reinterpret_cast<LRESULT>(self->editor_brush_);
            }
            SetBkMode(context, TRANSPARENT);
            SetTextColor(context, kMuted);
            return reinterpret_cast<LRESULT>(self->background_brush_);
        }
        case WM_CTLCOLOREDIT: {
            HDC context = reinterpret_cast<HDC>(wparam);
            SetBkColor(context, kEditorBackground);
            SetTextColor(context, kText);
            return reinterpret_cast<LRESULT>(self->editor_brush_);
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kEditorId && HIWORD(wparam) == EN_CHANGE) {
                self->observe_editor();
                return 0;
            }
            if (HIWORD(wparam) != BN_CLICKED) break;
            if (LOWORD(wparam) == kStopId) {
                self->request_stop();
                return 0;
            }
            if (LOWORD(wparam) == kCopyId) {
                self->copy_text();
                return 0;
            }
            if (LOWORD(wparam) == kAcceptId && !self->recording_ && !self->processing_ && !self->busy_) {
                const auto text = self->text_utf8();
                if (!text.empty() && self->accept_callback_) self->accept_callback_(text);
                return 0;
            }
            break;
        case WM_CLOSE:
            self->request_stop();
            self->hide();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK DictationWindow::EditWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<DictationWindow*>(GetPropW(hwnd, kOwnerProperty));
    if (!self || !self->original_edit_proc_) return DefWindowProcW(hwnd, message, wparam, lparam);

    // The parent window uses an ordinary message loop, so support focus traversal
    // explicitly. Shift+Tab is useful when returning from the action buttons.
    if (message == WM_CHAR && (wparam == L'\t' || wparam == 1)) return 0;
    if (message == WM_KEYDOWN && wparam == VK_TAB) {
        HWND next = GetNextDlgTabItem(self->hwnd_, hwnd, GetKeyState(VK_SHIFT) < 0);
        if (next) SetFocus(next);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }

    const auto original = self->original_edit_proc_;
    const auto result = CallWindowProcW(original, hwnd, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        RemovePropW(hwnd, kOwnerProperty);
    } else if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
               message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
               message == WM_LBUTTONDBLCLK || message == EM_SETSEL ||
               (message == WM_MOUSEMOVE && (wparam & MK_LBUTTON) != 0)) {
        self->observe_editor();
    }
    return result;
}

} // namespace vtt
