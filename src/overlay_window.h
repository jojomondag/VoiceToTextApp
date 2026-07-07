#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace vtt {

enum class OverlayState {
    Hidden,
    Listening,
    Processing,
    Downloading,
    Error
};

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    bool initialize(HINSTANCE hInstance);
    void shutdown();

    void show(OverlayState state, const std::string& text = "");
    void hide();
    void set_audio_level(float level); // 0.0 - 1.0
    void set_progress(float progress, const std::string& text = "");

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void paint();
    void update_position();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    OverlayState state_ = OverlayState::Hidden;
    std::string text_;
    float audio_level_ = 0.0f;
    float progress_ = 0.0f;
    std::vector<float> audio_levels_;
    bool initialized_ = false;
};

} // namespace vtt
