#include "gemma_runtime.h"

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

std::filesystem::path exe_directory() {
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).parent_path();
}

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

} // namespace

const std::vector<GemmaModelInfo>& gemma_models() {
    static const std::vector<GemmaModelInfo> models = {
        {
            "e2b",
            "Gemma 4 E2B - recommended",
            "ggml-org/gemma-4-E2B-it-GGUF",
            "Q8_0",
            "gemma-4-E2B-it-Q8_0.gguf",
            "models/llm/gemma-4-E2B-it-Q8_0.gguf",
            "Best default for dictation cleanup and profile transforms.",
            4967494592ull,
            8,
            16,
        },
        {
            "e4b",
            "Gemma 4 E4B - higher quality",
            "ggml-org/gemma-4-E4B-it-GGUF",
            "Q4_K_M",
            "gemma-4-E4B-it-Q4_K_M.gguf",
            "models/llm/gemma-4-E4B-it-Q4_K_M.gguf",
            "Better quality when you can afford a little more latency.",
            5335289824ull,
            12,
            24,
        },
        {
            "12b",
            "Gemma 4 12B - advanced",
            "ggml-org/gemma-4-12B-it-GGUF",
            "Q4_K_M",
            "gemma-4-12B-it-Q4_K_M.gguf",
            "models/llm/gemma-4-12B-it-Q4_K_M.gguf",
            "Useful only for heavier local reasoning workflows.",
            0,
            24,
            32,
        },
        {
            "26b-a4b",
            "Gemma 4 26B A4B - high end",
            "ggml-org/gemma-4-26B-A4B-it-GGUF",
            "Q4_K_M",
            "gemma-4-26B-A4B-it-Q4_K_M.gguf",
            "models/llm/gemma-4-26B-A4B-it-Q4_K_M.gguf",
            "High-end GPU/workstation option, not needed for normal dictation.",
            0,
            32,
            48,
        },
        {
            "31b",
            "Gemma 4 31B - workstation",
            "ggml-org/gemma-4-31B-it-GGUF",
            "Q4_K_M",
            "gemma-4-31B-it-Q4_K_M.gguf",
            "models/llm/gemma-4-31B-it-Q4_K_M.gguf",
            "Overkill for cleanup; keep for experimentation only.",
            0,
            48,
            64,
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

std::string gemma_model_path_for_id(const std::string& id) {
    const GemmaModelInfo* model = find_gemma_model(id);
    return model ? model->local_path : recommended_gemma_model().local_path;
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
    port_ = port;
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
    port_ = 0;
}

bool GemmaRuntime::running() const {
    if (!process_.hProcess) return false;
    DWORD exit_code = 0;
    return GetExitCodeProcess(process_.hProcess, &exit_code) &&
           exit_code == STILL_ACTIVE;
}

int GemmaRuntime::port() const {
    return port_;
}

} // namespace vtt
