#include "app.h"
#include "app_icon.h"
#include <sstream>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <functional>
#include <mutex>
#include <filesystem>
#include <urlmon.h>
#include <vector>
#include <algorithm>

#define WM_TRAYICON (WM_APP + 1)
#define WM_OPEN_HISTORY (WM_APP + 2)
#define WM_HISTORY_UPDATED (WM_APP + 3)

static const wchar_t* APP_CLASS_NAME = L"VoiceToTextMainWindow";

namespace {
// Diagnostic logging — writes to voicetotext.log next to the exe so the user
// can see recording/transcription flow even when launched detached from a
// console. Thread-safe.
std::mutex g_log_mutex;
FILE* g_log_file = nullptr;

FILE* log_file() {
    if (!g_log_file) {
        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::string log_path(exe_path);
        auto pos = log_path.find_last_of("\\/");
        if (pos != std::string::npos) log_path = log_path.substr(0, pos + 1);
        log_path += "voicetotext.log";
        g_log_file = fopen(log_path.c_str(), "a");
    }
    return g_log_file;
}

void vtlog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* f = log_file();
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
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

void sync_auto_start(bool enabled) {
    static const wchar_t* RUN_KEY =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t* VALUE_NAME = L"VoiceToText";

    if (!enabled) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME);
        return;
    }

    wchar_t exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
        return;
    }

    std::wstring command = L"\"";
    command += exe_path;
    command += L"\"";

    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME, REG_SZ,
                    command.c_str(),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
}

bool contains_text(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::filesystem::path exe_directory() {
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).parent_path();
}

void configure_runtime_dll_search_path() {
    std::filesystem::path dir = exe_directory();
    SetDllDirectoryW(dir.wstring().c_str());
}

bool is_ggml_backend_registered(const char* name);
bool has_cuda_library();

bool register_legacy_cuda_backend(HMODULE ggml) {
    static bool attempted = false;
    static bool registered = false;
    static HMODULE cuda_backend = nullptr;

    if (registered || is_ggml_backend_registered("CUDA")) {
        registered = true;
        return true;
    }
    if (attempted) return registered;
    attempted = true;

    std::filesystem::path cuda_path = exe_directory() / "ggml-cuda.dll";
    cuda_backend = LoadLibraryW(cuda_path.wstring().c_str());
    if (!cuda_backend) {
        vtlog("legacy cuda backend: failed to load ggml-cuda.dll");
        return false;
    }

    using cuda_reg_fn = void*(__cdecl *)();
    using backend_register_fn = void(__cdecl *)(void*);

    auto* cuda_reg = reinterpret_cast<cuda_reg_fn>(
        GetProcAddress(cuda_backend, "ggml_backend_cuda_reg"));
    auto* backend_register = reinterpret_cast<backend_register_fn>(
        GetProcAddress(ggml, "ggml_backend_register"));

    if (!cuda_reg || !backend_register) {
        vtlog("legacy cuda backend: required exports missing");
        return false;
    }

    void* reg = cuda_reg();
    if (!reg) {
        vtlog("legacy cuda backend: ggml_backend_cuda_reg returned null");
        return false;
    }

    backend_register(reg);
    registered = is_ggml_backend_registered("CUDA");
    vtlog("legacy cuda backend registered = %d", registered ? 1 : 0);
    return registered;
}

bool load_ggml_backends_from_exe_dir() {
    static bool attempted = false;
    static bool loaded = false;
    if (attempted) return loaded;
    attempted = true;

    std::filesystem::path ggml_path = exe_directory() / "ggml.dll";
    HMODULE ggml = LoadLibraryW(ggml_path.wstring().c_str());
    if (!ggml) {
        vtlog("ggml backend load: failed to load ggml.dll");
        return false;
    }

    using load_all_from_path_fn = void(__cdecl *)(const char*);
    using count_fn = size_t(__cdecl *)();

    auto* load_all_from_path = reinterpret_cast<load_all_from_path_fn>(
        GetProcAddress(ggml, "ggml_backend_load_all_from_path"));
    auto* reg_count = reinterpret_cast<count_fn>(
        GetProcAddress(ggml, "ggml_backend_reg_count"));
    auto* dev_count = reinterpret_cast<count_fn>(
        GetProcAddress(ggml, "ggml_backend_dev_count"));

    if (!load_all_from_path) {
        vtlog("ggml backend load: ggml_backend_load_all_from_path not exported");
        return false;
    }

    size_t before_regs = reg_count ? reg_count() : 0;
    size_t before_devs = dev_count ? dev_count() : 0;
    std::string dir = exe_directory().string();
    load_all_from_path(dir.c_str());
    size_t after_regs = reg_count ? reg_count() : 0;
    size_t after_devs = dev_count ? dev_count() : 0;

    vtlog("ggml backend load: regs %zu -> %zu, devices %zu -> %zu",
          before_regs, after_regs, before_devs, after_devs);
    if (!is_ggml_backend_registered("CUDA") && has_cuda_library()) {
        register_legacy_cuda_backend(ggml);
    }

    loaded = true;
    return true;
}

bool is_ggml_backend_registered(const char* name) {
    std::filesystem::path ggml_path = exe_directory() / "ggml.dll";
    HMODULE ggml = LoadLibraryW(ggml_path.wstring().c_str());
    if (!ggml) return false;

    using reg_by_name_fn = void*(__cdecl *)(const char*);
    auto* reg_by_name = reinterpret_cast<reg_by_name_fn>(
        GetProcAddress(ggml, "ggml_backend_reg_by_name"));
    return reg_by_name && reg_by_name(name) != nullptr;
}

bool has_nvidia_driver() {
    HMODULE driver = LoadLibraryA("nvcuda.dll");
    if (!driver) return false;
    FreeLibrary(driver);
    return true;
}

bool has_cuda_library() {
    return std::filesystem::exists(exe_directory() / "ggml-cuda.dll");
}

