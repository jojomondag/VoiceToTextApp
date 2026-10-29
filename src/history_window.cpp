#include "history_window.h"

#include "app_icon.h"
#include "config_manager.h"
#include "transcript_history.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dwmapi.h>
#include <iterator>
#include <richedit.h>
#include <string>
#include <string_view>
#include <windowsx.h>

namespace vtt {
namespace {

static constexpr int IDC_CLEAR_BUTTON = 1001;
static constexpr int IDC_PROMPT_SAVE  = 1010;
static constexpr int IDC_PROFILE_COMBO = 1011;
static constexpr int IDC_PROFILE_NEW = 1012;
static constexpr int IDC_PROFILE_RENAME = 1013;
static constexpr int IDC_PROMPT_BACK = 1014;
static constexpr int PROFILE_BUTTON_WIDTH = 24;
static constexpr int PROFILE_BUTTON_HEIGHT = 20;
static constexpr int PROFILE_BUTTON_GAP = 6;
static constexpr int SECTION_TITLE_INSET = 18;
static constexpr int PANEL_INNER = 12;
static constexpr int ACTION_GUTTER_WIDTH = 62;
static constexpr int ACTION_GUTTER_INSET = 4;
static constexpr int HISTORY_TEXT_LEFT_MARGIN = 10;
static constexpr int HISTORY_TEXT_RIGHT_MARGIN = 8;
static constexpr int PROMPT_TEXT_RIGHT_MARGIN = 12;
static constexpr int PANEL_RADIUS = 14;

static constexpr const wchar_t* HISTORY_CLASS_NAME = L"VTTHistoryWindow";
static constexpr const wchar_t* ACTION_GUTTER_CLASS_NAME =
    L"VTTHistoryActionGutter";
static constexpr const wchar_t* EDIT_OWNER_PROP = L"VTTHistoryWindowOwner";
static constexpr const wchar_t* ENTRY_BEGIN = L"[[VTT_ENTRY_BEGIN:";
static constexpr const wchar_t* ENTRY_END = L"[[VTT_ENTRY_END:";
static constexpr COLORREF COLOR_BG = RGB(17, 23, 31);
static constexpr COLORREF COLOR_PANEL = RGB(31, 40, 52);
static constexpr COLORREF COLOR_PANEL_EDGE = RGB(72, 88, 105);
static constexpr COLORREF COLOR_CONTROL = RGB(22, 29, 39);
static constexpr COLORREF COLOR_BUTTON = RGB(39, 101, 150);
static constexpr COLORREF COLOR_LABEL = RGB(232, 238, 244);
static constexpr COLORREF COLOR_MUTED = RGB(150, 164, 178);
static constexpr COLORREF COLOR_PROFILE = RGB(88, 210, 170);
static constexpr COLORREF COLOR_TEXT = RGB(223, 230, 236);
static constexpr COLORREF COLOR_ENTRY_CARD = RGB(28, 38, 50);
static constexpr COLORREF COLOR_ENTRY_HIGHLIGHT = RGB(55, 92, 125);

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

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                  static_cast<int>(text.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};

    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::string trim_ascii(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::wstring normalize_crlf(const std::wstring& text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            normalized.append(L"\r\n");
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (text[i] == L'\n') {
            normalized.append(L"\r\n");
        } else {
            normalized.push_back(text[i]);
        }
    }
    return normalized;
}

std::string trim_whitespace(const std::wstring& text, size_t start, size_t end) {
    while (start < end &&
           (text[start] == L' ' || text[start] == L'\t')) {
        ++start;
    }
    while (end > start &&
           (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
            text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        --end;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data() + start,
                                   static_cast<int>(end - start),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data() + start,
                        static_cast<int>(end - start),
                        result.data(), size, nullptr, nullptr);
    return result;
}

int rich_edit_cp_from_text_offset(const std::wstring& text, size_t offset) {
    offset = (std::min)(offset, text.size());

    int cp = 0;
    for (size_t i = 0; i < offset;) {
        if (text[i] == L'\r' && i + 1 < offset && text[i + 1] == L'\n') {
            ++cp;
            i += 2;
        } else {
            ++cp;
            ++i;
        }
    }
    return cp;
}

void apply_dark_caption(HWND hwnd) {
    if (!hwnd) return;
    BOOL enabled = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
}

void apply_dark_control_theme(HWND hwnd) {
    if (!hwnd) return;

    using SetWindowThemeFn = HRESULT (WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;

    auto set_window_theme = reinterpret_cast<SetWindowThemeFn>(
        GetProcAddress(ux, "SetWindowTheme"));
    if (set_window_theme) {
        set_window_theme(hwnd, L"DarkMode_Explorer", nullptr);
    }
    FreeLibrary(ux);
}

class ScopedWindowRedraw {
public:
    ScopedWindowRedraw(std::initializer_list<HWND> windows) {
        for (HWND window : windows) {
            if (window && IsWindowVisible(window)) {
                windows_.push_back(window);
            }
        }
        for (HWND window : windows_) {
            SendMessageW(window, WM_SETREDRAW, FALSE, 0);
        }
    }

    ~ScopedWindowRedraw() {
        for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
            SendMessageW(*it, WM_SETREDRAW, TRUE, 0);
        }
    }

    ScopedWindowRedraw(const ScopedWindowRedraw&) = delete;
    ScopedWindowRedraw& operator=(const ScopedWindowRedraw&) = delete;

private:
    std::vector<HWND> windows_;
};

} // namespace

bool HistoryWindow::initialize(HINSTANCE hInstance, TranscriptHistory* history,
                               ConfigManager* config) {
    hInstance_ = hInstance;
    history_ = history;
    config_ = config;
    rich_edit_module_ = LoadLibraryW(L"Msftedit.dll");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    app_icon_large_ = create_app_icon(32, RGB(0, 120, 215));
    app_icon_small_ = create_app_icon(16, RGB(0, 120, 215));
    wc.hIcon = app_icon_large_;
    wc.hIconSm = app_icon_small_;
    wc.lpszClassName = HISTORY_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    WNDCLASSEXW gutter_class = {};
    gutter_class.cbSize = sizeof(gutter_class);
    gutter_class.lpfnWndProc = ActionGutterWndProc;
    gutter_class.hInstance = hInstance_;
    gutter_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    gutter_class.hbrBackground = nullptr;
    gutter_class.lpszClassName = ACTION_GUTTER_CLASS_NAME;
    if (!RegisterClassExW(&gutter_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        0,
        HISTORY_CLASS_NAME,
        L"VoiceToText History",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1260,
        640,
        nullptr,
        nullptr,
        hInstance_,
        this);

    if (!hwnd_) return false;
    apply_dark_caption(hwnd_);
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(app_icon_large_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(app_icon_small_));

    create_fonts_and_brushes();
    create_child_controls();
    layout();
    refresh();
    return true;
}

void HistoryWindow::shutdown() {
    for (HWND h : { prompt_pre_label_, prompt_pre_edit_,
                    prompt_profile_label_, prompt_profile_edit_,
                    prompt_profile_combo_, prompt_profile_new_button_,
                    prompt_profile_rename_button_,
                    prompt_transform_label_, prompt_transform_edit_,
                    prompt_back_button_, prompt_save_button_,
                    prompt_status_ }) {
        if (h) DestroyWindow(h);
    }
    prompt_pre_label_ = nullptr;
    prompt_pre_edit_ = nullptr;
    prompt_profile_label_ = nullptr;
    prompt_profile_edit_ = nullptr;
    prompt_profile_combo_ = nullptr;
    prompt_profile_new_button_ = nullptr;
    prompt_profile_rename_button_ = nullptr;
    prompt_transform_label_ = nullptr;
    prompt_transform_edit_ = nullptr;
    prompt_back_button_ = nullptr;
    prompt_save_button_ = nullptr;
    prompt_status_ = nullptr;
    clear_entry_views();
    release_action_gutter_paint_buffers();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    recorded_edit_ = nullptr;
    transcribed_edit_ = nullptr;
    ai_language_edit_ = nullptr;
    recorded_action_gutter_ = nullptr;
    transcribed_action_gutter_ = nullptr;
    ai_language_action_gutter_ = nullptr;
    if (label_font_) DeleteObject(label_font_);
    if (text_font_) DeleteObject(text_font_);
    if (button_font_) DeleteObject(button_font_);
    if (background_brush_) DeleteObject(background_brush_);
    if (panel_brush_) DeleteObject(panel_brush_);
    if (control_brush_) DeleteObject(control_brush_);
    if (button_brush_) DeleteObject(button_brush_);
    if (app_icon_large_) DestroyIcon(app_icon_large_);
    if (app_icon_small_) DestroyIcon(app_icon_small_);
    label_font_ = nullptr;
    text_font_ = nullptr;
    button_font_ = nullptr;
    background_brush_ = nullptr;
    panel_brush_ = nullptr;
    control_brush_ = nullptr;
    button_brush_ = nullptr;
    app_icon_large_ = nullptr;
    app_icon_small_ = nullptr;
    if (hInstance_) {
        UnregisterClassW(ACTION_GUTTER_CLASS_NAME, hInstance_);
        UnregisterClassW(HISTORY_CLASS_NAME, hInstance_);
        hInstance_ = nullptr;
    }
    if (rich_edit_module_) {
        FreeLibrary(rich_edit_module_);
        rich_edit_module_ = nullptr;
    }
}

void HistoryWindow::show() {
    if (!hwnd_) return;
    // Showing the history window is a layout operation, not a data refresh.
    // Re-running all RichEdit formatting on every open made the first frame
    // noticeably slow even when the history had not changed.
    if (history_content_dirty_) refresh();
    const bool suspend_redraw = IsWindowVisible(hwnd_) != FALSE;
    if (suspend_redraw) SetWindowRedraw(hwnd_, FALSE);
    layout();
    update_entry_action_positions();
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    } else {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    }
    layout();
    update_entry_action_positions();
    if (suspend_redraw) SetWindowRedraw(hwnd_, TRUE);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW |
                 RDW_NOERASE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    scroll_history_to_top();
    update_entry_action_positions();
}

void HistoryWindow::refresh() {
    if (!history_ || !recorded_edit_ || !transcribed_edit_ ||
        !ai_language_edit_) return;

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    {
        ScopedWindowRedraw redraw({hwnd_, recorded_edit_, transcribed_edit_,
                                   ai_language_edit_});

        std::wstring recorded = normalize_crlf(
            utf8_to_wide(history_->format_recorded()));
        std::wstring transcribed = normalize_crlf(
            utf8_to_wide(history_->format_transcribed()));
        std::wstring ai_language = normalize_crlf(
            utf8_to_wide(history_->format_ai_language()));

        recorded = strip_delimiters_and_capture_ranges(
            recorded, panel_ranges_.recorded);
        transcribed = strip_delimiters_and_capture_ranges(
            transcribed, panel_ranges_.transcribed);
        ai_language = strip_delimiters_and_capture_ranges(
            ai_language, panel_ranges_.ai_language);

        SetWindowTextW(recorded_edit_, recorded.c_str());
        SetWindowTextW(transcribed_edit_, transcribed.c_str());
        SetWindowTextW(ai_language_edit_, ai_language.c_str());
        rebuild_entry_range_cache(recorded_edit_, recorded);
        rebuild_entry_range_cache(transcribed_edit_, transcribed);
        rebuild_entry_range_cache(ai_language_edit_, ai_language);
        apply_entry_paragraph_spacing();
        apply_header_colors(recorded_edit_);
        apply_header_colors(transcribed_edit_);
        apply_header_colors(ai_language_edit_);
        apply_entry_cards();
        rebuild_entry_views();
        scroll_history_to_top();
        update_entry_action_positions();
    }

    for (HWND edit : edits) InvalidateRect(edit, nullptr, FALSE);
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr,
                            RDW_INVALIDATE | RDW_ALLCHILDREN |
                            RDW_UPDATENOW | RDW_NOERASE);
    history_content_dirty_ = false;
}

void HistoryWindow::create_child_controls() {
    recorded_label_ = CreateWindowExW(
        0, L"STATIC", L"Recorded voice",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    transcribed_label_ = CreateWindowExW(
        0, L"STATIC", L"Clean transcript",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    ai_language_label_ = CreateWindowExW(
        0, L"STATIC", L"AI language",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);
    SetWindowTextW(recorded_label_, L"");
    SetWindowTextW(transcribed_label_, L"");
    SetWindowTextW(ai_language_label_, L"");
    ShowWindow(recorded_label_, SW_HIDE);
    ShowWindow(transcribed_label_, SW_HIDE);
    ShowWindow(ai_language_label_, SW_HIDE);

    DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                       WS_VSCROLL | ES_MULTILINE |
                       ES_AUTOVSCROLL | ES_READONLY | ES_LEFT | ES_NOHIDESEL;

    recorded_edit_ = CreateWindowExW(
        0, L"RICHEDIT50W", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    transcribed_edit_ = CreateWindowExW(
        0, L"RICHEDIT50W", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    ai_language_edit_ = CreateWindowExW(
        0, L"RICHEDIT50W", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    DWORD gutter_style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    recorded_action_gutter_ = CreateWindowExW(
        0, ACTION_GUTTER_CLASS_NAME, L"", gutter_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, this);
    transcribed_action_gutter_ = CreateWindowExW(
        0, ACTION_GUTTER_CLASS_NAME, L"", gutter_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, this);
    ai_language_action_gutter_ = CreateWindowExW(
        0, ACTION_GUTTER_CLASS_NAME, L"", gutter_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, this);

    clear_button_ = CreateWindowExW(
        0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLEAR_BUTTON)),
        hInstance_, nullptr);

    SendMessage(recorded_label_, WM_SETFONT, reinterpret_cast<WPARAM>(label_font_), TRUE);
    SendMessage(transcribed_label_, WM_SETFONT, reinterpret_cast<WPARAM>(label_font_), TRUE);
    SendMessage(ai_language_label_, WM_SETFONT, reinterpret_cast<WPARAM>(label_font_), TRUE);
    SendMessage(recorded_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
    SendMessage(transcribed_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
    SendMessage(ai_language_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), TRUE);
    SendMessage(clear_button_, WM_SETFONT, reinterpret_cast<WPARAM>(button_font_), TRUE);

    constexpr LPARAM history_text_limit = 16 * 1024 * 1024;
    SendMessageW(recorded_edit_, EM_EXLIMITTEXT, 0, history_text_limit);
    SendMessageW(transcribed_edit_, EM_EXLIMITTEXT, 0, history_text_limit);
    SendMessageW(ai_language_edit_, EM_EXLIMITTEXT, 0, history_text_limit);

    apply_modern_control_colors();
    apply_dark_control_theme(recorded_edit_);
    apply_dark_control_theme(transcribed_edit_);
    apply_dark_control_theme(ai_language_edit_);

    subclass_edit(recorded_edit_);
    subclass_edit(transcribed_edit_);
    subclass_edit(ai_language_edit_);

    // RichEdit sends EN_VSCROLL before it performs some scrollbar/thumb
    // operations.  Keep the existing event mask and add scroll notifications
    // so the sibling action gutter gets a final, post-scroll layout pass even
    // when no WM_MOUSEWHEEL reaches the subclassed edit.
    for (HWND edit : {recorded_edit_, transcribed_edit_, ai_language_edit_}) {
        if (!edit) continue;
        LRESULT mask = SendMessageW(edit, EM_GETEVENTMASK, 0, 0);
        SendMessageW(edit, EM_SETEVENTMASK, 0,
                     mask | static_cast<LRESULT>(ENM_SCROLL));
    }
}

void HistoryWindow::layout() {
    if (!hwnd_) return;

    if (profile_mode_) {
        layout_prompt_window();
        return;
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);

    int padding = 22;
    int gap = 16;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    int panel_width = (width - padding * 2 - gap * 2) / 3;
    int panel_top = padding;
    int panel_height = height - padding * 2;
    int edit_margin = PANEL_INNER;
    int edit_top_offset = 68;
    int edit_height = panel_height - edit_top_offset - edit_margin;
    int x1 = padding;
    int x2 = padding + panel_width + gap;
    int x3 = padding + (panel_width + gap) * 2;

    recorded_panel_rc_ = {x1, panel_top, x1 + panel_width, panel_top + panel_height};
    transcribed_panel_rc_ = {x2, panel_top, x2 + panel_width, panel_top + panel_height};
    ai_language_panel_rc_ = {x3, panel_top, x3 + panel_width, panel_top + panel_height};

    int button_width = 92;
    int button_x = x3 + panel_width - edit_margin - button_width;
    int button_y = panel_top + 16;

    // Reposition the complete layout first, then paint it once. Repainting
    // each child from MoveWindow made the opening/resize path present several
    // intermediate white frames.
    MoveWindow(recorded_label_, 0, 0, 0, 0, FALSE);
    MoveWindow(transcribed_label_, 0, 0, 0, 0, FALSE);
    MoveWindow(ai_language_label_, 0, 0, 0, 0, FALSE);
    MoveWindow(clear_button_, button_x, button_y, button_width, 32, FALSE);
    const int edit_width = panel_width - edit_margin * 2 -
                           ACTION_GUTTER_WIDTH;
    const int edit_y = panel_top + edit_top_offset;
    MoveWindow(recorded_action_gutter_, x1 + edit_margin, edit_y,
               ACTION_GUTTER_WIDTH, edit_height, FALSE);
    MoveWindow(transcribed_action_gutter_, x2 + edit_margin, edit_y,
               ACTION_GUTTER_WIDTH, edit_height, FALSE);
    MoveWindow(ai_language_action_gutter_, x3 + edit_margin, edit_y,
               ACTION_GUTTER_WIDTH, edit_height, FALSE);
    MoveWindow(recorded_edit_, x1 + edit_margin + ACTION_GUTTER_WIDTH, edit_y,
               edit_width, edit_height, FALSE);
    MoveWindow(transcribed_edit_, x2 + edit_margin + ACTION_GUTTER_WIDTH,
               edit_y, edit_width, edit_height, FALSE);
    MoveWindow(ai_language_edit_, x3 + edit_margin + ACTION_GUTTER_WIDTH,
               edit_y, edit_width, edit_height, FALSE);

    update_entry_action_positions();
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
}

void HistoryWindow::create_fonts_and_brushes() {
    label_font_ = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    text_font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    button_font_ = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    background_brush_ = CreateSolidBrush(COLOR_BG);
    panel_brush_ = CreateSolidBrush(COLOR_PANEL);
    control_brush_ = CreateSolidBrush(COLOR_CONTROL);
    button_brush_ = CreateSolidBrush(COLOR_BUTTON);
}

void HistoryWindow::paint_section(HDC hdc, const RECT& rc,
                                  const wchar_t* title,
                                  const wchar_t* subtitle) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    HPEN border_pen = CreatePen(PS_SOLID, 1, COLOR_PANEL_EDGE);
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);
    HGDIOBJ old_brush = SelectObject(hdc, panel_brush_);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom,
              PANEL_RADIUS, PANEL_RADIUS);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border_pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COLOR_LABEL);
    HGDIOBJ old_font = SelectObject(hdc, label_font_);
    RECT title_rc = {rc.left + SECTION_TITLE_INSET, rc.top + 14,
                     rc.right - SECTION_TITLE_INSET, rc.top + 36};
    DrawTextW(hdc, title, -1, &title_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SetTextColor(hdc, COLOR_MUTED);
    SelectObject(hdc, text_font_);
    RECT subtitle_rc = {rc.left + SECTION_TITLE_INSET, rc.top + 43,
                        rc.right - SECTION_TITLE_INSET, rc.top + 64};
    DrawTextW(hdc, subtitle, -1, &subtitle_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, old_font);
}

void HistoryWindow::draw_steel_button(const DRAWITEMSTRUCT* item) {
    if (!item) return;

    RECT rc = item->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC memDC = CreateCompatibleDC(item->hDC);
    HBITMAP memBmp = CreateCompatibleBitmap(item->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    RECT memRect = {0, 0, w, h};
    const bool inside_panel =
        item->CtlID == IDC_CLEAR_BUTTON ||
        item->CtlID == IDC_PROFILE_NEW ||
        item->CtlID == IDC_PROFILE_RENAME;
    FillRect(memDC, &memRect,
             inside_panel ? panel_brush_ : background_brush_);

    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;

    COLORREF fill = pressed
        ? RGB(29, 78, 118)
        : COLOR_BUTTON;
    COLORREF border = focused ? RGB(120, 185, 235) : RGB(68, 132, 178);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(memDC, brush);
    HGDIOBJ old_pen = SelectObject(memDC, pen);
    RoundRect(memDC, 0, 0, w, h, 10, 10);
    SelectObject(memDC, old_pen);
    SelectObject(memDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    RECT textRect = memRect;
    if (pressed) {
        OffsetRect(&textRect, 0, 1);
    }

    wchar_t text[128] = {};
    GetWindowTextW(item->hwndItem, text, 128);
    if (text[0] == L'\0') {
        HPEN icon_pen = CreatePen(PS_SOLID, 1, RGB(245, 250, 255));
        HGDIOBJ old_icon_pen = SelectObject(memDC, icon_pen);
        HGDIOBJ old_icon_brush =
            SelectObject(memDC, GetStockObject(NULL_BRUSH));

        int center_x = w / 2;
        int center_y = h / 2;
        RECT back = {center_x - 6, center_y - 6,
                     center_x + 3, center_y + 3};
        RECT front = {center_x - 3, center_y - 3,
                      center_x + 6, center_y + 6};
        Rectangle(memDC, back.left, back.top, back.right, back.bottom);
        Rectangle(memDC, front.left, front.top, front.right, front.bottom);

        SelectObject(memDC, old_icon_brush);
        SelectObject(memDC, old_icon_pen);
        DeleteObject(icon_pen);
    } else {
        HGDIOBJ old_font = SelectObject(memDC, button_font_);
        DrawTextW(memDC, text, -1, &textRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(memDC, old_font);
    }

    BitBlt(item->hDC, rc.left, rc.top, w, h, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

void HistoryWindow::draw_entry_action(HDC hdc, const RECT& rect,
                                      EntryAction action, bool hot,
                                      bool pressed, bool highlighted) const {
    if (!hdc || action == EntryAction::none ||
        rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    COLORREF fill = COLOR_BUTTON;
    COLORREF border = RGB(68, 132, 178);
    COLORREF glyph = RGB(245, 250, 255);
    if (action == EntryAction::profile) {
        fill = highlighted ? RGB(56, 110, 96) : RGB(35, 72, 64);
        border = highlighted ? RGB(145, 240, 210) : RGB(88, 170, 145);
        glyph = RGB(235, 255, 248);
        if (hot) fill = RGB(24, 86, 70);
        if (pressed) fill = RGB(39, 104, 84);
    } else {
        if (highlighted) {
            fill = COLOR_ENTRY_HIGHLIGHT;
            border = RGB(115, 170, 215);
        }
        if (hot) fill = RGB(27, 78, 118);
        if (pressed) fill = RGB(29, 68, 105);
    }

    int saved_dc = SaveDC(hdc);
    if (!saved_dc) return;
    SelectObject(hdc, GetStockObject(DC_BRUSH));
    SelectObject(hdc, GetStockObject(DC_PEN));
    SetDCBrushColor(hdc, fill);
    SetDCPenColor(hdc, border);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 9, 9);

    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2 + (pressed ? 1 : 0);
    SetDCPenColor(hdc, glyph);
    if (action == EntryAction::copy) {
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, cx - 6, cy - 6, cx + 3, cy + 3);
        Rectangle(hdc, cx - 3, cy - 3, cx + 6, cy + 6);
    } else {
        SetDCBrushColor(hdc, glyph);
        Ellipse(hdc, cx - 3, cy - 7, cx + 4, cy);
        RoundRect(hdc, cx - 7, cy + 2, cx + 8, cy + 8, 6, 6);
    }
    RestoreDC(hdc, saved_dc);
}

void HistoryWindow::paint_entry_actions(HWND edit, HDC hdc) const {
    if (!edit || !hdc || profile_mode_) {
        return;
    }

    auto intersects_clip = [&](const RECT& rect) {
        if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
        return RectVisible(hdc, &rect) != FALSE;
    };

    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.edit != edit) continue;

        // The same entry index is highlighted in every panel.  This is a
        // single-row update (old + new entry only), so it does not trigger a
        // full document reformat while the pointer moves.
        const bool highlighted = highlighted_entry_ == entry_view.entry_index;
        const bool copy_hot =
            hover_action_edit_ == edit &&
            hover_action_entry_ == entry_view.entry_index &&
            hover_action_ == EntryAction::copy;
        const bool copy_pressed =
            pressed_action_edit_ == edit &&
            pressed_action_entry_ == entry_view.entry_index &&
            pressed_action_ == EntryAction::copy;
        if (intersects_clip(entry_view.copy_rect)) {
            draw_entry_action(hdc, entry_view.copy_rect, EntryAction::copy,
                              copy_hot, copy_pressed, highlighted);
        }

        const bool profile_hot =
            hover_action_edit_ == edit &&
            hover_action_entry_ == entry_view.entry_index &&
            hover_action_ == EntryAction::profile;
        const bool profile_pressed =
            pressed_action_edit_ == edit &&
            pressed_action_entry_ == entry_view.entry_index &&
            pressed_action_ == EntryAction::profile;
        if (intersects_clip(entry_view.profile_rect)) {
            draw_entry_action(hdc, entry_view.profile_rect,
                              EntryAction::profile, profile_hot,
                              profile_pressed, highlighted);
        }
    }
}

HWND HistoryWindow::action_gutter_for_edit(HWND edit) const {
    if (edit == recorded_edit_) return recorded_action_gutter_;
    if (edit == transcribed_edit_) return transcribed_action_gutter_;
    if (edit == ai_language_edit_) return ai_language_action_gutter_;
    return nullptr;
}

HWND HistoryWindow::edit_for_action_gutter(HWND gutter) const {
    if (gutter == recorded_action_gutter_) return recorded_edit_;
    if (gutter == transcribed_action_gutter_) return transcribed_edit_;
    if (gutter == ai_language_action_gutter_) return ai_language_edit_;
    return nullptr;
}

HistoryWindow::PaintBuffer* HistoryWindow::paint_buffer_for_gutter(
    HWND gutter) {
    if (gutter == recorded_action_gutter_) {
        return &recorded_gutter_paint_buffer_;
    }
    if (gutter == transcribed_action_gutter_) {
        return &transcribed_gutter_paint_buffer_;
    }
    if (gutter == ai_language_action_gutter_) {
        return &ai_language_gutter_paint_buffer_;
    }
    return nullptr;
}

void HistoryWindow::release_action_gutter_paint_buffers() {
    auto release = [](PaintBuffer& buffer) {
        if (buffer.dc && buffer.bitmap && buffer.previous_bitmap) {
            SelectObject(buffer.dc, buffer.previous_bitmap);
        }
        if (buffer.bitmap) DeleteObject(buffer.bitmap);
        if (buffer.dc) DeleteDC(buffer.dc);
        buffer = {};
    };
    release(recorded_gutter_paint_buffer_);
    release(transcribed_gutter_paint_buffer_);
    release(ai_language_gutter_paint_buffer_);
}

void HistoryWindow::draw_action_gutter(HWND gutter, HDC hdc) const {
    if (!gutter || !hdc) return;
    RECT client = {};
    GetClientRect(gutter, &client);
    draw_action_gutter_region(gutter, hdc, client);
}

void HistoryWindow::draw_action_gutter_region(HWND gutter, HDC hdc,
                                              const RECT& dirty) const {
    if (!gutter || !hdc || dirty.right <= dirty.left ||
        dirty.bottom <= dirty.top) {
        return;
    }

    const int saved_dc = SaveDC(hdc);
    IntersectClipRect(hdc, dirty.left, dirty.top, dirty.right, dirty.bottom);

    // The gutter owns an opaque surface.  Only the invalidated region is
    // cleared and redrawn, so moving the pointer between actions cannot
    // cause every icon in the panel to be rasterized again.
    FillRect(hdc, &dirty, control_brush_);
    RECT client = {};
    GetClientRect(gutter, &client);
    {
        const int marker_dc = SaveDC(hdc);
        SelectObject(hdc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(hdc, COLOR_ENTRY_HIGHLIGHT);
        HWND edit = edit_for_action_gutter(gutter);
        for (const auto& entry_view : history_entry_views_) {
            if (entry_view.edit != edit ||
                entry_view.entry_index != highlighted_entry_ ||
                entry_view.copy_rect.right <= entry_view.copy_rect.left) {
                continue;
            }
            RECT marker = {0,
                           (std::max)(LONG(0), entry_view.copy_rect.top - 2),
                           (std::max)(LONG(0), client.right - 1),
                           (std::min)(client.bottom,
                                      entry_view.copy_rect.bottom + 2)};
            if (marker.bottom > marker.top) {
                FillRect(hdc, &marker,
                         reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            }
        }
        RestoreDC(hdc, marker_dc);
    }
    if (client.right > dirty.left && client.right <= dirty.right) {
        SelectObject(hdc, GetStockObject(DC_PEN));
        SetDCPenColor(hdc, COLOR_PANEL_EDGE);
        MoveToEx(hdc, client.right - 1, dirty.top, nullptr);
        LineTo(hdc, client.right - 1, dirty.bottom);
    }

    paint_entry_actions(edit_for_action_gutter(gutter), hdc);
    RestoreDC(hdc, saved_dc);
}

void HistoryWindow::paint_action_gutter(HWND gutter) {
    if (!gutter) return;
    HRGN update_region = CreateRectRgn(0, 0, 0, 0);
    const int update_region_state = update_region
        ? GetUpdateRgn(gutter, update_region, FALSE)
        : ERROR;
    PAINTSTRUCT paint = {};
    HDC target_dc = BeginPaint(gutter, &paint);
    if (!target_dc) {
        if (update_region) DeleteObject(update_region);
        EndPaint(gutter, &paint);
        return;
    }

    RECT client = {};
    GetClientRect(gutter, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    PaintBuffer* buffer = paint_buffer_for_gutter(gutter);
    if (buffer && !buffer->dc) {
        buffer->dc = CreateCompatibleDC(target_dc);
    }
    if (buffer && buffer->dc &&
        (!buffer->bitmap || buffer->width < width ||
         buffer->height < height)) {
        if (buffer->bitmap) {
            SelectObject(buffer->dc, buffer->previous_bitmap);
            DeleteObject(buffer->bitmap);
            buffer->bitmap = nullptr;
            buffer->previous_bitmap = nullptr;
        }
        buffer->width = (std::max)(buffer->width, width);
        buffer->height = (std::max)(buffer->height, height);
        buffer->bitmap = CreateCompatibleBitmap(
            target_dc, buffer->width, buffer->height);
        if (buffer->bitmap) {
            buffer->previous_bitmap =
                SelectObject(buffer->dc, buffer->bitmap);
            buffer->initialized = false;
        } else {
            buffer->width = 0;
            buffer->height = 0;
            buffer->initialized = false;
        }
    }

    if (buffer && buffer->dc && buffer->bitmap) {
        RECT dirty = paint.rcPaint;
        bool has_complex_update =
            update_region_state == SIMPLEREGION ||
            update_region_state == COMPLEXREGION;
        if (has_complex_update) {
            RECT region_box = {};
            if (GetRgnBox(update_region, &region_box) != NULLREGION) {
                dirty = region_box;
            } else {
                has_complex_update = false;
            }
        }
        if (!buffer->initialized) {
            draw_action_gutter(gutter, buffer->dc);
            buffer->initialized = true;
        } else {
            if (has_complex_update) {
                const int saved_dc = SaveDC(buffer->dc);
                SelectClipRgn(buffer->dc, update_region);
                draw_action_gutter_region(gutter, buffer->dc, dirty);
                RestoreDC(buffer->dc, saved_dc);
            } else {
                draw_action_gutter_region(gutter, buffer->dc, dirty);
            }
        }
        BitBlt(target_dc, dirty.left, dirty.top,
               dirty.right - dirty.left, dirty.bottom - dirty.top,
               buffer->dc, dirty.left, dirty.top, SRCCOPY);
    } else {
        draw_action_gutter(gutter, target_dc);
    }
    EndPaint(gutter, &paint);
    if (update_region) DeleteObject(update_region);
}

void HistoryWindow::redraw_action_gutter(HWND edit, bool immediate) const {
    HWND gutter = action_gutter_for_edit(edit);
    if (!gutter || !IsWindowVisible(gutter)) return;
    UINT flags = RDW_INVALIDATE | RDW_NOERASE;
    if (immediate) flags |= RDW_UPDATENOW;
    RedrawWindow(gutter, nullptr, nullptr, flags);
}

void HistoryWindow::draw_combo_item(const DRAWITEMSTRUCT* item) {
    if (!item || item->itemID == -1) return;

    RECT rc = item->rcItem;
    bool selected = (item->itemState & ODS_SELECTED) != 0;
    bool hot = (item->itemState & ODS_HOTLIGHT) != 0;

    COLORREF bg;
    if (selected) {
        bg = COLOR_ENTRY_HIGHLIGHT;
    } else if (hot) {
        bg = RGB(45, 65, 90);
    } else {
        bg = COLOR_CONTROL;
    }

    HBRUSH bg_brush = CreateSolidBrush(bg);
    HPEN border_pen = CreatePen(PS_SOLID, 1, bg);
    HGDIOBJ old_brush = SelectObject(item->hDC, bg_brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, border_pen);
    Rectangle(item->hDC, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(border_pen);
    DeleteObject(bg_brush);

    wchar_t buffer[256] = {};
    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID,
                 reinterpret_cast<LPARAM>(buffer));

    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC,
                 selected ? RGB(255, 255, 255) : COLOR_TEXT);
    HGDIOBJ old_font = SelectObject(item->hDC, text_font_);
    RECT text_rc = rc;
    text_rc.left += 6;
    DrawTextW(item->hDC, buffer, -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item->hDC, old_font);
}

void HistoryWindow::paint_background(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, background_brush_);

    if (hwnd == hwnd_ && profile_mode_) {
        paint_section(hdc, prompt_pre_panel_rc_, L"Cleanup role",
                      L"Repairs raw dictation before profile transform");
        paint_section(hdc, prompt_profile_panel_rc_, L"Profile",
                      L"Choose or edit the active transformation profile");
        paint_section(hdc, prompt_transform_panel_rc_, L"Transform role",
                      L"Profile-specific rewrite rules");
    } else if (hwnd == hwnd_) {
        paint_section(hdc, recorded_panel_rc_, L"Recorded voice",
                      L"Raw capture before cleanup");
        paint_section(hdc, transcribed_panel_rc_, L"Clean transcript",
                      L"Readable transcript output");
        paint_section(hdc, ai_language_panel_rc_, L"AI language",
                      L"Compact instruction view");
    }
}

void HistoryWindow::apply_modern_control_colors() {
    HWND rich_edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : rich_edits) {
        if (!edit) continue;
        SendMessage(edit, EM_SETBKGNDCOLOR, 0, COLOR_CONTROL);
        SendMessage(edit, EM_SETEDITSTYLE, SES_EXTENDBACKCOLOR,
                    SES_EXTENDBACKCOLOR);
        SendMessage(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                    MAKELPARAM(HISTORY_TEXT_LEFT_MARGIN,
                               HISTORY_TEXT_RIGHT_MARGIN));
    }
}

void HistoryWindow::apply_entry_paragraph_spacing() {
    HWND rich_edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : rich_edits) {
        if (!edit) continue;
        SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_SPACEAFTER | PFM_SPACEBEFORE;
        pf.dySpaceAfter = 60;
        pf.dySpaceBefore = 20;
        SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1),
                     static_cast<LPARAM>(-1));
    }
}

void HistoryWindow::apply_entry_cards() {
    HWND rich_edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : rich_edits) {
        if (!edit) continue;

        // Entry delimiters have already been removed from the RichEdit text,
        // so the card surface is one contiguous range.  Formatting every
        // entry separately caused hundreds of selection/scroll round-trips
        // while opening the history window.  One operation per panel keeps
        // the same visual background and makes refresh proportional to the
        // number of panels, not the number of entries.
        const int length = GetWindowTextLengthW(edit);
        if (length <= 0) continue;

        CHARRANGE saved = {};
        POINT scroll_pos = {};
        SendMessageW(edit, EM_EXGETSEL, 0,
                     reinterpret_cast<LPARAM>(&saved));
        SendMessageW(edit, EM_GETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&scroll_pos));
        set_char_range(edit, 0, length);

