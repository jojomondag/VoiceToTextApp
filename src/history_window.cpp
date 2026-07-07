#include "history_window.h"

#include "app_icon.h"
#include "config_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dwmapi.h>
#include <richedit.h>
#include <string>
#include <windowsx.h>

namespace vtt {
namespace {

static constexpr int IDC_CLEAR_BUTTON = 1001;
static constexpr int IDC_PROMPT_SAVE  = 1010;
static constexpr int IDC_PROFILE_COMBO = 1011;
static constexpr int IDC_PROFILE_NEW = 1012;
static constexpr int IDC_PROFILE_RENAME = 1013;
static constexpr int IDC_PROMPT_BACK = 1014;
static constexpr int CLEAR_BUTTON_WIDTH = 90;
static constexpr int CLEAR_BUTTON_GAP = 8;
static constexpr int COPY_BUTTON_WIDTH = 34;
static constexpr int COPY_BUTTON_HEIGHT = 20;
static constexpr int PROFILE_BUTTON_WIDTH = 24;
static constexpr int PROFILE_BUTTON_HEIGHT = 20;
static constexpr int PROFILE_BUTTON_GAP = 6;
static constexpr int SECTION_TITLE_INSET = 18;
static constexpr int PANEL_INNER = 12;
static constexpr int HISTORY_ICON_TEXT_GAP = 8;
static constexpr int HISTORY_TEXT_LEFT_MARGIN = 54;
static constexpr int HISTORY_TEXT_RIGHT_MARGIN = 8;
static constexpr int PROMPT_TEXT_RIGHT_MARGIN = 12;
static constexpr int PANEL_RADIUS = 14;

static constexpr const wchar_t* HISTORY_CLASS_NAME = L"VTTHistoryWindow";
static constexpr const wchar_t* EDIT_OWNER_PROP = L"VTTHistoryWindowOwner";
static constexpr const wchar_t* COPY_BUTTON_OWNER_PROP = L"VTTCopyButtonOwner";
static constexpr const wchar_t* ENTRY_BEGIN = L"[[VTT_ENTRY_BEGIN:";
static constexpr const wchar_t* ENTRY_END = L"[[VTT_ENTRY_END:";
static constexpr COLORREF COLOR_BG = RGB(17, 23, 31);
static constexpr COLORREF COLOR_BG_TOP = RGB(27, 36, 48);
static constexpr COLORREF COLOR_PANEL = RGB(31, 40, 52);
static constexpr COLORREF COLOR_PANEL_EDGE = RGB(72, 88, 105);
static constexpr COLORREF COLOR_CONTROL = RGB(22, 29, 39);
static constexpr COLORREF COLOR_BUTTON = RGB(39, 101, 150);
static constexpr COLORREF COLOR_LABEL = RGB(232, 238, 244);
static constexpr COLORREF COLOR_MUTED = RGB(150, 164, 178);
static constexpr COLORREF COLOR_DATE = RGB(110, 170, 235);
static constexpr COLORREF COLOR_PROFILE = RGB(88, 210, 170);
static constexpr COLORREF COLOR_TOKENS = RGB(230, 166, 80);
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
    destroy_copy_buttons();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
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
    refresh();
    SetWindowRedraw(hwnd_, FALSE);
    layout();
    update_copy_button_positions();
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    } else {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    }
    layout();
    update_copy_button_positions();
    SetWindowRedraw(hwnd_, TRUE);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    scroll_history_to_top();
    update_copy_button_positions();
    PostMessageW(hwnd_, WM_APP + 2, 0, 0);
}

