#include "tray_icon.h"
#include "app_icon.h"
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#define WM_TRAYICON (WM_APP + 1)

// Context menu item IDs
#define IDM_TOGGLE    40001
#define IDM_LANGUAGE  40002
#define IDM_SETTINGS  40003
#define IDM_QUIT      40004
#define IDM_HISTORY   40005
#define IDM_DICTATION 40006
#define IDM_MODEL_TINY   40100
#define IDM_MODEL_BASE   40101
#define IDM_MODEL_SMALL  40102
#define IDM_MODEL_MEDIUM 40103
#define IDM_MODEL_LARGE  40104
#define IDM_COMPUTE_WHISPER_GPU  40203
#define IDM_COMPUTE_WHISPER_CPU  40204
#define IDM_COMPUTE_GEMMA_GPU    40205
#define IDM_COMPUTE_GEMMA_CPU    40206
#define IDM_GEMMA_AUTO      40300
#define IDM_GEMMA_E2B       40301
#define IDM_GEMMA_E4B       40302
#define IDM_GEMMA_12B       40303
#define IDM_GEMMA_26B_A4B   40304
#define IDM_GEMMA_31B       40305

namespace vtt {

namespace {
// A stable GUID lets Windows remember this notification-area icon instead of
// treating every launch as a new, unrecognised tray application.
const GUID VOICETOTEXT_TRAY_GUID = {
    0x6e8d2f15, 0x4f41, 0x4c40,
    {0x9a, 0x2c, 0x56, 0x9d, 0x0b, 0x4d, 0x2e, 0x71}
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return L"";

    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";

    std::wstring wide(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), len);
    return wide;
}

void tray_log(const char* message, DWORD value = 0) {
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string log_path(exe_path);
    auto pos = log_path.find_last_of("\\/");
    if (pos != std::string::npos) log_path = log_path.substr(0, pos + 1);
    log_path += "voicetotext.log";

    FILE* f = fopen(log_path.c_str(), "a");
    if (!f) return;
    if (value) {
        fprintf(f, "%s (%lu)\n", message, static_cast<unsigned long>(value));
    } else {
        fprintf(f, "%s\n", message);
    }
    fclose(f);
}
}  // namespace

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    shutdown();
}

bool TrayIcon::initialize(HWND hwnd, HINSTANCE hInstance) {
    if (initialized_) return true;

    hwnd_ = hwnd;
    menu_hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"STATIC",
        L"VoiceToTextMenuOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr);

    // Create icons: app logo for idle, red-centered logo for recording.
    icon_idle_ = create_app_icon(32, RGB(0, 120, 215));
    icon_recording_ = create_recording_app_icon(32, RGB(220, 50, 50));

    if (!icon_idle_) {
        // Fallback to a system icon
        icon_idle_ = LoadIcon(nullptr, IDI_APPLICATION);
    }
    if (!icon_recording_) {
        icon_recording_ = LoadIcon(nullptr, IDI_WARNING);
    }

    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = icon_idle_;
    nid_.guidItem = VOICETOTEXT_TRAY_GUID;
    wcscpy_s(nid_.szTip, L"VoiceToText - Ready");

    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        const DWORD first_error = GetLastError();
        tray_log("Shell_NotifyIconW(NIM_ADD) failed", first_error);

        // Some Explorer/taskbar hosts reject GUID/show-tip registrations
        // (notably after Explorer restarts or in a newly created desktop
        // session). Retry with the classic hWnd/uID registration so the app
        // still has a usable tray entry instead of appearing to start and
        // then becoming unreachable.
        nid_.uFlags &= ~(NIF_GUID | NIF_SHOWTIP);
        ZeroMemory(&nid_.guidItem, sizeof(nid_.guidItem));
        if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
            tray_log("Shell_NotifyIconW fallback failed", GetLastError());
            return false;
        }
        tray_log("Shell_NotifyIconW fallback succeeded");
    }
    tray_log("Shell_NotifyIconW(NIM_ADD) succeeded");

    // Set version for modern balloon support
    nid_.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid_)) {
        tray_log("Shell_NotifyIconW(NIM_SETVERSION) failed", GetLastError());
    }

    initialized_ = true;
    return true;
}