bool has_cuda_backend() {
    if (!has_nvidia_driver() || !has_cuda_library()) return false;

    configure_runtime_dll_search_path();
    load_ggml_backends_from_exe_dir();
    std::filesystem::path cuda_path = exe_directory() / "ggml-cuda.dll";
    HMODULE backend = LoadLibraryW(cuda_path.wstring().c_str());
    if (!backend) return false;

    FreeLibrary(backend);
    bool registered = is_ggml_backend_registered("CUDA");
    vtlog("cuda backend registered = %d", registered ? 1 : 0);
    return registered;
}

bool should_auto_use_gpu_for_model(const std::string& model_size) {
    return model_size == "small" || model_size == "medium" || model_size == "large";
}

long long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

struct AudioTrimResult {
    std::vector<float> samples;
    size_t start_sample = 0;
    size_t end_sample = 0;
    float threshold = 0.0f;
    bool trimmed = false;
};

AudioTrimResult trim_silence_for_transcription(const std::vector<float>& input,
                                               float vad_threshold,
                                               int padding_ms) {
    constexpr int sample_rate = 16000;
    constexpr int frame_samples = sample_rate / 100; // 10 ms

    AudioTrimResult result;
    result.end_sample = input.size();

    if (input.size() <= static_cast<size_t>(frame_samples)) {
        return result;
    }

    size_t frame_count = (input.size() + frame_samples - 1) / frame_samples;
    std::vector<float> frame_rms(frame_count, 0.0f);

    float peak_rms = 0.0f;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        size_t begin = frame * frame_samples;
        size_t end = (std::min)(input.size(), begin + static_cast<size_t>(frame_samples));
        float sum_sq = 0.0f;
        for (size_t i = begin; i < end; ++i) {
            sum_sq += input[i] * input[i];
        }
        float rms = std::sqrt(sum_sq / static_cast<float>(end - begin));
        frame_rms[frame] = rms;
        peak_rms = (std::max)(peak_rms, rms);
    }

    if (peak_rms < 0.0007f) {
        return result;
    }

    float adaptive_threshold = (std::max)(0.0007f, peak_rms * 0.08f);
    if (vad_threshold > 0.0f) {
        adaptive_threshold = (std::max)(
            adaptive_threshold,
            (std::min)(vad_threshold * 0.20f, peak_rms * 0.18f));
    }
    result.threshold = adaptive_threshold;

    size_t first_voice_frame = frame_count;
    size_t last_voice_frame = 0;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        if (frame_rms[frame] >= adaptive_threshold) {
            first_voice_frame = frame;
            break;
        }
    }
    if (first_voice_frame == frame_count) {
        return result;
    }
    for (size_t frame = frame_count; frame > first_voice_frame; --frame) {
        if (frame_rms[frame - 1] >= adaptive_threshold) {
            last_voice_frame = frame - 1;
            break;
        }
    }

    int padding_frames = (std::max)(0, padding_ms) / 10;
    size_t start_frame = first_voice_frame > static_cast<size_t>(padding_frames)
        ? first_voice_frame - static_cast<size_t>(padding_frames)
        : 0;
    size_t end_frame = (std::min)(frame_count,
        last_voice_frame + 1 + static_cast<size_t>(padding_frames));

    result.start_sample = start_frame * frame_samples;
    result.end_sample = (std::min)(input.size(), end_frame * static_cast<size_t>(frame_samples));

    if (result.start_sample == 0 && result.end_sample >= input.size()) {
        return result;
    }

    result.samples.assign(input.begin() + static_cast<std::ptrdiff_t>(result.start_sample),
                          input.begin() + static_cast<std::ptrdiff_t>(result.end_sample));
    result.trimmed = true;
    return result;
}

std::string trim_copy(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string shorten_label(const std::string& label, size_t max_len = 54) {
    if (label.size() <= max_len) return label;
    if (max_len <= 3) return label.substr(0, max_len);
    return label.substr(0, max_len - 3) + "...";
}

bool contains_case_insensitive(const std::string& text, const std::string& needle) {
    auto lower = [](std::string value) {
        for (char& ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    };
    return lower(text).find(lower(needle)) != std::string::npos;
}

std::string detect_cpu_label() {
    char buffer[256] = {};
    DWORD size = sizeof(buffer);
    LSTATUS status = RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString",
        RRF_RT_REG_SZ,
        nullptr,
        buffer,
        &size);

    if (status != ERROR_SUCCESS || buffer[0] == '\0') {
        return "Unknown CPU";
    }

    return shorten_label(trim_copy(buffer));
}

std::string detect_gpu_label() {
    std::vector<std::string> gpu_names;

    for (DWORD i = 0; i < 16; ++i) {
        DISPLAY_DEVICEA device = {};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesA(nullptr, i, &device, 0)) {
            break;
        }

        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 ||
            device.DeviceString[0] == '\0') {
            continue;
        }

        std::string name = trim_copy(device.DeviceString);
        if (contains_case_insensitive(name, "Microsoft Basic")) {
            continue;
        }
        gpu_names.push_back(name);
    }

    if (gpu_names.empty()) {
        return "No GPU detected";
    }

    for (const auto& name : gpu_names) {
        if (contains_case_insensitive(name, "NVIDIA")) {
            return shorten_label(name);
        }
    }

    for (const auto& name : gpu_names) {
        if (!contains_case_insensitive(name, "Intel")) {
            return shorten_label(name);
        }
    }

    return shorten_label(gpu_names.front());
}

class DownloadProgressCallback : public IBindStatusCallback {
public:
    explicit DownloadProgressCallback(std::function<void(float)> on_progress)
        : on_progress_(std::move(on_progress)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppvObject = static_cast<IBindStatusCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --ref_count_;
        if (count == 0) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG progress_max,
                                         ULONG, LPCWSTR) override {
        if (progress_max > 0 && on_progress_) {
            on_progress_(static_cast<float>(progress) /
                         static_cast<float>(progress_max));
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

private:
    std::atomic<ULONG> ref_count_{1};
    std::function<void(float)> on_progress_;
};
}  // anon namespace

namespace vtt {

App::App() = default;

App::~App() {
    shutdown();
}

bool App::initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    configure_runtime_dll_search_path();
    load_ggml_backends_from_exe_dir();
    taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");