void HistoryWindow::refresh() {
    if (!history_ || !recorded_edit_ || !transcribed_edit_ ||
        !ai_language_edit_) return;

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    if (hwnd_) {
        SetWindowRedraw(hwnd_, FALSE);
    }
    for (HWND edit : edits) {
        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    }

    std::wstring recorded = utf8_to_wide(history_->format_recorded());
    std::wstring transcribed = utf8_to_wide(history_->format_transcribed());
    std::wstring ai_language = utf8_to_wide(history_->format_ai_language());

    recorded = strip_delimiters_and_capture_ranges(recorded,
                                                   panel_ranges_.recorded);
    transcribed = strip_delimiters_and_capture_ranges(transcribed,
                                                      panel_ranges_.transcribed);
    ai_language = strip_delimiters_and_capture_ranges(ai_language,
                                                      panel_ranges_.ai_language);

    SetWindowTextW(recorded_edit_, recorded.c_str());
    SetWindowTextW(transcribed_edit_, transcribed.c_str());
    SetWindowTextW(ai_language_edit_, ai_language.c_str());
    apply_entry_paragraph_spacing();
    highlighted_entry_ = -1;
    set_profile_link_hover(nullptr, -1, -1);
    rebuild_copy_buttons();
    apply_header_colors(recorded_edit_);
    apply_header_colors(transcribed_edit_);
    apply_header_colors(ai_language_edit_);
    apply_entry_cards();
    scroll_history_to_top();
    update_copy_button_positions();

    for (HWND edit : edits) {
        SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(edit, nullptr, TRUE);
    }
    if (hwnd_) {
        SetWindowRedraw(hwnd_, TRUE);
        RedrawWindow(hwnd_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
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
    int label_height = 30;
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

    int button_width = (std::max)(CLEAR_BUTTON_WIDTH, 92);
    int button_x = x3 + panel_width - edit_margin - button_width;
    int button_y = panel_top + 16;

    MoveWindow(recorded_label_, 0, 0, 0, 0, TRUE);
    MoveWindow(transcribed_label_, 0, 0, 0, 0, TRUE);
    MoveWindow(ai_language_label_, 0, 0, 0, 0, TRUE);
    MoveWindow(clear_button_, button_x, button_y, button_width, 32, TRUE);
    MoveWindow(recorded_edit_, x1 + edit_margin, panel_top + edit_top_offset,
               panel_width - edit_margin * 2, edit_height, TRUE);
    MoveWindow(transcribed_edit_, x2 + edit_margin, panel_top + edit_top_offset,
               panel_width - edit_margin * 2, edit_height, TRUE);
    MoveWindow(ai_language_edit_, x3 + edit_margin, panel_top + edit_top_offset,
               panel_width - edit_margin * 2, edit_height, TRUE);

    update_copy_button_positions();
    InvalidateRect(hwnd_, nullptr, TRUE);
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
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;
    bool hot = item->hwndItem == hover_button_;
    bool highlighted_copy = false;
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.copy_button == item->hwndItem) {
            highlighted_copy = entry_view.entry_index == highlighted_entry_;
            break;
        }
    }

    COLORREF fill = pressed
        ? RGB(29, 78, 118)
        : (hot ? RGB(27, 78, 118)
               : (highlighted_copy ? COLOR_ENTRY_HIGHLIGHT : COLOR_BUTTON));
    COLORREF border = highlighted_copy
        ? RGB(115, 170, 215)
        : (focused ? RGB(120, 185, 235) : RGB(68, 132, 178));

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, RGB(255, 255, 255));
    if (pressed) {
        OffsetRect(&rc, 0, 1);
    }

    wchar_t text[128] = {};
    GetWindowTextW(item->hwndItem, text, 128);
    if (text[0] == L'\0') {
        HPEN icon_pen = CreatePen(PS_SOLID, 1, RGB(245, 250, 255));
        HGDIOBJ old_icon_pen = SelectObject(item->hDC, icon_pen);
        HGDIOBJ old_icon_brush =
            SelectObject(item->hDC, GetStockObject(NULL_BRUSH));

        int center_x = (rc.left + rc.right) / 2;
        int center_y = (rc.top + rc.bottom) / 2;
        RECT back = {center_x - 6, center_y - 6,
                     center_x + 3, center_y + 3};
        RECT front = {center_x - 3, center_y - 3,
                      center_x + 6, center_y + 6};
        Rectangle(item->hDC, back.left, back.top, back.right, back.bottom);
        Rectangle(item->hDC, front.left, front.top, front.right, front.bottom);

        SelectObject(item->hDC, old_icon_brush);
        SelectObject(item->hDC, old_icon_pen);
        DeleteObject(icon_pen);
        return;
    }

    HGDIOBJ old_font = SelectObject(item->hDC, button_font_);
    DrawTextW(item->hDC, text, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item->hDC, old_font);
}

void HistoryWindow::draw_profile_button(const DRAWITEMSTRUCT* item) {
    if (!item) return;

    RECT rc = item->rcItem;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;
    bool hot = item->hwndItem == hover_button_;
    bool highlighted_profile = false;
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.profile_button == item->hwndItem) {
            highlighted_profile = entry_view.entry_index == highlighted_entry_;
            break;
        }
    }

    COLORREF fill = pressed
        ? RGB(39, 86, 74)
        : (hot ? RGB(24, 58, 50)
               : (highlighted_profile ? RGB(56, 110, 96) : RGB(35, 72, 64)));
    COLORREF border = highlighted_profile
        ? RGB(145, 240, 210)
        : (focused ? RGB(120, 224, 190) : RGB(88, 170, 145));

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN border_pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, border_pen);
    RoundRect(item->hDC, rc.left, rc.top, rc.right, rc.bottom, 9, 9);
    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(border_pen);
    DeleteObject(brush);

    if (pressed) OffsetRect(&rc, 0, 1);

    HPEN icon_pen = CreatePen(PS_SOLID, 1, RGB(245, 255, 252));
    HBRUSH icon_brush = CreateSolidBrush(RGB(235, 255, 248));
    HGDIOBJ old_icon_pen = SelectObject(item->hDC, icon_pen);
    HGDIOBJ old_icon_brush = SelectObject(item->hDC, icon_brush);

    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    Ellipse(item->hDC, cx - 3, cy - 7, cx + 4, cy);
    RoundRect(item->hDC, cx - 7, cy + 2, cx + 8, cy + 8, 6, 6);

    SelectObject(item->hDC, old_icon_brush);
    SelectObject(item->hDC, old_icon_pen);
    DeleteObject(icon_brush);
    DeleteObject(icon_pen);
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
        SendMessageW(edit, EM_SETSEL, 0, -1);
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_SPACEAFTER | PFM_SPACEBEFORE;
        pf.dySpaceAfter = 60;
        pf.dySpaceBefore = 20;
        SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
        SendMessageW(edit, EM_SETSEL, -1, -1);
    }
}

