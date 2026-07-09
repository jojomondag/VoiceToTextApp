#include "overlay_window.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <gdiplus.h>
#include <string>
#include <utility>
#include <windowsx.h>

using namespace Gdiplus;

namespace vtt {

static constexpr int OVERLAY_WIDTH = 500;
static constexpr int OVERLAY_HEIGHT = 140;
static constexpr int OVERLAY_COMPACT_HEIGHT = 60;
static constexpr int DOT_RADIUS = 10;
static constexpr int CORNER_RADIUS = 20;
static constexpr int PADDING = 18;
static constexpr int WAVEFORM_HEIGHT = 28;
static constexpr int WAVEFORM_BARS = 28;
static constexpr int PASTE_BUTTON_COUNT = 3;
static constexpr const wchar_t* OVERLAY_CLASS_NAME = L"VTTOverlayWindow";

static constexpr COLORREF COLOR_LISTENING  = RGB(66, 133, 244);
static constexpr COLORREF COLOR_PROCESSING = RGB(251, 188, 4);
static constexpr COLORREF COLOR_ERROR      = RGB(234, 67, 53);
static constexpr COLORREF COLOR_BG         = RGB(32, 33, 36);
static constexpr COLORREF COLOR_TEXT       = RGB(255, 255, 255);

static int imax(int a, int b) {
    return a > b ? a : b;
}

static float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static float maxf(float a, float b) {
    return a > b ? a : b;
}

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

static Color gdip_color(COLORREF color, BYTE alpha = 255) {
    return Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

static bool file_exists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring exe_directory() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";

    std::wstring dir(path, len);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return dir.substr(0, slash);
}

static Bitmap* load_resource_bitmap(const wchar_t* filename) {
    const std::wstring name(filename);
    const std::wstring exe_dir = exe_directory();
    const std::wstring candidates[] = {
        exe_dir + L"\\resources\\" + name,
        L"resources\\" + name,
        L"..\\..\\resources\\" + name,
    };

    for (const auto& path : candidates) {
        if (!file_exists(path)) continue;
        Bitmap* bitmap = Bitmap::FromFile(path.c_str(), FALSE);
        if (bitmap && bitmap->GetLastStatus() == Ok) return bitmap;
        delete bitmap;
    }

    return nullptr;
}

static std::string normalize_paste_mode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode == "ai" || mode == "ai_language" || mode == "ai-language") return "ai";
    if (mode == "recorded" || mode == "recorded_voice" || mode == "voice") return "recorded";
    return "clean";
}

static void add_rounded_rect(GraphicsPath& path, REAL x, REAL y,
                             REAL width, REAL height, REAL radius) {
    REAL diameter = radius * 2.0f;
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    shutdown();
}

bool OverlayWindow::initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    GdiplusStartupInput gdiplus_input;
    if (GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Ok) {
        gdiplus_token_ = 0;
        return false;
    }
    load_paste_button_icons();

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

    update_position();
    initialized_ = true;
    return true;
}

void OverlayWindow::shutdown() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    release_paste_button_icons();
    if (hInstance_) {
        UnregisterClassW(OVERLAY_CLASS_NAME, hInstance_);
        hInstance_ = nullptr;
    }
    if (gdiplus_token_) {
        GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
    }
    initialized_ = false;
}

void OverlayWindow::load_paste_button_icons() {
    release_paste_button_icons();
    paste_button_icons_[0] = load_resource_bitmap(L"overlay_voice_mode.png");
    paste_button_icons_[1] = load_resource_bitmap(L"overlay_ai_language_mode.png");
    paste_button_icons_[2] = load_resource_bitmap(L"overlay_cleanup_mode.png");
}

void OverlayWindow::release_paste_button_icons() {
    for (auto*& icon : paste_button_icons_) {
        delete icon;
        icon = nullptr;
    }
}

