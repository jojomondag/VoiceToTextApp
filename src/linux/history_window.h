#pragma once

#include "win_compat.h"

namespace vtt {

class ConfigManager;
class TranscriptHistory;

// The History window: three columns (recorded voice, clean transcript, AI
// language) with per-entry copy buttons, and the embedded profile editor
// reached by clicking an entry's profile. Qt counterpart of the Win32 window;
// must be used from the UI thread.
class HistoryWindow {
public:
    HistoryWindow();
    ~HistoryWindow();

    bool initialize(HINSTANCE hInstance, TranscriptHistory* history,
                    ConfigManager* config);
    void shutdown();
    void show();
    // Closes the window; when it had focus, the window used before it gets
    // focus back.
    void hide();
    // True when the window is open where the user can see it: on a workspace
    // that a monitor is showing. Open on another workspace does not count.
    bool on_screen() const;
    void refresh();

private:
    class Widget;
    Widget* widget_ = nullptr;
};

} // namespace vtt
