#pragma once

#include <windows.h>

#include "transcript_history.h"

namespace vtt {

class HistoryWindow {
public:
    bool initialize(HINSTANCE hInstance, TranscriptHistory* history);
    void shutdown();
    void show();
    void refresh();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void create_child_controls();
    void layout();

    HWND hwnd_ = nullptr;
    HWND recorded_label_ = nullptr;
    HWND transcribed_label_ = nullptr;
    HWND ai_language_label_ = nullptr;
    HWND recorded_edit_ = nullptr;
    HWND transcribed_edit_ = nullptr;
    HWND ai_language_edit_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    TranscriptHistory* history_ = nullptr;
};

} // namespace vtt
