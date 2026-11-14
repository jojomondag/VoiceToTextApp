#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <windows.h>

namespace vtt {

class DictationDiagnostics;

// Dictation follows the external application's focused text caret. Calls may
// originate on the app UI thread; UI Automation runs on a dedicated MTA thread.
class ExternalDictationTarget {
public:
    struct DictationRevision {
        std::uint64_t token = 0;
        std::uint64_t target_token = 0;
        std::string raw_text;
        bool can_replace = false;
        bool readable_target = false;
        bool available = false;
        // Keep the capture drain alive while delayed input/selection recovery
        // is still protecting speech that has not been safely delivered.
        bool pending_output = false;
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

    // Only the last contiguous, verified insertion belongs to dictation. A
    // background result may revise its raw prefix while later speech remains
    // intact. Moving the caret, editing, or changing fields revokes ownership.
    DictationRevision revision_context();
    bool apply_revision(std::uint64_t token, const std::string& source_raw_text,
                        const std::string& transformed_text);
    bool apply_revision(std::uint64_t token, const std::string& transformed_text);
    // Opaque editors cannot safely revise existing text. A completed processed
    // chunk can instead be entered once, if its original caret is still valid.
    bool append_processed(std::uint64_t target_token, int utterance_id,
                          const std::string& transformed_text);

    // Only text delivered during this session, never the external document.
    std::string text_utf8() const;
    bool has_delivered() const;
    bool is_available() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtt
