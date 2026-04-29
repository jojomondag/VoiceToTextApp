#include "tray_icon.h"
#include <shellapi.h>

#define WM_TRAYICON (WM_APP + 1)

// Context menu item IDs
#define IDM_TOGGLE    40001
#define IDM_LANGUAGE  40002
#define IDM_SETTINGS  40003
#define IDM_QUIT      40004
#define IDM_HISTORY   40005

namespace vtt {

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    shutdown();
}

HICON TrayIcon::create_color_icon(COLORREF color) {
    // Create a simple 16x16 solid-color icon
    int size = 16;

    HDC screen_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color_bmp = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color_bmp || !bits) {
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return nullptr;
    }

    // Fill the bitmap with the color
    DWORD* pixels = static_cast<DWORD*>(bits);
    BYTE r = GetRValue(color);
    BYTE g = GetGValue(color);
    BYTE b = GetBValue(color);
    DWORD pixel = (0xFF << 24) | (r << 16) | (g << 8) | b;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Make a rounded square by skipping corner pixels
            int dx = (x < size / 2) ? x : (size - 1 - x);
            int dy = (y < size / 2) ? y : (size - 1 - y);
            if (dx + dy >= 2) {
                pixels[y * size + x] = pixel;
            } else {
                pixels[y * size + x] = 0; // transparent corner
            }
        }
    }

    // Create a mask bitmap (all black = fully opaque where color bitmap has alpha)
    HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, nullptr);
    HBITMAP old_mask = static_cast<HBITMAP>(SelectObject(mem_dc, mask_bmp));
    // Fill mask with black (0 = opaque) for all non-transparent pixels
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int dx = (x < size / 2) ? x : (size - 1 - x);
            int dy = (y < size / 2) ? y : (size - 1 - y);
            if (dx + dy >= 2) {
                SetPixel(mem_dc, x, y, RGB(0, 0, 0)); // opaque
            } else {
                SetPixel(mem_dc, x, y, RGB(255, 255, 255)); // transparent
            }
        }
    }
    SelectObject(mem_dc, old_mask);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask_bmp;
    ii.hbmColor = color_bmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);

    return icon;
}

bool TrayIcon::initialize(HWND hwnd, HINSTANCE hInstance) {
    if (initialized_) return true;

    hwnd_ = hwnd;
    hInstance_ = hInstance;
    menu_hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"STATIC",
        L"VoiceToTextMenuOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance_, nullptr);

    // Create icons: green for idle, red for recording
    icon_idle_ = create_color_icon(RGB(80, 180, 80));
    icon_recording_ = create_color_icon(RGB(220, 50, 50));

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
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = icon_idle_;
    wcscpy_s(nid_.szTip, L"VoiceToText - Ready");

    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        return false;
    }

    // Set version for modern balloon support
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);

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

void TrayIcon::handle_message(WPARAM wParam, LPARAM lParam) {
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
                callback_(TrayAction::ToggleRecording);
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
    AppendMenuW(menu, MF_STRING, IDM_LANGUAGE, L"Switch Language");
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
            callback_(TrayAction::ToggleRecording);
            break;
        case IDM_LANGUAGE:
            callback_(TrayAction::SwitchLanguage);
            break;
        case IDM_HISTORY:
            callback_(TrayAction::OpenHistory);
            break;
        case IDM_SETTINGS:
            callback_(TrayAction::OpenSettings);
            break;
        case IDM_QUIT:
            callback_(TrayAction::Quit);
            break;
    }
}

} // namespace vtt
