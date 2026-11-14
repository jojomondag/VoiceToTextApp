#include "log.h"
#include "platform.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace vtt {
namespace {

std::mutex g_log_mutex;
FILE* g_log_file = nullptr;

FILE* log_file() {
    if (!g_log_file) {
        std::filesystem::path dir = platform::data_directory({});
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string log_path = (dir / "voicetotext.log").string();
        g_log_file = fopen(log_path.c_str(), "a");
    }
    return g_log_file;
}

} // namespace

void vtlog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* f = log_file();
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf = platform::local_time(t);
    fprintf(f, "[%02d:%02d:%02d.%03lld] ",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            static_cast<long long>(ms));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

} // namespace vtt