        CHARFORMAT2W format = {};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_BACKCOLOR;
        format.crBackColor = COLOR_ENTRY_CARD;
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&format));

        SendMessageW(edit, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&saved));
        SendMessageW(edit, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&scroll_pos));
    }
}

void HistoryWindow::subclass_edit(HWND edit) {
    if (!edit) return;

    SetPropW(edit, EDIT_OWNER_PROP, this);
    WNDPROC previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(EditWndProc)));
    if (!original_edit_proc_) {
        original_edit_proc_ = previous;
    }
}

void HistoryWindow::subclass_combo_box(HWND combo) {
    if (!combo) return;

    SetPropW(combo, L"VTTComboOwner", this);
    original_combo_proc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(combo, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ComboSubclassProc)));
}

LRESULT CALLBACK HistoryWindow::ComboSubclassProc(HWND hwnd, UINT msg,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
    HistoryWindow* self = reinterpret_cast<HistoryWindow*>(
        GetPropW(hwnd, L"VTTComboOwner"));

    switch (msg) {
    case WM_NCPAINT:
        if (self) {
            LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);

            HDC hdc = GetWindowDC(hwnd);
            if (hdc) {
                RECT rc;
                GetWindowRect(hwnd, &rc);
                OffsetRect(&rc, -rc.left, -rc.top);
                --rc.right;
                --rc.bottom;

                COLORREF border = self->combo_focused_
                                      ? RGB(68, 132, 178)
                                      : COLOR_PANEL_EDGE;
                HPEN pen = CreatePen(PS_SOLID, 1, border);
                HGDIOBJ old_pen = SelectObject(hdc, pen);
                SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(hdc, old_pen);
                DeleteObject(pen);

                ReleaseDC(hwnd, hdc);
            }
            return result;
        }
        break;

    case WM_SETFOCUS:
        if (self) {
            self->combo_focused_ = true;
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_FRAME | RDW_INVALIDATE);
        }
        break;

    case WM_KILLFOCUS:
        if (self) {
            self->combo_focused_ = false;
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_FRAME | RDW_INVALIDATE);
        }
        break;
    }

    return CallWindowProcW(
        self ? self->original_combo_proc_ : nullptr, hwnd, msg, wParam,
        lParam);
}

