#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace vtt {

// Local, bounded diagnostics. Callers must supply only dictation-owned text and
// cursor/operation metadata, never surrounding document text or window titles.
class DictationDiagnostics {
public:
    bool configure(const std::string& path, bool enabled = true) noexcept;
    void write(const std::string& event, const nlohmann::json& fields) noexcept;
    std::string path() const;

private:
    bool open_locked();
    bool rotate_locked();

    mutable std::mutex mutex_;
    std::filesystem::path file_path_;
    std::ofstream stream_;
    std::string session_id_;
    std::uintmax_t bytes_written_ = 0;
    std::uint64_t sequence_ = 0;
    bool enabled_ = false;
};

} // namespace vtt
