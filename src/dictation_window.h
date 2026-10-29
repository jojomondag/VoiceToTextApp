#pragma once

#include "live_dictation_buffer.h"

#include <functional>
#include <string>
#include <windows.h>

namespace vtt {

// All methods run on the application UI thread. Worker transcription events
// should be marshalled through the application's existing window message loop.
class DictationWindow {
public:
    ~DictationWindow();

    bool initialize(HINSTANCE instance);
    void shutdown();
    void show(bool activate = false);
    void hide();
    bool is_visible() const;
    HWND handle() const { return hwnd_; }
    bool owns_window(HWND window) const;

    void begin_session(bool clear = true);
    void update_transcript(int utterance_id, const std::string& text, bool is_final);
    void finish_session();
    void set_busy(bool busy);
    void set_status(const std::wstring& status);
    std::string text_utf8() const;

    void set_stop_callback(std::function<void()> callback);
    void set_accept_callback(std::function<void(const std::string&)> callback);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK EditWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void layout();
    void observe_editor();
    void request_stop();
    void copy_text();
    std::wstring document() const;
    void update_buttons();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hint_ = nullptr;
    HWND editor_ = nullptr;
    HWND status_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND accept_button_ = nullptr;
    HWND copy_button_ = nullptr;
    WNDPROC original_edit_proc_ = nullptr;
    HFONT font_ = nullptr;
    HFONT editor_font_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    HBRUSH editor_brush_ = nullptr;
    HICON icon_ = nullptr;
    HICON small_icon_ = nullptr;
    bool recording_ = false;
    bool processing_ = false;
    bool busy_ = false;
    bool applying_ = false;
    LiveDictationBuffer insertion_;
    std::function<void()> stop_callback_;
    std::function<void(const std::string&)> accept_callback_;
};

} // namespace vtt