std::wstring HistoryWindow::get_edit_text(HWND edit) const {
    if (!edit) return {};
    int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};

    std::wstring text(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

const std::vector<std::pair<int, int>>&
HistoryWindow::entry_display_ranges(HWND edit) const {
    static const std::vector<std::pair<int, int>> empty;
    if (edit == recorded_edit_) return panel_ranges_.recorded;
    if (edit == transcribed_edit_) return panel_ranges_.transcribed;
    if (edit == ai_language_edit_) return panel_ranges_.ai_language;
    return empty;
}

const std::vector<std::pair<int, int>>&
HistoryWindow::entry_ranges(HWND edit) const {
    static const std::vector<std::pair<int, int>> empty;
    if (edit == recorded_edit_) return panel_cp_ranges_.recorded;
    if (edit == transcribed_edit_) return panel_cp_ranges_.transcribed;
    if (edit == ai_language_edit_) return panel_cp_ranges_.ai_language;
    return empty;
}

void HistoryWindow::rebuild_entry_range_cache(HWND edit,
                                              const std::wstring& text) {
    std::vector<std::pair<int, int>>* cached = nullptr;
    if (edit == recorded_edit_) cached = &panel_cp_ranges_.recorded;
    if (edit == transcribed_edit_) cached = &panel_cp_ranges_.transcribed;
    if (edit == ai_language_edit_) cached = &panel_cp_ranges_.ai_language;
    if (!cached) return;

    const auto& display_ranges = entry_display_ranges(edit);
    cached->clear();
    cached->reserve(display_ranges.size());

    size_t cursor = 0;
    int cp = 0;
    auto advance_to = [&](size_t target) {
        target = (std::min)(target, text.size());
        while (cursor < target) {
            if (text[cursor] == L'\r' && cursor + 1 < target &&
                text[cursor + 1] == L'\n') {
                cursor += 2;
            } else {
                ++cursor;
            }
            ++cp;
        }
        return cp;
    };

    for (const auto& display_range : display_ranges) {
        size_t start = static_cast<size_t>((std::max)(0, display_range.first));
        size_t end = static_cast<size_t>((std::max)(display_range.first,
                                                     display_range.second));
        end = (std::min)(end, text.size());
        while (end > start &&
               (text[end - 1] == L'\r' || text[end - 1] == L'\n' ||
                text[end - 1] == L' ' || text[end - 1] == L'\t')) {
            --end;
        }

        const int cp_start = advance_to(start);
        const int cp_end = advance_to(end);
        cached->emplace_back(cp_start, cp_end);
    }
}

int HistoryWindow::entry_index_from_char(HWND edit, int char_index) const {
    const auto& ranges = entry_ranges(edit);
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), char_index,
        [](int value, const std::pair<int, int>& range) {
            return value < range.first;
        });
    if (it == ranges.begin()) return -1;
    --it;
    if (char_index >= it->first && char_index < it->second) {
        return static_cast<int>(std::distance(ranges.begin(), it));
    }
    return -1;
}

