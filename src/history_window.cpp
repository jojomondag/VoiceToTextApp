#include "history_window.h"

#include "config_manager.h"

#include <algorithm>
#include <richedit.h>
#include <string>
#include <windowsx.h>

namespace vtt {
namespace {

static constexpr int IDC_CLEAR_BUTTON = 1001;
static constexpr int IDC_PROMPT_SAVE  = 1010;
static constexpr int CLEAR_BUTTON_WIDTH = 90;
static constexpr int CLEAR_BUTTON_GAP = 8;

static constexpr const wchar_t* HISTORY_CLASS_NAME = L"VTTHistoryWindow";
static constexpr const wchar_t* PROMPT_CLASS_NAME = L"VTTProfilePromptWindow";
static constexpr const wchar_t* EDIT_OWNER_PROP = L"VTTHistoryWindowOwner";
static constexpr const wchar_t* ENTRY_BEGIN = L"[[VTT_ENTRY_BEGIN:";
static constexpr const wchar_t* ENTRY_END = L"[[VTT_ENTRY_END:";
static constexpr COLORREF COLOR_DATE = RGB(32, 92, 170);
static constexpr COLORREF COLOR_PROFILE = RGB(32, 128, 96);
static constexpr COLORREF COLOR_TOKENS = RGB(150, 92, 12);
static constexpr COLORREF COLOR_TEXT = RGB(32, 32, 32);

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
    if (prompt_hwnd_) {
        DestroyWindow(prompt_hwnd_);
        prompt_hwnd_ = nullptr;
        prompt_pre_label_ = nullptr;
        prompt_pre_edit_ = nullptr;
        prompt_profile_label_ = nullptr;
        prompt_profile_edit_ = nullptr;
        prompt_transform_label_ = nullptr;
        prompt_transform_edit_ = nullptr;
        prompt_save_button_ = nullptr;
        prompt_status_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hInstance_) {
        UnregisterClassW(PROMPT_CLASS_NAME, hInstance_);
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
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    } else {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void HistoryWindow::refresh() {
    if (!history_ || !recorded_edit_ || !transcribed_edit_ ||
        !ai_language_edit_) return;

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
    highlighted_entry_ = -1;
    apply_header_colors(recorded_edit_);
    apply_header_colors(transcribed_edit_);
    apply_header_colors(ai_language_edit_);
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
                       ES_AUTOVSCROLL | ES_READONLY | ES_LEFT | ES_NOHIDESEL;

    recorded_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    transcribed_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    ai_language_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
        edit_style,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    clear_button_ = CreateWindowExW(
        0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLEAR_BUTTON)),
        hInstance_, nullptr);

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessage(recorded_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(transcribed_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(ai_language_label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(recorded_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(transcribed_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(ai_language_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(clear_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessage(recorded_edit_, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    SendMessage(transcribed_edit_, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    SendMessage(ai_language_edit_, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));

    subclass_edit(recorded_edit_);
    subclass_edit(transcribed_edit_);
    subclass_edit(ai_language_edit_);
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

    // Reserve space at the right end of the header row for the Clear button.
    // Parenthesize std::max to avoid the windows.h `max` macro collision.
    int ai_label_width = (std::max)(panel_width - CLEAR_BUTTON_WIDTH - CLEAR_BUTTON_GAP, 60);
    int button_x = x3 + ai_label_width + CLEAR_BUTTON_GAP;
    int button_width = (std::max)(panel_width - ai_label_width - CLEAR_BUTTON_GAP,
                                  CLEAR_BUTTON_WIDTH);

    MoveWindow(recorded_label_, x1, padding, panel_width, label_height, TRUE);
    MoveWindow(transcribed_label_, x2, padding, panel_width, label_height, TRUE);
    MoveWindow(ai_language_label_, x3, padding, ai_label_width, label_height, TRUE);
    MoveWindow(clear_button_, button_x, padding, button_width, label_height, TRUE);
    MoveWindow(recorded_edit_, x1, edit_top, panel_width, edit_height, TRUE);
    MoveWindow(transcribed_edit_, x2, edit_top, panel_width, edit_height, TRUE);
    MoveWindow(ai_language_edit_, x3, edit_top, panel_width, edit_height, TRUE);
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

std::wstring HistoryWindow::get_edit_text(HWND edit) const {
    int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};

    std::wstring text(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::vector<std::pair<int, int>> HistoryWindow::entry_ranges(HWND edit) const {
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
        if (line.find(L"]    Profile:") != std::wstring::npos &&
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
        clear_highlight();
        return;
    }

    HDC hdc = GetDC(edit);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    if (old_font) SelectObject(hdc, old_font);
    ReleaseDC(edit, hdc);

    int line_height = tm.tmHeight + tm.tmExternalLeading;
    if (line_height <= 0) line_height = 16;

    int first_visible_line = static_cast<int>(
        SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
    int line_index = first_visible_line + (y / line_height);
    int entry_index = entry_index_from_line(edit, line_index);
    if (entry_index < 0) {
        clear_highlight();
        return;
    }
    if (entry_index == highlighted_entry_) return;

    highlight_entry(entry_index);
}

void HistoryWindow::highlight_entry(int entry_index) {
    if (updating_highlight_) return;
    updating_highlight_ = true;

    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        auto ranges = entry_ranges(edit);
        if (entry_index < 0 ||
            entry_index >= static_cast<int>(ranges.size())) {
            continue;
        }

        set_char_range(edit, ranges[entry_index].first,
                       ranges[entry_index].second);
    }

    highlighted_entry_ = entry_index;
    updating_highlight_ = false;
}

void HistoryWindow::clear_highlight() {
    HWND edits[] = {recorded_edit_, transcribed_edit_, ai_language_edit_};
    for (HWND edit : edits) {
        set_char_range(edit, -1, -1);
    }
    highlighted_entry_ = -1;
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

    int length = GetWindowTextLengthW(edit);
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

    std::wstring text = get_edit_text(edit);
    auto ranges = entry_ranges(edit);
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
            apply_text_color(edit, static_cast<int>(header_start),
                             static_cast<int>(date_end + 1),
                             COLOR_DATE);
        }
        if (profile_start != std::wstring::npos && profile_start < header_end) {
            int profile_end = tokens_start != std::wstring::npos
                ? static_cast<int>(tokens_start)
                : static_cast<int>(header_end);
            apply_text_color(edit, static_cast<int>(profile_start), profile_end,
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
                apply_profile_link_format(edit, static_cast<int>(value_start),
                                          static_cast<int>(value_end));
                ProfileLink link;
                link.start = static_cast<int>(value_start);
                link.end = static_cast<int>(value_end);
                link.profile = trim_whitespace(text, value_start, value_end);
                if (!link.profile.empty()) {
                    links.push_back(std::move(link));
                }
            }
        }
        if (tokens_start != std::wstring::npos && tokens_start < header_end) {
            apply_text_color(edit, static_cast<int>(tokens_start),
                             static_cast<int>(header_end), COLOR_TOKENS);
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

void HistoryWindow::show_profile_prompt(const std::string& profile) {
    if (profile.empty()) return;

    // Lazily create the window and its child controls on first use. Three
    // editable sections mirror the two-step LLM pipeline:
    //   Step 1 (editable) — cleanup role applied to raw dictation
    //   Step 2 (read-only) — runtime profile selection (informational)
    //   Step 3 (editable) — per-profile transform role applied to cleaned text
    if (!prompt_hwnd_) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PromptWndProc;
        wc.hInstance = hInstance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = PROMPT_CLASS_NAME;
        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        }

        prompt_hwnd_ = CreateWindowExW(
            0, PROMPT_CLASS_NAME, L"Profile prompt",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 900, 720,
            hwnd_, nullptr, hInstance_, this);
        if (!prompt_hwnd_) return;

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
            prompt_hwnd_, nullptr, hInstance_, nullptr);
        prompt_pre_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", edit_editable,
            0, 0, 0, 0, prompt_hwnd_, nullptr, hInstance_, nullptr);

        prompt_profile_label_ = CreateWindowExW(
            0, L"STATIC",
            L"Step 2 — Selected profile (read-only, runtime classifier output)",
            label_style, 0, 0, 0, 0,
            prompt_hwnd_, nullptr, hInstance_, nullptr);
        prompt_profile_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", edit_readonly,
            0, 0, 0, 0, prompt_hwnd_, nullptr, hInstance_, nullptr);

        prompt_transform_label_ = CreateWindowExW(
            0, L"STATIC",
            L"Step 3 — Transform role (editable). "
            L"Per-profile rewrite rules applied to the cleaned text from Step 1.",
            label_style, 0, 0, 0, 0,
            prompt_hwnd_, nullptr, hInstance_, nullptr);
        prompt_transform_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", edit_editable,
            0, 0, 0, 0, prompt_hwnd_, nullptr, hInstance_, nullptr);

        prompt_save_button_ = CreateWindowExW(
            0, L"BUTTON", L"Save roles",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, prompt_hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROMPT_SAVE)),
            hInstance_, nullptr);

        prompt_status_ = CreateWindowExW(
            0, L"STATIC", L"",
            label_style, 0, 0, 0, 0,
            prompt_hwnd_, nullptr, hInstance_, nullptr);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND h : { prompt_pre_label_, prompt_pre_edit_,
                        prompt_profile_label_, prompt_profile_edit_,
                        prompt_transform_label_, prompt_transform_edit_,
                        prompt_save_button_, prompt_status_ }) {
            SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        SendMessage(prompt_pre_edit_, EM_SETLIMITTEXT, 0, 0);
        SendMessage(prompt_transform_edit_, EM_SETLIMITTEXT, 0, 0);
    }

    std::wstring title = L"Profile prompt: ";
    title.append(utf8_to_wide(profile));
    SetWindowTextW(prompt_hwnd_, title.c_str());

    if (config_) {
        const auto& cfg = config_->config();
        SetWindowTextW(prompt_pre_edit_, utf8_to_wide(cfg.llm_pre_prompt).c_str());
        SetWindowTextW(prompt_transform_edit_,
                       utf8_to_wide(cfg.llm_profile_transform_prompt).c_str());
    }
    SetWindowTextW(prompt_profile_edit_, utf8_to_wide(profile).c_str());
    SetWindowTextW(prompt_status_, L"");

    layout_prompt_window();

    if (IsIconic(prompt_hwnd_)) {
        ShowWindow(prompt_hwnd_, SW_RESTORE);
    } else {
        ShowWindow(prompt_hwnd_, SW_SHOWMAXIMIZED);
    }
    SetForegroundWindow(prompt_hwnd_);
    SetFocus(prompt_pre_edit_);
}

void HistoryWindow::layout_prompt_window() {
    if (!prompt_hwnd_) return;

    RECT rc;
    GetClientRect(prompt_hwnd_, &rc);
    int width  = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    int padding       = 14;
    int label_height  = 36;       // multi-line label: heading + description
    int section_gap   = 10;
    int button_height = 30;
    int button_width  = 130;
    int button_area   = button_height + padding;
    int short_height  = 28;       // step 2 (profile name) is a single line

    int content_width = width - padding * 2;
    int section_total = height - padding - button_area;
    // Two flex sections (steps 1 and 3) share the remaining height.
    int flex_total = section_total - padding
                     - label_height * 3 - section_gap * 2 - short_height;
    int flex_height = (std::max)(flex_total / 2, 60);

    int y = padding;
    MoveWindow(prompt_pre_label_, padding, y, content_width, label_height, TRUE);
    y += label_height;
    MoveWindow(prompt_pre_edit_, padding, y, content_width, flex_height, TRUE);
    y += flex_height + section_gap;

    MoveWindow(prompt_profile_label_, padding, y, content_width, label_height, TRUE);
    y += label_height;
    MoveWindow(prompt_profile_edit_, padding, y, content_width, short_height, TRUE);
    y += short_height + section_gap;

    MoveWindow(prompt_transform_label_, padding, y, content_width, label_height, TRUE);
    y += label_height;
    MoveWindow(prompt_transform_edit_, padding, y, content_width, flex_height, TRUE);
    y += flex_height;

    int button_x = width - padding - button_width;
    int button_y = height - padding - button_height;
    MoveWindow(prompt_save_button_, button_x, button_y,
               button_width, button_height, TRUE);
    MoveWindow(prompt_status_, padding, button_y + 6,
               button_x - padding * 2, button_height, TRUE);
}

void HistoryWindow::save_prompt_edits() {
    if (!config_) return;

    auto wide_to_utf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
        if (len <= 0) return std::string();
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                            static_cast<int>(w.size()),
                            out.data(), len, nullptr, nullptr);
        return out;
    };

    std::string pre = wide_to_utf8(get_edit_text_w(prompt_pre_edit_));
    std::string transform = wide_to_utf8(get_edit_text_w(prompt_transform_edit_));

    auto& cfg = config_->config();
    cfg.llm_pre_prompt = pre;
    cfg.llm_profile_transform_prompt = transform;

    bool ok = config_->save();
    if (prompt_status_) {
        SetWindowTextW(prompt_status_,
                       ok ? L"Roles saved to config.json."
                          : L"Save failed — check write permissions.");
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

        case WM_COMMAND:
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
            break;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
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
    return CallWindowProcW(original, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK HistoryWindow::PromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
            if (self) self->layout_prompt_window();
            return 0;

        case WM_COMMAND:
            if (self && LOWORD(wParam) == IDC_PROMPT_SAVE &&
                HIWORD(wParam) == BN_CLICKED) {
                self->save_prompt_edits();
                return 0;
            }
            break;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            if (self && self->prompt_hwnd_ == hwnd) {
                self->prompt_hwnd_ = nullptr;
                self->prompt_pre_label_ = nullptr;
                self->prompt_pre_edit_ = nullptr;
                self->prompt_profile_label_ = nullptr;
                self->prompt_profile_edit_ = nullptr;
                self->prompt_transform_label_ = nullptr;
                self->prompt_transform_edit_ = nullptr;
                self->prompt_save_button_ = nullptr;
                self->prompt_status_ = nullptr;
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vtt
