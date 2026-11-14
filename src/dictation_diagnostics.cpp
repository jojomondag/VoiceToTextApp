#include "dictation_diagnostics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace vtt {
namespace {

constexpr std::uintmax_t kMaxFileBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxStringBytes = 4096;
constexpr std::size_t kMaxRecordBytes = 64 * 1024;
constexpr std::size_t kMaxContainerItems = 64;
constexpr unsigned kMaxDepth = 6;

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return result.str();
}

std::string new_session_id() {
    static std::atomic<std::uint64_t> next_id{0};
    std::ostringstream result;
    result << std::hex << std::chrono::system_clock::now().time_since_epoch().count()
           << '-' << ++next_id;
    return result.str();
}

// Keep complete UTF-8 code points, replacing malformed input with U+FFFD. The
// cap applies to output bytes, including replacement characters.
std::string safe_utf8(const std::string& input, std::size_t limit, bool& changed) {
    std::string result;
    result.reserve(std::min(input.size(), limit));
    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<unsigned char>(input[i]);
        std::size_t width = 1;
        bool valid = true;
        if (first >= 0xC2 && first <= 0xDF) width = 2;
        else if (first >= 0xE0 && first <= 0xEF) width = 3;
        else if (first >= 0xF0 && first <= 0xF4) width = 4;
        else if (first >= 0x80) valid = false;

        if (valid && i + width > input.size()) valid = false;
        for (std::size_t j = 1; valid && j < width; ++j) {
            const auto continuation = static_cast<unsigned char>(input[i + j]);
            if (continuation < 0x80 || continuation > 0xBF) valid = false;
        }
        if (valid && width >= 3) {
            const auto second = static_cast<unsigned char>(input[i + 1]);
            if ((first == 0xE0 && second < 0xA0) ||
                (first == 0xED && second > 0x9F) ||
                (first == 0xF0 && second < 0x90) ||
                (first == 0xF4 && second > 0x8F)) valid = false;
        }

        const std::size_t output_width = valid ? width : 3;
        if (result.size() + output_width > limit) {
            changed = true;
            break;
        }
        if (valid) result.append(input, i, width);
        else {
            result.append("\xEF\xBF\xBD", 3);
            changed = true;
        }
        i += valid ? width : 1;
    }
    return result;
}

struct Sanitizer {
    std::size_t remaining_nodes = 256;
    std::size_t remaining_text_bytes = 8192;
    bool changed = false;

    std::string text(const std::string& input, std::size_t cap = kMaxStringBytes) {
        auto result = safe_utf8(input, std::min(cap, remaining_text_bytes), changed);
        remaining_text_bytes -= result.size();
        return result;
    }

    nlohmann::json value(const nlohmann::json& input, unsigned depth = 0) {
        if (remaining_nodes == 0) {
            changed = true;
            return "[truncated]";
        }
        --remaining_nodes;
        if (input.is_string()) return text(input.get_ref<const std::string&>());
        if (input.is_object() || input.is_array()) {
            if (depth >= kMaxDepth) {
                changed = true;
                return "[depth limit]";
            }
            nlohmann::json output = input.is_object()
                ? nlohmann::json::object() : nlohmann::json::array();
            std::size_t count = 0;
            for (auto item = input.begin(); item != input.end(); ++item) {
                if (count == kMaxContainerItems || remaining_nodes == 0 ||
                    remaining_text_bytes == 0) {
                    changed = true;
                    break;
                }
                if (input.is_object()) {
                    const auto key = text(item.key(), 128);
                    output[key] = value(item.value(), depth + 1);
                } else output.push_back(value(item.value(), depth + 1));
                ++count;
            }
            return output;
        }
        if (input.is_binary() || input.is_discarded()) {
            changed = true;
            return "[unsupported value]";
        }
        return input;
    }
};

} // namespace

bool DictationDiagnostics::configure(const std::string& path, bool enabled) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_.close();
        stream_.clear();
        enabled_ = false;
        file_path_ = std::filesystem::u8path(path);
        bytes_written_ = 0;
        sequence_ = 0;
        session_id_ = new_session_id();
        if (!enabled) return true;
        if (file_path_.empty()) return false;
        enabled_ = true;
        return open_locked();
    } catch (...) {
        return false;
    }
}

std::string DictationDiagnostics::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_path_.u8string();
}

bool DictationDiagnostics::open_locked() {
    std::error_code error;
    const auto parent = file_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }
    const bool exists = std::filesystem::exists(file_path_, error);
    if (error) return false;
    bytes_written_ = exists ? std::filesystem::file_size(file_path_, error) : 0;
    if (error) return false;
    // An old/external oversized file must not break the documented bound. New
    // files created here never exceed this limit in the first place.
    const auto mode = bytes_written_ > kMaxFileBytes ? std::ios::trunc : std::ios::app;
    stream_.clear();
    stream_.open(file_path_, std::ios::binary | std::ios::out | mode);
    if (!stream_.is_open()) return false;
    if (mode == std::ios::trunc) bytes_written_ = 0;
    return true;
}

bool DictationDiagnostics::rotate_locked() {
    stream_.close();
    stream_.clear();
    auto previous = file_path_;
    previous.replace_filename(std::filesystem::u8path(
        file_path_.stem().u8string() + ".previous" + file_path_.extension().u8string()));
    std::error_code error;
    std::filesystem::remove(previous, error);
    if (error) return false;
    const bool exists = std::filesystem::exists(file_path_, error);
    if (error) return false;
    if (exists) {
        std::filesystem::rename(file_path_, previous, error);
        if (error) return false;
    }
    bytes_written_ = 0;
    return open_locked();
}

void DictationDiagnostics::write(const std::string& event,
                                 const nlohmann::json& fields) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        Sanitizer sanitizer;
        bool event_changed = false;
        nlohmann::json record = {
            {"timestamp_utc", utc_timestamp()},
            {"session_id", session_id_},
            {"sequence", ++sequence_},
            {"event", safe_utf8(event, 128, event_changed)},
            {"fields", sanitizer.value(fields)}
        };
        if (sanitizer.changed || event_changed) record["payload_sanitized"] = true;
        auto line = record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (line.size() + 1 > kMaxRecordBytes) {
            record["fields"] = {{"omitted", "record size limit"}};
            record["payload_sanitized"] = true;
            line = record.dump();
        }
        line.push_back('\n');
        if (!stream_.is_open() && !open_locked()) return;
        if (bytes_written_ + line.size() > kMaxFileBytes && !rotate_locked()) return;
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        stream_.flush();
        if (!stream_) {
            stream_.close();
            stream_.clear();
            return;
        }
        bytes_written_ += line.size();
    } catch (...) {
        // Diagnostics are best-effort and must never interrupt dictation.
    }
}

} // namespace vtt