void TrayIcon::shutdown() {
    if (!initialized_) return;

    Shell_NotifyIconW(NIM_DELETE, &nid_);

    if (menu_hwnd_) {
        DestroyWindow(menu_hwnd_);
        menu_hwnd_ = nullptr;
    }

    if (icon_idle_ && icon_idle_ != LoadIcon(nullptr, IDI_APPLICATION)) {
        DestroyIcon(icon_idle_);
    }
    if (icon_recording_ && icon_recording_ != LoadIcon(nullptr, IDI_WARNING)) {
        DestroyIcon(icon_recording_);
    }

    icon_idle_ = nullptr;
    icon_recording_ = nullptr;
    initialized_ = false;
}

void TrayIcon::set_callback(TrayCallback callback) {
    callback_ = std::move(callback);
}

void TrayIcon::set_recording(bool recording) {
    recording_ = recording;

    if (!initialized_) return;

    nid_.hIcon = recording_ ? icon_recording_ : icon_idle_;
    nid_.uFlags = NIF_ICON | NIF_TIP;

    if (recording_) {
        wcscpy_s(nid_.szTip, L"VoiceToText - Recording...");
    } else {
        wcscpy_s(nid_.szTip, L"VoiceToText - Ready");
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::set_current_model(const std::string& model_size) {
    if (!model_size.empty()) {
        current_model_size_ = model_size;
    }
}

void TrayIcon::set_current_compute_mode(const std::string& whisper_mode,
                                        const std::string& gemma_mode) {
    if (!whisper_mode.empty()) {
        current_whisper_compute_mode_ = whisper_mode;
    }
    if (!gemma_mode.empty()) {
        current_gemma_compute_mode_ = gemma_mode;
    }
}

void TrayIcon::set_current_gemma_model(const std::string& model_id,
                                       const std::string& recommended_model_id) {
    if (!model_id.empty()) {
        current_gemma_model_ = model_id;
    }
    if (!recommended_model_id.empty()) {
        recommended_gemma_model_ = recommended_model_id;
    }
}

void TrayIcon::set_installed_gemma_models(const std::vector<std::string>& model_ids) {
    installed_gemma_models_ = model_ids;
}

void TrayIcon::set_tooltip(const std::string& tooltip) {
    if (!initialized_) return;

    // Convert UTF-8 to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, tooltip.c_str(), -1, nullptr, 0);
    if (len > 0 && len < 128) {
        MultiByteToWideChar(CP_UTF8, 0, tooltip.c_str(), -1, nid_.szTip, 128);
    }

    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

std::string TrayIcon::model_size_from_command(UINT cmd) const {
    switch (cmd) {
        case IDM_MODEL_TINY: return "gguf-q8";
        case IDM_MODEL_BASE: return "onnx-int4";
        case IDM_MODEL_SMALL: return "onnx-int8";
        case IDM_MODEL_MEDIUM: return "onnx-fp16";
        case IDM_MODEL_LARGE: return "onnx-fp32";
        default: return {};
    }
}

std::string TrayIcon::whisper_compute_from_command(UINT cmd) const {
    switch (cmd) {
        case IDM_COMPUTE_WHISPER_GPU: return "gpu";
        case IDM_COMPUTE_WHISPER_CPU: return "cpu";
        default: return {};
    }
}

std::string TrayIcon::gemma_compute_from_command(UINT cmd) const {
    switch (cmd) {
        case IDM_COMPUTE_GEMMA_GPU: return "gpu";
        case IDM_COMPUTE_GEMMA_CPU: return "cpu";
        default: return {};
    }
}

std::string TrayIcon::gemma_model_from_command(UINT cmd) const {
    switch (cmd) {
        case IDM_GEMMA_AUTO: return "auto";
        case IDM_GEMMA_E2B: return "e2b";
        case IDM_GEMMA_E4B: return "e4b";
        case IDM_GEMMA_12B: return "12b";
        case IDM_GEMMA_26B_A4B: return "26b-a4b";
        case IDM_GEMMA_31B: return "31b";
        default: return {};
    }
}

void TrayIcon::show_notification(const std::string& title, const std::string& message) {
    if (!initialized_) return;

    nid_.uFlags = NIF_INFO;
    nid_.dwInfoFlags = NIIF_INFO;

    // Convert title
    int title_len = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    if (title_len > 0 && title_len < 64) {
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nid_.szInfoTitle, 64);
    }

    // Convert message
    int msg_len = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    if (msg_len > 0 && msg_len < 256) {
        MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nid_.szInfo, 256);
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::handle_message(LPARAM lParam) {
    UINT msg = LOWORD(lParam);

    switch (msg) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        case NIN_KEYSELECT:
            if (menu_open_) {
                break;
            }
            show_context_menu();
            break;
        case WM_LBUTTONDBLCLK:
            if (callback_) {
                callback_(TrayAction::ToggleRecording, {});
            }
            break;
    }
}

void TrayIcon::show_context_menu() {
    menu_open_ = true;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        menu_open_ = false;
        return;
    }

    if (recording_) {
        AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"Stop Recording");
    } else {
        AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"Start Recording");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_DICTATION, L"Live dictation...");
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                L"Speech language: Swedish (Pianissimo)");

    // Compute Mode at the top — per-component submenus
    HMENU computeMenu = CreatePopupMenu();
    if (computeMenu) {
        HMENU whisperCompute = CreatePopupMenu();
        if (whisperCompute) {
            const bool onnx = current_model_size_.rfind("onnx-", 0) == 0;
            AppendMenuW(whisperCompute, MF_STRING |
                (onnx ? MF_DISABLED | MF_GRAYED : 0) |
                (current_whisper_compute_mode_ == "gpu" ? MF_CHECKED : 0),
                IDM_COMPUTE_WHISPER_GPU, L"GPU (Vulkan, GGUF)");
            AppendMenuW(whisperCompute, MF_STRING |
                (current_whisper_compute_mode_ == "cpu" ? MF_CHECKED : 0),
                IDM_COMPUTE_WHISPER_CPU, L"CPU (GGUF / ONNX)");
            AppendMenuW(computeMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(whisperCompute), L"Pianissimo");
        }

        HMENU gemmaCompute = CreatePopupMenu();
        if (gemmaCompute) {
            AppendMenuW(gemmaCompute, MF_STRING |
                (current_gemma_compute_mode_ == "gpu" ? MF_CHECKED : 0),
                IDM_COMPUTE_GEMMA_GPU, L"GPU");
            AppendMenuW(gemmaCompute, MF_STRING |
                (current_gemma_compute_mode_ == "cpu" ? MF_CHECKED : 0),
                IDM_COMPUTE_GEMMA_CPU, L"CPU");
            AppendMenuW(computeMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(gemmaCompute), L"Gemma");
        }

        AppendMenuW(computeMenu, MF_SEPARATOR, 0, nullptr);
        std::wstring whisperStatus = L"Pianissimo: " + utf8_to_wide(current_whisper_compute_mode_);
        std::wstring gemmaStatus = L"Gemma: " + utf8_to_wide(current_gemma_compute_mode_);
        AppendMenuW(computeMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    whisperStatus.c_str());
        AppendMenuW(computeMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    gemmaStatus.c_str());

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(computeMenu),
                    L"Compute Mode");
    }

    HMENU modelMenu = CreatePopupMenu();
    if (modelMenu) {
        struct ModelItem {
            UINT id;
            const wchar_t* label;
            const char* size;
        };
        const ModelItem models[] = {
            {IDM_MODEL_TINY, L"GGUF Q8 (~740 MB)", "gguf-q8"},
            {IDM_MODEL_BASE, L"ONNX int4 - smallest (~515 MB)", "onnx-int4"},
            {IDM_MODEL_SMALL, L"ONNX int8 - recommended (~660 MB)", "onnx-int8"},
            {IDM_MODEL_MEDIUM, L"ONNX fp16 (~1.28 GB)", "onnx-fp16"},
            {IDM_MODEL_LARGE, L"ONNX fp32 (~2.56 GB)", "onnx-fp32"},
        };

        for (const auto& model : models) {
            UINT flags = MF_STRING;
            if (current_model_size_ == model.size) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(modelMenu, flags, model.id, model.label);
        }

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modelMenu),
                    L"Pianissimo Model");
    }

    HMENU gemmaMenu = CreatePopupMenu();
    if (gemmaMenu) {
        struct GemmaItem {
            UINT id;
            std::wstring label;
            const char* model;
        };

        std::wstring autoLabel = L"Auto - recommends ";
        autoLabel += utf8_to_wide(recommended_gemma_model_);
        const GemmaItem models[] = {
            {IDM_GEMMA_AUTO, autoLabel, "auto"},
            {IDM_GEMMA_E2B, L"Gemma 4 E2B - recommended", "e2b"},
            {IDM_GEMMA_E4B, L"Gemma 4 E4B - higher quality", "e4b"},
            {IDM_GEMMA_12B, L"Gemma 4 12B - large", "12b"},
            {IDM_GEMMA_26B_A4B, L"Gemma 4 26B A4B - very large", "26b-a4b"},
            {IDM_GEMMA_31B, L"Gemma 4 31B - overkill", "31b"},
        };

        for (const auto& model : models) {
            UINT flags = MF_STRING;
            if (current_gemma_model_ == model.model) {
                flags |= MF_CHECKED;
            }
            std::wstring label = model.label;
            bool installed = std::find(installed_gemma_models_.begin(),
                                       installed_gemma_models_.end(),
                                       model.model) != installed_gemma_models_.end();
            if (installed) {
                label += L" - installed";
            }
            AppendMenuW(gemmaMenu, flags, model.id, label.c_str());
        }

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(gemmaMenu),
                    L"Gemma Model");
    }

    AppendMenuW(menu, MF_STRING, IDM_HISTORY, L"History...");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    // Required for menu to work correctly from tray
    HWND owner = menu_hwnd_ ? menu_hwnd_ : hwnd_;
    SetForegroundWindow(owner);

    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                               pt.x, pt.y, 0, owner, nullptr);

    DestroyMenu(menu);
    menu_open_ = false;

    // Post a dummy message so the menu dismisses properly
    PostMessage(owner, WM_NULL, 0, 0);

    if (!callback_) return;

    switch (cmd) {
        case IDM_TOGGLE:
            callback_(TrayAction::ToggleRecording, {});
            break;
        case IDM_LANGUAGE:
            callback_(TrayAction::SwitchLanguage, {});
            break;
        case IDM_HISTORY:
            callback_(TrayAction::OpenHistory, {});
            break;
        case IDM_DICTATION:
            callback_(TrayAction::OpenDictation, {});
            break;
        case IDM_SETTINGS:
            callback_(TrayAction::OpenSettings, {});
            break;
        case IDM_QUIT:
            callback_(TrayAction::Quit, {});
            break;
        default: {
            std::string model_size = model_size_from_command(cmd);
            if (!model_size.empty()) {
                callback_(TrayAction::SelectModel, model_size);
                break;
            }
            std::string whisper_mode = whisper_compute_from_command(cmd);
            if (!whisper_mode.empty()) {
                callback_(TrayAction::SelectWhisperComputeMode, whisper_mode);
                break;
            }
            std::string gemma_mode = gemma_compute_from_command(cmd);
            if (!gemma_mode.empty()) {
                callback_(TrayAction::SelectGemmaComputeMode, gemma_mode);
                break;
            }
            std::string gemma_model = gemma_model_from_command(cmd);
            if (!gemma_model.empty()) {
                callback_(TrayAction::SelectGemmaModel, gemma_model);
            }
            break;
        }
    }
}

} // namespace vtt