std::wstring HistoryWindow::strip_delimiters_and_capture_ranges(
    const std::wstring& input,
    std::vector<std::pair<int, int>>& ranges) const {
    ranges.clear();
    std::wstring output;
    output.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        size_t begin = input.find(ENTRY_BEGIN, pos);
        if (begin == std::wstring::npos) {
            output.append(input.substr(pos));
            break;
        }

        output.append(input.substr(pos, begin - pos));

        size_t begin_close = input.find(L"]]", begin);
        if (begin_close == std::wstring::npos) {
            output.append(input.substr(begin));
            break;
        }

        size_t visible_start = output.size();
        size_t content_start = begin_close + 2;
        size_t end = input.find(ENTRY_END, content_start);
        if (end == std::wstring::npos) {
            output.append(input.substr(content_start));
            ranges.emplace_back(static_cast<int>(visible_start),
                                static_cast<int>(output.size()));
            break;
        }

        output.append(input.substr(content_start, end - content_start));
        size_t visible_end = output.size();
        ranges.emplace_back(static_cast<int>(visible_start),
                            static_cast<int>(visible_end));

        size_t end_close = input.find(L"]]", end);
        pos = end_close == std::wstring::npos ? end + wcslen(ENTRY_END)
                                              : end_close + 2;
    }

    return output;
}

void HistoryWindow::handle_edit_mouse_move(HWND edit, LPARAM lParam) {
    if (tracked_mouse_edit_ != edit) {
        TRACKMOUSEEVENT tracking = {};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = edit;
        TrackMouseEvent(&tracking);
        tracked_mouse_edit_ = edit;
    }

    RECT rc;
    GetClientRect(edit, &rc);
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    if (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) {
        set_entry_action_hover(nullptr, -1, EntryAction::none);
        set_highlighted_entry(-1);
        return;
    }

    set_entry_action_hover(nullptr, -1, EntryAction::none);

    int char_index = char_index_at(edit, x, y);
    const int entry_index = entry_index_from_char(edit, char_index);
    set_highlighted_entry(entry_index);
    ensure_highlighted_entry_visible(edit, entry_index);
}

void HistoryWindow::handle_action_gutter_mouse_move(HWND gutter,
                                                    int x, int y) {
    HWND edit = edit_for_action_gutter(gutter);
    if (!edit) return;

    if (tracked_action_gutter_ != gutter) {
        TRACKMOUSEEVENT tracking = {};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = gutter;
        TrackMouseEvent(&tracking);
        tracked_action_gutter_ = gutter;
    }

    int entry_index = -1;
    EntryAction action = EntryAction::none;
    if (entry_action_at(edit, x, y, entry_index, action)) {
        set_entry_action_hover(edit, entry_index, action);
    } else {
        // Empty gutter space still belongs to the row under the pointer.  It
        // lets the same entry remain marked when moving from text to its
        // action area, without scanning or repainting the whole document.
        const int char_index = char_index_at(edit, HISTORY_TEXT_LEFT_MARGIN, y);
        entry_index = entry_index_from_char(edit, char_index);
        set_entry_action_hover(nullptr, -1, EntryAction::none);
    }
    set_highlighted_entry(entry_index);
    ensure_highlighted_entry_visible(edit, entry_index);
}

bool HistoryWindow::entry_action_at(HWND edit, int x, int y,
                                    int& entry_index,
                                    EntryAction& action) const {
    POINT point = {x, y};
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.edit != edit) continue;
        if (entry_view.profile_rect.right > entry_view.profile_rect.left &&
            PtInRect(&entry_view.profile_rect, point)) {
            entry_index = entry_view.entry_index;
            action = EntryAction::profile;
            return true;
        }
        if (entry_view.copy_rect.right > entry_view.copy_rect.left &&
            PtInRect(&entry_view.copy_rect, point)) {
            entry_index = entry_view.entry_index;
            action = EntryAction::copy;
            return true;
        }
    }
    entry_index = -1;
    action = EntryAction::none;
    return false;
}

void HistoryWindow::invalidate_entry_action(HWND edit, int entry_index,
                                            EntryAction action) const {
    if (!edit || entry_index < 0 || action == EntryAction::none) return;
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.edit != edit ||
            entry_view.entry_index != entry_index) {
            continue;
        }
        RECT rect = action == EntryAction::profile
            ? entry_view.profile_rect
            : entry_view.copy_rect;
        if (rect.right > rect.left && rect.bottom > rect.top) {
            InflateRect(&rect, 1, 1);
            HWND gutter = action_gutter_for_edit(edit);
            if (gutter) InvalidateRect(gutter, &rect, FALSE);
        }
        return;
    }
}

void HistoryWindow::set_entry_action_hover(HWND edit, int entry_index,
                                           EntryAction action) {
    if (hover_action_edit_ == edit &&
        hover_action_entry_ == entry_index &&
        hover_action_ == action) {
        return;
    }

    invalidate_entry_action(hover_action_edit_, hover_action_entry_,
                            hover_action_);
    hover_action_edit_ = edit;
    hover_action_entry_ = entry_index;
    hover_action_ = action;
    invalidate_entry_action(hover_action_edit_, hover_action_entry_,
                            hover_action_);
}

void HistoryWindow::set_highlighted_entry(int entry_index) {
    if (entry_index < 0) entry_index = -1;
    if (highlighted_entry_ == entry_index) return;

    const int old_entry = highlighted_entry_;
    const bool old_scroll_guard = handling_history_scroll_;

    // RichEdit repaints (and can briefly reposition its viewport) for every
    // EM_SETCHARFORMAT call.  Doing that once per panel while the mouse moves
    // produces the visible blink/jump that used to make the three history
    // columns look unsynchronised.  Freeze the visible edits, change both
    // rows, then restore each edit's selection/scroll position and present
    // one small invalidation per affected row.
    struct EditState {
        HWND edit = nullptr;
        CHARRANGE selection = {};
        POINT scroll = {};
        RECT dirty = {};
        bool frozen = false;
    };
    std::vector<EditState> states;
    states.reserve(3);

    auto add_dirty_range = [&](EditState& state, int row) {
        if (row < 0) return;
        int start = 0;
        int end = 0;
        if (!entry_cp_range(state.edit, row, start, end)) return;

        RECT client = {};
        GetClientRect(state.edit, &client);
        POINTL first = {};
        POINTL last = {};
        SendMessageW(state.edit, EM_POSFROMCHAR,
                     reinterpret_cast<WPARAM>(&first), start);
        SendMessageW(state.edit, EM_POSFROMCHAR,
                     reinterpret_cast<WPARAM>(&last),
                     (std::max)(start, end - 1));

        // EM_POSFROMCHAR returns large negative values for characters outside
        // the viewport.  Such a row needs no immediate repaint; RichEdit will
        // paint its new colour when it is scrolled into view.
        if (first.y < -32000 || last.y < -32000) return;
        const int first_y = static_cast<int>(first.y);
        const int last_y = static_cast<int>(last.y);
        const int top = (std::max)(0, (std::min)(first_y, last_y) - 2);
        const int bottom = (std::min)(static_cast<int>(client.bottom),
                                      (std::max)(first_y, last_y) + 20);
        if (bottom <= top) return;
        RECT row_rect = {0, top, client.right, bottom};
        if (state.dirty.right <= state.dirty.left ||
            state.dirty.bottom <= state.dirty.top) {
            state.dirty = row_rect;
        } else {
            UnionRect(&state.dirty, &state.dirty, &row_rect);
        }
    };

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        if (!edit) continue;
        EditState state;
        state.edit = edit;
        SendMessageW(edit, EM_EXGETSEL, 0,
                     reinterpret_cast<LPARAM>(&state.selection));
        SendMessageW(edit, EM_GETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&state.scroll));
        add_dirty_range(state, old_entry);
        add_dirty_range(state, entry_index);
        state.frozen = IsWindowVisible(edit) != FALSE;
        if (state.frozen) SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
        states.push_back(state);
    }

    handling_history_scroll_ = true;

    auto apply_background_without_viewport_roundtrip =
        [&](HWND edit, int start, int end, COLORREF color) {
            if (!edit || start >= end) return;
            set_char_range(edit, start, end);
            CHARFORMAT2W format = {};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_BACKCOLOR;
            format.crBackColor = color;
            format.dwEffects = 0;
            SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                         reinterpret_cast<LPARAM>(&format));
        };

    for (const auto& state : states) {
        if (old_entry >= 0) {
            int start = 0;
            int end = 0;
            if (entry_cp_range(state.edit, old_entry, start, end)) {
                apply_background_without_viewport_roundtrip(
                    state.edit, start, end, COLOR_ENTRY_CARD);
            }
        }
        if (entry_index >= 0) {
            int start = 0;
            int end = 0;
            if (entry_cp_range(state.edit, entry_index, start, end)) {
                apply_background_without_viewport_roundtrip(
                    state.edit, start, end, COLOR_ENTRY_HIGHLIGHT);
            }
        }
    }

    for (const auto& state : states) {
        SendMessageW(state.edit, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&state.selection));
        SendMessageW(state.edit, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&state.scroll));
    }

    highlighted_entry_ = entry_index;

    for (const auto& state : states) {
        if (!state.frozen) continue;
        // Discard the large invalidation RichEdit may have accumulated while
        // formatting.  Repaint only the old/new visible rows below.
        ValidateRect(state.edit, nullptr);
        SendMessageW(state.edit, WM_SETREDRAW, TRUE, 0);
        if (state.dirty.right > state.dirty.left &&
            state.dirty.bottom > state.dirty.top) {
            RedrawWindow(state.edit, &state.dirty, nullptr,
                         RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
        }
    }

    handling_history_scroll_ = old_scroll_guard;

    auto invalidate_row = [&](int row) {
        if (row < 0) return;
        for (const auto& entry_view : history_entry_views_) {
            if (entry_view.entry_index != row) continue;
            HWND gutter = action_gutter_for_edit(entry_view.edit);
            if (!gutter || entry_view.copy_rect.right <= entry_view.copy_rect.left) {
                continue;
            }
            RECT dirty = entry_view.copy_rect;
            if (entry_view.profile_rect.right > entry_view.profile_rect.left) {
                UnionRect(&dirty, &dirty, &entry_view.profile_rect);
            }
            dirty.left = 0;
            dirty.right = ACTION_GUTTER_WIDTH;
            InflateRect(&dirty, 0, 3);
            InvalidateRect(gutter, &dirty, FALSE);
        }
    };
    invalidate_row(old_entry);
    invalidate_row(entry_index);
}

void HistoryWindow::ensure_highlighted_entry_visible(HWND source_edit,
                                                     int entry_index) {
    if (!source_edit || entry_index < 0 || handling_history_scroll_) return;

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    std::vector<HWND> to_scroll;
    for (HWND edit : edits) {
        if (!edit || edit == source_edit || !IsWindowVisible(edit)) continue;

        int start = 0;
        int end = 0;
        if (!entry_cp_range(edit, entry_index, start, end)) continue;

        RECT client = {};
        GetClientRect(edit, &client);
        const int top_cp = char_index_at(edit, HISTORY_TEXT_LEFT_MARGIN, 1);
        const int bottom_cp = char_index_at(
            edit, HISTORY_TEXT_LEFT_MARGIN,
            (std::max)(1, static_cast<int>(client.bottom) - 2));
        const bool visible = top_cp >= 0 && bottom_cp >= top_cp &&
            start < bottom_cp && end > top_cp;
        if (!visible) to_scroll.push_back(edit);
    }
    if (to_scroll.empty()) return;

    // Align only panels where the linked row is outside the viewport.  This
    // preserves the user's current scroll position in the source panel and
    // avoids the old behaviour of recentering all three columns on every
    // mouse move.  Redraw is suspended for the short batch so the text and
    // gutter icons appear at their new positions atomically.
    const bool old_scroll_guard = handling_history_scroll_;
    handling_history_scroll_ = true;
    for (HWND edit : to_scroll) {
        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    }

    for (HWND edit : to_scroll) {
        int start = 0;
        int end = 0;
        if (!entry_cp_range(edit, entry_index, start, end)) continue;

        const int target_line = static_cast<int>(
            SendMessageW(edit, EM_EXLINEFROMCHAR, 0, start));
        const int first_line = static_cast<int>(
            SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
        const int lead_lines = (std::max)(1, visible_line_count(edit) / 3);
        const int desired_first = (std::max)(0, target_line - lead_lines);
        const int delta = desired_first - first_line;
        if (delta != 0) {
            SendMessageW(edit, EM_LINESCROLL, 0, delta);
        }
    }

    for (HWND edit : to_scroll) {
        SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
        update_entry_action_positions(edit);
        RedrawWindow(edit, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
        redraw_action_gutter(edit, true);
    }
    handling_history_scroll_ = old_scroll_guard;
}

bool HistoryWindow::handle_entry_action_left_down(HWND edit, int x, int y) {
    int entry_index = -1;
    EntryAction action = EntryAction::none;
    if (!entry_action_at(edit, x, y, entry_index, action)) return false;

    set_entry_action_hover(edit, entry_index, action);
    pressed_action_edit_ = edit;
    pressed_action_entry_ = entry_index;
    pressed_action_ = action;
    HWND gutter = action_gutter_for_edit(edit);
    if (gutter) SetCapture(gutter);
    SetFocus(edit);
    invalidate_entry_action(edit, entry_index, action);
    return true;
}

void HistoryWindow::clear_pressed_entry_action() {
    HWND pressed_edit = pressed_action_edit_;
    int pressed_entry = pressed_action_entry_;
    EntryAction pressed_action = pressed_action_;
    pressed_action_edit_ = nullptr;
    pressed_action_entry_ = -1;
    pressed_action_ = EntryAction::none;
    invalidate_entry_action(pressed_edit, pressed_entry, pressed_action);
    if (GetCapture() == action_gutter_for_edit(pressed_edit)) {
        ReleaseCapture();
    }
}

bool HistoryWindow::handle_entry_action_left_up(HWND edit, int x, int y) {
    if (!pressed_action_edit_ || pressed_action_ == EntryAction::none) {
        return false;
    }

    HWND pressed_edit = pressed_action_edit_;
    int pressed_entry = pressed_action_entry_;
    EntryAction pressed_action = pressed_action_;
    int hit_entry = -1;
    EntryAction hit_action = EntryAction::none;
    bool activate = edit == pressed_edit &&
        entry_action_at(edit, x, y, hit_entry, hit_action) &&
        hit_entry == pressed_entry && hit_action == pressed_action;
    clear_pressed_entry_action();

    if (!activate) return true;
    if (pressed_action == EntryAction::copy) {
        copy_highlighted_entry(pressed_edit, pressed_entry);
        return true;
    }

    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.edit == pressed_edit &&
            entry_view.entry_index == pressed_entry &&
            !entry_view.profile.empty()) {
            show_profile_prompt(entry_view.profile);
            break;
        }
    }
    return true;
}

void HistoryWindow::clear_entry_views() {
    set_entry_action_hover(nullptr, -1, EntryAction::none);
    set_highlighted_entry(-1);
    clear_pressed_entry_action();
    history_entry_views_.clear();
}

void HistoryWindow::rebuild_entry_views() {
    clear_entry_views();

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        if (!edit) continue;
        const auto& ranges = entry_ranges(edit);
        const auto& links = links_for(edit);
        size_t link_index = 0;
        for (int i = 0; i < static_cast<int>(ranges.size()); ++i) {
            HistoryEntryView entry_view;
            entry_view.edit = edit;
            entry_view.entry_index = i;
            entry_view.start = ranges[static_cast<size_t>(i)].first;
            entry_view.end = ranges[static_cast<size_t>(i)].second;
            while (link_index < links.size() &&
                   links[link_index].end <= entry_view.start) {
                ++link_index;
            }
            if (link_index < links.size()) {
                const auto& link = links[link_index];
                if (link.start >= entry_view.start &&
                    link.end <= entry_view.end) {
                    entry_view.profile = link.profile;
                }
            }
            history_entry_views_.push_back(std::move(entry_view));
        }
    }
    update_entry_action_positions();
}

void HistoryWindow::scroll_history_to_top() {
    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    POINT top = {0, 0};
    const bool was_handling_scroll = handling_history_scroll_;
    handling_history_scroll_ = true;

    for (HWND edit : edits) {
        if (!edit) continue;
        SendMessageW(edit, EM_SETSEL, 0, 0);
        SendMessageW(edit, WM_VSCROLL, SB_TOP, 0);
        SendMessageW(edit, EM_SCROLLCARET, 0, 0);
        SendMessageW(edit, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&top));
        SendMessageW(edit, WM_VSCROLL, SB_TOP, 0);
    }
    handling_history_scroll_ = was_handling_scroll;
}

void HistoryWindow::update_entry_action_positions(HWND only_edit) {
    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        if (!edit || (only_edit && edit != only_edit)) continue;

        RECT edit_rect = {};
        GetClientRect(edit, &edit_rect);
        const int visible_top = 2;
        const int visible_bottom = edit_rect.bottom - 2;
        const int text_length = GetWindowTextLengthW(edit);

        int visible_start = char_index_at(edit, HISTORY_TEXT_LEFT_MARGIN,
                                          visible_top);
        int visible_end = char_index_at(edit, HISTORY_TEXT_LEFT_MARGIN,
                                        (std::max)(visible_top,
                                                   visible_bottom - 1));
        if (visible_start < 0) visible_start = 0;
        if (visible_end < visible_start) visible_end = text_length;

        HDC hdc = GetDC(edit);
        HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(edit, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = hdc && font ? SelectObject(hdc, font) : nullptr;
        TEXTMETRICW metrics = {};
        if (hdc) GetTextMetricsW(hdc, &metrics);
        if (hdc && old_font) SelectObject(hdc, old_font);
        if (hdc) ReleaseDC(edit, hdc);
        int line_height = metrics.tmHeight + metrics.tmExternalLeading;
        if (line_height <= 0) line_height = 16;

        for (auto& entry_view : history_entry_views_) {
            if (entry_view.edit != edit) continue;
            SetRectEmpty(&entry_view.copy_rect);
            SetRectEmpty(&entry_view.profile_rect);

            if (visible_bottom <= visible_top + ENTRY_COPY_HEIGHT ||
                entry_view.end <= visible_start ||
                entry_view.start > visible_end) {
                continue;
            }

            POINTL header_position = {};
            SendMessageW(edit, EM_POSFROMCHAR,
                         reinterpret_cast<WPARAM>(&header_position),
                         entry_view.start);
            const int header_y = static_cast<int>(header_position.y);
            if (header_y < 0 || header_y >= visible_bottom) {
                continue;
            }

            const int copy_x = ACTION_GUTTER_INSET;
            // Keep the icon in the same coordinate space as the header line.
            // Do not clamp it to the gutter edges: clamping made an icon
            // appear stuck at the top/bottom while its text continued to
            // scroll.  The gutter's clip naturally hides the offscreen part.
            const int copy_y = header_y +
                               (line_height - ENTRY_COPY_HEIGHT) / 2;
            if (copy_y >= visible_bottom ||
                copy_y + ENTRY_COPY_HEIGHT <= visible_top) {
                continue;
            }
            entry_view.copy_rect = {
                copy_x,
                copy_y,
                copy_x + ENTRY_COPY_WIDTH,
                copy_y + ENTRY_COPY_HEIGHT,
            };

            if (entry_view.profile.empty()) continue;
            const int profile_x = ACTION_GUTTER_INSET + ENTRY_COPY_WIDTH +
                                  PROFILE_BUTTON_GAP;
            const int profile_y = header_y +
                                  (line_height - PROFILE_BUTTON_HEIGHT) / 2;
            if (profile_y >= visible_bottom ||
                profile_y + PROFILE_BUTTON_HEIGHT <= visible_top) {
                continue;
            }
            entry_view.profile_rect = {
                profile_x,
                profile_y,
                profile_x + PROFILE_BUTTON_WIDTH,
                profile_y + PROFILE_BUTTON_HEIGHT,
            };
        }
        redraw_action_gutter(edit);
    }

}