void HistoryWindow::apply_entry_cards() {
    HWND rich_edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : rich_edits) {
        if (!edit) continue;

        auto ranges = entry_ranges(edit);
        for (const auto& range : ranges) {
            apply_entry_background(edit, range.first, range.second,
                                   COLOR_ENTRY_CARD);
        }
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

void HistoryWindow::subclass_copy_button(HWND button) {
    if (!button) return;

    SetPropW(button, COPY_BUTTON_OWNER_PROP, this);
    WNDPROC previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(button, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(CopyButtonWndProc)));
    if (!original_copy_button_proc_) {
        original_copy_button_proc_ = previous;
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
    int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};

    std::wstring text(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::vector<std::pair<int, int>> HistoryWindow::entry_display_ranges(HWND edit) const {
    if (edit == recorded_edit_) return panel_ranges_.recorded;
    if (edit == transcribed_edit_) return panel_ranges_.transcribed;
    if (edit == ai_language_edit_) return panel_ranges_.ai_language;

    std::wstring text = get_edit_text(edit);
    std::vector<int> starts;

    for (size_t pos = 0; pos < text.size(); ++pos) {
        bool at_line_start = pos == 0 || text[pos - 1] == L'\r' ||
                             text[pos - 1] == L'\n';
        if (!at_line_start || text[pos] != L'[') continue;

        size_t line_end = text.find_first_of(L"\r\n", pos);
        if (line_end == std::wstring::npos) line_end = text.size();

        std::wstring line = text.substr(pos, line_end - pos);
        if (line.find(L"]") != std::wstring::npos &&
            line.find(L"Profile:") != std::wstring::npos &&
            line.find(L"Tokens:") != std::wstring::npos) {
            starts.push_back(static_cast<int>(pos));
        }
    }

    std::vector<std::pair<int, int>> ranges;
    for (size_t i = 0; i < starts.size(); ++i) {
        int start = starts[i];
        int end = (i + 1 < starts.size())
            ? starts[i + 1]
            : static_cast<int>(text.size());
        ranges.emplace_back(start, end);
    }

    return ranges;
}

std::vector<std::pair<int, int>> HistoryWindow::entry_ranges(HWND edit) const {
    std::wstring text = get_edit_text(edit);
    auto display_ranges = entry_display_ranges(edit);
    std::vector<std::pair<int, int>> ranges;
    ranges.reserve(display_ranges.size());

    for (const auto& range : display_ranges) {
        ranges.emplace_back(
            rich_edit_cp_from_text_offset(text, static_cast<size_t>(range.first)),
            rich_edit_cp_from_text_offset(text, static_cast<size_t>(range.second)));
    }

    return ranges;
}

int HistoryWindow::entry_index_from_char(HWND edit, int char_index) const {
    auto ranges = entry_ranges(edit);
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (char_index >= ranges[i].first && char_index < ranges[i].second) {
            return static_cast<int>(i);
        }
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

int HistoryWindow::entry_index_from_line(HWND edit, int line_index) const {
    if (!edit || line_index < 0) return -1;

    int line_count = static_cast<int>(SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
    if (line_index >= line_count) return -1;

    int line_start = static_cast<int>(SendMessageW(edit, EM_LINEINDEX,
                                                   line_index, 0));
    if (line_start < 0) return -1;

    int length = GetWindowTextLengthW(edit);
    int next_line_start = (line_index + 1 < line_count)
        ? static_cast<int>(SendMessageW(edit, EM_LINEINDEX, line_index + 1, 0))
        : length;
    if (next_line_start < line_start) next_line_start = length;

    std::wstring text = get_edit_text(edit);
    int line_end = next_line_start;
    if (line_end > static_cast<int>(text.size())) {
        line_end = static_cast<int>(text.size());
    }
    while (line_end > line_start &&
           (text[static_cast<size_t>(line_end - 1)] == L'\r' ||
            text[static_cast<size_t>(line_end - 1)] == L'\n' ||
            text[static_cast<size_t>(line_end - 1)] == L' ' ||
            text[static_cast<size_t>(line_end - 1)] == L'\t')) {
        --line_end;
    }
    if (line_end <= line_start) return -1;

    return entry_index_from_char(edit, line_start);
}

void HistoryWindow::handle_edit_mouse_move(HWND edit, LPARAM lParam) {
    if (updating_highlight_) return;

    int length = GetWindowTextLengthW(edit);
    if (length <= 0) {
        clear_highlight();
        return;
    }

    RECT rc;
    GetClientRect(edit, &rc);
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    if (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) {
        set_profile_link_hover(nullptr, -1, -1);
        clear_highlight();
        return;
    }

    int char_index = char_index_at(edit, x, y);
    update_profile_link_hover(edit, char_index);
    int entry_index = entry_index_from_char(edit, char_index);
    if (entry_index < 0) {
        clear_highlight();
        return;
    }
    if (entry_index == highlighted_entry_) return;

    highlight_entry(entry_index, edit);
}

void HistoryWindow::handle_copy_button_mouse_move(HWND button) {
    if (updating_highlight_) return;

    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.copy_button != button &&
            entry_view.profile_button != button) {
            continue;
        }
        if (entry_view.entry_index == highlighted_entry_) return;
        highlight_entry(entry_view.entry_index, entry_view.edit);
        return;
    }
}

void HistoryWindow::highlight_entry(int entry_index, HWND source_edit) {
    if (updating_highlight_) return;
    updating_highlight_ = true;

    int previous_entry = highlighted_entry_;
    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);

        int previous_start = 0;
        int previous_end = 0;
        if (entry_cp_range(edit, previous_entry, previous_start, previous_end)) {
            clear_entry_background(edit, previous_start, previous_end);
        }

        int start = 0;
        int end = 0;
        if (entry_cp_range(edit, entry_index, start, end)) {
            apply_entry_background(edit, start, end, COLOR_ENTRY_HIGHLIGHT);
            if (edit != source_edit) {
                center_entry(edit, start);
            }
        }

        SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(edit, nullptr, FALSE);
    }

    highlighted_entry_ = entry_index;
    update_copy_button_positions();
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.copy_button) {
            InvalidateRect(entry_view.copy_button, nullptr, TRUE);
        }
        if (entry_view.profile_button) {
            InvalidateRect(entry_view.profile_button, nullptr, TRUE);
        }
    }
    updating_highlight_ = false;
}