    // --- 1. Load configuration ---
    config_ = std::make_unique<ConfigManager>();
    config_->load();

    const auto& cfg = config_->config();

    sync_auto_start(cfg.auto_start);

    // --- 2. Create hidden owner window ---
    // A real hidden top-level window is more reliable than HWND_MESSAGE for
    // notification-area icons, while WS_EX_TOOLWINDOW keeps it off Alt-Tab and
    // the normal taskbar.
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance_;
    wc.lpszClassName  = APP_CLASS_NAME;
    wc.hIcon          = create_app_icon(32, RGB(0, 120, 215));
    wc.hIconSm        = create_app_icon(16, RGB(0, 120, 215));

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, APP_CLASS_NAME, L"VoiceToText",
                            WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, hInstance_, this);
    if (!hwnd_) return false;
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    // --- 3. Initialize Whisper engine ---
    engine_ = std::make_unique<WhisperEngine>();
    engine_->set_language(cfg.language);
    engine_->set_n_threads(cfg.n_threads);
    engine_->set_fast_mode(cfg.whisper_fast_mode);

    if (!ensure_startup_model_available()) {
        MessageBoxW(nullptr,
                    L"Failed to download the default KB Whisper model.\n"
                    L"Please check your internet connection and try again.",
                    L"VoiceToText", MB_ICONWARNING);
        return false;
    }

    const auto& active_cfg = config_->config();
    bool startup_use_gpu = effective_use_gpu(current_model_size(), active_cfg.compute_mode);
    std::string startup_model_path = resolve_model_path(active_cfg.model_path);
    if (!engine_->initialize(startup_model_path, startup_use_gpu)) {
        MessageBoxW(nullptr,
                    L"Failed to load the KB Whisper model.",
                    L"VoiceToText", MB_ICONWARNING);
        return false;
    }
    vtlog("startup whisper system info:\n%s", engine_->get_system_info().c_str());
    start_engine_warmup();

    // --- 4. Initialize audio capture ---
    audio_ = std::make_unique<AudioCapture>();
    if (!audio_->initialize()) {
        MessageBoxW(nullptr, L"Failed to initialize audio capture.",
                    L"VoiceToText", MB_ICONWARNING);
        return false;
    }

    if (!cfg.audio_device.empty()) {
        audio_->select_device(cfg.audio_device);
    }
    {
        auto audio_prepare_start = std::chrono::steady_clock::now();
        bool audio_prepared = audio_->prepare_device();
        vtlog("startup audio prepare returned %d in %lld ms",
              audio_prepared ? 1 : 0, elapsed_ms(audio_prepare_start));
    }

    // --- 5. Initialize VAD ---
    vad_ = std::make_unique<VoiceActivityDetector>();
    vad_->set_threshold(cfg.vad_threshold);
    vad_->set_silence_duration_ms(cfg.silence_duration_ms);
    vad_->set_speech_duration_ms(cfg.speech_min_duration_ms);

    // --- 6. Initialize hotkey manager ---
    hotkeys_ = std::make_unique<HotkeyManager>();
    if (!hotkeys_->initialize(hwnd_)) {
        MessageBoxW(nullptr, L"Failed to initialize hotkeys.",
                    L"VoiceToText", MB_ICONWARNING);
        return false;
    }

    hotkeys_->set_callback([this](HotkeyAction action) {
        on_hotkey(action);
    });

    // Register hotkeys from config.
    // NOTE: CancelRecording (ESC by default) is registered *only* while
    // recording is active. A bare-ESC global hotkey would otherwise swallow
    // every ESC press system-wide and break the key in other applications.
    hotkeys_->register_hotkey(HotkeyAction::ToggleRecording,
                              cfg.toggle_modifiers, cfg.toggle_key);
    hotkeys_->register_hotkey(HotkeyAction::SwitchLanguage,
                              MOD_CONTROL | MOD_SHIFT, 'L');

    // --- 7. Initialize text injector ---
    injector_ = std::make_unique<TextInjector>();
    injector_->initialize();
    injector_->set_method(cfg.injection_method);
    injector_->set_keystroke_delay_ms(cfg.keystroke_delay_ms);

    // --- 8. Initialize local LLM cleanup ---
    llm_cleaner_ = std::make_unique<LlmCleaner>();
    LlmCleanerConfig cleaner_config{
        cfg.llm_cleanup_enabled,
        cfg.llm_endpoint,
        cfg.llm_model,
        cfg.llm_pre_prompt,
        cfg.llm_timeout_ms,
        cfg.llm_max_tokens,
        cfg.llm_intent_profiles_enabled,
        cfg.llm_intent_detection_prompt,
        cfg.llm_profile_transform_prompt,
    };
    cleaner_config.profiles.clear();
    for (const auto& profile : cfg.llm_profiles) {
        cleaner_config.profiles.push_back({profile.name, profile.instructions});
    }
    llm_cleaner_->configure(cleaner_config);

    correction_store_ = std::make_unique<CorrectionStore>();
    std::filesystem::path correction_path(cfg.llm_corrections_path);
    if (correction_path.is_relative()) {
        correction_path = std::filesystem::path(config_->get_config_path())
                              .parent_path() / correction_path;
    }
    correction_store_->initialize(correction_path.string(),
                                  cfg.llm_corrections_max_entries);

    transcript_history_ = std::make_unique<TranscriptHistory>();
    std::filesystem::path history_path(cfg.transcript_history_path);
    if (history_path.is_relative()) {
        history_path = std::filesystem::path(config_->get_config_path())
                           .parent_path() / history_path;
    }
    transcript_history_->initialize(history_path.string(),
                                    cfg.transcript_history_max_entries);

