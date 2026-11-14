#include "gemma_runtime.h"
#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace vtt {
namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

#ifdef _WIN32
int installed_ram_gb() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    unsigned long long gb =
        (status.ullTotalPhys + (1024ull * 1024ull * 1024ull - 1)) /
        (1024ull * 1024ull * 1024ull);
    return static_cast<int>(gb);
}

bool has_nvidia_driver() {
    HMODULE driver = LoadLibraryA("nvcuda.dll");
    if (!driver) return false;
    FreeLibrary(driver);
    return true;
}

std::string quote_arg(const std::string& arg) {
    std::string quoted = "\"";
    for (char c : arg) {
        if (c == '"') quoted += '\\';
        quoted += c;
    }
    quoted += "\"";
    return quoted;
}

#else
int installed_ram_gb() {
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    unsigned long long kilobytes = 0;
    while (meminfo >> key >> kilobytes) {
        if (key == "MemTotal:") {
            return static_cast<int>((kilobytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull));
        }
        meminfo.ignore(256, '\n');
    }
    return 0;
}
bool has_nvidia_driver() {
    std::error_code ec;
    return std::filesystem::exists("/proc/driver/nvidia/version", ec);
}
#endif
} // namespace

const std::vector<GemmaModelInfo>& gemma_models() {
    static const std::vector<GemmaModelInfo> models = {
        {
            "e2b",
            "ggml-org/gemma-4-E2B-it-GGUF",
            "gemma-4-E2B-it-Q8_0.gguf",
            "models/llm/gemma-4-E2B-it-Q8_0.gguf",
            4967497152ull,
        },
        {
            "e4b",
            "ggml-org/gemma-4-E4B-it-GGUF",
            "gemma-4-E4B-it-Q4_0.gguf",
            "models/llm/gemma-4-E4B-it-Q4_0.gguf",
            4590807392ull,
        },
        {
            "12b",
            "ggml-org/gemma-4-12B-it-GGUF",
            "gemma-4-12B-it-Q4_0.gguf",
            "models/llm/gemma-4-12B-it-Q4_0.gguf",
            7219673216ull,
        },
        {
            "26b-a4b",
            "ggml-org/gemma-4-26B-A4B-it-GGUF",
            "gemma-4-26B-A4B-it-Q4_0.gguf",
            "models/llm/gemma-4-26B-A4B-it-Q4_0.gguf",
            14618145824ull,
        },
        {
            "31b",
            "ggml-org/gemma-4-31B-it-GGUF",
            "gemma-4-31B-it-Q4_0.gguf",
            "models/llm/gemma-4-31B-it-Q4_0.gguf",
            17992313088ull,
        },
    };
    return models;
}

const GemmaModelInfo* find_gemma_model(const std::string& id) {
    std::string normalized = normalize_gemma_model_id(id);
    for (const auto& model : gemma_models()) {
        if (model.id == normalized) return &model;
    }
    return nullptr;
}

const GemmaModelInfo& recommended_gemma_model() {
    return gemma_models().front();
}

std::string recommend_gemma_model_for_hardware() {
    int ram_gb = installed_ram_gb();
    bool nvidia = has_nvidia_driver();
    if (nvidia && ram_gb >= 24) return "e4b";
    return "e2b";
}

std::string gemma_download_url_for_id(const std::string& id) {
    const GemmaModelInfo* model = find_gemma_model(id);
    if (!model) model = &recommended_gemma_model();
    return "https://huggingface.co/" + model->repo + "/resolve/main/" +
           model->filename;
}

std::string normalize_gemma_model_id(const std::string& id) {
    std::string value = lower_copy(id);
    if (value == "auto") return recommend_gemma_model_for_hardware();
    if (value == "e2b" || value == "gemma-4-e2b") return "e2b";
    if (value == "e4b" || value == "gemma-4-e4b") return "e4b";
    if (value == "12b" || value == "gemma-4-12b") return "12b";
    if (value == "26b" || value == "26b-a4b" || value == "gemma-4-26b-a4b") {
        return "26b-a4b";
    }
    if (value == "31b" || value == "gemma-4-31b") return "31b";
    return recommended_gemma_model().id;
}

GemmaRuntime::~GemmaRuntime() {
    stop();
}

#ifdef _WIN32
bool GemmaRuntime::start(const std::string& llama_server_path,
                         const std::string& model_path,
                         const std::string& compute_mode,
                         int port,
                         std::string& error) {
    stop();

    if (llama_server_path.empty() ||
        !std::filesystem::exists(llama_server_path)) {
        error = "llama-server.exe was not found.";
        return false;
    }
    if (model_path.empty() || !std::filesystem::exists(model_path)) {
        error = "Gemma model file was not found.";
        return false;
    }

    std::ostringstream command;
    command << quote_arg(llama_server_path)
            << " -m " << quote_arg(model_path)
            << " --host 127.0.0.1"
            << " --port " << port
            << " --ctx-size 4096"
            << " --parallel 1";
    if (compute_mode == "gpu" || compute_mode == "auto") {
        command << " --n-gpu-layers 999";
    } else {
        command << " --n-gpu-layers 0";
    }

    std::string command_string = command.str();
    std::vector<char> mutable_command(command_string.begin(), command_string.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};

    std::filesystem::path working_dir =
        std::filesystem::path(llama_server_path).parent_path();
    std::filesystem::path log_path = working_dir / "llama-server-vtt.log";
    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log_file = CreateFileW(log_path.wstring().c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_file != INVALID_HANDLE_VALUE) {
        SetFilePointer(log_file, 0, nullptr, FILE_END);
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = log_file;
        startup.hStdError = log_file;
    }

    BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr,
                             working_dir.string().c_str(), &startup, &process);
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
    }
    if (!ok) {
        error = "Failed to start llama-server.exe.";
        return false;
    }

    process_ = process;
    return true;
}

void GemmaRuntime::stop() {
    if (process_.hProcess) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_.hProcess, &exit_code) &&
            exit_code == STILL_ACTIVE) {
            TerminateProcess(process_.hProcess, 0);
            WaitForSingleObject(process_.hProcess, 3000);
        }
        CloseHandle(process_.hProcess);
        process_.hProcess = nullptr;
    }
    if (process_.hThread) {
        CloseHandle(process_.hThread);
        process_.hThread = nullptr;
    }
}

bool GemmaRuntime::running() const {
    if (!process_.hProcess) return false;
    DWORD exit_code = 0;
    return GetExitCodeProcess(process_.hProcess, &exit_code) &&
           exit_code == STILL_ACTIVE;
}

#else
bool GemmaRuntime::start(const std::string& llama_server_path,
                         const std::string& model_path,
                         const std::string& compute_mode,
                         int port,
                         std::string& error) {
    stop();
    if (llama_server_path.empty() ||
        !std::filesystem::exists(llama_server_path)) {
        error = "llama-server was not found.";
        return false;
    }
    if (model_path.empty() || !std::filesystem::exists(model_path)) {
        error = "Gemma model file was not found.";
        return false;
    }

    const bool gpu = compute_mode == "gpu" || compute_mode == "auto";
    std::vector<std::string> arguments = {
        llama_server_path, "-m", model_path, "--host", "127.0.0.1",
        "--port", std::to_string(port), "--ctx-size", "4096", "--parallel", "1",
        "--n-gpu-layers", gpu ? "999" : "0",
        // Frees the GPU memory after 5 idle minutes; the next request reloads the model (~1 s).
        "--sleep-idle-seconds", "300",
    };
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    const std::filesystem::path working_dir =
        std::filesystem::path(llama_server_path).parent_path();
    const std::string log_path = (working_dir / "llama-server-vtt.log").string();

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (!working_dir.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, working_dir.c_str());
    }

    pid_t pid = 0;
    const int result = posix_spawn(&pid, llama_server_path.c_str(), &actions, nullptr,
                                   argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        error = "Failed to start llama-server.";
        return false;
    }
    process_ = pid;
    return true;
}
void GemmaRuntime::stop() {
    if (process_ <= 0) return;
    int status = 0;
    if (waitpid(process_, &status, WNOHANG) == 0) {
        kill(process_, SIGTERM);
        for (int waited = 0; waited < 3000; waited += 50) {
            if (waitpid(process_, &status, WNOHANG) != 0) { process_ = 0; return; }
            usleep(50 * 1000);
        }
        kill(process_, SIGKILL);
        waitpid(process_, &status, 0);
    }
    process_ = 0;
}
bool GemmaRuntime::running() const {
    if (process_ <= 0) return false;
    // The child stays a zombie until stop() reaps it; /proc tells them apart.
    std::ifstream stat("/proc/" + std::to_string(process_) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return false;
    const auto close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}
#endif
} // namespace vtt