void HistoryWindow::clear_highlight() {
    if (highlighted_entry_ < 0 || updating_highlight_) return;
    updating_highlight_ = true;

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        int start = 0;
        int end = 0;
        if (!entry_cp_range(edit, highlighted_entry_, start, end)) continue;

        SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
        clear_entry_background(edit, start, end);
        SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(edit, nullptr, FALSE);
    }

    highlighted_entry_ = -1;
    set_profile_link_hover(nullptr, -1, -1);
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.copy_button) {
            InvalidateRect(entry_view.copy_button, nullptr, TRUE);
        }
        if (entry_view.profile_button) {
            InvalidateRect(entry_view.profile_button, nullptr, TRUE);
        }
    }
    updating_highlight_ = false;
}

void HistoryWindow::destroy_copy_buttons() {
    for (auto& entry_view : history_entry_views_) {
        if (entry_view.copy_button) DestroyWindow(entry_view.copy_button);
        if (entry_view.profile_button) DestroyWindow(entry_view.profile_button);
    }
    hover_button_ = nullptr;
    history_entry_views_.clear();
}

void HistoryWindow::rebuild_copy_buttons() {
    destroy_copy_buttons();

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        if (!edit) continue;
        auto ranges = entry_display_ranges(edit);
        for (int i = 0; i < static_cast<int>(ranges.size()); ++i) {
            HWND btn = CreateWindowExW(
                0, L"BUTTON", L"",
                WS_CHILD | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_OWNERDRAW,
                0, 0, ENTRY_COPY_WIDTH, ENTRY_COPY_HEIGHT,
                edit, nullptr, hInstance_, nullptr);
            if (!btn) continue;
            SendMessageW(btn, WM_SETFONT,
                         reinterpret_cast<WPARAM>(button_font_), TRUE);
            subclass_copy_button(btn);
            HWND profile_btn = CreateWindowExW(
                0, L"BUTTON", L"",
                WS_CHILD | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_OWNERDRAW,
                0, 0, PROFILE_BUTTON_WIDTH, PROFILE_BUTTON_HEIGHT,
                edit, nullptr, hInstance_, nullptr);
            if (profile_btn) {
                SendMessageW(profile_btn, WM_SETFONT,
                             reinterpret_cast<WPARAM>(button_font_), TRUE);
                subclass_copy_button(profile_btn);
            }
            history_entry_views_.push_back({btn, profile_btn, edit, i});
        }
    }
    update_copy_button_positions();
}

void HistoryWindow::scroll_history_to_top() {
    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    POINT top = {0, 0};

    for (HWND edit : edits) {
        if (!edit) continue;
        SendMessageW(edit, EM_SETSEL, 0, 0);
        SendMessageW(edit, WM_VSCROLL, SB_TOP, 0);
        SendMessageW(edit, EM_SCROLLCARET, 0, 0);
        SendMessageW(edit, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&top));
        SendMessageW(edit, WM_VSCROLL, SB_TOP, 0);
    }
}

