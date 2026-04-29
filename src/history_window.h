#pragma once

#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include "transcript_history.h"

namespace vtt {

class ConfigManager;

class HistoryWindow {
public:
    bool initialize(HINSTANCE hInstance, TranscriptHistory* history,
                    ConfigManager* config);
    void shutdown();
    void show();
    void refresh();

private:
    struct PanelRanges {
        std::vector<std::pair<int, int>> recorded;
        std::vector<std::pair<int, int>> transcribed;
        std::vector<std::pair<int, int>> ai_language;
    };

    struct ProfileLink {
        int start = 0;
        int end = 0;
        std::string profile;
    };

    struct PanelLinks {
        std::vector<ProfileLink> recorded;
        std::vector<ProfileLink> transcribed;
        std::vector<ProfileLink> ai_language;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void create_child_controls();
    void layout();
    void subclass_edit(HWND edit);
    void handle_edit_mouse_move(HWND edit, LPARAM lParam);
    bool handle_edit_left_click(HWND edit, LPARAM lParam);
    bool is_over_profile(HWND edit, int x, int y) const;
    void highlight_entry(int entry_index);
    void clear_highlight();
    void apply_header_colors(HWND edit);
    void apply_text_color(HWND edit, int start, int end, COLORREF color);
    void apply_profile_link_format(HWND edit, int start, int end);
    void set_char_range(HWND edit, int start, int end);
    int char_index_at(HWND edit, int x, int y) const;
    int entry_index_from_char(HWND edit, int char_index) const;
    int entry_index_from_line(HWND edit, int line_index) const;
    std::vector<std::pair<int, int>> entry_ranges(HWND edit) const;
    std::vector<ProfileLink>& links_for(HWND edit);
    const std::vector<ProfileLink>& links_for(HWND edit) const;
    std::wstring strip_delimiters_and_capture_ranges(
        const std::wstring& input,
        std::vector<std::pair<int, int>>& ranges) const;
    std::wstring get_edit_text(HWND edit) const;

    void show_profile_prompt(const std::string& profile);
    void layout_prompt_window();
    void save_prompt_edits();
    std::wstring get_edit_text_w(HWND edit) const;

    HWND hwnd_ = nullptr;
    HWND recorded_label_ = nullptr;
    HWND transcribed_label_ = nullptr;
    HWND ai_language_label_ = nullptr;
    HWND recorded_edit_ = nullptr;
    HWND transcribed_edit_ = nullptr;
    HWND ai_language_edit_ = nullptr;
    HWND clear_button_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    HMODULE rich_edit_module_ = nullptr;
    TranscriptHistory* history_ = nullptr;
    ConfigManager* config_ = nullptr;
    WNDPROC original_edit_proc_ = nullptr;
    PanelRanges panel_ranges_;
    PanelLinks panel_links_;
    int highlighted_entry_ = -1;
    bool updating_highlight_ = false;

    HWND prompt_hwnd_ = nullptr;
    HWND prompt_pre_label_ = nullptr;
    HWND prompt_pre_edit_ = nullptr;
    HWND prompt_profile_label_ = nullptr;
    HWND prompt_profile_edit_ = nullptr;
    HWND prompt_transform_label_ = nullptr;
    HWND prompt_transform_edit_ = nullptr;
    HWND prompt_save_button_ = nullptr;
    HWND prompt_status_ = nullptr;
};

} // namespace vtt
