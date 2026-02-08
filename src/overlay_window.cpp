#include "overlay_window.h"
#include <dwmapi.h>

namespace vtt {

static constexpr int OVERLAY_WIDTH = 200;
static constexpr int OVERLAY_HEIGHT = 50;
static constexpr int DOT_RADIUS = 8;
static constexpr int CORNER_RADIUS = 16;
static constexpr int PADDING = 12;
static constexpr int LEVEL_BAR_HEIGHT = 4;
static constexpr const wchar_t* OVERLAY_CLASS_NAME = L"VTTOverlayWindow";

static constexpr COLORREF COLOR_LISTENING  = RGB(66, 133, 244);
static constexpr COLORREF COLOR_PROCESSING = RGB(251, 188, 4);
static constexpr COLORREF COLOR_ERROR      = RGB(234, 67, 53);
static constexpr COLORREF COLOR_BG         = RGB(32, 33, 36);
static constexpr COLORREF COLOR_TEXT       = RGB(255, 255, 255);

static std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                                  static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                        static_cast<int>(str.size()), wide.data(), len);
    return wide;
}

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    shutdown();
}

bool OverlayWindow::initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = OVERLAY_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS_NAME,
        L"VoiceToText Overlay",
        WS_POPUP,
        0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT,
        nullptr, nullptr, hInstance, this
    );

    if (!hwnd_) return false;

    SetLayeredWindowAttributes(hwnd_, 0, 230, LWA_ALPHA);

    HRGN rgn = CreateRoundRectRgn(0, 0, OVERLAY_WIDTH + 1, OVERLAY_HEIGHT + 1,
                                   CORNER_RADIUS, CORNER_RADIUS);
    SetWindowRgn(hwnd_, rgn, TRUE);

    update_position();
    initialized_ = true;
    return true;
}

void OverlayWindow::shutdown() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hInstance_) {
        UnregisterClassW(OVERLAY_CLASS_NAME, hInstance_);
        hInstance_ = nullptr;
    }
    initialized_ = false;
}

void OverlayWindow::show(OverlayState state, const std::string& text) {
    state_ = state;
    text_ = text;

    if (state == OverlayState::Hidden) {
        hide();
        return;
    }

    if (hwnd_) {
        update_position();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void OverlayWindow::hide() {
    state_ = OverlayState::Hidden;
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void OverlayWindow::set_audio_level(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    audio_level_ = level;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_PAINT: {
            if (self) self->paint();
            else {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
            }
            return 0;
        }

        case WM_NCHITTEST:
            return HTCAPTION;

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OverlayWindow::paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc;
    GetClientRect(hwnd_, &rc);

    // Double-buffer
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    // Background
    HBRUSH bgBrush = CreateSolidBrush(COLOR_BG);
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    // State color and default text
    COLORREF dotColor = COLOR_LISTENING;
    std::string displayText = text_;

    switch (state_) {
        case OverlayState::Listening:
            dotColor = COLOR_LISTENING;
            if (displayText.empty()) displayText = "Listening...";
            break;
        case OverlayState::Processing:
            dotColor = COLOR_PROCESSING;
            if (displayText.empty()) displayText = "Processing...";
            break;
        case OverlayState::Error:
            dotColor = COLOR_ERROR;
            if (displayText.empty()) displayText = "Error";
            break;
        case OverlayState::Hidden:
            break;
    }

    // Status dot
    int dotX = PADDING + DOT_RADIUS;
    int dotY = (rc.bottom - LEVEL_BAR_HEIGHT) / 2;

    HBRUSH dotBrush = CreateSolidBrush(dotColor);
    HPEN dotPen = CreatePen(PS_SOLID, 1, dotColor);
    HGDIOBJ oldPen = SelectObject(memDC, dotPen);
    HGDIOBJ oldBrush = SelectObject(memDC, dotBrush);

    Ellipse(memDC,
            dotX - DOT_RADIUS, dotY - DOT_RADIUS,
            dotX + DOT_RADIUS, dotY + DOT_RADIUS);

    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBrush);
    DeleteObject(dotBrush);
    DeleteObject(dotPen);

    // Text
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, COLOR_TEXT);

    HFONT font = CreateFontW(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"
    );
    HFONT oldFont = static_cast<HFONT>(SelectObject(memDC, font));

    RECT textRect;
    textRect.left = dotX + DOT_RADIUS + 8;
    textRect.top = 4;
    textRect.right = rc.right - PADDING;
    textRect.bottom = rc.bottom - LEVEL_BAR_HEIGHT - 2;

    std::wstring wText = utf8_to_wide(displayText);
    DrawTextW(memDC, wText.c_str(), static_cast<int>(wText.size()),
              &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(memDC, oldFont);
    DeleteObject(font);

    // Audio level bar at bottom
    if (state_ == OverlayState::Listening && audio_level_ > 0.0f) {
        int barWidth = static_cast<int>((rc.right - 2 * PADDING) * audio_level_);
        RECT barRect;
        barRect.left = PADDING;
        barRect.top = rc.bottom - LEVEL_BAR_HEIGHT - 2;
        barRect.right = PADDING + barWidth;
        barRect.bottom = rc.bottom - 2;

        HBRUSH barBrush = CreateSolidBrush(dotColor);
        FillRect(memDC, &barRect, barBrush);
        DeleteObject(barBrush);
    }

    // Blit to screen
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hwnd_, &ps);
}

void OverlayWindow::update_position() {
    if (!hwnd_) return;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenWidth - OVERLAY_WIDTH) / 2;
    int y = 20;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT,
                 SWP_NOACTIVATE | SWP_NOSIZE);
}

} // namespace vtt