void HistoryWindow::update_copy_button_positions() {
    for (auto& entry_view : history_entry_views_) {
        if (!entry_view.copy_button || !entry_view.edit) continue;

        auto hide_entry_buttons = [&]() {
            if (hover_button_ == entry_view.copy_button ||
                hover_button_ == entry_view.profile_button) {
                hover_button_ = nullptr;
            }
            ShowWindow(entry_view.copy_button, SW_HIDE);
            if (entry_view.profile_button) {
                ShowWindow(entry_view.profile_button, SW_HIDE);
            }
        };

        int start = 0, end = 0;
        if (!entry_cp_range(entry_view.edit, entry_view.entry_index, start, end)) {
            hide_entry_buttons();
            continue;
        }

        RECT edit_rc;
        GetClientRect(entry_view.edit, &edit_rc);

        int edit_height = edit_rc.bottom - edit_rc.top;
        if (edit_height <= ENTRY_COPY_HEIGHT + 8) {
            hide_entry_buttons();
            continue;
        }

        HDC hdc = GetDC(entry_view.edit);
        HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(entry_view.edit, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
        TEXTMETRICW tm = {};
        GetTextMetricsW(hdc, &tm);
        if (old_font) SelectObject(hdc, old_font);
        ReleaseDC(entry_view.edit, hdc);

        int line_height = tm.tmHeight + tm.tmExternalLeading;
        if (line_height <= 0) line_height = 16;

        POINTL start_pos = {};
        SendMessageW(entry_view.edit, EM_POSFROMCHAR,
                     reinterpret_cast<WPARAM>(&start_pos), start);
        if (start_pos.y <= -32000) {
            hide_entry_buttons();
            continue;
        }

        int end_char = (std::max)(start, end - 1);
        POINTL end_pos = {};
        SendMessageW(entry_view.edit, EM_POSFROMCHAR,
                     reinterpret_cast<WPARAM>(&end_pos), end_char);
        if (end_pos.y <= -32000) {
            hide_entry_buttons();
            continue;
        }

        int visible_top = 2;
        int visible_bottom = edit_rc.bottom - 2;
        int entry_top = static_cast<int>(start_pos.y);
        int entry_bottom = static_cast<int>(end_pos.y) + line_height;
        if (entry_view.edit == ai_language_edit_) {
            entry_bottom = (std::max)(entry_bottom,
                                      entry_top + ENTRY_COPY_HEIGHT + 4);
        }
        int y = entry_top + (entry_bottom - entry_top - ENTRY_COPY_HEIGHT) / 2;
        if (y + ENTRY_COPY_HEIGHT < visible_top || y > visible_bottom) {
            hide_entry_buttons();
            continue;
        }

        int x = (std::max)(4, (static_cast<int>(start_pos.x) -
                               ENTRY_COPY_WIDTH) / 2);

        MoveWindow(entry_view.copy_button, x, y,
                   ENTRY_COPY_WIDTH, ENTRY_COPY_HEIGHT, TRUE);
        SetWindowPos(entry_view.copy_button, HWND_TOP, x, y,
                     ENTRY_COPY_WIDTH, ENTRY_COPY_HEIGHT,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);

        if (!entry_view.profile_button) continue;

        const ProfileLink* profile_link = nullptr;
        const auto& links = links_for(entry_view.edit);
        for (const auto& link : links) {
            if (link.start >= start && link.end <= end) {
                profile_link = &link;
                break;
            }
        }
        if (!profile_link) {
            ShowWindow(entry_view.profile_button, SW_HIDE);
            continue;
        }

        auto display_ranges = entry_display_ranges(entry_view.edit);
        if (entry_view.entry_index >= static_cast<int>(display_ranges.size())) {
            ShowWindow(entry_view.profile_button, SW_HIDE);
            continue;
        }

        std::wstring text = get_edit_text(entry_view.edit);
        const auto& display_range =
            display_ranges[static_cast<size_t>(entry_view.entry_index)];
        size_t profile_label = text.find(
            L"Profile:", static_cast<size_t>(display_range.first));
        if (profile_label == std::wstring::npos ||
            profile_label >= static_cast<size_t>(display_range.second)) {
            ShowWindow(entry_view.profile_button, SW_HIDE);
            continue;
        }

        int profile_label_cp = rich_edit_cp_from_text_offset(text, profile_label);
        POINTL profile_pos = {};
        SendMessageW(entry_view.edit, EM_POSFROMCHAR,
                     reinterpret_cast<WPARAM>(&profile_pos),
                     profile_label_cp);
        if (profile_pos.y <= -32000) {
            ShowWindow(entry_view.profile_button, SW_HIDE);
            continue;
        }

        int profile_x = (std::max)(4, static_cast<int>(profile_pos.x) -
                                      PROFILE_BUTTON_GAP -
                                      PROFILE_BUTTON_WIDTH);
        int profile_y = static_cast<int>(profile_pos.y) +
                        (line_height - PROFILE_BUTTON_HEIGHT) / 2;
        if (profile_y < visible_top) {
            profile_y = visible_top;
        }
        if (profile_y + PROFILE_BUTTON_HEIGHT > visible_bottom ||
            profile_x + PROFILE_BUTTON_WIDTH > edit_rc.right - 4) {
            ShowWindow(entry_view.profile_button, SW_HIDE);
            continue;
        }

        MoveWindow(entry_view.profile_button, profile_x, profile_y,
                   PROFILE_BUTTON_WIDTH, PROFILE_BUTTON_HEIGHT, TRUE);
        SetWindowPos(entry_view.profile_button, HWND_TOP, profile_x, profile_y,
                     PROFILE_BUTTON_WIDTH, PROFILE_BUTTON_HEIGHT,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
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

    auto ranges = entry_display_ranges(edit);
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

void HistoryWindow::apply_entry_background(HWND edit, int start, int end,
                                           COLORREF color) {
    if (!edit || start >= end) return;

    CHARRANGE saved;
    POINT scroll_pos = {};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));

    set_char_range(edit, start, end);
    CHARFORMAT2W format = {};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_BACKCOLOR;
    format.crBackColor = color;
    format.dwEffects = 0;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));

    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    SendMessageW(edit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));
}

void HistoryWindow::clear_entry_background(HWND edit, int start, int end) {
    if (!edit || start >= end) return;

    CHARRANGE saved;
    POINT scroll_pos = {};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));

    set_char_range(edit, start, end);
    CHARFORMAT2W format = {};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_BACKCOLOR;
    format.crBackColor = COLOR_ENTRY_CARD;
    format.dwEffects = 0;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));

    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    SendMessageW(edit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));
}

