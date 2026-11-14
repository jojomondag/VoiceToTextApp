#pragma once

#include <windows.h>

#include <memory>
#include <string>

namespace vtt {

// A single-worker clipboard lease. Keep it alive until the target has consumed
// the paste; restoring immediately after SendInput can race the queued paste.
// Unsupported clipboard contents cause publish() to fail without changing them.
class ClipboardTextTransaction {
public:
    struct FailureInfo {
        UINT format = 0;
        SIZE_T bytes = 0;
        UINT global_flags = GMEM_INVALID_HANDLE;
        DWORD data_error = 0;
        DWORD size_error = 0;
        DWORD flags_error = 0;
        const char* stage = "none";
    };
    ClipboardTextTransaction();
    ~ClipboardTextTransaction();
    ClipboardTextTransaction(const ClipboardTextTransaction&) = delete;
    ClipboardTextTransaction& operator=(const ClipboardTextTransaction&) = delete;

    bool publish(HWND owner, const std::wstring& text);
    bool owns_clipboard() const;
    bool restore();
    // Final shutdown only, when a dispatched paste is still unconfirmed. Leave
    // its payload available so delayed Ctrl+V cannot paste the previous value.
    // This deliberately forfeits restoration of the old clipboard contents.
    void abandon_without_restore() noexcept;
    const char* status() const;
    UINT failure_format() const noexcept;
    const char* failure_reason() const noexcept;
    FailureInfo failure_info() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtt
