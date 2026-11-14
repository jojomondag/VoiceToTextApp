#pragma once

#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace vtt {

class ConfigManager;
class TranscriptHistory;

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
    static LRESULT CALLBACK ActionGutterWndProc(HWND hwnd, UINT msg,
                                                WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ComboSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    enum class EntryAction {
        none,
        copy,
        profile,
    };

    struct PaintBuffer {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous_bitmap = nullptr;
        int width = 0;
        int height = 0;
        bool initialized = false;
    };

    void create_child_controls();
    void layout();
    void create_fonts_and_brushes();
    void paint_background(HWND hwnd, HDC hdc);
    void paint_section(HDC hdc, const RECT& rc, const wchar_t* title,
                       const wchar_t* subtitle);
    void draw_steel_button(const DRAWITEMSTRUCT* item);
    void draw_entry_action(HDC hdc, const RECT& rect, EntryAction action,
                           bool hot, bool pressed, bool highlighted) const;
    void paint_entry_actions(HWND edit, HDC hdc) const;
    void paint_action_gutter(HWND gutter);
    void draw_action_gutter(HWND gutter, HDC hdc) const;
    void draw_action_gutter_region(HWND gutter, HDC hdc,
                                   const RECT& dirty) const;
    PaintBuffer* paint_buffer_for_gutter(HWND gutter);
    void release_action_gutter_paint_buffers();
    HWND action_gutter_for_edit(HWND edit) const;
    HWND edit_for_action_gutter(HWND gutter) const;
    void redraw_action_gutter(HWND edit, bool immediate = false) const;
    void draw_combo_item(const DRAWITEMSTRUCT* item);
    void apply_modern_control_colors();
    void apply_entry_paragraph_spacing();
    void apply_entry_cards();
    void subclass_edit(HWND edit);
    void handle_edit_mouse_move(HWND edit, LPARAM lParam);
    void handle_action_gutter_mouse_move(HWND gutter, int x, int y);
    void set_highlighted_entry(int entry_index);
    void ensure_highlighted_entry_visible(HWND source_edit, int entry_index);
    bool handle_edit_left_click(HWND edit, LPARAM lParam);
    bool handle_entry_action_left_down(HWND edit, int x, int y);
    bool handle_entry_action_left_up(HWND edit, int x, int y);
    void clear_pressed_entry_action();
    void set_entry_action_hover(HWND edit, int entry_index,
                                EntryAction action);
    bool entry_action_at(HWND edit, int x, int y, int& entry_index,
                         EntryAction& action) const;
    void invalidate_entry_action(HWND edit, int entry_index,
                                 EntryAction action) const;
    bool is_over_profile(HWND edit, int x, int y) const;
    void rebuild_entry_range_cache(HWND edit, const std::wstring& text);
    void rebuild_entry_views();
    void clear_entry_views();
    void scroll_history_to_top();
    void update_entry_action_positions(HWND only_edit = nullptr);
    void copy_highlighted_entry(HWND edit, int entry_index);
    std::wstring entry_body_text(HWND edit, int entry_index) const;
    bool entry_cp_range(HWND edit, int entry_index, int& start, int& end) const;
    int visible_line_count(HWND edit) const;
    void apply_header_colors(HWND edit);
    void apply_profile_link_format(HWND edit, int start, int end);
    void set_char_range(HWND edit, int start, int end);
    int char_index_at(HWND edit, int x, int y) const;
    int entry_index_from_char(HWND edit, int char_index) const;
    const std::vector<std::pair<int, int>>& entry_display_ranges(HWND edit) const;
    const std::vector<std::pair<int, int>>& entry_ranges(HWND edit) const;
    std::vector<ProfileLink>& links_for(HWND edit);
    const std::vector<ProfileLink>& links_for(HWND edit) const;
    std::wstring strip_delimiters_and_capture_ranges(
        const std::wstring& input,
        std::vector<std::pair<int, int>>& ranges) const;
    std::wstring get_edit_text(HWND edit) const;

    void show_profile_prompt(const std::string& profile);
    void show_history_view();
    void layout_prompt_window();
    void save_prompt_edits();
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
    HWND recorded_action_gutter_ = nullptr;
    HWND transcribed_action_gutter_ = nullptr;
    HWND ai_language_action_gutter_ = nullptr;
    HWND clear_button_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    HMODULE rich_edit_module_ = nullptr;
    TranscriptHistory* history_ = nullptr;
    ConfigManager* config_ = nullptr;
    struct HistoryEntryView {
        HWND edit = nullptr;
        int entry_index = -1;
        int start = 0;
        int end = 0;
        std::string profile;
        RECT copy_rect = {};
        RECT profile_rect = {};
    };
    std::vector<HistoryEntryView> history_entry_views_;
    static constexpr int ENTRY_COPY_WIDTH = 24;
    static constexpr int ENTRY_COPY_HEIGHT = 22;

    WNDPROC original_edit_proc_ = nullptr;
    WNDPROC original_combo_proc_ = nullptr;
    bool combo_focused_ = false;
    PanelRanges panel_ranges_;
    PanelRanges panel_cp_ranges_;
    PanelLinks panel_links_;
    bool profile_mode_ = false;
    HWND hover_action_edit_ = nullptr;
    int hover_action_entry_ = -1;
    EntryAction hover_action_ = EntryAction::none;
    // One shared row selection is used by all three history panels.  It is
    // intentionally kept separate from action hover so moving between the
    // text and the fixed gutter does not clear the linked-row marker.
    int highlighted_entry_ = -1;
    HWND pressed_action_edit_ = nullptr;
    int pressed_action_entry_ = -1;
    EntryAction pressed_action_ = EntryAction::none;
    HWND tracked_mouse_edit_ = nullptr;
    HWND tracked_action_gutter_ = nullptr;
    bool handling_history_scroll_ = false;
    bool action_position_update_posted_ = false;
    PaintBuffer recorded_gutter_paint_buffer_;
    PaintBuffer transcribed_gutter_paint_buffer_;
    PaintBuffer ai_language_gutter_paint_buffer_;
    bool history_content_dirty_ = true;
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

    HWND prompt_pre_label_ = nullptr;
    HWND prompt_pre_edit_ = nullptr;
    HWND prompt_profile_label_ = nullptr;
    HWND prompt_profile_edit_ = nullptr;
    HWND prompt_profile_combo_ = nullptr;
    HWND prompt_profile_new_button_ = nullptr;
    HWND prompt_profile_rename_button_ = nullptr;
    HWND prompt_transform_label_ = nullptr;
    HWND prompt_transform_edit_ = nullptr;
    HWND prompt_back_button_ = nullptr;
    HWND prompt_save_button_ = nullptr;
    HWND prompt_status_ = nullptr;
    std::string active_prompt_profile_;
};

} // namespace vtt