void HistoryWindow::copy_highlighted_entry(HWND edit, int entry_index) {
    if (!edit || entry_index < 0) return;

    std::wstring text = entry_body_text(edit, entry_index);
    if (text.empty()) return;

    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;

    void* dest = GlobalLock(memory);
    if (!dest) {
        GlobalFree(memory);
        return;
    }
    memcpy(dest, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(hwnd_)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
}

std::wstring HistoryWindow::entry_body_text(HWND edit, int entry_index) const {
    if (!edit || entry_index < 0) return {};

    const auto& ranges = entry_display_ranges(edit);
    if (entry_index >= static_cast<int>(ranges.size())) return {};

    std::wstring text = get_edit_text(edit);
    size_t start = static_cast<size_t>(ranges[entry_index].first);
    size_t end = static_cast<size_t>(ranges[entry_index].second);
    if (start >= text.size() || end > text.size() || start >= end) return {};

    size_t body_start = text.find_first_of(L"\r\n", start);
    if (body_start == std::wstring::npos || body_start >= end) {
        body_start = start;
    } else {
        while (body_start < end &&
               (text[body_start] == L'\r' || text[body_start] == L'\n')) {
            ++body_start;
        }
    }

    while (end > body_start &&
           (text[end - 1] == L'\r' || text[end - 1] == L'\n' ||
            text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }

    return body_start < end ? text.substr(body_start, end - body_start)
                            : std::wstring();
}

bool HistoryWindow::entry_cp_range(HWND edit, int entry_index,
                                   int& start, int& end) const {
    if (!edit || entry_index < 0) return false;

    const auto& ranges = entry_ranges(edit);
    if (entry_index >= static_cast<int>(ranges.size())) return false;
    start = ranges[static_cast<size_t>(entry_index)].first;
    end = ranges[static_cast<size_t>(entry_index)].second;
    return start < end;
}

int HistoryWindow::visible_line_count(HWND edit) const {
    if (!edit) return 12;

    RECT rc;
    GetClientRect(edit, &rc);

    HDC hdc = GetDC(edit);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    if (old_font) SelectObject(hdc, old_font);
    ReleaseDC(edit, hdc);

    int line_height = tm.tmHeight + tm.tmExternalLeading;
    if (line_height <= 0) line_height = 16;
    return (std::max)(1, static_cast<int>(rc.bottom - rc.top) / line_height);
}

void HistoryWindow::set_char_range(HWND edit, int start, int end) {
    CHARRANGE range;
    range.cpMin = start;
    range.cpMax = end;
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
}

void HistoryWindow::apply_header_colors(HWND edit) {
    if (!edit) return;

    CHARRANGE saved;
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));

    auto& links = links_for(edit);
    links.clear();

    std::wstring text = get_edit_text(edit);
    int length = rich_edit_cp_from_text_offset(text, text.size());
    if (length > 0) {
        set_char_range(edit, 0, length);
        CHARFORMAT2W base = {};
        base.cbSize = sizeof(base);
        base.dwMask = CFM_COLOR | CFM_HIDDEN | CFM_UNDERLINE;
        base.crTextColor = COLOR_TEXT;
        base.dwEffects = 0;
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&base));
    }

    const auto& ranges = entry_display_ranges(edit);
    const auto& cp_ranges = entry_ranges(edit);
    for (size_t range_index = 0; range_index < ranges.size(); ++range_index) {
        const auto& range = ranges[range_index];
        size_t header_start = text.find(L"[", static_cast<size_t>(range.first));
        if (header_start == std::wstring::npos ||
            header_start >= static_cast<size_t>(range.second)) {
            continue;
        }

        size_t header_end = text.find_first_of(L"\r\n", header_start);
        if (header_end == std::wstring::npos ||
            header_end > static_cast<size_t>(range.second)) {
            header_end = static_cast<size_t>(range.second);
        }

        std::wstring_view header(text.data() + header_start,
                                 header_end - header_start);
        auto absolute_header_offset = [&](size_t offset) {
            return offset == std::wstring_view::npos
                ? std::wstring::npos
                : header_start + offset;
        };
        size_t profile_start =
            absolute_header_offset(header.find(L"Profile:"));
        size_t tokens_start =
            absolute_header_offset(header.find(L"Tokens:"));

        int header_cp_start = range_index < cp_ranges.size()
            ? cp_ranges[range_index].first +
                  static_cast<int>(header_start -
                                   static_cast<size_t>(range.first))
            : rich_edit_cp_from_text_offset(text, header_start);
        auto header_cp_at = [&](size_t offset) {
            return header_cp_start +
                   static_cast<int>(offset - header_start);
        };

        if (profile_start != std::wstring::npos && profile_start < header_end) {
            int profile_end = tokens_start != std::wstring::npos
                ? static_cast<int>(tokens_start)
                : static_cast<int>(header_end);

            const wchar_t* prefix = L"Profile:";
            size_t value_start = profile_start + wcslen(prefix);
            while (value_start < static_cast<size_t>(profile_end) &&
                   (text[value_start] == L' ' || text[value_start] == L'\t')) {
                ++value_start;
            }
            size_t value_end = static_cast<size_t>(profile_end);
            while (value_end > value_start &&
                   (text[value_end - 1] == L' ' || text[value_end - 1] == L'\t')) {
                --value_end;
            }
            if (value_start < value_end) {
                int value_cp_start = header_cp_at(value_start);
                int value_cp_end = header_cp_at(value_end);
                apply_profile_link_format(edit, value_cp_start, value_cp_end);
                ProfileLink link;
                link.start = value_cp_start;
                link.end = value_cp_end;
                link.profile = trim_whitespace(text, value_start, value_end);
                if (!link.profile.empty()) {
                    links.push_back(std::move(link));
                }
            }
        }
        // Date/token text intentionally keeps the base COLOR_TEXT style. The
        // old implementation issued two extra RichEdit format operations for
        // every entry, which made the first history frame expensive for no
        // interaction benefit. Profile values remain styled and clickable.
    }

    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
}

void HistoryWindow::apply_profile_link_format(HWND edit, int start, int end) {
    if (!edit || start >= end) return;
    set_char_range(edit, start, end);
    CHARFORMAT2W format = {};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_UNDERLINE | CFM_BOLD;
    format.crTextColor = COLOR_PROFILE;
    format.dwEffects = CFE_UNDERLINE | CFE_BOLD;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));
}

std::vector<HistoryWindow::ProfileLink>& HistoryWindow::links_for(HWND edit) {
    if (edit == transcribed_edit_) return panel_links_.transcribed;
    if (edit == ai_language_edit_) return panel_links_.ai_language;
    return panel_links_.recorded;
}

const std::vector<HistoryWindow::ProfileLink>&
HistoryWindow::links_for(HWND edit) const {
    if (edit == transcribed_edit_) return panel_links_.transcribed;
    if (edit == ai_language_edit_) return panel_links_.ai_language;
    return panel_links_.recorded;
}

int HistoryWindow::char_index_at(HWND edit, int x, int y) const {
    if (!edit) return -1;
    POINTL pt;
    pt.x = x;
    pt.y = y;
    LRESULT result = SendMessageW(edit, EM_CHARFROMPOS, 0,
                                  reinterpret_cast<LPARAM>(&pt));
    if (result < 0) return -1;
    return static_cast<int>(result);
}

bool HistoryWindow::is_over_profile(HWND edit, int x, int y) const {
    int index = char_index_at(edit, x, y);
    if (index < 0) return false;
    const auto& links = links_for(edit);
    const auto it = std::lower_bound(
        links.begin(), links.end(), index,
        [](const ProfileLink& link, int position) {
            return link.end <= position;
        });
    return it != links.end() && index >= it->start && index < it->end;
}

bool HistoryWindow::handle_edit_left_click(HWND edit, LPARAM lParam) {
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    int index = char_index_at(edit, x, y);
    if (index < 0) return false;
    const auto& links = links_for(edit);
    const auto it = std::lower_bound(
        links.begin(), links.end(), index,
        [](const ProfileLink& link, int position) {
            return link.end <= position;
        });
    if (it != links.end() && index >= it->start && index < it->end) {
        show_profile_prompt(it->profile);
        return true;
    }
    return false;
}

void HistoryWindow::populate_profile_combo(const std::string& selected) {
    if (!prompt_profile_combo_ || !config_) return;

    SendMessageW(prompt_profile_combo_, CB_RESETCONTENT, 0, 0);
    for (const auto& profile : config_->config().llm_profiles) {
        SendMessageW(prompt_profile_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         utf8_to_wide(profile.name).c_str()));
    }

    std::string profile_name = selected.empty() ? "general" : selected;
    SetWindowTextW(prompt_profile_combo_, utf8_to_wide(profile_name).c_str());
    SendMessageW(prompt_profile_combo_, CB_SELECTSTRING, static_cast<WPARAM>(-1),
                 reinterpret_cast<LPARAM>(utf8_to_wide(profile_name).c_str()));
}

void HistoryWindow::select_profile_for_editing(const std::string& profile) {
    if (!config_) return;

    std::string profile_name = profile.empty() ? "general" : profile;
    const auto& profiles = config_->config().llm_profiles;
    auto it = std::find_if(profiles.begin(), profiles.end(),
                           [&](const ProfileDefinition& item) {
                               return item.name == profile_name;
                           });
    if (it == profiles.end()) {
        it = std::find_if(profiles.begin(), profiles.end(),
                          [](const ProfileDefinition& item) {
                              return item.name == "general";
                          });
    }
    if (it == profiles.end() && !profiles.empty()) {
        it = profiles.begin();
    }

    if (it == profiles.end()) return;
    active_prompt_profile_ = it->name;
    SetWindowTextW(prompt_profile_combo_, utf8_to_wide(it->name).c_str());
    SendMessageW(prompt_profile_combo_, CB_SELECTSTRING, static_cast<WPARAM>(-1),
                 reinterpret_cast<LPARAM>(utf8_to_wide(it->name).c_str()));
    SetWindowTextW(prompt_transform_edit_,
                   utf8_to_wide(it->instructions).c_str());
}

std::string HistoryWindow::current_profile_name_from_ui() const {
    std::string name = trim_ascii(wide_to_utf8(get_edit_text(prompt_profile_combo_)));
    return name;
}

void HistoryWindow::create_new_profile() {
    if (!config_) return;

    auto& profiles = config_->config().llm_profiles;
    std::string base = "new-profile";
    std::string name = base;
    int suffix = 2;
    auto exists = [&](const std::string& candidate) {
        return std::any_of(profiles.begin(), profiles.end(),
                           [&](const ProfileDefinition& item) {
                               return item.name == candidate;
                           });
    };
    while (exists(name)) {
        name = base + "-" + std::to_string(suffix++);
    }

    profiles.push_back({
        name,
        u8"Beskriv hur den h\u00E4r profilen ska skriva om texten."
    });
    populate_profile_combo(name);
    select_profile_for_editing(name);
    config_->save();
    SetWindowTextW(prompt_status_, L"New profile created. Edit instructions, then save.");
    SetFocus(prompt_profile_combo_);
}

