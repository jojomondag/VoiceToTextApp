#pragma once
#include "../live_dictation_buffer.h"
#include "win_compat.h"

#include <functional>
#include <string>

namespace vtt {

// The optional transcript window ("Live dictation..." in the tray): the draft
// of the current dictation, editable while speaking, with Stop / Copy /
// Insert. It also holds the session transcript that History is saved from,
// whether it is visible or not. All methods run on the UI thread.
class DictationWindow {
public:
    ~DictationWindow();

    bool initialize(HINSTANCE instance);
    void shutdown();
    void show(bool activate = false);
    void hide();
    bool is_visible() const;
    HWND handle() const { return nullptr; }
    // True when this window has keyboard focus (`window` is the compositor's
    // focused window, which cannot be compared with a Qt window directly).
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
    class Widget;

    void observe_editor();
    void request_stop();
    void copy_text();
    void update_buttons();

    Widget* widget_ = nullptr;
    bool recording_ = false;
    bool processing_ = false;
    bool busy_ = false;
    bool applying_ = false;
    LiveDictationBuffer insertion_;
    std::function<void()> stop_callback_;
    std::function<void(const std::string&)> accept_callback_;
};

} // namespace vtt
