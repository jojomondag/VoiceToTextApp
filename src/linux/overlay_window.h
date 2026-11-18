#pragma once
#include <functional>
#include <memory>
#include <string>

#include "win_compat.h"

namespace vtt {

enum class OverlayState {
    Hidden,
    Listening,
    Processing,
    Downloading,
    Error
};

// Floating status pill at the top of the screen. While listening it carries
// the three live-mode buttons (Rösttext, AI-språk, Städa text) with hover
// explanations. On Wayland it is a layer-shell surface that never takes
// keyboard focus, and only the buttons accept the pointer, so dictation keeps
// going into the window behind it. Every method may be called from any thread.
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
    // A start pressed while the previous dictation finishes: a line under the
    // Processing pill. It goes away with hide() or any other state, so
    // queued_start_shown() tells whether it has stayed on screen since it was
    // set.
    void set_queued_start(bool queued);
    bool queued_start_shown() const;
    // Called on the UI thread with "recorded", "ai" or "clean".
    void set_paste_mode_callback(std::function<void(const std::string&)> callback);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace vtt
