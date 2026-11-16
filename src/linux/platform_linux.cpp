// Linux implementation of the services declared in platform.h.
#include "platform.h"
#include "log.h"

#include <QApplication>
#include <QDesktopServices>
#include <QMetaObject>
#include <QThread>
#include <QUrl>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace vtt {
namespace platform {
namespace {

std::filesystem::path home_directory() {
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path("/tmp");
}

// $XDG_xxx_HOME when it is set to an absolute path, else ~/<fallback>.
std::filesystem::path xdg_directory(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    if (value && value[0] == '/') return std::filesystem::path(value);
    return home_directory() / fallback;
}

void ensure_curl() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_to_file(char* data, size_t size, size_t count, void* user) {
    return fwrite(data, size, count, static_cast<FILE*>(user));
}

size_t write_to_string(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

int report_progress(void* user, curl_off_t total, curl_off_t now,
                    curl_off_t, curl_off_t) {
    auto* on_progress = static_cast<const std::function<void(float)>*>(user);
    if (total > 0 && *on_progress) {
        (*on_progress)(static_cast<float>(now) / static_cast<float>(total));
    }
    return 0;
}

} // namespace

std::filesystem::path exe_path() {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path("VoiceToText") : path;
}

std::filesystem::path exe_directory() {
    return exe_path().parent_path();
}

std::filesystem::path default_config_path() {
    return xdg_directory("XDG_CONFIG_HOME", ".config") / "VoiceToText" / "config.json";
}

std::filesystem::path data_directory(const std::filesystem::path& /*config_path*/) {
    return xdg_directory("XDG_DATA_HOME", ".local/share") / "VoiceToText";
}

std::tm local_time(std::time_t time) {
    std::tm tm_buf{};
    localtime_r(&time, &tm_buf);
    return tm_buf;
}

void sleep_ms(int milliseconds) {
    if (milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
}

// XDG autostart entry: ~/.config/autostart/voicetotext.desktop
void sync_auto_start(bool enabled) {
    std::filesystem::path entry =
        xdg_directory("XDG_CONFIG_HOME", ".config") / "autostart" / "voicetotext.desktop";
    std::error_code ec;

    if (!enabled) {
        std::filesystem::remove(entry, ec);
        return;
    }

    std::filesystem::create_directories(entry.parent_path(), ec);
    std::ofstream file(entry);
    if (!file.is_open()) return;
    file << "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=VoiceToText\n"
            "Comment=Local speech-to-text dictation\n"
            "Exec=\"" << exe_path().string() << "\"\n"
            "Icon=voicetotext\n"
            "Terminal=false\n";
}

bool download_file(const std::string& url,
                   const std::filesystem::path& destination,
                   const std::function<void(float)>& on_progress) {
    ensure_curl();

    FILE* file = fopen(destination.c_str(), "wb");
    if (!file) return false;

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VoiceToText/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    // Give up on a stalled transfer (under 1 kB/s for a minute).
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    if (on_progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, report_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &on_progress);
    }

    CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    bool closed = fclose(file) == 0;

    if (result != CURLE_OK) {
        vtlog("download: %s failed: %s", url.c_str(), curl_easy_strerror(result));
        return false;
    }
    return closed;
}

bool http_post_json(const std::string& url, const std::string& body,
                    int timeout_ms, std::string& response,
                    const std::string& bearer_token) {
    response.clear();
    ensure_curl();

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_slist* headers = curl_slist_append(
        nullptr, "Content-Type: application/json; charset=utf-8");
    if (!bearer_token.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer_token).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VoiceToText/1.0");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    if (timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result == CURLE_OK && !response.empty();
}

bool http_get_ok(const std::string& url, int timeout_ms) {
    ensure_curl();

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string ignored;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ignored);

    CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

long http_get(const std::string& url, int timeout_ms, std::string& response,
              const std::string& bearer_token) {
    response.clear();
    ensure_curl();

    CURL* curl = curl_easy_init();
    if (!curl) return 0;

    curl_slist* headers = nullptr;
    if (!bearer_token.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer_token).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VoiceToText/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

std::string find_program(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path || name.empty()) return {};

    std::string directories = path;
    size_t begin = 0;
    while (begin <= directories.size()) {
        size_t end = directories.find(':', begin);
        if (end == std::string::npos) end = directories.size();
        if (end > begin) {
            std::filesystem::path candidate =
                std::filesystem::path(directories.substr(begin, end - begin)) / name;
            if (access(candidate.c_str(), X_OK) == 0) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
            }
        }
        begin = end + 1;
    }
    return {};
}

bool install_llama_server(const std::filesystem::path& install_dir, bool gpu,
                          std::string& error) {
    error.clear();
    ensure_curl();

    // GitHub API GET; empty on failure.
    const auto fetch = [](const std::string& address) {
        std::string body;
        CURL* curl = curl_easy_init();
        if (!curl) return body;
        curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
        curl_easy_setopt(curl, CURLOPT_URL, address.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VoiceToText/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        if (curl_easy_perform(curl) != CURLE_OK) body.clear();
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return body;
    };

    std::string url;
    std::string asset_name;
    // Picks the Linux x64 archive of a release; `pointer` receives the link
    // to nightly-tag.txt when the release only names the build to use.
    const auto pick = [&](const std::string& listing, std::string* pointer) {
        try {
            const auto release = nlohmann::json::parse(listing);
            for (const bool vulkan : {gpu, false}) {
                for (const auto& asset : release.at("assets")) {
                    const std::string name = asset.value("name", std::string());
                    if (pointer && name == "nightly-tag.txt") {
                        *pointer = asset.value("browser_download_url", std::string());
                    }
                    const bool is_archive = name.size() > 4 &&
                        (name.find(".tar.gz") != std::string::npos ||
                         name.compare(name.size() - 4, 4, ".zip") == 0);
                    if (!is_archive || name.find("ubuntu") == std::string::npos ||
                        name.find("x64") == std::string::npos) continue;
                    const bool has_vulkan = name.find("vulkan") != std::string::npos;
                    const bool other_backend = name.find("cuda") != std::string::npos ||
                                               name.find("rocm") != std::string::npos ||
                                               name.find("sycl") != std::string::npos ||
                                               name.find("openvino") != std::string::npos;
                    if (other_backend || has_vulkan != vulkan) continue;
                    url = asset.value("browser_download_url", std::string());
                    asset_name = name;
                    return true;
                }
                if (!gpu) break;
            }
        } catch (const nlohmann::json::exception&) {
        }
        return false;
    };

    // The newest release either carries the builds or points at the build
    // tag that does.
    const std::string api = "https://api.github.com/repos/ggml-org/llama.cpp/releases/";
    std::string pointer;
    if (!pick(fetch(api + "latest"), &pointer) && !pointer.empty()) {
        std::string tag = fetch(pointer);
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back()))) tag.pop_back();
        const bool plain = !tag.empty() && tag.size() < 64 &&
            std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '.' || c == '-' || c == '_';
            });
        if (plain) pick(fetch(api + "tags/" + tag), nullptr);
    }
    if (url.empty()) {
        error = "no Linux x64 llama.cpp build found in the latest release";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(install_dir, ec);
    std::filesystem::path archive = install_dir / asset_name;
    vtlog("llama-server: downloading %s", url.c_str());
    if (!download_file(url, archive)) {
        std::filesystem::remove(archive, ec);
        error = "download of " + asset_name + " failed";
        return false;
    }

    bool zip = asset_name.compare(asset_name.size() - 4, 4, ".zip") == 0;
    std::vector<std::string> command = zip
        ? std::vector<std::string>{"unzip", "-o", "-q", archive.string(), "-d", install_dir.string()}
        : std::vector<std::string>{"tar", "-xzf", archive.string(), "-C", install_dir.string()};
    std::vector<char*> argv;
    for (auto& argument : command) argv.push_back(argument.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    int status = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0 ||
        waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "could not unpack " + asset_name;
        return false;
    }
    std::filesystem::remove(archive, ec);
    return true;
}

void open_in_default_app(const std::string& path) {
    post_to_main_thread([path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
    });
}

int run_main_loop() {
    return QApplication::exec();
}

void post_to_main_thread(std::function<void()> task) {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return;
    QMetaObject::invokeMethod(app, std::move(task), Qt::QueuedConnection);
}

} // namespace platform
} // namespace vtt
