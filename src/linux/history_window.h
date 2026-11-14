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
    void refresh();

private:
    class Widget;
    Widget* widget_ = nullptr;
};

} // namespace vtt