bool HistoryWindow::entry_cp_range(HWND edit, int entry_index,
                                   int& start, int& end) const {
    if (!edit || entry_index < 0) return false;

    auto ranges = entry_display_ranges(edit);
    if (entry_index >= static_cast<int>(ranges.size())) return false;

    std::wstring text = get_edit_text(edit);
    int display_start = ranges[entry_index].first;
    int display_end = ranges[entry_index].second;
    while (display_end > display_start &&
           display_end <= static_cast<int>(text.size()) &&
           (text[static_cast<size_t>(display_end - 1)] == L'\r' ||
            text[static_cast<size_t>(display_end - 1)] == L'\n' ||
            text[static_cast<size_t>(display_end - 1)] == L' ' ||
            text[static_cast<size_t>(display_end - 1)] == L'\t')) {
        --display_end;
    }

    start = rich_edit_cp_from_text_offset(text,
                                          static_cast<size_t>(display_start));
    end = rich_edit_cp_from_text_offset(text,
                                        static_cast<size_t>(display_end));
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

void HistoryWindow::center_entry(HWND edit, int start) const {
    if (!edit || start < 0) return;

    int target_line = static_cast<int>(
        SendMessageW(edit, EM_EXLINEFROMCHAR, 0, start));
    if (target_line < 0) return;

    int first_visible = static_cast<int>(
        SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
    int desired_first = (std::max)(0, target_line - visible_line_count(edit) / 3);
    int delta = desired_first - first_visible;
    if (delta != 0) {
        SendMessageW(edit, EM_LINESCROLL, 0, delta);
    }
}

void HistoryWindow::set_char_range(HWND edit, int start, int end) {
    CHARRANGE range;
    range.cpMin = start;
    range.cpMax = end;
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
}

void HistoryWindow::apply_text_color(HWND edit, int start, int end, COLORREF color) {
    if (!edit || start >= end) return;

    set_char_range(edit, start, end);
    CHARFORMAT2W format = {};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = color;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&format));
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

    auto ranges = entry_display_ranges(edit);
    for (const auto& range : ranges) {
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

        size_t date_end = text.find(L"]", header_start);
        size_t profile_start = text.find(L"Profile:", header_start);
        size_t tokens_start = text.find(L"Tokens:", header_start);

        if (date_end != std::wstring::npos && date_end <= header_end) {
            apply_text_color(edit,
                             rich_edit_cp_from_text_offset(text, header_start),
                             rich_edit_cp_from_text_offset(text, date_end + 1),
                             COLOR_DATE);
        }
        if (profile_start != std::wstring::npos && profile_start < header_end) {
            int profile_end = tokens_start != std::wstring::npos
                ? static_cast<int>(tokens_start)
                : static_cast<int>(header_end);
            apply_text_color(edit,
                             rich_edit_cp_from_text_offset(text, profile_start),
                             rich_edit_cp_from_text_offset(
                                 text, static_cast<size_t>(profile_end)),
                             COLOR_PROFILE);

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
                int value_cp_start = rich_edit_cp_from_text_offset(
                    text, value_start);
                int value_cp_end = rich_edit_cp_from_text_offset(
                    text, value_end);
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
        if (tokens_start != std::wstring::npos && tokens_start < header_end) {
            apply_text_color(edit,
                             rich_edit_cp_from_text_offset(text, tokens_start),
                             rich_edit_cp_from_text_offset(text, header_end),
                             COLOR_TOKENS);
        }
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

void HistoryWindow::set_profile_link_hover(HWND edit, int start, int end) {
    if (edit == hover_profile_edit_ &&
        start == hover_profile_start_ &&
        end == hover_profile_end_) {
        return;
    }

    HWND previous_edit = hover_profile_edit_;
    int previous_start = hover_profile_start_;
    int previous_end = hover_profile_end_;

    auto format_link = [&](HWND target, int link_start, int link_end,
                           COLORREF color) {
        if (!target || link_start < 0 || link_end <= link_start) return;

        CHARRANGE saved = {};
        POINT scroll_pos = {};
        SendMessageW(target, EM_EXGETSEL, 0,
                     reinterpret_cast<LPARAM>(&saved));
        SendMessageW(target, EM_GETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&scroll_pos));

        set_char_range(target, link_start, link_end);
        CHARFORMAT2W format = {};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR | CFM_UNDERLINE | CFM_BOLD;
        format.crTextColor = color;
        format.dwEffects = CFE_UNDERLINE | CFE_BOLD;
        SendMessageW(target, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&format));

        SendMessageW(target, EM_EXSETSEL, 0,
                     reinterpret_cast<LPARAM>(&saved));
        SendMessageW(target, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&scroll_pos));
        InvalidateRect(target, nullptr, FALSE);
    };

    format_link(previous_edit, previous_start, previous_end, COLOR_PROFILE);

    hover_profile_edit_ = edit;
    hover_profile_start_ = start;
    hover_profile_end_ = end;

    format_link(hover_profile_edit_, hover_profile_start_,
                hover_profile_end_, RGB(36, 155, 122));
}