    // --- 9. Initialize overlay ---
    overlay_ = std::make_unique<OverlayWindow>();
    if (cfg.show_overlay) {
        overlay_->initialize(hInstance_);
    }

    // --- 10. Initialize system tray ---
    initialize_tray();

    // Find initial language index
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i] == cfg.language) {
            language_index_ = static_cast<int>(i);
            break;
        }
    }

    // Show startup notification
    std::string lang_name;
    if (cfg.language == "sv") lang_name = "Swedish";
    else if (cfg.language == "en") lang_name = "English";
    else lang_name = "Auto-detect";

    std::string toggle_desc = hotkeys_->get_binding_description(HotkeyAction::ToggleRecording);
    bool active_gpu = effective_use_gpu(current_model_size(), cfg.compute_mode);

    tray_->show_notification("VoiceToText Ready",
        "Press " + toggle_desc + " to start recording.\n"
        "Language: " + lang_name + "\n"
        "Compute: " + cfg.compute_mode + (active_gpu ? " (GPU)" : " (CPU)"));

    return true;
}

int App::run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void App::shutdown() {
    // Wait for processing thread to finish
    if (process_thread_.joinable()) {
        process_thread_.join();
    }
    if (model_download_thread_.joinable()) {
        model_download_thread_.join();
    }
    join_engine_warmup();
    if (history_append_thread_.joinable()) {
        history_append_thread_.join();
    }

    if (audio_ && audio_->is_recording()) {
        audio_->stop_recording();
    }

    if (tray_) tray_->shutdown();
    if (history_window_) history_window_->shutdown();
    if (overlay_) overlay_->shutdown();
    if (hotkeys_) hotkeys_->shutdown();
    if (injector_) injector_->shutdown();
    if (audio_) audio_->shutdown();
    if (engine_) engine_->shutdown();

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    UnregisterClassW(APP_CLASS_NAME, hInstance_);
}

void App::initialize_tray() {
    if (!tray_) {
        tray_ = std::make_unique<TrayIcon>();
    }

    if (!tray_->initialize(hwnd_, hInstance_)) {
        MessageBoxW(nullptr, L"Failed to create system tray icon.",
                    L"VoiceToText", MB_ICONWARNING);
        // Non-fatal: continue without tray
    }

    tray_->set_current_model(current_model_size());
    tray_->set_current_compute_mode(current_compute_mode());
    update_tray_compute_info();
    tray_->set_callback([this](TrayAction action, const std::string& value) {
        on_tray_action(action, value);
    });

    if (recording_.load()) {
        tray_->set_recording(true);
    }
}

void App::open_history_window() {
    if (!history_window_) {
        history_window_ = std::make_unique<HistoryWindow>();
        if (!history_window_->initialize(hInstance_, transcript_history_.get(),
                                         config_.get())) {
            history_window_.reset();
            if (tray_) {
                tray_->show_notification("VoiceToText",
                                         "Failed to open history window.");
            }
            return;
        }
    }

    history_window_->show();
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == self->taskbar_created_msg_) {
        if (self->tray_) {
            self->tray_->shutdown();
        }
        self->initialize_tray();
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY:
            self->hotkeys_->handle_hotkey(static_cast<int>(wParam));
            return 0;

        case WM_TRAYICON:
            if (self->tray_) {
                self->tray_->handle_message(wParam, lParam);
            }
            return 0;

        case WM_OPEN_HISTORY:
            self->open_history_window();
            return 0;

        case WM_HISTORY_UPDATED:
            if (self->history_window_) {
                self->history_window_->refresh();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void App::on_hotkey(HotkeyAction action) {
    vtlog("on_hotkey: action=%d recording=%d processing=%d",
          static_cast<int>(action), recording_.load(), processing_.load());
    switch (action) {
        case HotkeyAction::ToggleRecording:
            if (processing_.load()) {
                vtlog("on_hotkey: ignored (processing in progress)");
                break;
            }
            if (recording_.load()) {
                stop_recording();
            } else {
                start_recording();
            }
            break;

        case HotkeyAction::CancelRecording:
            if (recording_.load()) {
                cancel_recording();
            }
            break;

        case HotkeyAction::SwitchLanguage:
            switch_language();
            break;

        case HotkeyAction::OpenSettings:
            // Open the config file in the default editor
            ShellExecuteA(nullptr, "open", config_->get_config_path().c_str(),
                         nullptr, nullptr, SW_SHOW);
            break;
    }
}

void App::on_tray_action(TrayAction action, const std::string& value) {
    switch (action) {
        case TrayAction::ToggleRecording:
            on_hotkey(HotkeyAction::ToggleRecording);
            break;

        case TrayAction::SwitchLanguage:
            switch_language();
            break;

        case TrayAction::OpenSettings:
            on_hotkey(HotkeyAction::OpenSettings);
            break;

        case TrayAction::OpenHistory:
            PostMessage(hwnd_, WM_OPEN_HISTORY, 0, 0);
            break;

        case TrayAction::SelectModel:
            select_model(value);
            break;

        case TrayAction::SelectComputeMode:
            select_compute_mode(value);
            break;

        case TrayAction::Quit:
            if (recording_.load()) {
                cancel_recording();
            }
            PostMessage(hwnd_, WM_CLOSE, 0, 0);
            break;
    }
}

std::string App::current_model_size() const {
    if (!config_) return "tiny";

    const std::string& path = config_->config().model_path;
    const std::string sizes[] = {"tiny", "base", "small", "medium", "large"};
    for (const auto& size : sizes) {
        if (contains_text(path, "ggml-model-" + size + "-q5_0.bin") ||
            contains_text(path, "kb-whisper-" + size)) {
            return size;
        }
    }
    if (path == "models/ggml-model-q5_0.bin") {
        return "medium";
    }
    return "custom";
}

std::string App::current_compute_mode() const {
    if (!config_) return "auto";
    const std::string& mode = config_->config().compute_mode;
    if (mode == "gpu" || mode == "cpu" || mode == "auto") {
        return mode;
    }
    return "auto";
}

std::string App::resolve_model_path(const std::string& model_path) const {
    std::filesystem::path path(model_path);
    if (path.is_absolute()) {
        return path.string();
    }

    std::filesystem::path config_dir = exe_directory();
    if (config_) {
        std::filesystem::path config_path(config_->get_config_path());
        if (config_path.has_parent_path()) {
            config_dir = config_path.parent_path();
        }
    }

    return (config_dir / path).string();
}

std::string App::model_path_for_size(const std::string& model_size) const {
    std::filesystem::path models_dir = exe_directory() / "models";
    if (config_) {
        std::filesystem::path config_dir(config_->get_config_path());
        if (config_dir.has_parent_path()) {
            models_dir = config_dir.parent_path() / "models";
        }
    }

    return (models_dir / ("ggml-model-" + model_size + "-q5_0.bin")).string();
}

std::string App::model_url_for_size(const std::string& model_size) const {
    return "https://huggingface.co/KBLab/kb-whisper-" + model_size +
           "/resolve/main/ggml-model-q5_0.bin";
}

bool App::ensure_startup_model_available() {
    std::string model_size = current_model_size();
    if (model_size == "custom") {
        return std::filesystem::exists(resolve_model_path(config_->config().model_path));
    }

    std::string model_path = model_path_for_size(model_size);
    if (std::filesystem::exists(model_path)) {
        if (config_->config().model_path != model_path) {
            config_->config().model_path = model_path;
            config_->save();
        }
        return true;
    }

    std::filesystem::path final_path(model_path);
    std::filesystem::path temp_path = final_path;
    temp_path += ".download";

    std::error_code ec;
    std::filesystem::create_directories(final_path.parent_path(), ec);
    std::filesystem::remove(temp_path, ec);

    std::string url = model_url_for_size(model_size);
    HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(),
                                    temp_path.string().c_str(), 0, nullptr);
    if (FAILED(hr)) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }

    std::filesystem::remove(final_path, ec);
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }

    config_->config().model_path = final_path.string();
    config_->save();
    return true;
}

