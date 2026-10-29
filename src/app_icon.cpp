#include "app_icon.h"
#include "../resources/resource.h"

namespace vtt {

namespace {

HICON create_fallback_app_icon(int size, COLORREF background) {
    if (size <= 0) size = 32;

    HDC screen_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color_bmp = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS,
                                         &bits, nullptr, 0);
    if (!color_bmp || !bits) {
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return nullptr;
    }

    BYTE r = GetRValue(background);
    BYTE g = GetGValue(background);
    BYTE b = GetBValue(background);
    DWORD pixel = (0xFFu << 24) | (r << 16) | (g << 8) | b;
    DWORD* pixels = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; ++i) {
        pixels[i] = pixel;
    }

    HBITMAP old_color = static_cast<HBITMAP>(SelectObject(mem_dc, color_bmp));
    SetBkMode(mem_dc, TRANSPARENT);
    SetTextColor(mem_dc, RGB(255, 255, 255));

    int font_height = size >= 32 ? -18 : -11;
    HFONT font = CreateFontW(font_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old_font = static_cast<HFONT>(SelectObject(mem_dc, font));
    RECT text_rect = {0, 0, size, size};
    DrawTextW(mem_dc, L"V", -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem_dc, old_font);
    DeleteObject(font);
    SelectObject(mem_dc, old_color);

    HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, nullptr);
    HBITMAP old_mask = static_cast<HBITMAP>(SelectObject(mem_dc, mask_bmp));
    RECT mask_rect = {0, 0, size, size};
    HBRUSH opaque = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem_dc, &mask_rect, opaque);
    DeleteObject(opaque);
    SelectObject(mem_dc, old_mask);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color_bmp;
    ii.hbmMask = mask_bmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
    return icon;
}

HICON load_app_icon(int resource_id, int size, COLORREF background) {
    if (size <= 0) size = 32;

    HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(resource_id),
        IMAGE_ICON,
        size,
        size,
        LR_DEFAULTCOLOR));

    if (icon) return icon;

    return create_fallback_app_icon(size, background);
}

} // namespace

HICON create_app_icon(int size, COLORREF background) {
    return load_app_icon(IDI_APP_ICON, size, background);
}

HICON create_recording_app_icon(int size, COLORREF background) {
    return load_app_icon(IDI_RECORDING_ICON, size, background);
}

} // namespace vtt