void HistoryWindow::update_profile_link_hover(HWND edit, int char_index) {
    if (!edit || char_index < 0) {
        set_profile_link_hover(nullptr, -1, -1);
        return;
    }

    const auto& links = links_for(edit);
    for (const auto& link : links) {
        if (char_index >= link.start && char_index < link.end) {
            set_profile_link_hover(edit, link.start, link.end);
            return;
        }
    }

    set_profile_link_hover(nullptr, -1, -1);
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
    for (const auto& link : links) {
        if (index >= link.start && index < link.end) return true;
    }
    return false;
}

bool HistoryWindow::handle_edit_left_click(HWND edit, LPARAM lParam) {
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    int index = char_index_at(edit, x, y);
    if (index < 0) return false;
    const auto& links = links_for(edit);
    for (const auto& link : links) {
        if (index >= link.start && index < link.end) {
            show_profile_prompt(link.profile);
            return true;
        }
    }
    return false;
}

std::wstring HistoryWindow::get_edit_text_w(HWND edit) const {
    if (!edit) return {};
    int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
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
    std::string name = trim_ascii(wide_to_utf8(get_edit_text_w(prompt_profile_combo_)));
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
        DWORD edit_common = WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_LEFT |
                            ES_NOHIDESEL | ES_WANTRETURN;
        DWORD edit_editable = edit_common;
        DWORD edit_readonly = edit_common | ES_READONLY;
        DWORD label_style = WS_CHILD | WS_VISIBLE | SS_LEFT;

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
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                CBS_DROPDOWN | CBS_AUTOHSCROLL |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_COMBO)),
            hInstance_, nullptr);
        subclass_combo_box(prompt_profile_combo_);

        prompt_profile_new_button_ = CreateWindowExW(
            0, L"BUTTON", L"New",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_NEW)),
            hInstance_, nullptr);

        prompt_profile_rename_button_ = CreateWindowExW(
            0, L"BUTTON", L"Rename",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
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
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROMPT_BACK)),
            hInstance_, nullptr);

        prompt_save_button_ = CreateWindowExW(
            0, L"BUTTON", L"Save roles",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROMPT_SAVE)),
            hInstance_, nullptr);

        prompt_status_ = CreateWindowExW(
            0, L"STATIC", L"",
            label_style, 0, 0, 0, 0,
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

    profile_mode_ = true;
    for (HWND h : { recorded_edit_, transcribed_edit_, ai_language_edit_,
                    clear_button_ }) {
        if (h) ShowWindow(h, SW_HIDE);
    }
    for (const auto& entry_view : history_entry_views_) {
        if (entry_view.copy_button) ShowWindow(entry_view.copy_button, SW_HIDE);
        if (entry_view.profile_button) ShowWindow(entry_view.profile_button, SW_HIDE);
    }
    for (HWND h : { prompt_pre_label_, prompt_pre_edit_,
                    prompt_profile_label_, prompt_profile_combo_,
                    prompt_profile_new_button_, prompt_profile_rename_button_,
                    prompt_transform_label_, prompt_transform_edit_,
                    prompt_back_button_, prompt_save_button_,
                    prompt_status_ }) {
        if (h) ShowWindow(h, SW_SHOW);
    }
    layout();
    SetFocus(prompt_pre_edit_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HistoryWindow::show_history_view() {
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
                    clear_button_ }) {
        if (h) ShowWindow(h, SW_SHOW);
    }
    layout();
    update_copy_button_positions();
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HistoryWindow::layout_prompt_window() {
    if (!hwnd_ || !profile_mode_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int width  = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    int padding       = 24;
    int section_gap   = 14;
    int footer_gap    = 18;
    int button_height = 34;
    int button_width  = 132;
    int short_height  = 32;       // step 2 profile controls row
    int small_button_width = 86;
    int control_gap = 10;

    int content_width = (std::max)(100, width - padding * 2);
    int footer_top = height - padding - button_height;
    int content_bottom = footer_top - footer_gap;
    int panel_header = 76;
    int panel_inner = SECTION_TITLE_INSET;
    int profile_panel_height = panel_header + panel_inner + short_height
                               + panel_inner;
    int flex_total = content_bottom - padding - section_gap * 2
                     - profile_panel_height
                     - (panel_header + panel_inner * 2) * 2;
    int flex_height = (std::max)(48, flex_total / 2);

    int y = padding;
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
    y += transform_panel_height;

    int button_x = width - padding - button_width;
    int button_y = footer_top;
    MoveWindow(prompt_back_button_, padding, button_y,
               button_width, button_height, TRUE);
    MoveWindow(prompt_save_button_, button_x, button_y,
               button_width, button_height, TRUE);
    MoveWindow(prompt_status_, padding + button_width + control_gap,
               button_y + 8,
               (std::max)(0, button_x - padding * 3 - button_width),
               button_height, TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HistoryWindow::save_prompt_edits() {
    if (!config_) return;

    std::string pre = wide_to_utf8(get_edit_text_w(prompt_pre_edit_));
    std::string transform = wide_to_utf8(get_edit_text_w(prompt_transform_edit_));
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

        case WM_ERASEBKGND:
            return 1;

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
                for (const auto& entry_view : self->history_entry_views_) {
                    if (entry_view.profile_button == item->hwndItem) {
                        self->draw_profile_button(item);
                        return TRUE;
                    }
                    if (entry_view.copy_button == item->hwndItem) {
                        self->draw_steel_button(item);
                        return TRUE;
                    }
                }
            }
            break;

        case WM_COMMAND:
            if (self && HIWORD(wParam) == EN_VSCROLL) {
                HWND edit = reinterpret_cast<HWND>(lParam);
                if (edit == self->recorded_edit_ ||
                    edit == self->transcribed_edit_ ||
                    edit == self->ai_language_edit_) {
                    self->update_copy_button_positions();
                }
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
            if (self && HIWORD(wParam) == BN_CLICKED) {
                HWND button = reinterpret_cast<HWND>(lParam);
                for (const auto& entry_view : self->history_entry_views_) {
                    if (entry_view.profile_button == button) {
                        int start = 0, end = 0;
                        if (self->entry_cp_range(entry_view.edit,
                                                 entry_view.entry_index,
                                                 start, end)) {
                            const auto& links = self->links_for(entry_view.edit);
                            for (const auto& link : links) {
                                if (link.start >= start && link.end <= end) {
                                    self->show_profile_prompt(link.profile);
                                    return 0;
                                }
                            }
                        }
                    }
                    if (entry_view.copy_button == button) {
                        self->copy_highlighted_entry(entry_view.edit,
                                                     entry_view.entry_index);
                        return 0;
                    }
                }
            }
            break;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            return 0;

        case WM_APP + 1:
            if (self) self->update_copy_button_positions();
            return 0;

        case WM_APP + 2:
            if (self) {
                self->scroll_history_to_top();
                self->update_copy_button_positions();
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
            case WM_DRAWITEM: {
                const auto* item =
                    reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                for (const auto& entry_view : self->history_entry_views_) {
                    if (entry_view.profile_button == item->hwndItem) {
                        self->draw_profile_button(item);
                        return TRUE;
                    }
                    if (entry_view.copy_button == item->hwndItem) {
                        self->draw_steel_button(item);
                        return TRUE;
                    }
                }
                break;
            }
            case WM_COMMAND:
                if (HIWORD(wParam) == BN_CLICKED) {
                    HWND button = reinterpret_cast<HWND>(lParam);
                    for (const auto& entry_view : self->history_entry_views_) {
                        if (entry_view.profile_button == button) {
                            int start = 0, end = 0;
                            if (self->entry_cp_range(entry_view.edit,
                                                     entry_view.entry_index,
                                                     start, end)) {
                                const auto& links =
                                    self->links_for(entry_view.edit);
                                for (const auto& link : links) {
                                    if (link.start >= start && link.end <= end) {
                                        self->show_profile_prompt(link.profile);
                                        return 0;
                                    }
                                }
                            }
                        }
                        if (entry_view.copy_button == button) {
                            self->copy_highlighted_entry(entry_view.edit,
                                                         entry_view.entry_index);
                            return 0;
                        }
                    }
                }
                break;
            case WM_MOUSEMOVE:
                self->handle_edit_mouse_move(hwnd, lParam);
                break;
            case WM_LBUTTONDOWN:
                if (self->is_over_profile(hwnd,
                                          GET_X_LPARAM(lParam),
                                          GET_Y_LPARAM(lParam))) {
                    SetFocus(hwnd);
                    return 0;
                }
                break;
            case WM_LBUTTONUP:
                if (self->handle_edit_left_click(hwnd, lParam)) {
                    return 0;
                }
                break;
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {
                    POINT pt;
                    if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) &&
                        self->is_over_profile(hwnd, pt.x, pt.y)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
                break;
        }
    }

    WNDPROC original = self ? self->original_edit_proc_ : nullptr;
    if (!original) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    LRESULT result = CallWindowProcW(original, hwnd, msg, wParam, lParam);
    bool scroll_related_key =
        msg == WM_KEYDOWN &&
        (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_PRIOR ||
         wParam == VK_NEXT || wParam == VK_HOME || wParam == VK_END);
    if (self && (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL ||
                 msg == WM_HSCROLL || scroll_related_key)) {
        self->update_copy_button_positions();
    }
    return result;
}

LRESULT CALLBACK HistoryWindow::CopyButtonWndProc(HWND hwnd, UINT msg,
                                                  WPARAM wParam,
                                                  LPARAM lParam) {
    auto* self = reinterpret_cast<HistoryWindow*>(
        GetPropW(hwnd, COPY_BUTTON_OWNER_PROP));
    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, COPY_BUTTON_OWNER_PROP);
    }

    if (self) {
        switch (msg) {
            case WM_MOUSEMOVE: {
                self->set_profile_link_hover(nullptr, -1, -1);
                if (self->hover_button_ != hwnd) {
                    HWND previous = self->hover_button_;
                    self->hover_button_ = hwnd;
                    if (previous) InvalidateRect(previous, nullptr, TRUE);
                    InvalidateRect(hwnd, nullptr, TRUE);

                    TRACKMOUSEEVENT tme = {};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                }
                self->handle_copy_button_mouse_move(hwnd);
                break;
            }
            case WM_MOUSELEAVE:
                if (self->hover_button_ == hwnd) {
                    self->hover_button_ = nullptr;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                break;
            case WM_SETCURSOR:
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
        }
    }

    WNDPROC original = self ? self->original_copy_button_proc_ : nullptr;
    if (!original) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(original, hwnd, msg, wParam, lParam);
}

} // namespace vtt