void HistoryWindow::rename_current_profile() {
    if (!config_) return;

    std::string new_name = current_profile_name_from_ui();
    if (new_name.empty() || active_prompt_profile_.empty()) {
        SetWindowTextW(prompt_status_, L"Type a profile name before renaming.");
        return;
    }

    auto& profiles = config_->config().llm_profiles;
    auto active = std::find_if(profiles.begin(), profiles.end(),
                               [&](const ProfileDefinition& item) {
                                   return item.name == active_prompt_profile_;
                               });
    if (active == profiles.end()) return;

    auto duplicate = std::find_if(profiles.begin(), profiles.end(),
                                  [&](const ProfileDefinition& item) {
                                      return item.name == new_name &&
                                             item.name != active_prompt_profile_;
                                  });
    if (duplicate != profiles.end()) {
        SetWindowTextW(prompt_status_, L"That profile name already exists.");
        return;
    }

    active->name = new_name;
    active_prompt_profile_ = new_name;
    populate_profile_combo(new_name);
    config_->save();
    SetWindowTextW(prompt_status_, L"Profile renamed.");
}

void HistoryWindow::show_profile_prompt(const std::string& profile) {
    if (profile.empty()) return;

    // Lazily create the window and its child controls on first use. Three
    // editable sections mirror the two-step LLM pipeline:
    //   Step 1 (editable) — cleanup role applied to raw dictation
    //   Step 2 (read-only) — runtime profile selection (informational)
    //   Step 3 (editable) — per-profile transform role applied to cleaned text
    if (!prompt_pre_edit_) {
        DWORD edit_common = WS_CHILD | WS_VSCROLL |
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_LEFT |
                            ES_NOHIDESEL | ES_WANTRETURN;
        DWORD edit_editable = edit_common;
        DWORD edit_readonly = edit_common | ES_READONLY;
        DWORD label_style = WS_CHILD | SS_LEFT;

        prompt_pre_label_ = CreateWindowExW(
            0, L"STATIC",
            L"Step 1 — Cleanup role (editable). "
            L"Fixes transcription errors, punctuation, casing, grammar, and "
            L"structure on the raw dictation before profile transform.",
            label_style, 0, 0, 0, 0,
            hwnd_, nullptr, hInstance_, nullptr);
        prompt_pre_edit_ = CreateWindowExW(
            0, L"EDIT", L"", edit_editable,
            0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

        prompt_profile_label_ = CreateWindowExW(
            0, L"STATIC",
            L"Step 2 — Selected profile (read-only, runtime classifier output)",
            label_style, 0, 0, 0, 0,
            hwnd_, nullptr, hInstance_, nullptr);
        prompt_profile_edit_ = CreateWindowExW(
            0, L"EDIT", L"", edit_readonly,
            0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);
        ShowWindow(prompt_profile_edit_, SW_HIDE);

        prompt_profile_combo_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VSCROLL | WS_BORDER |
                CBS_DROPDOWN | CBS_AUTOHSCROLL |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_COMBO)),
            hInstance_, nullptr);
        subclass_combo_box(prompt_profile_combo_);

        prompt_profile_new_button_ = CreateWindowExW(
            0, L"BUTTON", L"New",
            WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_NEW)),
            hInstance_, nullptr);

        prompt_profile_rename_button_ = CreateWindowExW(
            0, L"BUTTON", L"Rename",
            WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_RENAME)),
            hInstance_, nullptr);

        prompt_transform_label_ = CreateWindowExW(
            0, L"STATIC",
            L"Step 3 — Transform role (editable). "
            L"Per-profile rewrite rules applied to the cleaned text from Step 1.",
            label_style, 0, 0, 0, 0,
            hwnd_, nullptr, hInstance_, nullptr);
        prompt_transform_edit_ = CreateWindowExW(
            0, L"EDIT", L"", edit_editable,
            0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

        prompt_back_button_ = CreateWindowExW(
            0, L"BUTTON", L"Back",
            WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROMPT_BACK)),
            hInstance_, nullptr);

        prompt_save_button_ = CreateWindowExW(
            0, L"BUTTON", L"Save roles",
            WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROMPT_SAVE)),
            hInstance_, nullptr);

        prompt_status_ = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | SS_CENTER, 0, 0, 0, 0,
            hwnd_, nullptr, hInstance_, nullptr);

        SetWindowTextW(prompt_pre_label_,
                       L"Step 1 - Cleanup role (editable). "
                       L"Fixes transcription errors, punctuation, casing, "
                       L"grammar, and structure before profile transform.");
        SetWindowTextW(prompt_profile_label_,
                       L"Step 2 - Detected profile (read-only). "
                       L"The app chose this profile for the clicked entry.");
        SetWindowTextW(prompt_transform_label_,
                       L"Step 3 - Transform role (editable). "
                       L"Profile-specific rewrite rules for the cleaned text.");

        for (HWND h : { prompt_pre_label_, prompt_pre_edit_,
                        prompt_profile_label_, prompt_profile_edit_,
                        prompt_profile_combo_, prompt_profile_new_button_,
                        prompt_profile_rename_button_,
                        prompt_transform_label_, prompt_transform_edit_,
                        prompt_back_button_, prompt_save_button_,
                        prompt_status_ }) {
            SendMessage(h, WM_SETFONT,
                        reinterpret_cast<WPARAM>(
                            (h == prompt_save_button_ ||
                             h == prompt_back_button_ ||
                             h == prompt_profile_new_button_ ||
                             h == prompt_profile_rename_button_)
                                ? button_font_
                                : text_font_),
                        TRUE);
        }
        SendMessage(prompt_pre_edit_, EM_SETLIMITTEXT, 0, 0);
        SendMessage(prompt_transform_edit_, EM_SETLIMITTEXT, 0, 0);
        for (HWND h : { prompt_pre_edit_, prompt_profile_edit_, prompt_transform_edit_ }) {
            SendMessage(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                        MAKELPARAM(SECTION_TITLE_INSET - PANEL_INNER,
                                   PROMPT_TEXT_RIGHT_MARGIN));
            apply_dark_control_theme(h);
        }
        apply_dark_control_theme(prompt_profile_combo_);
    }

    if (config_) {
        const auto& cfg = config_->config();
        SetWindowTextW(prompt_pre_edit_, utf8_to_wide(cfg.llm_pre_prompt).c_str());
        populate_profile_combo(profile);
        select_profile_for_editing(profile);
    }
    SetWindowTextW(prompt_status_, L"");

    {
        ScopedWindowRedraw redraw({hwnd_});
        profile_mode_ = true;
        for (HWND h : { recorded_edit_, transcribed_edit_, ai_language_edit_,
                        recorded_action_gutter_, transcribed_action_gutter_,
                        ai_language_action_gutter_, clear_button_ }) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        set_entry_action_hover(nullptr, -1, EntryAction::none);
        clear_pressed_entry_action();
        for (HWND h : { prompt_pre_label_, prompt_pre_edit_,
                        prompt_profile_label_, prompt_profile_combo_,
                        prompt_profile_new_button_,
                        prompt_profile_rename_button_,
                        prompt_transform_label_, prompt_transform_edit_,
                        prompt_back_button_, prompt_save_button_,
                        prompt_status_ }) {
            if (h) ShowWindow(h, SW_SHOW);
        }
        layout();
    }
    SetFocus(prompt_pre_edit_);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW |
                 RDW_NOERASE);
}

void HistoryWindow::show_history_view() {
    {
        ScopedWindowRedraw redraw({hwnd_});
        profile_mode_ = false;
        for (HWND h : { prompt_pre_label_, prompt_pre_edit_,
                        prompt_profile_label_, prompt_profile_edit_,
                        prompt_profile_combo_, prompt_profile_new_button_,
                        prompt_profile_rename_button_,
                        prompt_transform_label_, prompt_transform_edit_,
                        prompt_back_button_, prompt_save_button_,
                        prompt_status_ }) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        for (HWND h : { recorded_edit_, transcribed_edit_, ai_language_edit_,
                        recorded_action_gutter_, transcribed_action_gutter_,
                        ai_language_action_gutter_, clear_button_ }) {
            if (h) ShowWindow(h, SW_SHOW);
        }
        layout();
        update_entry_action_positions();
    }
    SetFocus(hwnd_);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW |
                 RDW_NOERASE);
}