void App::select_model(const std::string& model_size) {
    if (model_size.empty()) return;

    if (recording_.load() || processing_.load()) {
        if (tray_) {
            tray_->show_notification("VoiceToText",
                                     "Cannot switch model while recording or processing.");
        }
        return;
    }
    if (downloading_model_.load()) {
        if (tray_) {
            tray_->show_notification("VoiceToText",
                                     "A model download is already running.");
        }
        return;
    }

    std::string new_path = model_path_for_size(model_size);
    if (!std::filesystem::exists(new_path)) {
        download_and_select_model(model_size, new_path);
        return;
    }

    bool use_gpu = effective_use_gpu(model_size, current_compute_mode());

    if (!reload_engine(new_path, use_gpu)) {
        if (tray_) {
            tray_->show_notification("VoiceToText",
                                     "Failed to load KB Whisper " + model_size + ".");
        }
        return;
    }

    config_->config().model_path = new_path;
    config_->config().use_gpu = use_gpu;
    config_->save();

    if (tray_) {
        tray_->set_current_model(model_size);
        tray_->set_current_compute_mode(current_compute_mode());
        update_tray_compute_info();
        tray_->set_tooltip("VoiceToText - Model: " + model_size +
                           (use_gpu ? " GPU" : " CPU"));
        tray_->show_notification("KB Whisper model changed",
                                 "Now using: " + model_size +
                                 (use_gpu ? " on GPU" : " on CPU"));
    }
}

void App::download_and_select_model(const std::string& model_size,
                                    const std::string& model_path) {
    if (model_download_thread_.joinable()) {
        model_download_thread_.join();
    }

    downloading_model_ = true;
    std::string url = model_url_for_size(model_size);
    std::filesystem::path final_path(model_path);
    std::filesystem::path temp_path = final_path;
    temp_path += ".download";

    if (overlay_) {
        overlay_->show(OverlayState::Downloading, "Downloading " + model_size + "...");
        overlay_->set_progress(0.0f, "Downloading " + model_size + "...");
    }
    if (tray_) {
        tray_->show_notification("Downloading KB Whisper model",
                                 model_size + " will be selected when ready.");
    }

    model_download_thread_ = std::thread([this, model_size, url, final_path, temp_path]() {
        std::error_code ec;
        std::filesystem::create_directories(final_path.parent_path(), ec);
        std::filesystem::remove(temp_path, ec);

        DownloadProgressCallback* callback =
            new DownloadProgressCallback([this, model_size](float progress) {
                if (overlay_) {
                    overlay_->set_progress(progress,
                                           "Downloading " + model_size + "...");
                }
            });

        HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(),
                                        temp_path.string().c_str(), 0, callback);
        callback->Release();

        if (SUCCEEDED(hr)) {
            std::filesystem::remove(final_path, ec);
            std::filesystem::rename(temp_path, final_path, ec);
        }

        if (FAILED(hr) || ec) {
            std::filesystem::remove(temp_path, ec);
            downloading_model_ = false;
            if (overlay_) overlay_->show(OverlayState::Error, "Download failed");
            if (tray_) {
                tray_->show_notification("Model download failed",
                                         "Could not download " + model_size + ".");
            }
            Sleep(1800);
            if (overlay_) overlay_->hide();
            return;
        }

        if (overlay_) {
            overlay_->set_progress(1.0f, "Loading " + model_size + "...");
        }

        bool use_gpu = effective_use_gpu(model_size, current_compute_mode());
        if (reload_engine(final_path.string(), use_gpu)) {
            config_->config().model_path = final_path.string();
            config_->config().use_gpu = use_gpu;
            config_->save();

            if (tray_) {
                tray_->set_current_model(model_size);
                tray_->set_current_compute_mode(current_compute_mode());
                update_tray_compute_info();
                tray_->set_tooltip("VoiceToText - Model: " + model_size +
                                   (use_gpu ? " GPU" : " CPU"));
                tray_->show_notification("KB Whisper model ready",
                                         "Now using: " + model_size +
                                         (use_gpu ? " on GPU" : " on CPU"));
            }
            if (overlay_) overlay_->show(OverlayState::Processing, "Model ready");
        } else {
            if (tray_) {
                tray_->show_notification("VoiceToText",
                                         "Downloaded but failed to load " + model_size + ".");
            }
            if (overlay_) overlay_->show(OverlayState::Error, "Load failed");
        }

        downloading_model_ = false;
        Sleep(1200);
        if (overlay_) overlay_->hide();
    });
}

void App::select_compute_mode(const std::string& compute_mode) {
    if (compute_mode != "auto" && compute_mode != "gpu" && compute_mode != "cpu") {
        return;
    }

    if (recording_.load() || processing_.load()) {
        if (tray_) {
            tray_->show_notification("VoiceToText",
                                     "Cannot switch compute mode while recording or processing.");
        }
        return;
    }

    std::string model_size = current_model_size();
    std::string model_path = resolve_model_path(config_->config().model_path);
    bool use_gpu = effective_use_gpu(model_size, compute_mode);
    bool requested_gpu = compute_mode == "gpu";

    if (!reload_engine(model_path, use_gpu)) {
        if (tray_) {
            tray_->show_notification("VoiceToText",
                                     "Failed to switch compute mode.");
        }
        return;
    }

    config_->config().compute_mode = compute_mode;
    config_->config().use_gpu = use_gpu;
    config_->save();

    if (tray_) {
        tray_->set_current_compute_mode(compute_mode);
        update_tray_compute_info();
        tray_->set_tooltip("VoiceToText - " + compute_mode +
                           (use_gpu ? " GPU" : " CPU"));
        if (requested_gpu && !use_gpu) {
            tray_->show_notification("GPU backend unavailable",
                                     "Using CPU. Build with CUDA to enable GPU.");
        } else {
            tray_->show_notification("Compute mode changed",
                                     compute_mode + (use_gpu ? " (GPU active)" : " (CPU active)"));
        }
    }
}

bool App::effective_use_gpu(const std::string& model_size,
                            const std::string& compute_mode) const {
    if (compute_mode == "cpu") return false;

    bool cuda_ready = has_cuda_backend();
    if (compute_mode == "gpu") {
        return cuda_ready;
    }

    return cuda_ready && should_auto_use_gpu_for_model(model_size);
}

bool App::reload_engine(const std::string& model_path, bool use_gpu) {
    join_engine_warmup();
    load_ggml_backends_from_exe_dir();

    std::string old_path = resolve_model_path(config_->config().model_path);
    bool old_use_gpu = config_->config().use_gpu;

    if (engine_->initialize(model_path, use_gpu)) {
        engine_->set_language(config_->config().language);
        engine_->set_n_threads(config_->config().n_threads);
        engine_->set_fast_mode(config_->config().whisper_fast_mode);
        vtlog("reload whisper system info:\n%s", engine_->get_system_info().c_str());
        start_engine_warmup();
        return true;
    }

    engine_->initialize(old_path, old_use_gpu);
    engine_->set_language(config_->config().language);
    engine_->set_n_threads(config_->config().n_threads);
    engine_->set_fast_mode(config_->config().whisper_fast_mode);
    start_engine_warmup();
    return false;
}

void App::start_engine_warmup() {
    if (!config_ || !config_->config().warm_up_model_on_load || !engine_) {
        return;
    }

    join_engine_warmup();
    warmup_thread_ = std::thread([this]() {
        auto start = std::chrono::steady_clock::now();
        vtlog("warmup: starting whisper warmup");
        bool ok = engine_->warm_up();
        vtlog("warmup: finished in %lld ms (ok=%d)",
              elapsed_ms(start), ok ? 1 : 0);
    });
}

void App::join_engine_warmup() {
    if (warmup_thread_.joinable()) {
        warmup_thread_.join();
    }
}

void App::update_tray_compute_info() {
    if (!tray_) return;

    std::string model_size = current_model_size();
    std::string mode = current_compute_mode();
    bool cuda_library = has_cuda_library();
    bool nvidia_driver = has_nvidia_driver();
    bool cuda_ready = has_cuda_backend();
    bool use_gpu = effective_use_gpu(model_size, mode);

    std::string reason;
    if (mode == "cpu") {
        reason = "Manual CPU";
    } else if (mode == "gpu") {
        if (cuda_ready) {
            reason = "Manual GPU";
        } else if (!cuda_library) {
            reason = "CUDA backend missing";
        } else if (!nvidia_driver) {
            reason = "NVIDIA driver missing";
        } else {
            reason = "CUDA dependency missing";
        }
    } else {
        if (!cuda_library) {
            reason = "CUDA backend missing";
        } else if (!nvidia_driver) {
            reason = "NVIDIA driver missing";
        } else if (!cuda_ready) {
            reason = "CUDA dependency missing";
        } else if (should_auto_use_gpu_for_model(model_size)) {
            reason = "Large model prefers GPU";
        } else {
            reason = "Tiny/base prefers CPU";
        }
    }

    tray_->set_compute_info(use_gpu ? "GPU" : "CPU",
                            detect_cpu_label(),
                            detect_gpu_label(),
                            reason);
}

