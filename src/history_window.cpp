#include "history_window.h"

#include <string>

namespace vtt {
namespace {

static constexpr const wchar_t* HISTORY_CLASS_NAME = L"VTTHistoryWindow";

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                  static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};

    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), wide.data(), len);
    return wide;
}

} // namespace

bool HistoryWindow::initialize(HINSTANCE hInstance, TranscriptHistory* history) {
    hInstance_ = hInstance;
    history_ = history;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = HISTORY_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    hwnd_ = CreateWindowExW(
        0,
        HISTORY_CLASS_NAME,
        L"VoiceToText History",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1260,
        640,
        nullptr,
        nullptr,
        hInstance_,
        this);

    if (!hwnd_) return false;

    create_child_controls();
    layout();
    refresh();
    return true;
}

void HistoryWindow::shutdown() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hInstance_) {
        UnregisterClassW(HISTORY_CLASS_NAME, hInstance_);
        hInstance_ = nullptr;
    }
}

void HistoryWindow::show() {
    if (!hwnd_) return;
    refresh();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void HistoryWindow::refresh() {
    if (!history_ || !recorded_edit_ || !transcribed_edit_ ||
        !ai_language_edit_) return;

    std::wstring recorded = utf8_to_wide(history_->format_recorded());
    std::wstring transcribed = utf8_to_wide(history_->format_transcribed());
    std::wstring ai_language = utf8_to_wide(history_->format_ai_language());

    SetWindowTextW(recorded_edit_, recorded.c_str());
    SetWindowTextW(transcribed_edit_, transcribed.c_str());
    SetWindowTextW(ai_language_edit_, ai_language.c_str());
}

void HistoryWindow::create_child_controls() {
    recorded_label_ = CreateWindowExW(
        0, L"STATIC", L"Recorded",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    transcribed_label_ = CreateWindowExW(
        0, L"STATIC", L"Transcribed",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    ai_language_label_ = CreateWindowExW(
        0, L"STATIC", L"AI Language",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                       ES_AUTOVSCROLL | ES_READONLY | ES_LEFT;

    recorded_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    transcribed_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    ai_language_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessage(recorded_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(transcribed_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(ai_language_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(recorded_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(transcribed_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(ai_language_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void HistoryWindow::layout() {
    if (!hwnd_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);

    int padding = 12;
    int label_height = 24;
    int gap = 12;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    int panel_width = (width - padding * 2 - gap * 2) / 3;
    int edit_top = padding + label_height;
    int edit_height = height - edit_top - padding;
    int x1 = padding;
    int x2 = padding + panel_width + gap;
    int x3 = padding + (panel_width + gap) * 2;

    MoveWindow(recorded_label_, x1, padding, panel_width, label_height, TRUE);
    MoveWindow(transcribed_label_, x2, padding, panel_width, label_height, TRUE);
    MoveWindow(ai_language_label_, x3, padding, panel_width, label_height, TRUE);
    MoveWindow(recorded_edit_, x1, edit_top, panel_width, edit_height, TRUE);
    MoveWindow(transcribed_edit_, x2, edit_top, panel_width, edit_height, TRUE);
    MoveWindow(ai_language_edit_, x3, edit_top, panel_width, edit_height, TRUE);
}

LRESULT CALLBACK HistoryWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HistoryWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HistoryWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HistoryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_SIZE:
            if (self) self->layout();
            return 0;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vtt
