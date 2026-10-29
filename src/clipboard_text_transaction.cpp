#include "clipboard_text_transaction.h"

#include <cstring>
#include <utility>
#include <vector>

namespace vtt {
namespace clipboard_detail {

// Refuse an unusually large snapshot rather than retaining unbounded clipboard
// contents on every revision. The caller can retain a readable compact layout.
constexpr SIZE_T kSnapshotLimit = 32 * 1024 * 1024;

void release(UINT format, HANDLE handle) {
    if (!handle) return;
    if (format == CF_BITMAP || format == CF_DSPBITMAP || format == CF_PALETTE) {
        DeleteObject(handle);
    } else if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE) {
        DeleteEnhMetaFile(static_cast<HENHMETAFILE>(handle));
    } else if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT) {
        if (auto* picture = static_cast<METAFILEPICT*>(GlobalLock(handle))) {
            DeleteMetaFile(picture->hMF);
            GlobalUnlock(handle);
        }
        GlobalFree(handle);
    } else {
        GlobalFree(handle);
    }
}

struct Entry {
    UINT format = 0;
    HANDLE handle = nullptr;
    Entry(UINT value, HANDLE data) : format(value), handle(data) {}
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&& other) noexcept : format(other.format), handle(other.handle) {
        other.handle = nullptr;
    }
    ~Entry() { release(format, handle); }
};

const char* copy_failure_reason(UINT format, HANDLE source, SIZE_T budget) {
    if (!source) return "format_data_unavailable";
    if (format == CF_OWNERDISPLAY ||
        (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST) ||
        (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST)) return "owner_managed_format";
    if (format == CF_BITMAP || format == CF_DSPBITMAP || format == CF_PALETTE ||
        format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE ||
        format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT) return "gdi_copy_failed";
    const SIZE_T bytes = GlobalSize(source);
    if (!bytes) return GlobalFlags(source) == GMEM_INVALID_HANDLE
        ? "invalid_memory_handle" : "empty_memory_marker";
    if (bytes > budget) return "snapshot_limit_exceeded";
    return "memory_copy_failed";
}

HANDLE duplicate(UINT format, HANDLE source, SIZE_T& budget) {
    if (!source || format == CF_OWNERDISPLAY ||
        (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST) ||
        (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST)) return nullptr;
    if (format == CF_BITMAP || format == CF_DSPBITMAP) {
        BITMAP bitmap{};
        if (!GetObjectW(source, sizeof(bitmap), &bitmap) || bitmap.bmHeight < 0 ||
            bitmap.bmWidthBytes < 0) return nullptr;
        const auto size = static_cast<unsigned long long>(bitmap.bmWidthBytes) * bitmap.bmHeight;
        if (size > budget) return nullptr;
        budget -= static_cast<SIZE_T>(size);
        return CopyImage(source, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    }
    if (format == CF_PALETTE) {
        const UINT count = GetPaletteEntries(static_cast<HPALETTE>(source), 0, 0, nullptr);
        if (!count || count > 65535) return nullptr;
        const SIZE_T bytes = sizeof(LOGPALETTE) + (count - 1) * sizeof(PALETTEENTRY);
        if (bytes > budget) return nullptr;
        std::vector<unsigned char> storage(bytes);
        auto* palette = reinterpret_cast<LOGPALETTE*>(storage.data());
        palette->palVersion = 0x300;
        palette->palNumEntries = static_cast<WORD>(count);
        if (GetPaletteEntries(static_cast<HPALETTE>(source), 0, count, palette->palPalEntry) != count)
            return nullptr;
        budget -= bytes;
        return CreatePalette(palette);
    }
    if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE) {
        const UINT size = GetEnhMetaFileBits(static_cast<HENHMETAFILE>(source), 0, nullptr);
        if (!size || size > budget) return nullptr;
        budget -= size;
        return CopyEnhMetaFileW(static_cast<HENHMETAFILE>(source), nullptr);
    }
    if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT) {
        if (GlobalSize(source) < sizeof(METAFILEPICT)) return nullptr;
        auto* original = static_cast<METAFILEPICT*>(GlobalLock(source));
        if (!original) return nullptr;
        METAFILEPICT value = *original;
        GlobalUnlock(source);
        const UINT size = GetMetaFileBitsEx(value.hMF, 0, nullptr);
        if (!size || size > budget || sizeof(value) > budget - size) return nullptr;
        value.hMF = CopyMetaFileW(value.hMF, nullptr);
        if (!value.hMF) return nullptr;
        const auto result = GlobalAlloc(GMEM_MOVEABLE, sizeof(value));
        auto* target = result ? GlobalLock(result) : nullptr;
        if (!target) {
            if (result) GlobalFree(result);
            DeleteMetaFile(value.hMF);
            return nullptr;
        }
        std::memcpy(target, &value, sizeof(value));
        GlobalUnlock(result);
        budget -= size + sizeof(value);
        return result;
    }
    const SIZE_T bytes = GlobalSize(source);
    if (!bytes) {
        // Some registered clipboard formats are presence-only markers. A
        // valid zero-byte movable allocation is deliberately not lockable;
        // duplicate its empty handle rather than dropping the marker.
        if (format >= 0xc000 && format <= 0xffff && GlobalFlags(source) != GMEM_INVALID_HANDLE)
            return GlobalAlloc(GMEM_MOVEABLE, 0);
        return nullptr;
    }
    if (bytes > budget) return nullptr;
    const auto original = GlobalLock(source);
    if (!original) return nullptr;
    const auto result = GlobalAlloc(GMEM_MOVEABLE, bytes);
    const auto target = result ? GlobalLock(result) : nullptr;
    if (target) {
        std::memcpy(target, original, bytes);
        GlobalUnlock(result);
    }
    GlobalUnlock(source);
    if (!target) {
        if (result) GlobalFree(result);
        return nullptr;
    }
    budget -= bytes;
    return result;
}