void App::start_recording() {
    vtlog("start_recording: enter (recording=%d processing=%d)",
          recording_.load(), processing_.load());
    if (downloading_model_.load()) {
        if (tray_) {
            tray_->show_notification("VoiceToText",
                                     "Please wait for the model download to finish.");
        }
        return;
    }
    if (recording_.load() || processing_.load()) {
        vtlog("start_recording: early-exit (already active)");
        return;
    }

    vad_->reset();

    auto audio_start = std::chrono::steady_clock::now();
    bool audio_ok = audio_->start_recording();
    vtlog("start_recording: audio_->start_recording() returned %d in %lld ms",
          audio_ok, elapsed_ms(audio_start));
    if (!audio_ok) {
        if (overlay_) overlay_->show(OverlayState::Error, "Mic error");
        tray_->show_notification("VoiceToText", "Failed to start recording. Check microphone.");
        return;
    }

    recording_ = true;

    // Register the cancel hotkey only while recording, so a bare ESC binding
    // does not block the ESC key in other applications.
    const auto& cfg = config_->config();
    hotkeys_->register_hotkey(HotkeyAction::CancelRecording,
                              cfg.cancel_modifiers, cfg.cancel_key);

    if (overlay_) overlay_->show(OverlayState::Listening);
    if (tray_) tray_->set_recording(true);

    // Start a thread that monitors audio level for the overlay
    // (The actual audio data is buffered by AudioCapture)
    std::thread([this]() {
        while (recording_.load()) {
            if (overlay_) {
                overlay_->set_audio_level(audio_->get_audio_level());
            }
            Sleep(50);
        }
    }).detach();
}

void App::stop_recording() {
    vtlog("stop_recording: enter");
    if (!recording_.load()) {
        vtlog("stop_recording: early-exit (not recording)");
        return;
    }

    recording_ = false;
    hotkeys_->unregister_hotkey(HotkeyAction::CancelRecording);
    auto audio_stop = std::chrono::steady_clock::now();
    audio_->stop_recording();
    vtlog("stop_recording: audio_->stop_recording() returned in %lld ms",
          elapsed_ms(audio_stop));

    if (overlay_) overlay_->show(OverlayState::Processing);
    if (tray_) tray_->set_recording(false);

    // Process in background thread to keep UI responsive
    if (process_thread_.joinable()) {
        process_thread_.join();
    }

    processing_ = true;
    process_thread_ = std::thread([this]() {
        process_audio();
        processing_ = false;
    });
}

void App::cancel_recording() {
    if (!recording_.load()) return;

    recording_ = false;
    hotkeys_->unregister_hotkey(HotkeyAction::CancelRecording);
    auto audio_stop = std::chrono::steady_clock::now();
    audio_->stop_recording();
    vtlog("cancel_recording: audio_->stop_recording() returned in %lld ms",
          elapsed_ms(audio_stop));

    // Discard the audio buffer
    audio_->get_audio_buffer();

    if (overlay_) overlay_->hide();
    if (tray_) tray_->set_recording(false);

    tray_->set_tooltip("VoiceToText - Ready (cancelled)");
}

void App::process_audio() {
    auto process_start = std::chrono::steady_clock::now();
    vtlog("process_audio: enter");

    // Get all recorded audio
    std::vector<float> audio_data = audio_->get_audio_buffer();
    const auto& cfg = config_->config();
    vtlog("process_audio: audio buffer size = %zu samples (%.2f s @16kHz)",
          audio_data.size(), audio_data.size() / 16000.0);

    if (audio_data.empty()) {
        vtlog("process_audio: BUFFER EMPTY -- audio capture produced no samples");
        if (overlay_) overlay_->hide();
        return;
    }

    // Check minimum audio length (at least 0.5 seconds at 16kHz)
    if (audio_data.size() < 8000) {
        vtlog("process_audio: too short (<0.5s), aborting");
        if (overlay_) overlay_->hide();
        tray_->set_tooltip("VoiceToText - Too short");
        return;
    }

    // Check if the audio has enough energy (skip if it's basically silence)
    {
        float sum_sq = 0.0f;
        for (float s : audio_data) {
            sum_sq += s * s;
        }
        float rms = std::sqrt(sum_sq / static_cast<float>(audio_data.size()));
        vtlog("process_audio: RMS=%.6f (threshold 0.0005)", rms);
        if (rms < 0.0005f) {
            vtlog("process_audio: silence detected, aborting");
            if (overlay_) overlay_->hide();
            tray_->set_tooltip("VoiceToText - No speech detected");
            return;
        }
    }

    if (cfg.trim_silence_before_transcription) {
        size_t original_samples = audio_data.size();
        AudioTrimResult trim = trim_silence_for_transcription(
            audio_data, cfg.vad_threshold, cfg.trim_silence_padding_ms);
        if (trim.trimmed) {
            audio_data = std::move(trim.samples);
            vtlog("process_audio: trimmed silence %.2fs -> %.2fs "
                  "(kept %.2fs..%.2fs, threshold %.6f)",
                  original_samples / 16000.0,
                  audio_data.size() / 16000.0,
                  trim.start_sample / 16000.0,
                  trim.end_sample / 16000.0,
                  trim.threshold);
            if (audio_data.size() < 8000) {
                vtlog("process_audio: too short after trim (<0.5s), aborting");
                if (overlay_) overlay_->hide();
                tray_->set_tooltip("VoiceToText - Too short");
                return;
            }
        } else {
            vtlog("process_audio: silence trim kept full buffer (threshold %.6f)",
                  trim.threshold);
        }
    }

    // Run transcription
    vtlog("process_audio: calling whisper transcribe...");
    auto whisper_start = std::chrono::steady_clock::now();
    TranscriptionResult result = engine_->transcribe(audio_data);
    vtlog("process_audio: transcribe returned in %lld ms, text length = %zu",
          elapsed_ms(whisper_start), result.text.size());

    // Filter out common whisper hallucinations on silence/noise
    {
        std::string t = result.text;
        // Strip whitespace and punctuation for comparison
        std::string stripped;
        for (char c : t) {
            if (!std::isspace(static_cast<unsigned char>(c)) &&
                c != '.' && c != ',' && c != '!' && c != '?' &&
                c != '-' && c != '*' && c != '[' && c != ']' &&
                c != '(' && c != ')') {
                stripped += std::tolower(static_cast<unsigned char>(c));
            }
        }
        if (stripped.empty() || stripped == "blank" ||
            stripped == "blankaudio" || stripped == "silence" ||
            stripped == "inaudible" || stripped == "music" ||
            stripped == "applause" || stripped == "thankyou" ||
            stripped == "youtu" || stripped == "subscribenow" ||
            stripped == "thanksfowatching" || stripped == "thankyouforwatching") {
            result.text.clear();
        }
    }

    if (!result.text.empty()) {
        std::string recorded_text = result.text;
        std::string ai_language_text;
        std::string intent_profile = "general";

        bool cleanup_succeeded = false;
        const bool llm_configured = cfg.llm_cleanup_enabled && llm_cleaner_ &&
                                    !cfg.llm_endpoint.empty() &&
                                    !cfg.llm_model.empty();

        if (llm_configured) {
            vtlog("process_audio: calling LM Studio cleanup/profile transform...");
            auto cleanup_start = std::chrono::steady_clock::now();
            LlmCleanerConfig cleaner_config{
                cfg.llm_cleanup_enabled,
                cfg.llm_endpoint,
                cfg.llm_model,
                cfg.llm_pre_prompt,
                cfg.llm_timeout_ms,
                cfg.llm_max_tokens,
                cfg.llm_intent_profiles_enabled,
                cfg.llm_intent_detection_prompt,
                cfg.llm_profile_transform_prompt,
            };
            cleaner_config.profiles.clear();
            for (const auto& profile : cfg.llm_profiles) {
                cleaner_config.profiles.push_back({
                    profile.name,
                    profile.instructions,
                });
            }
            llm_cleaner_->configure(cleaner_config);

            std::string learned_corrections;
            if (config_->config().llm_corrections_enabled && correction_store_) {
                learned_corrections = correction_store_->build_prompt_block(
                    config_->config().llm_corrections_prompt_limit,
                    config_->config().llm_corrections_min_count);
            }

            LlmProfileResult profile_result;
            if (llm_cleaner_->clean_text_with_profile(result.text,
                                                      learned_corrections,
                                                      profile_result)) {
                vtlog("process_audio: transform succeeded in %lld ms, profile=%s, text length = %zu",
                      elapsed_ms(cleanup_start), profile_result.profile.c_str(),
                      profile_result.text.size());
                result.text = profile_result.text;
                intent_profile = profile_result.profile;
                cleanup_succeeded = true;
                if (config_->config().llm_corrections_enabled &&
                    correction_store_) {
                    correction_store_->learn_from(recorded_text,
                                                  profile_result.text);
                }
            } else {
                vtlog("process_audio: cleanup failed after %lld ms, using raw transcription",
                      elapsed_ms(cleanup_start));
            }
        } else if (config_->config().llm_cleanup_enabled) {
            vtlog("process_audio: LLM cleanup enabled but endpoint/model is missing; skipping");
        }

        if (cleanup_succeeded &&
            config_->config().llm_ai_language_enabled && llm_cleaner_) {
            vtlog("process_audio: calling AI language transform...");
            auto ai_language_start = std::chrono::steady_clock::now();
            if (llm_cleaner_->transform_text(result.text,
                                            config_->config().llm_ai_language_prompt,
                                            ai_language_text)) {
                vtlog("process_audio: AI language succeeded in %lld ms, text length = %zu",
                      elapsed_ms(ai_language_start), ai_language_text.size());
            } else {
                vtlog("process_audio: AI language failed after %lld ms",
                      elapsed_ms(ai_language_start));
            }
        } else if (config_->config().llm_ai_language_enabled &&
                   config_->config().llm_cleanup_enabled) {
            vtlog("process_audio: skipping AI language because cleanup did not run successfully");
        }

        if (overlay_) overlay_->hide();
        vtlog("process_audio: injecting text: \"%s\"", result.text.c_str());
        // Small delay to let the user release the hotkey before injecting.
        Sleep(static_cast<DWORD>((std::max)(0, cfg.pre_injection_delay_ms)));

        // Inject the transcribed text into the focused application
        auto inject_start = std::chrono::steady_clock::now();
        injector_->inject_text(result.text);
        vtlog("process_audio: injection returned in %lld ms",
              elapsed_ms(inject_start));

        if (transcript_history_) {
            if (history_append_thread_.joinable()) {
                history_append_thread_.join();
            }

            std::string history_recorded = recorded_text;
            std::string history_transcribed = result.text;
            std::string history_ai_language = ai_language_text;
            std::string history_profile = intent_profile;
            history_append_thread_ = std::thread([this,
                                                  history_recorded = std::move(history_recorded),
                                                  history_transcribed = std::move(history_transcribed),
                                                  history_ai_language = std::move(history_ai_language),
                                                  history_profile = std::move(history_profile)]() {
                auto history_start = std::chrono::steady_clock::now();
                transcript_history_->append(history_recorded, history_transcribed,
                                            history_ai_language, history_profile);
                PostMessage(hwnd_, WM_HISTORY_UPDATED, 0, 0);
                vtlog("history append: returned in %lld ms",
                      elapsed_ms(history_start));
            });
        }

        // Update tray tooltip with last transcription info
        std::ostringstream oss;
        oss << "VoiceToText - Last: " << result.text.substr(0, 50);
        if (result.text.size() > 50) oss << "...";
        tray_->set_tooltip(oss.str());
    } else {
        if (overlay_) overlay_->hide();
        vtlog("process_audio: transcription text was empty (or filtered)");
        tray_->set_tooltip("VoiceToText - No speech detected");
    }
    vtlog("process_audio: exit after %lld ms total", elapsed_ms(process_start));
}

void App::switch_language() {
    language_index_ = (language_index_ + 1) % static_cast<int>(languages_.size());
    const std::string& lang = languages_[language_index_];

    engine_->set_language(lang);

    // Update config
    config_->config().language = lang;
    config_->save();

    // Show notification
    std::string lang_name;
    if (lang == "sv") lang_name = "Swedish";
    else if (lang == "en") lang_name = "English";
    else lang_name = "Auto-detect";

    tray_->show_notification("Language Changed", "Now using: " + lang_name);
    tray_->set_tooltip("VoiceToText - " + lang_name);
}

} // namespace vtt