void HistoryWindow::layout_prompt_window() {
    if (!hwnd_ || !profile_mode_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int width  = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    int padding       = 24;
    int section_gap   = 14;
    int toolbar_gap   = 18;
    int toolbar_height = 38;
    int button_height = 34;
    int button_width  = 132;
    int short_height  = 32;       // step 2 profile controls row
    int small_button_width = 86;
    int control_gap = 10;

    int content_width = (std::max)(100, width - padding * 2);
    int toolbar_y = padding;
    int toolbar_status_x = padding + button_width + control_gap;
    int toolbar_status_width = (std::max)(
        0, width - padding * 2 - button_width * 2 - control_gap * 2);
    MoveWindow(prompt_back_button_, padding, toolbar_y,
               button_width, button_height, TRUE);
    MoveWindow(prompt_save_button_, width - padding - button_width,
               toolbar_y, button_width, button_height, TRUE);
    MoveWindow(prompt_status_, toolbar_status_x, toolbar_y + 8,
               toolbar_status_width, button_height, TRUE);

    int content_top = padding + toolbar_height + toolbar_gap;
    int content_bottom = height - padding;
    int panel_header = 76;
    int panel_inner = SECTION_TITLE_INSET;
    int profile_panel_height = panel_header + panel_inner + short_height
                               + panel_inner;
    int flex_total = content_bottom - content_top - section_gap * 2
                     - profile_panel_height
                     - (panel_header + panel_inner * 2) * 2;
    int flex_height = (std::max)(48, flex_total / 2);

    int y = content_top;
    int pre_panel_height = panel_header + panel_inner + flex_height + panel_inner;
    prompt_pre_panel_rc_ = {padding, y, padding + content_width, y + pre_panel_height};
    MoveWindow(prompt_pre_label_, 0, 0, 0, 0, TRUE);
    MoveWindow(prompt_pre_edit_, padding + panel_inner, y + panel_header,
               content_width - panel_inner * 2, flex_height, TRUE);
    y += pre_panel_height + section_gap;

    prompt_profile_panel_rc_ = {padding, y, padding + content_width,
                                y + profile_panel_height};
    MoveWindow(prompt_profile_label_, 0, 0, 0, 0, TRUE);
    MoveWindow(prompt_profile_edit_, 0, 0, 0, 0, TRUE);
    int profile_controls_y = y + panel_header;
    int profile_x = padding + panel_inner;
    int profile_buttons_width = small_button_width * 2 + control_gap * 2;
    int combo_width = (std::max)(
        160, content_width - panel_inner * 2 - profile_buttons_width);
    MoveWindow(prompt_profile_combo_, profile_x, profile_controls_y,
               combo_width, 180, TRUE);
    profile_x += combo_width + control_gap;
    MoveWindow(prompt_profile_new_button_, profile_x, profile_controls_y,
               small_button_width, short_height, TRUE);
    profile_x += small_button_width + control_gap;
    MoveWindow(prompt_profile_rename_button_, profile_x, profile_controls_y,
               small_button_width, short_height, TRUE);
    y += profile_panel_height + section_gap;

    int transform_panel_height = panel_header + panel_inner + flex_height + panel_inner;
    prompt_transform_panel_rc_ = {padding, y, padding + content_width,
                                  y + transform_panel_height};
    MoveWindow(prompt_transform_label_, 0, 0, 0, 0, TRUE);
    MoveWindow(prompt_transform_edit_, padding + panel_inner, y + panel_header,
               content_width - panel_inner * 2, flex_height, TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HistoryWindow::save_prompt_edits() {
    if (!config_) return;

    std::string pre = wide_to_utf8(get_edit_text(prompt_pre_edit_));
    std::string transform = wide_to_utf8(get_edit_text(prompt_transform_edit_));
    std::string profile_name = current_profile_name_from_ui();
    if (profile_name.empty()) {
        SetWindowTextW(prompt_status_, L"Type a profile name before saving.");
        return;
    }

    auto& cfg = config_->config();
    cfg.llm_pre_prompt = pre;
    auto profile = std::find_if(cfg.llm_profiles.begin(), cfg.llm_profiles.end(),
                                [&](const ProfileDefinition& item) {
                                    return item.name == profile_name;
                                });
    if (profile == cfg.llm_profiles.end()) {
        cfg.llm_profiles.push_back({profile_name, transform});
    } else {
        profile->instructions = transform;
    }
    active_prompt_profile_ = profile_name;

    bool ok = config_->save();
    if (prompt_status_) {
        if (!ok) {
            SetWindowTextW(prompt_status_,
                           L"Save failed - check write permissions.");
            return;
        }
        SetWindowTextW(prompt_status_,
                       ok ? L"Roles saved to config.json."
                          : L"Save failed — check write permissions.");
    }
    populate_profile_combo(profile_name);
    if (prompt_status_) {
        SetWindowTextW(prompt_status_,
                       L"Roles and profile saved to config.json.");
    }
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

        case WM_ERASEBKGND: {
            if (self) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                RECT client = {};
                GetClientRect(hwnd, &client);
                FillRect(hdc, &client, self->background_brush_);
            }
            return 1;
        }

        case WM_PAINT:
            if (self) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                self->paint_background(hwnd, hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            break;

        case WM_CTLCOLORSTATIC:
            if (self) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                HWND child = reinterpret_cast<HWND>(lParam);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, child == self->prompt_status_ ? COLOR_MUTED : COLOR_LABEL);
                return reinterpret_cast<LRESULT>(self->background_brush_);
            }
            break;

        case WM_CTLCOLORBTN:
            if (self) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetBkColor(hdc, COLOR_BUTTON);
                SetTextColor(hdc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(self->button_brush_);
            }
            break;

        case WM_CTLCOLOREDIT:
            if (self) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetBkColor(hdc, COLOR_CONTROL);
                SetTextColor(hdc, COLOR_TEXT);
                return reinterpret_cast<LRESULT>(self->control_brush_);
            }
            break;

        case WM_CTLCOLORLISTBOX:
            if (self) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetBkColor(hdc, COLOR_CONTROL);
                SetTextColor(hdc, COLOR_TEXT);
                return reinterpret_cast<LRESULT>(self->control_brush_);
            }
            break;

        case WM_MEASUREITEM:
            if (self && wParam == IDC_PROFILE_COMBO) {
                MEASUREITEMSTRUCT* mis =
                    reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
                mis->itemHeight = 26;
                return TRUE;
            }
            break;

        case WM_DRAWITEM:
            if (self) {
                const auto* item =
                    reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_COMBOBOX &&
                    item->CtlID == IDC_PROFILE_COMBO) {
                    self->draw_combo_item(item);
                    return TRUE;
                }
                if (wParam == IDC_CLEAR_BUTTON) {
                    self->draw_steel_button(item);
                    return TRUE;
                }
                if (item->CtlID == IDC_PROMPT_SAVE ||
                    item->CtlID == IDC_PROMPT_BACK ||
                    item->CtlID == IDC_PROFILE_NEW ||
                    item->CtlID == IDC_PROFILE_RENAME) {
                    self->draw_steel_button(item);
                    return TRUE;
                }
            }
            break;

        case WM_COMMAND:
            if (self && HIWORD(wParam) == EN_VSCROLL) {
                HWND edit = reinterpret_cast<HWND>(lParam);
                const bool known_edit = edit == self->recorded_edit_ ||
                    edit == self->transcribed_edit_ ||
                    edit == self->ai_language_edit_;
                if (known_edit && !self->action_position_update_posted_) {
                    self->action_position_update_posted_ = true;
                    if (!PostMessageW(hwnd, WM_APP + 1,
                                      reinterpret_cast<WPARAM>(edit), 0)) {
                        self->action_position_update_posted_ = false;
                    }
                }
                return 0;
            }
            if (self && LOWORD(wParam) == IDC_CLEAR_BUTTON &&
                HIWORD(wParam) == BN_CLICKED) {
                int answer = MessageBoxW(hwnd,
                    L"Clear all history entries? This cannot be undone.",
                    L"VoiceToText", MB_YESNO | MB_ICONWARNING);
                if (answer == IDYES && self->history_) {
                    self->history_->clear();
                    self->refresh();
                }
                return 0;
            }
            if (self && LOWORD(wParam) == IDC_PROMPT_BACK &&
                HIWORD(wParam) == BN_CLICKED) {
                self->show_history_view();
                return 0;
            }
            if (self && LOWORD(wParam) == IDC_PROMPT_SAVE &&
                HIWORD(wParam) == BN_CLICKED) {
                self->save_prompt_edits();
                return 0;
            }
            if (self && LOWORD(wParam) == IDC_PROFILE_NEW &&
                HIWORD(wParam) == BN_CLICKED) {
                self->create_new_profile();
                return 0;
            }
            if (self && LOWORD(wParam) == IDC_PROFILE_RENAME &&
                HIWORD(wParam) == BN_CLICKED) {
                self->rename_current_profile();
                return 0;
            }
            if (self && LOWORD(wParam) == IDC_PROFILE_COMBO &&
                HIWORD(wParam) == CBN_SELCHANGE) {
                int index = static_cast<int>(
                    SendMessageW(self->prompt_profile_combo_, CB_GETCURSEL, 0, 0));
                if (index >= 0) {
                    wchar_t buffer[256] = {};
                    SendMessageW(self->prompt_profile_combo_, CB_GETLBTEXT,
                                 static_cast<WPARAM>(index),
                                 reinterpret_cast<LPARAM>(buffer));
                    self->select_profile_for_editing(wide_to_utf8(buffer));
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            return 0;

        case WM_APP + 1:
            if (self) {
                self->action_position_update_posted_ = false;
                HWND edit = reinterpret_cast<HWND>(wParam);
                self->update_entry_action_positions(
                    edit == self->recorded_edit_ ||
                            edit == self->transcribed_edit_ ||
                            edit == self->ai_language_edit_
                        ? edit
                        : nullptr);
                if (edit) self->redraw_action_gutter(edit, true);
            }
            return 0;

        case WM_APP + 2:
            if (self) {
                self->scroll_history_to_top();
                self->update_entry_action_positions();
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK HistoryWindow::EditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<HistoryWindow*>(GetPropW(hwnd, EDIT_OWNER_PROP));
    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, EDIT_OWNER_PROP);
    }

    if (self) {
        switch (msg) {
            case WM_ERASEBKGND: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                if (hdc) {
                    RECT client = {};
                    GetClientRect(hwnd, &client);
                    FillRect(hdc, &client, self->control_brush_);
                }
                return 1;
            }
            case WM_MOUSEMOVE:
                self->handle_edit_mouse_move(hwnd, lParam);
                break;
            case WM_MOUSELEAVE: {
                if (self->tracked_mouse_edit_ == hwnd) {
                    self->tracked_mouse_edit_ = nullptr;
                }
                POINT cursor = {};
                bool moved_to_gutter = false;
                if (GetCursorPos(&cursor)) {
                    HWND under = WindowFromPoint(cursor);
                    moved_to_gutter = under == self->action_gutter_for_edit(hwnd);
                }
                if (moved_to_gutter) break;
                self->set_entry_action_hover(nullptr, -1,
                                             EntryAction::none);
                self->set_highlighted_entry(-1);
                break;
            }
            case WM_LBUTTONDOWN: {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                if (self->is_over_profile(hwnd,
                                          x, y)) {
                    SetFocus(hwnd);
                    return 0;
                }
                break;
            }
            case WM_LBUTTONUP: {
                if (self->handle_edit_left_click(hwnd, lParam)) {
                    return 0;
                }
                break;
            }
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {
                    POINT pt;
                    if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                        if (self->is_over_profile(hwnd, pt.x, pt.y)) {
                            SetCursor(LoadCursor(nullptr, IDC_HAND));
                            return TRUE;
                        }
                    }
                }
                break;
        }
    }

    WNDPROC original = self ? self->original_edit_proc_ : nullptr;
    if (!original) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    const bool scroll_related_key =
        msg == WM_KEYDOWN &&
        (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_PRIOR ||
         wParam == VK_NEXT || wParam == VK_HOME || wParam == VK_END);
    const bool scroll_message =
        msg == WM_MOUSEWHEEL || msg == WM_VSCROLL ||
        msg == WM_MOUSEHWHEEL || msg == WM_HSCROLL ||
        scroll_related_key;
    const bool programmatic_scroll =
        msg == EM_LINESCROLL || msg == EM_SCROLL ||
        msg == EM_SETSCROLLPOS || msg == EM_SCROLLCARET;
    const bool selection_auto_scroll = GetCapture() == hwnd &&
        ((msg == WM_MOUSEMOVE && (wParam & MK_LBUTTON) != 0) ||
         msg == WM_TIMER);
    const bool owns_scroll = self &&
                             (scroll_message || programmatic_scroll ||
                              selection_auto_scroll) &&
                             !self->handling_history_scroll_;
    POINT scroll_before = {};

    if (owns_scroll) {
        SendMessageW(hwnd, EM_GETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&scroll_before));
        self->handling_history_scroll_ = true;
        if (scroll_message) {
            self->set_entry_action_hover(nullptr, -1, EntryAction::none);
        }
    }

    LRESULT result = CallWindowProcW(original, hwnd, msg, wParam, lParam);

    if (owns_scroll) {
        POINT scroll_after = {};
        SendMessageW(hwnd, EM_GETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&scroll_after));
        const bool scroll_changed =
            scroll_before.x != scroll_after.x ||
            scroll_before.y != scroll_after.y;
        // Update immediately for the common path and once more after the
        // native RichEdit has finished processing any deferred thumb/wheel
        // scroll.  The posted pass is coalesced so a fast wheel does not
        // queue one full layout per message.
        if (scroll_changed || scroll_message || programmatic_scroll) {
            self->update_entry_action_positions(hwnd);
        }
        self->handling_history_scroll_ = false;
        if (scroll_message || programmatic_scroll || scroll_changed) {
            self->redraw_action_gutter(hwnd, true);
            if (!self->action_position_update_posted_ && self->hwnd_) {
                self->action_position_update_posted_ = true;
                if (!PostMessageW(self->hwnd_, WM_APP + 1,
                                  reinterpret_cast<WPARAM>(hwnd), 0)) {
                    self->action_position_update_posted_ = false;
                }
            }
        }
    }
    return result;
}

LRESULT CALLBACK HistoryWindow::ActionGutterWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HistoryWindow* self = reinterpret_cast<HistoryWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HistoryWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    HWND edit = self->edit_for_action_gutter(hwnd);

    switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (hdc) {
                RECT client = {};
                GetClientRect(hwnd, &client);
                FillRect(hdc, &client, self->control_brush_);
            }
            return 1;
        }

        case WM_PAINT:
            self->paint_action_gutter(hwnd);
            return 0;

        case WM_PRINTCLIENT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (hdc) self->draw_action_gutter(hwnd, hdc);
            return 0;
        }

        case WM_MOUSEMOVE:
            self->handle_action_gutter_mouse_move(
                hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSELEAVE: {
            if (self->tracked_action_gutter_ == hwnd) {
                self->tracked_action_gutter_ = nullptr;
            }
            POINT cursor = {};
            if (edit && GetCursorPos(&cursor) &&
                WindowFromPoint(cursor) == edit) {
                break;
            }
            self->set_entry_action_hover(nullptr, -1, EntryAction::none);
            self->set_highlighted_entry(-1);
            return 0;
        }

        case WM_LBUTTONDOWN:
            if (edit && self->handle_entry_action_left_down(
                            edit, GET_X_LPARAM(lParam),
                            GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (edit) SetFocus(edit);
            return 0;

        case WM_LBUTTONUP:
            if (edit) {
                self->handle_entry_action_left_up(
                    edit, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (edit && self->pressed_action_edit_ == edit) {
                self->clear_pressed_entry_action();
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && edit) {
                POINT cursor = {};
                if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)) {
                    int entry_index = -1;
                    EntryAction action = EntryAction::none;
                    if (self->entry_action_at(edit, cursor.x, cursor.y,
                                              entry_index, action)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
            }
            break;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return edit ? SendMessageW(edit, msg, wParam, lParam) : 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vtt
