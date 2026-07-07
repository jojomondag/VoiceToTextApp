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
    static LRESULT CALLBACK CopyButtonWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ComboSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void create_child_controls();
    void layout();
    void create_fonts_and_brushes();
    void paint_background(HWND hwnd, HDC hdc);
    void paint_section(HDC hdc, const RECT& rc, const wchar_t* title,
                       const wchar_t* subtitle);
    void draw_steel_button(const DRAWITEMSTRUCT* item);
    void draw_profile_button(const DRAWITEMSTRUCT* item);
    void draw_combo_item(const DRAWITEMSTRUCT* item);
    void apply_modern_control_colors();
    void apply_entry_paragraph_spacing();
    void apply_entry_cards();
    void subclass_edit(HWND edit);
    void subclass_copy_button(HWND button);
    void handle_edit_mouse_move(HWND edit, LPARAM lParam);
    void handle_copy_button_mouse_move(HWND button);
    bool handle_edit_left_click(HWND edit, LPARAM lParam);
    bool is_over_profile(HWND edit, int x, int y) const;
    void highlight_entry(int entry_index, HWND source_edit);
    void clear_highlight();
    void rebuild_copy_buttons();
    void destroy_copy_buttons();
    void scroll_history_to_top();
    void update_copy_button_positions();
    void copy_highlighted_entry(HWND edit, int entry_index);
    std::wstring entry_body_text(HWND edit, int entry_index) const;
    void apply_entry_background(HWND edit, int start, int end, COLORREF color);
    void clear_entry_background(HWND edit, int start, int end);
    bool entry_cp_range(HWND edit, int entry_index, int& start, int& end) const;
    void center_entry(HWND edit, int start) const;
    int visible_line_count(HWND edit) const;
    void apply_header_colors(HWND edit);
    void apply_text_color(HWND edit, int start, int end, COLORREF color);
    void apply_profile_link_format(HWND edit, int start, int end);
    void update_profile_link_hover(HWND edit, int char_index);
    void set_profile_link_hover(HWND edit, int start, int end);
    void set_char_range(HWND edit, int start, int end);
    int char_index_at(HWND edit, int x, int y) const;
    int entry_index_from_char(HWND edit, int char_index) const;
    int entry_index_from_line(HWND edit, int line_index) const;
    std::vector<std::pair<int, int>> entry_display_ranges(HWND edit) const;
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
    void populate_profile_combo(const std::string& selected);
    void select_profile_for_editing(const std::string& profile);
    std::string current_profile_name_from_ui() const;
    void create_new_profile();
    void rename_current_profile();

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
    struct HistoryEntryView {
        HWND copy_button = nullptr;
        HWND profile_button = nullptr;
        HWND edit = nullptr;
        int entry_index = -1;
    };
    std::vector<HistoryEntryView> history_entry_views_;
    static constexpr int ENTRY_COPY_WIDTH = 24;
    static constexpr int ENTRY_COPY_HEIGHT = 22;

    WNDPROC original_edit_proc_ = nullptr;
    WNDPROC original_copy_button_proc_ = nullptr;
    WNDPROC original_combo_proc_ = nullptr;
    bool combo_focused_ = false;
    PanelRanges panel_ranges_;
    PanelLinks panel_links_;
    int highlighted_entry_ = -1;
    bool updating_highlight_ = false;
    HWND hover_button_ = nullptr;
    HWND hover_profile_edit_ = nullptr;
    int hover_profile_start_ = -1;
    int hover_profile_end_ = -1;
    HFONT label_font_ = nullptr;
    HFONT text_font_ = nullptr;
    HFONT button_font_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
    HBRUSH control_brush_ = nullptr;
    HBRUSH button_brush_ = nullptr;
    HICON app_icon_large_ = nullptr;
    HICON app_icon_small_ = nullptr;
    RECT recorded_panel_rc_ = {};
    RECT transcribed_panel_rc_ = {};
    RECT ai_language_panel_rc_ = {};
    RECT prompt_pre_panel_rc_ = {};
    RECT prompt_profile_panel_rc_ = {};
    RECT prompt_transform_panel_rc_ = {};

    void subclass_combo_box(HWND combo);

    HWND prompt_hwnd_ = nullptr;
    HWND prompt_pre_label_ = nullptr;
    HWND prompt_pre_edit_ = nullptr;
    HWND prompt_profile_label_ = nullptr;
    HWND prompt_profile_edit_ = nullptr;
    HWND prompt_profile_combo_ = nullptr;
    HWND prompt_profile_new_button_ = nullptr;
    HWND prompt_profile_rename_button_ = nullptr;
    HWND prompt_transform_label_ = nullptr;
    HWND prompt_transform_edit_ = nullptr;
    HWND prompt_save_button_ = nullptr;
    HWND prompt_status_ = nullptr;
    std::string active_prompt_profile_;
};

} // namespace vtt
