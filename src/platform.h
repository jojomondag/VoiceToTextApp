#pragma once
// Operating-system services for the non-Windows build, implemented in
// src/linux/platform_linux.cpp. (The Windows build keeps its Win32 calls
// where they are.)

#include <ctime>
#include <filesystem>
#include <functional>
#include <string>

namespace vtt {
namespace platform {

// Full path of the running executable, and its directory.
std::filesystem::path exe_path();
std::filesystem::path exe_directory();

// $XDG_CONFIG_HOME/VoiceToText/config.json (~/.config/VoiceToText).
std::filesystem::path default_config_path();

// Base directory for models, tools, transcript history, learned corrections,
// diagnostics and the log: $XDG_DATA_HOME/VoiceToText
// (~/.local/share/VoiceToText). Relative paths from config.json are resolved
// against it.
std::filesystem::path data_directory(const std::filesystem::path& config_path);

std::tm local_time(std::time_t time);
void sleep_ms(int milliseconds);

// Start (or stop starting) the app when the user logs in.
void sync_auto_start(bool enabled);

// Blocking download to `destination`. `on_progress` receives 0.0 - 1.0.
bool download_file(const std::string& url,
                   const std::filesystem::path& destination,
                   const std::function<void(float)>& on_progress = {});

// Blocking JSON POST. Returns true on a 2xx answer with a non-empty body.
// `bearer_token`, when not empty, is sent as the Authorization header.
bool http_post_json(const std::string& url, const std::string& body,
                    int timeout_ms, std::string& response,
                    const std::string& bearer_token = {});

// True when a GET answers 2xx within the timeout.
bool http_get_ok(const std::string& url, int timeout_ms);

// Full path of a program on PATH, empty when it is not there.
std::string find_program(const std::string& name);

// Downloads the latest official llama.cpp build for Linux x64 (Vulkan when
// `gpu`, else CPU) and unpacks it into `install_dir`.
bool install_llama_server(const std::filesystem::path& install_dir, bool gpu,
                          std::string& error);

// Open a file with the application registered for it.
void open_in_default_app(const std::string& path);

// The UI event loop.
int run_main_loop();
void post_to_main_thread(std::function<void()> task);

} // namespace platform
} // namespace vtt
