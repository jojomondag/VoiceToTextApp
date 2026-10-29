#include "clipboard_test_fixture.h"


// Exercise the actual transaction with local memory and a private fake
// clipboard. No test reads, changes or empties the user's Windows clipboard.
#define OpenClipboard clipboard_fixture::open
#define CloseClipboard clipboard_fixture::close
#define EmptyClipboard clipboard_fixture::empty
#define EnumClipboardFormats clipboard_fixture::enumerate
#define GetClipboardData clipboard_fixture::get
#define SetClipboardData clipboard_fixture::set
#define GetClipboardSequenceNumber clipboard_fixture::current_sequence
#define GetClipboardOwner clipboard_fixture::current_owner
#define GlobalSize clipboard_fixture::memory_size
#define GlobalFlags clipboard_fixture::memory_flags
#include "../src/clipboard_text_transaction.cpp"
#undef OpenClipboard
#undef CloseClipboard
#undef EmptyClipboard
#undef EnumClipboardFormats
#undef GetClipboardData
#undef SetClipboardData
#undef GetClipboardSequenceNumber
#undef GetClipboardOwner
#undef GlobalSize
#undef GlobalFlags

int main() {
    try {
        int passed = 0;
        const auto run = [&](const char* name, const auto& test) {
            clipboard_fixture::reset();
            test();
            ++passed;
            std::cout << "PASS: " << name << '\n';
        };
        run("multiline Unicode payload restores every original clipboard format byte for byte", [&] {
            clipboard_fixture::seed_text(L"Original user text");
            const char html[] = "<b>Original user text</b>";
            const unsigned char binary[] = {0, 0xff, 0x04, 0, 0xa5};
            clipboard_fixture::seed(clipboard_fixture::html, html, sizeof(html));
            clipboard_fixture::seed(clipboard_fixture::binary, binary, sizeof(binary));
            const auto original = clipboard_fixture::snapshot();
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner,
                L"Mål: Bygg ett spel.\r\n\r\nKrav:\r\n  - 14 karaktärer."), "publish failed");
            clipboard_fixture::require(transaction.owns_clipboard() && clipboard_fixture::formats.size() == 1 &&
                clipboard_fixture::text() == L"Mål: Bygg ett spel.\r\n\r\nKrav:\r\n  - 14 karaktärer.",
                "published payload lost formatting or mixed clipboard formats");
            clipboard_fixture::require(transaction.restore() && !transaction.owns_clipboard() &&
                clipboard_fixture::snapshot() == original, "original clipboard formats were not restored exactly");
        });
        run("destruction restores clipboard after an acknowledged paste", [&] {
            clipboard_fixture::seed_text(L"Keep this clipboard value");
            const auto original = clipboard_fixture::snapshot();
            { vtt::ClipboardTextTransaction transaction;
              clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"A\n  - B"), "publish failed"); }
            clipboard_fixture::require(clipboard_fixture::snapshot() == original, "destructor lost original clipboard");
        });
        run("a newer user copy is preserved rather than overwritten by restoration", [&] {
            clipboard_fixture::seed_text(L"Original");
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"AI prompt"), "publish failed");
            clipboard_fixture::clear();
            clipboard_fixture::seed_text(L"New user copy");
            const auto newer = clipboard_fixture::snapshot();
            clipboard_fixture::require(!transaction.owns_clipboard() && transaction.restore() &&
                clipboard_fixture::snapshot() == newer, "restoration overwrote a newer clipboard value");
        });
        run("owner-managed clipboard formats abort before any mutation", [&] {
            for (UINT format : {CF_PRIVATEFIRST, CF_OWNERDISPLAY, CF_GDIOBJFIRST, CF_GDIOBJLAST}) {
                clipboard_fixture::reset();
                clipboard_fixture::seed_text(L"Original");
                const unsigned char opaque[] = {1, 2, 3, 4};
                clipboard_fixture::seed(format, opaque, sizeof(opaque));
                const auto original = clipboard_fixture::snapshot();
                vtt::ClipboardTextTransaction transaction;
                clipboard_fixture::require(!transaction.publish(clipboard_fixture::owner, L"AI prompt") &&
                    clipboard_fixture::empty_calls == 0 && clipboard_fixture::snapshot() == original,
                    "unsupported data was emptied before it could be preserved");
                const auto failure = transaction.failure_info();
                clipboard_fixture::require(transaction.failure_format() == format && failure.format == format &&
                    std::string(transaction.failure_reason()) == "owner_managed_format" &&
                    std::string(failure.stage) == "owner_managed_format",
                    "unsupported-format diagnostics did not identify the exact format and ownership restriction");
            }
        });
        run("failed payload publication rolls back all clipboard formats", [&] {
            clipboard_fixture::seed_text(L"Original");
            const char html[] = "<p>Original</p>";
            clipboard_fixture::seed(clipboard_fixture::html, html, sizeof(html));
            const auto original = clipboard_fixture::snapshot();
            clipboard_fixture::fail_format = CF_UNICODETEXT;
            clipboard_fixture::failures = 1;
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(!transaction.publish(clipboard_fixture::owner, L"AI prompt") &&
                !transaction.owns_clipboard() && clipboard_fixture::snapshot() == original,
                "failed publication did not roll back the original clipboard");
        });
        run("busy clipboard is unchanged and restoration can be retried", [&] {
            clipboard_fixture::seed_text(L"Original");
            const auto original = clipboard_fixture::snapshot();
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::busy = true;
            clipboard_fixture::require(!transaction.publish(clipboard_fixture::owner, L"AI prompt") &&
                clipboard_fixture::empty_calls == 0 && clipboard_fixture::snapshot() == original,
                "busy clipboard publication mutated existing content");
            clipboard_fixture::busy = false;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"AI prompt"), "retry publish failed");
            clipboard_fixture::busy = true;
            clipboard_fixture::require(!transaction.restore() && transaction.owns_clipboard(),
                "busy restore discarded original backup");
            clipboard_fixture::busy = false;
            clipboard_fixture::require(transaction.restore() && clipboard_fixture::snapshot() == original,
                "restoration retry did not recover the complete original clipboard");
        });
        run("partial format restoration retains a complete backup for retry", [&] {
            clipboard_fixture::seed_text(L"Original");
            const char html[] = "<p>Original</p>";
            clipboard_fixture::seed(clipboard_fixture::html, html, sizeof(html));
            const auto original = clipboard_fixture::snapshot();
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"AI prompt"), "publish failed");
            clipboard_fixture::fail_format = clipboard_fixture::html;
            clipboard_fixture::failures = 1;
            clipboard_fixture::require(!transaction.restore() && transaction.owns_clipboard(),
                "partial restoration incorrectly reported complete or lost backup ownership");
            clipboard_fixture::require(transaction.restore() && clipboard_fixture::snapshot() == original,
                "partial restoration retry lost an original format");
        });
        run("an originally empty clipboard returns to empty", [&] {
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"AI prompt") && transaction.restore() &&
                clipboard_fixture::formats.empty(), "empty original clipboard was not restored");
        });
        run("unconfirmed shutdown preserves the queued paste payload instead of restoring unrelated text", [&] {
            clipboard_fixture::seed_text(L"Unrelated original clipboard");
            {
                vtt::ClipboardTextTransaction transaction;
                clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"Pending AI prompt\n  - Requirement"),
                    "publish failed");
                transaction.abandon_without_restore();
                clipboard_fixture::require(!transaction.owns_clipboard(), "abandoned transaction retained active ownership");
            }
            clipboard_fixture::require(clipboard_fixture::text() == L"Pending AI prompt\n  - Requirement",
                "shutdown changed the payload before a queued paste could consume it");
        });
        run("publication retains ownership when Windows advances the sequence while closing", [&] {
            clipboard_fixture::seed_text(L"Original");
            const auto original = clipboard_fixture::snapshot();
            clipboard_fixture::close_advances_sequence = true;
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"Formatted\r\n    - Requirement") &&
                transaction.owns_clipboard(), "post-close sequence invalidated the transaction's own payload");
            clipboard_fixture::require(transaction.restore() && clipboard_fixture::snapshot() == original,
                "post-close sequence caused original clipboard restoration to be skipped");
        });
        run("a partial restore remains retryable when close advances the sequence", [&] {
            clipboard_fixture::seed_text(L"Original");
            const char html[] = "<p>Original</p>";
            clipboard_fixture::seed(clipboard_fixture::html, html, sizeof(html));
            const auto original = clipboard_fixture::snapshot();
            clipboard_fixture::close_advances_sequence = true;
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner, L"Formatted\r\n    - Requirement"), "publish failed");
            clipboard_fixture::fail_format = clipboard_fixture::html;
            clipboard_fixture::failures = 1;
            clipboard_fixture::require(!transaction.restore() && transaction.owns_clipboard(),
                "post-close sequence discarded a partially restored clipboard backup");
            clipboard_fixture::require(transaction.restore() && clipboard_fixture::snapshot() == original,
                "post-close restoration retry failed to recover every original format");
        });
        run("foreign clipboard content arriving during close is never adopted as our payload", [&] {
            clipboard_fixture::seed_text(L"Original");
            clipboard_fixture::close_advances_sequence = true;
            clipboard_fixture::foreign_copy_on_close = true;
            vtt::ClipboardTextTransaction transaction;
            const bool published = transaction.publish(clipboard_fixture::owner, L"Formatted\r\n    - Requirement");
            clipboard_fixture::require(!published && !transaction.owns_clipboard() &&
                clipboard_fixture::text() == L"Concurrent new user copy",
                "transaction adopted another owner's clipboard after closing");
            clipboard_fixture::require(transaction.restore() && clipboard_fixture::text() == L"Concurrent new user copy",
                "restoration overwrote a clipboard copy made during close");
        });
        run("registered zero-byte marker formats survive a formatted paste and restoration", [&] {
            clipboard_fixture::seed_text(L"User text with a registered marker");
            clipboard_fixture::seed_empty_marker(clipboard_fixture::binary);
            const auto original = clipboard_fixture::snapshot();
            const auto marker_flags = GlobalFlags(clipboard_fixture::formats.at(clipboard_fixture::binary));
            clipboard_fixture::require((marker_flags & GMEM_DISCARDED) != 0,
                "fixture marker does not reproduce the valid discarded zero-byte HGLOBAL");
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(transaction.publish(clipboard_fixture::owner,
                L"1. **Goal:** Implement it.\r\n    - Preserve requirements.") && transaction.owns_clipboard(),
                "valid registered empty marker prevented a formatted paste");
            clipboard_fixture::require(transaction.restore() && clipboard_fixture::snapshot() == original &&
                GlobalSize(clipboard_fixture::formats.at(clipboard_fixture::binary)) == 0 &&
                GlobalFlags(clipboard_fixture::formats.at(clipboard_fixture::binary)) != GMEM_INVALID_HANDLE,
                "restoration lost the empty marker or substituted invalid memory");
        });
        run("missing clipboard data remains protected and identifies the failing format", [&] {
            clipboard_fixture::seed_text(L"Original");
            clipboard_fixture::formats[clipboard_fixture::binary] = nullptr;
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(!transaction.publish(clipboard_fixture::owner, L"AI prompt") &&
                clipboard_fixture::empty_calls == 0 && clipboard_fixture::text() == L"Original",
                "missing clipboard data was discarded instead of preserving the original clipboard");
            const auto failure = transaction.failure_info();
            clipboard_fixture::require(failure.format == clipboard_fixture::binary &&
                failure.data_error == ERROR_ACCESS_DENIED &&
                std::string(failure.stage) == "format_data_unavailable",
                "missing-data diagnostics lost its format, stage or Windows error");
        });
        run("an invalid zero-size handle cannot masquerade as an empty registered marker", [&] {
            clipboard_fixture::seed_text(L"Original");
            clipboard_fixture::formats[clipboard_fixture::binary] = clipboard_fixture::invalid_memory;
            vtt::ClipboardTextTransaction transaction;
            clipboard_fixture::require(!transaction.publish(clipboard_fixture::owner, L"AI prompt") &&
                clipboard_fixture::empty_calls == 0 && clipboard_fixture::text() == L"Original",
                "invalid clipboard memory was accepted as a valid empty marker");
            const auto failure = transaction.failure_info();
            clipboard_fixture::require(failure.format == clipboard_fixture::binary &&
                failure.global_flags == GMEM_INVALID_HANDLE &&
                std::string(failure.stage) == "invalid_memory_handle",
                "invalid memory diagnostics did not distinguish it from a valid empty marker");
            clipboard_fixture::formats.erase(clipboard_fixture::binary);
        });
        clipboard_fixture::reset();
        std::cout << passed << " clipboard transaction checks passed; real clipboard never accessed.\n";
        return 0;
    } catch (const std::exception& error) {
        clipboard_fixture::reset();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
