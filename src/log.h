#pragma once

namespace vtt {

// Diagnostic logging: appends to voicetotext.log in the data directory (next
// to the executable on Windows) so the recording/transcription flow can be
// followed even when the app runs detached from a console. Thread-safe.
void vtlog(const char* fmt, ...);

} // namespace vtt