struct ClipboardLock {
    bool opened;
    void* context;
    void (*after_close)(void*);
    explicit ClipboardLock(HWND owner, void* value = nullptr, void (*callback)(void*) = nullptr)
        : opened(OpenClipboard(owner) != FALSE), context(value), after_close(callback) {}
    bool close() {
        if (!opened) return true;
        if (!CloseClipboard()) return false;
        opened = false;
        if (after_close) after_close(context);
        return true;
    }
    ~ClipboardLock() { close(); }
};

} // namespace clipboard_detail

struct ClipboardTextTransaction::Impl {
    HWND owner = nullptr;
    DWORD sequence = 0;
    bool active = false;
    const char* state = "unused";
    UINT failed_format = 0;
    const char* failed_reason = "none";
    ClipboardTextTransaction::FailureInfo failure;
    std::vector<clipboard_detail::Entry> original;

    void remember_failure(UINT format, HANDLE source, SIZE_T available, DWORD data_error = 0) {
        failed_format = format;
        failed_reason = clipboard_detail::copy_failure_reason(format, source, available);
        failure = {};
        failure.format = format;
        failure.stage = failed_reason;
        failure.data_error = data_error;
        // Bitmap/palette/metafile handles are not HGLOBAL allocations. Keep
        // their format/stage diagnostics without querying them as heap memory.
        if (source && std::strcmp(failed_reason, "gdi_copy_failed") != 0) {
            SetLastError(ERROR_SUCCESS);
            failure.bytes = GlobalSize(source);
            failure.size_error = GetLastError();
            SetLastError(ERROR_SUCCESS);
            failure.global_flags = GlobalFlags(source);
            failure.flags_error = GetLastError();
        }
    }

    void closed() {
        if (!active) return;
        // Windows can finalize synthesized formats and advance the sequence
        // when the clipboard closes. Adopt that final value only while our
        // dedicated window still owns it; a user's intervening copy wins.
        const DWORD final_sequence = GetClipboardSequenceNumber();
        if (final_sequence && GetClipboardOwner() == owner &&
            GetClipboardSequenceNumber() == final_sequence) {
            sequence = final_sequence;
            return;
        }
        active = false;
        original.clear();
        state = "newer_clipboard_preserved";
    }

    bool restore_locked() {
        // Keep the original independent copies until the entire restore
        // succeeds, so a partial SetClipboardData failure can be retried.
        std::vector<clipboard_detail::Entry> copies;
        copies.reserve(original.size());
        SIZE_T budget = clipboard_detail::kSnapshotLimit;
        for (const auto& entry : original) {
            const SIZE_T available = budget;
            HANDLE copy = clipboard_detail::duplicate(entry.format, entry.handle, budget);
            if (!copy) {
                remember_failure(entry.format, entry.handle, available);
                state = "restore_copy_failed";
                return false;
            }
            copies.emplace_back(entry.format, copy);
        }
        if (!EmptyClipboard()) { state = "restore_empty_failed"; return false; }
        bool complete = true;
        for (auto& entry : copies) {
            if (SetClipboardData(entry.format, entry.handle)) entry.handle = nullptr;
            else complete = false;
        }
        sequence = GetClipboardSequenceNumber();
        if (!complete) { state = "restore_set_failed"; return false; }
        active = false;
        original.clear();
        state = "restored";
        return true;
    }
};

ClipboardTextTransaction::ClipboardTextTransaction() : impl_(std::make_unique<Impl>()) {}
ClipboardTextTransaction::~ClipboardTextTransaction() { restore(); }

