#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace Gdiplus {
class Bitmap;
}

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
    void set_status_text(const std::string& text);
    void set_paste_output_mode(const std::string& mode);
    void set_paste_mode_callback(std::function<void(const std::string&)> callback);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void paint();
    void update_position();
    bool initialize_paste_tooltips();
    void dismiss_paste_tooltip();
    void load_paste_button_icons();
    void release_paste_button_icons();
    bool handle_button_click(int x, int y);
    void update_hover(int x, int y);
    int hit_test_paste_button(int x, int y) const;

    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    OverlayState state_ = OverlayState::Hidden;
    std::string text_;
    std::string paste_output_mode_ = "clean";
    std::function<void(const std::string&)> paste_mode_callback_;
    float audio_level_ = 0.0f;
    float progress_ = 0.0f;
    std::vector<float> audio_levels_;
    std::array<RECT, 3> paste_button_rects_{};
    std::array<Gdiplus::Bitmap*, 3> paste_button_icons_{};
    int hover_button_ = -1;
};

} // namespace vtt