void OverlayWindow::show(OverlayState state, const std::string& text) {
    state_ = state;
    text_ = text;
    progress_ = 0.0f;
    if (state == OverlayState::Listening) {
        audio_levels_.clear();
    }

    if (state == OverlayState::Hidden) {
        hide();
        return;
    }

    if (hwnd_) {
        update_position();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        paint();
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

    // Raw microphone RMS is usually tiny (speech is often around 0.003-0.01),
    // so map it into a UI meter range instead of drawing the raw value.
    float meter = clampf((level - 0.0005f) * 140.0f, 0.0f, 1.0f);
    float alpha = meter > audio_level_ ? 0.80f : 0.35f; // fast attack, slower release
    audio_level_ = (audio_level_ * (1.0f - alpha)) + (meter * alpha);
    audio_levels_.push_back(audio_level_);
    if (audio_levels_.size() > WAVEFORM_BARS) {
        audio_levels_.erase(audio_levels_.begin());
    }
    if (hwnd_ && IsWindowVisible(hwnd_)) paint();
}

void OverlayWindow::set_progress(float progress, const std::string& text) {
    progress_ = clampf(progress, 0.0f, 1.0f);
    if (!text.empty()) {
        text_ = text;
    }
    if (hwnd_ && IsWindowVisible(hwnd_)) paint();
}

void OverlayWindow::set_paste_output_mode(const std::string& mode) {
    paste_output_mode_ = normalize_paste_mode(mode);
    if (hwnd_ && IsWindowVisible(hwnd_)) paint();
}

void OverlayWindow::set_paste_mode_callback(
    std::function<void(const std::string&)> callback) {
    paste_mode_callback_ = std::move(callback);
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
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (self) self->paint();
            return 0;
        }

        case WM_NCHITTEST: {
            if (self && self->state_ == OverlayState::Listening) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                if (self->hit_test_paste_button(pt.x, pt.y) >= 0) return HTCLIENT;
            }
            return HTCAPTION;
        }

        case WM_MOUSEMOVE:
            if (self && self->state_ == OverlayState::Listening) {
                self->update_hover(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;

        case WM_MOUSELEAVE:
            if (self && self->hover_button_ != -1) {
                self->hover_button_ = -1;
                self->paint();
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (self && self->state_ == OverlayState::Listening &&
                self->handle_button_click(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int OverlayWindow::hit_test_paste_button(int x, int y) const {
    for (int i = 0; i < PASTE_BUTTON_COUNT; ++i) {
        const RECT& rc = paste_button_rects_[i];
        if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom) {
            return i;
        }
    }
    return -1;
}

void OverlayWindow::update_hover(int x, int y) {
    int hit = hit_test_paste_button(x, y);
    if (hit != hover_button_) {
        hover_button_ = hit;
        paint();
    }
}

bool OverlayWindow::handle_button_click(int x, int y) {
    int hit = hit_test_paste_button(x, y);
    if (hit < 0) return false;

    const char* modes[] = {"recorded", "ai", "clean"};
    paste_output_mode_ = modes[hit];
    if (paste_mode_callback_) {
        paste_mode_callback_(paste_output_mode_);
    }
    paint();
    return true;
}

static void draw_robot_icon(Graphics& g, const RectF& rc, Color color) {
    Pen pen(color, 1.7f);
    SolidBrush brush(color);
    REAL headX = rc.X + 4.0f;
    REAL headY = rc.Y + 6.0f;
    REAL headW = rc.Width - 8.0f;
    REAL headH = rc.Height - 9.0f;
    GraphicsPath head;
    add_rounded_rect(head, headX, headY, headW, headH, 4.0f);
    g.DrawPath(&pen, &head);
    g.FillEllipse(&brush, headX + 4.0f, headY + 6.0f, 3.0f, 3.0f);
    g.FillEllipse(&brush, headX + headW - 7.0f, headY + 6.0f, 3.0f, 3.0f);
    g.DrawLine(&pen, headX + 6.0f, headY + headH - 6.0f,
               headX + headW - 6.0f, headY + headH - 6.0f);
    g.DrawLine(&pen, rc.X + rc.Width / 2.0f, rc.Y + 6.0f,
               rc.X + rc.Width / 2.0f, rc.Y + 2.0f);
    g.FillEllipse(&brush, rc.X + rc.Width / 2.0f - 1.7f, rc.Y, 3.4f, 3.4f);
}

static void draw_mic_icon(Graphics& g, const RectF& rc, Color color) {
    Pen pen(color, 1.8f);
    GraphicsPath mic;
    add_rounded_rect(mic, rc.X + 7.0f, rc.Y + 1.5f,
                     rc.Width - 14.0f, rc.Height - 8.0f, 6.0f);
    g.DrawPath(&pen, &mic);
    g.DrawArc(&pen, rc.X + 3.0f, rc.Y + 8.0f, rc.Width - 6.0f,
              rc.Height - 8.0f, 0.0f, 180.0f);
    g.DrawLine(&pen, rc.X + rc.Width / 2.0f, rc.Y + rc.Height - 4.0f,
               rc.X + rc.Width / 2.0f, rc.Y + rc.Height);
    g.DrawLine(&pen, rc.X + 7.0f, rc.Y + rc.Height,
               rc.X + rc.Width - 7.0f, rc.Y + rc.Height);
}

static void draw_clean_icon(Graphics& g, const RectF& rc, Color color) {
    Pen pen(color, 1.7f);
    REAL x = rc.X + 5.0f;
    REAL y = rc.Y + 1.5f;
    REAL w = rc.Width - 10.0f;
    REAL h = rc.Height - 3.0f;
    g.DrawRectangle(&pen, x, y, w, h);
    g.DrawLine(&pen, x + 4.0f, y + 6.0f, x + w - 4.0f, y + 6.0f);
    g.DrawLine(&pen, x + 4.0f, y + 11.0f, x + w - 7.0f, y + 11.0f);
    g.DrawLine(&pen, x + 4.0f, y + 16.0f, x + w - 10.0f, y + 16.0f);
}

void OverlayWindow::paint() {
    if (!hwnd_ || !gdiplus_token_) return;

    const int overlayHeight = state_ == OverlayState::Processing
                                  ? OVERLAY_COMPACT_HEIGHT
                                  : OVERLAY_HEIGHT;

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
        case OverlayState::Downloading:
            dotColor = COLOR_PROCESSING;
            if (displayText.empty()) displayText = "Downloading...";
            break;
        case OverlayState::Error:
            dotColor = COLOR_ERROR;
            if (displayText.empty()) displayText = "Error";
            break;
        case OverlayState::Hidden:
            break;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = OVERLAY_WIDTH;
    bmi.bmiHeader.biHeight = -overlayHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP memBmp = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    if (!memBmp || !bits) {
        if (memBmp) DeleteObject(memBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }

    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));

    GraphicsPath bgPath;
    add_rounded_rect(bgPath, 0.5f, 0.5f,
                     static_cast<REAL>(OVERLAY_WIDTH - 1),
                     static_cast<REAL>(overlayHeight - 1),
                     static_cast<REAL>(CORNER_RADIUS));
    SolidBrush bgBrush(Color(235, 32, 33, 36));
    g.FillPath(&bgBrush, &bgPath);

    Pen borderPen(Color(60, 255, 255, 255), 1.0f);
    g.DrawPath(&borderPen, &bgPath);

    float seconds = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
    float pulse = (std::sin(seconds * 4.2f) + 1.0f) * 0.5f;

    const REAL modeIconSize = 42.0f;
    const REAL modeIconGap = 8.0f;
    const REAL modeIconTotalW =
        PASTE_BUTTON_COUNT * modeIconSize + (PASTE_BUTTON_COUNT - 1) * modeIconGap;

    REAL dotX = static_cast<REAL>(PADDING + DOT_RADIUS);
    REAL textLeft = 0;
    REAL textWidthAllowed = 0;

    std::wstring wText = utf8_to_wide(displayText);
    FontFamily fontFamily(L"Segoe UI");
    Font font(&fontFamily, 20.0f, FontStyleBold, UnitPixel);
    PointF measureOrigin(0, 0);
    RectF measureRect;
    g.MeasureString(wText.c_str(), -1, &font, measureOrigin, &measureRect);
    REAL textWidth = measureRect.Width;

    if (state_ == OverlayState::Listening) {
        dotX = static_cast<REAL>(PADDING + DOT_RADIUS);
        REAL iconAreaX = static_cast<REAL>(OVERLAY_WIDTH - PADDING - modeIconTotalW);
        REAL leftBound = dotX + DOT_RADIUS + 12.0f;
        REAL rightBound = iconAreaX - 8.0f;
        if (rightBound <= leftBound) rightBound = leftBound + 1.0f;
        REAL availableW = rightBound - leftBound;
        REAL useW = (std::min)(textWidth, availableW);
        textLeft = leftBound + (availableW - useW) / 2.0f;
        textWidthAllowed = useW;
    } else {
        dotX = static_cast<REAL>(PADDING + DOT_RADIUS);
        REAL leftBound = dotX + DOT_RADIUS + 12.0f;
        REAL rightBound = static_cast<REAL>(OVERLAY_WIDTH) - PADDING;
        if (rightBound <= leftBound) rightBound = leftBound + 1.0f;
        REAL availableW = rightBound - leftBound;
        REAL useW = (std::min)(textWidth, availableW);
        textLeft = (state_ == OverlayState::Processing)
                       ? leftBound + (availableW - useW) / 2.0f
                       : leftBound;
        textWidthAllowed = useW;
    }
    REAL dotY = static_cast<REAL>(PADDING + DOT_RADIUS + 1);
    if (state_ == OverlayState::Listening) {
        REAL haloRadius = DOT_RADIUS + 3.5f + pulse * 4.5f;
        BYTE haloAlpha = static_cast<BYTE>(45 + pulse * 50);
        Pen haloPen(gdip_color(COLOR_LISTENING, haloAlpha), 2.0f);
        g.DrawEllipse(&haloPen, dotX - haloRadius, dotY - haloRadius,
                      haloRadius * 2.0f, haloRadius * 2.0f);
    }

    SolidBrush dotBrush(gdip_color(dotColor));
    g.FillEllipse(&dotBrush, dotX - DOT_RADIUS, dotY - DOT_RADIUS,
                  DOT_RADIUS * 2.0f, DOT_RADIUS * 2.0f);

    SolidBrush textBrush(gdip_color(COLOR_TEXT));
    RectF textRect(textLeft, PADDING - 1.0f,
                   textWidthAllowed, 28.0f);
    StringFormat textFormat;
    textFormat.SetLineAlignment(StringAlignmentCenter);
    textFormat.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(wText.c_str(), -1, &font, textRect, &textFormat, &textBrush);

    if (state_ == OverlayState::Downloading) {
        REAL barX = static_cast<REAL>(PADDING);
        REAL barY = static_cast<REAL>(OVERLAY_HEIGHT - PADDING - 22);
        REAL barW = static_cast<REAL>(OVERLAY_WIDTH - PADDING * 2);
        REAL barH = 22.0f;

        GraphicsPath trackPath;
        add_rounded_rect(trackPath, barX, barY, barW, barH, 9.0f);
        SolidBrush trackBrush(Color(150, 48, 49, 54));
        g.FillPath(&trackBrush, &trackPath);

        REAL fillW = barW * clampf(progress_, 0.0f, 1.0f);
        if (fillW > 1.0f) {
            GraphicsPath fillPath;
            add_rounded_rect(fillPath, barX, barY, fillW, barH, 9.0f);
            SolidBrush fillBrush(gdip_color(COLOR_PROCESSING, 230));
            g.FillPath(&fillBrush, &fillPath);
        }

        int percent = static_cast<int>(progress_ * 100.0f + 0.5f);
        std::wstring percentText = std::to_wstring(percent) + L"%";
        Font percentFont(&fontFamily, 13.0f, FontStyleBold, UnitPixel);
        SolidBrush percentBrush(gdip_color(COLOR_TEXT, 235));
        RectF percentRect(barX, barY, barW, barH);
        StringFormat percentFormat;
        percentFormat.SetAlignment(StringAlignmentCenter);
        percentFormat.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(percentText.c_str(), -1, &percentFont,
                     percentRect, &percentFormat, &percentBrush);
    } else if (state_ == OverlayState::Listening) {
        const char* modes[] = {"recorded", "ai", "clean"};
        REAL modeIconY = static_cast<REAL>(PADDING - 4);
        REAL iconStartX = static_cast<REAL>(OVERLAY_WIDTH - PADDING - modeIconTotalW);

        for (int i = 0; i < PASTE_BUTTON_COUNT; ++i) {
            REAL x = iconStartX + i * (modeIconSize + modeIconGap);
            REAL y = modeIconY;
            paste_button_rects_[i] = {
                static_cast<LONG>(std::floor(x - 4.0f)),
                static_cast<LONG>(std::floor(y - 4.0f)),
                static_cast<LONG>(std::ceil(x + modeIconSize + 4.0f)),
                static_cast<LONG>(std::ceil(y + modeIconSize + 4.0f))
            };

            bool selected = paste_output_mode_ == modes[i];
            bool hovered = hover_button_ == i;
            if (selected || hovered) {
                BYTE alpha = selected ? 85 : 45;
                SolidBrush haloBrush(gdip_color(COLOR_LISTENING, alpha));
                g.FillEllipse(&haloBrush, x - 3.0f, y - 3.0f,
                              modeIconSize + 6.0f, modeIconSize + 6.0f);
            }

            Color iconColor;
            if (selected) {
                iconColor = Color(255, 255, 255, 255);
            } else {
                iconColor = Color(55, 255, 255, 255);
            }
            RectF iconRect(x, y, modeIconSize, modeIconSize);
            if (paste_button_icons_[i]) {
                if (selected) {
                    g.DrawImage(paste_button_icons_[i], iconRect);
                } else {
                    ColorMatrix fadeMatrix = {
                        0.299f, 0.299f, 0.299f, 0, 0,
                        0.587f, 0.587f, 0.587f, 0, 0,
                        0.114f, 0.114f, 0.114f, 0, 0,
                        0,      0,      0,      0.22f, 0,
                        0,      0,      0,      0,     1
                    };
                    ImageAttributes attr;
                    attr.SetColorMatrix(&fadeMatrix);
                    g.DrawImage(paste_button_icons_[i], iconRect,
                                0, 0,
                                paste_button_icons_[i]->GetWidth(),
                                paste_button_icons_[i]->GetHeight(),
                                UnitPixel, &attr);
                }
            } else if (i == 0) {
                draw_mic_icon(g, iconRect, iconColor);
            } else if (i == 1) {
                draw_robot_icon(g, iconRect, iconColor);
            } else {
                draw_clean_icon(g, iconRect, iconColor);
            }
        }

        REAL graphX = static_cast<REAL>(PADDING);
        REAL graphY = static_cast<REAL>(OVERLAY_HEIGHT - PADDING - WAVEFORM_HEIGHT);
        REAL graphW = static_cast<REAL>(OVERLAY_WIDTH - PADDING * 2);
        REAL graphH = static_cast<REAL>(WAVEFORM_HEIGHT);

        GraphicsPath graphPath;
        add_rounded_rect(graphPath, graphX, graphY, graphW, graphH, 8.0f);
        SolidBrush graphBg(Color(150, 48, 49, 54));
        g.FillPath(&graphBg, &graphPath);

        REAL barGap = 4.0f;
        REAL barWidth = (graphW - (WAVEFORM_BARS - 1) * barGap) / WAVEFORM_BARS;
        if (barWidth < 4.0f) barWidth = 4.0f;
        REAL centerY = graphY + graphH / 2.0f;

        SolidBrush barBrush(gdip_color(COLOR_LISTENING, 235));
        SolidBrush quietBrush(Color(150, 95, 125, 185));
        for (int i = 0; i < WAVEFORM_BARS; ++i) {
            int historyOffset = static_cast<int>(audio_levels_.size()) - WAVEFORM_BARS + i;
            float value = historyOffset >= 0 ? audio_levels_[historyOffset] : 0.0f;
            float idleWave = 0.08f + 0.045f * (std::sin(seconds * 5.0f + i * 0.72f) + 1.0f) * 0.5f;
            value = maxf(value, idleWave);
            value = clampf(value, 0.03f, 1.0f);

            REAL barHeight = static_cast<REAL>(imax(5, static_cast<int>(value * graphH)));
            REAL x = graphX + i * (barWidth + barGap);
            GraphicsPath barPath;
            add_rounded_rect(barPath, x, centerY - barHeight / 2.0f,
                             barWidth, barHeight, barWidth / 2.0f);
            g.FillPath(value > 0.18f ? &barBrush : &quietBrush, &barPath);
        }
    }

    RECT windowRect;
    GetWindowRect(hwnd_, &windowRect);
    POINT dst = {windowRect.left, windowRect.top};
    POINT src = {0, 0};
    SIZE size = {OVERLAY_WIDTH, overlayHeight};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, screenDC, &dst, &size, memDC, &src,
                        0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

void OverlayWindow::update_position() {
    if (!hwnd_) return;

    int overlayHeight = state_ == OverlayState::Processing
                            ? OVERLAY_COMPACT_HEIGHT
                            : OVERLAY_HEIGHT;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenWidth - OVERLAY_WIDTH) / 2;
    int y = 20;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, OVERLAY_WIDTH, overlayHeight,
                 SWP_NOACTIVATE);
}

} // namespace vtt