bool ClipboardTextTransaction::publish(HWND owner, const std::wstring& text) {
    auto& value = *impl_;
    value.failed_format = 0;
    value.failed_reason = "none";
    value.failure = {};
    if (value.active || !owner || text.empty() || text.find(L'\0') != std::wstring::npos ||
        text.size() >= clipboard_detail::kSnapshotLimit / sizeof(wchar_t)) {
        value.state = "invalid_publish";
        return false;
    }
    try {
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        clipboard_detail::Entry payload(CF_UNICODETEXT, GlobalAlloc(GMEM_MOVEABLE, bytes));
        auto* target = payload.handle ? GlobalLock(payload.handle) : nullptr;
        if (!target) { value.state = "text_allocation_failed"; return false; }
        std::memcpy(target, text.c_str(), bytes);
        GlobalUnlock(payload.handle);
        clipboard_detail::ClipboardLock lock(owner, &value,
            [](void* context) { static_cast<Impl*>(context)->closed(); });
        if (!lock.opened) { value.state = "clipboard_busy"; return false; }

        value.original.clear();
        SIZE_T budget = clipboard_detail::kSnapshotLimit;
        UINT format = 0;
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            format = EnumClipboardFormats(format);
            if (!format) {
                if (GetLastError() != ERROR_SUCCESS) {
                    value.state = "snapshot_enumeration_failed";
                    value.original.clear();
                    return false;
                }
                break;
            }
            // These owner-managed handles need WM_DESTROYCLIPBOARD support;
            // restoring cloned handles without their owner would leak them.
            if (format == CF_OWNERDISPLAY ||
                (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST) ||
                (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST)) {
                value.failed_format = format;
                value.failed_reason = "owner_managed_format";
                value.failure.format = format;
                value.failure.stage = value.failed_reason;
                value.state = "clipboard_format_not_preservable";
                value.original.clear();
                return false;
            }
            SetLastError(ERROR_SUCCESS);
            const HANDLE source = GetClipboardData(format);
            const DWORD data_error = GetLastError();
            const SIZE_T available = budget;
            HANDLE copy = clipboard_detail::duplicate(format, source, budget);
            if (!copy) {
                value.remember_failure(format, source, available, data_error);
                value.state = "clipboard_format_not_preservable";
                value.original.clear();
                return false;
            }
            clipboard_detail::Entry entry(format, copy);
            value.original.push_back(std::move(entry));
        }
        if (!GetClipboardSequenceNumber()) { value.state = "sequence_unavailable"; return false; }
        if (!EmptyClipboard()) { value.state = "publish_empty_failed"; return false; }
        value.owner = owner;
        value.active = true;
        value.sequence = GetClipboardSequenceNumber();
        if (!SetClipboardData(CF_UNICODETEXT, payload.handle)) {
            value.restore_locked();
            value.state = value.active ? "publish_and_restore_failed" : "publish_failed_restored";
            lock.close();
            return false;
        }
        payload.handle = nullptr; // Clipboard now owns the published memory.
        value.sequence = GetClipboardSequenceNumber();
        if (!lock.close()) { value.state = "publish_close_failed"; return false; }
        if (!value.active) return false;
        value.state = "published";
        return true;
    } catch (...) {
        value.state = "clipboard_allocation_failed";
        restore();
        return false;
    }
}

bool ClipboardTextTransaction::owns_clipboard() const {
    return impl_->active && impl_->sequence != 0 &&
        GetClipboardSequenceNumber() == impl_->sequence &&
        GetClipboardOwner() == impl_->owner &&
        GetClipboardSequenceNumber() == impl_->sequence;
}

bool ClipboardTextTransaction::restore() {
    auto& value = *impl_;
    if (!value.active) return true;
    try {
        clipboard_detail::ClipboardLock lock(value.owner, &value,
            [](void* context) { static_cast<Impl*>(context)->closed(); });
        if (!lock.opened) { value.state = "restore_clipboard_busy"; return false; }
        if (!value.sequence || GetClipboardSequenceNumber() != value.sequence ||
            GetClipboardOwner() != value.owner) {
            // The user (or another app) has copied something since our paste.
            value.active = false;
            value.original.clear();
            value.state = "newer_clipboard_preserved";
            return true;
        }
        const bool restored = value.restore_locked();
        if (!lock.close()) { value.state = "restore_close_failed"; return false; }
        return restored;
    } catch (...) {
        value.state = "restore_allocation_failed";
        return false;
    }
}

void ClipboardTextTransaction::abandon_without_restore() noexcept {
    impl_->active = false;
    impl_->original.clear();
    impl_->state = "unconfirmed_paste_payload_retained";
}

const char* ClipboardTextTransaction::status() const { return impl_->state; }
UINT ClipboardTextTransaction::failure_format() const noexcept { return impl_->failed_format; }
const char* ClipboardTextTransaction::failure_reason() const noexcept { return impl_->failed_reason; }
ClipboardTextTransaction::FailureInfo ClipboardTextTransaction::failure_info() const noexcept {
    return impl_->failure;
}

} // namespace vtt
