#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "win_compat.h"

namespace vtt {

class DictationDiagnostics;

// Dictation follows the keyboard focus of whatever application the user is in.
//
// Stable words are typed at the caret as they are recognised. The
// words of the current session that were typed into the focused window, with
// nothing in between, form the span dictation "owns"; a language-mode result
// (Städa text, AI-språk, or back to Rösttext) replaces that span in place.
//
// Wayland offers no way to read another application's text or to watch the
// user's input, so ownership is proved at the moment of the replacement
// instead: the span is selected backwards from the caret with Shift+Left and
// the selection the application publishes (the primary selection) must equal
// exactly what was typed. If the user typed, clicked elsewhere, or the field
// does not select that way, the texts differ, nothing is replaced, and the
// written text is kept.
//
// A terminal cannot be asked what is selected. There the span is revised by
// deleting as many characters as dictation typed and typing the new text on
// the same line, which is only right while the user leaves that line alone.
//
// Window focus is tracked through Hyprland; on other compositors words are
// typed but never revised. Calls come from the UI thread; all input is sent
// from one worker thread.
class ExternalDictationTarget {
public:
    struct DictationRevision {
        std::uint64_t token = 0;
        std::uint64_t target_token = 0;
        std::string raw_text;
        bool can_replace = false;
        bool readable_target = false;
        bool available = false;
        // Words or a revision are still being written.
        bool pending_output = false;
        // Words or a language-mode result are held because the window they
        // belong in lacks focus.
        bool waiting_for_focus = false;
    };

    ExternalDictationTarget();
    ~ExternalDictationTarget();
    ExternalDictationTarget(const ExternalDictationTarget&) = delete;
    ExternalDictationTarget& operator=(const ExternalDictationTarget&) = delete;

    bool initialize(HWND owner);
    void set_toggle_hotkey(UINT modifiers, UINT key);
    void set_diagnostics(std::shared_ptr<DictationDiagnostics> diagnostics);
    void begin_session();
    bool update_transcript(int utterance_id, const std::string& text, bool is_final);
    void poll_focus();
    void finish_session();
    void cancel_session();
    // Gives up on words and language-mode results waiting for a window
    // without focus; they are not written. Lasts until the next session.
    void drop_held();

    // The window dictation writes to has keyboard focus right now (asks the
    // compositor; false while no window has been chosen yet).
    bool target_has_focus();
    // Presses Enter, after everything queued before it has been written.
    // With `into_target` only when the window dictation writes to still has
    // focus and its output was not given up (drop_held); otherwise the key
    // goes to whatever has focus.
    void press_return(bool into_target);

    // Only the last contiguous insertion belongs to dictation. A background
    // result may revise its raw prefix while later speech remains intact.
    DictationRevision revision_context();
    bool apply_revision(std::uint64_t token, const std::string& source_raw_text,
                        const std::string& transformed_text);
    bool apply_revision(std::uint64_t token, const std::string& transformed_text);

    // Only text delivered during this session, never the external document.
    std::string text_utf8() const;
    bool has_delivered() const;
    // The speech typed this session, exactly as it was typed; language-mode
    // revisions do not change it. False when nothing was typed, or when
    // words were left untyped (dropped for a window that never got focus
    // back, or a failed write) and `text` is not the whole dictation.
    bool typed_speech(std::string& text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtt
