// Overlay.cpp - layered window helpers and software sprite drawing.
#include "Overlay.h"
#include <cmath>
#include <cstring>

namespace kex {

bool Surface::ensure(int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (bmp && width <= stride && height <= capH) { w = width; h = height; return true; }
    int nw = std::max(width, stride), nh = std::max(height, capH);
    nw = (nw + 63) & ~63; nh = (nh + 63) & ~63;
    release();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    dc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!bmp || !dc) { release(); return false; }
    old = ::SelectObject(dc, bmp);
    px = static_cast<uint32_t*>(bits);
    stride = nw; capH = nh; w = width; h = height;
    return true;
}

void Surface::clear()
{
    if (!px) return;
    for (int y = 0; y < h; ++y) memset(px + static_cast<size_t>(y) * stride, 0, static_cast<size_t>(w) * 4);
}

void Surface::release()
{
    if (dc) { if (old) ::SelectObject(dc, old); ::DeleteDC(dc); }
    if (bmp) ::DeleteObject(bmp);
    dc = nullptr; bmp = nullptr; old = nullptr; px = nullptr; w = h = stride = capH = 0;
}

static LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    }
    return ::DefWindowProcW(h, m, w, l);
}

HWND createOverlayWindow(HWND owner, bool topmost, const wchar_t* title)
{
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"KexSuiteOverlay";
        reg = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (topmost) ex |= WS_EX_TOPMOST;
    return ::CreateWindowExW(ex, L"KexSuiteOverlay", title, WS_POPUP, 0, 0, 1, 1, owner, nullptr, hInst, nullptr);
}

bool pushLayered(HWND wnd, Surface& s, POINT pos, SIZE size)
{
    if (!wnd || !s.dc) return false;
    if (binaryAlpha())
    {
        // Wine (no compositor) ignores partial alpha: snap to opaque/transparent so colours stay true.
        for (int y = 0; y < size.cy && y < s.h; ++y)
        {
            uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
            for (int x = 0; x < size.cx && x < s.w; ++x)
            {
                uint32_t p = row[x];
                uint32_t a = p >> 24;
                if (a >= 110)
                {
                    uint32_t b = (p & 0xFF) * 255 / a, g = ((p >> 8) & 0xFF) * 255 / a, r = ((p >> 16) & 0xFF) * 255 / a;
                    if (b > 255) b = 255; if (g > 255) g = 255; if (r > 255) r = 255;
                    row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
                else row[x] = 0;
            }
        }
    }
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    return ::UpdateLayeredWindow(wnd, nullptr, &pos, &size, s.dc, &src, 0, &bf, ULW_ALPHA) != FALSE;
}

bool renderRuns(Sprite& out, const std::vector<TextRun>& runs, int w, int h, int baseline, int pad)
{
    Surface mask;
    int W = w + 2 * pad, H = h + 2 * pad;
    if (!mask.ensure(W, H)) return false;
    out.w = W; out.h = H;
    out.px.assign(static_cast<size_t>(W) * H, 0);
    ::SetBkMode(mask.dc, TRANSPARENT);
    ::SetTextColor(mask.dc, RGB(255, 255, 255));
    ::SetTextAlign(mask.dc, TA_BASELINE | TA_LEFT);
    for (const TextRun& r : runs)
    {
        mask.clear();
        HGDIOBJ of = ::SelectObject(mask.dc, r.font ? r.font : ::GetStockObject(DEFAULT_GUI_FONT));
        ::ExtTextOutW(mask.dc, pad + r.dx, pad + baseline, 0, nullptr, r.text.c_str(), static_cast<UINT>(r.text.size()), nullptr);
        ::SelectObject(mask.dc, of);
        ::GdiFlush();
        uint32_t cr = GetRValue(r.color), cg = GetGValue(r.color), cb = GetBValue(r.color);
        for (int y = 0; y < H; ++y)
        {
            const uint32_t* mrow = mask.px + static_cast<size_t>(y) * mask.stride;
            uint32_t* orow = out.px.data() + static_cast<size_t>(y) * W;
            for (int x = 0; x < W; ++x)
            {
                uint32_t m = mrow[x];
                uint32_t a = std::max(std::max(m & 0xFF, (m >> 8) & 0xFF), (m >> 16) & 0xFF);
                if (!a) continue;
                uint32_t d = orow[x];
                uint32_t inv = 255 - a;
                uint32_t na = a + (((d >> 24) * inv) / 255);
                uint32_t nr = (cr * a) / 255 + ((((d >> 16) & 0xFF) * inv) / 255);
                uint32_t ng = (cg * a) / 255 + ((((d >> 8) & 0xFF) * inv) / 255);
                uint32_t nb = (cb * a) / 255 + (((d & 0xFF) * inv) / 255);
                orow[x] = (na << 24) | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
    return true;
}

static inline uint32_t samplePremul(const Sprite& sp, float fx, float fy)
{
    // bilinear sample with transparent outside
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    float tx = fx - x0, ty = fy - y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            int x = x0 + i, y = y0 + j;
            if (x < 0 || y < 0 || x >= sp.w || y >= sp.h) continue;
            float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            uint32_t p = sp.px[static_cast<size_t>(y) * sp.w + x];
            acc[0] += wgt * (p & 0xFF);
            acc[1] += wgt * ((p >> 8) & 0xFF);
            acc[2] += wgt * ((p >> 16) & 0xFF);
            acc[3] += wgt * (p >> 24);
        }
    return (static_cast<uint32_t>(acc[3] + 0.5f) << 24) | (static_cast<uint32_t>(acc[2] + 0.5f) << 16) |
           (static_cast<uint32_t>(acc[1] + 0.5f) << 8) | static_cast<uint32_t>(acc[0] + 0.5f);
}

static inline void blendOver(uint32_t& d, uint32_t s, float alpha)
{
    uint32_t sa = s >> 24;
    if (alpha < 1.0f)
    {
        uint32_t k = static_cast<uint32_t>(alpha * 256.0f);
        s = ((((s >> 24) * k) >> 8) << 24) | (((((s >> 16) & 0xFF) * k) >> 8) << 16) |
            (((((s >> 8) & 0xFF) * k) >> 8) << 8) | (((s & 0xFF) * k) >> 8);
        sa = s >> 24;
    }
    if (!sa) return;
    if (sa == 255) { d = s; return; }
    uint32_t inv = 255 - sa;
    uint32_t na = sa + (((d >> 24) * inv) / 255);
    uint32_t nr = ((s >> 16) & 0xFF) + ((((d >> 16) & 0xFF) * inv) / 255);
    uint32_t ng = ((s >> 8) & 0xFF) + ((((d >> 8) & 0xFF) * inv) / 255);
    uint32_t nb = (s & 0xFF) + (((d & 0xFF) * inv) / 255);
    d = (std::min(na, 255u) << 24) | (std::min(nr, 255u) << 16) | (std::min(ng, 255u) << 8) | std::min(nb, 255u);
}

void drawSprite(Surface& s, const Sprite& sp, float cx, float cy, float angle, float alpha)
{
    if (!s.px || sp.w <= 0 || sp.h <= 0 || alpha <= 0.0f) return;
    float c = std::cos(angle), sn = std::sin(angle);
    float hw = sp.w * 0.5f, hh = sp.h * 0.5f;
    float ex = std::fabs(c) * hw + std::fabs(sn) * hh, ey = std::fabs(sn) * hw + std::fabs(c) * hh;
    int x0 = std::max(0, static_cast<int>(std::floor(cx - ex))), x1 = std::min(s.w - 1, static_cast<int>(std::ceil(cx + ex)));
    int y0 = std::max(0, static_cast<int>(std::floor(cy - ey))), y1 = std::min(s.h - 1, static_cast<int>(std::ceil(cy + ey)));
    bool axisAligned = std::fabs(angle) < 0.0005f;
    for (int y = y0; y <= y1; ++y)
    {
        uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
        float dy = y + 0.5f - cy;
        for (int x = x0; x <= x1; ++x)
        {
            float dx = x + 0.5f - cx;
            float sx, sy;
            if (axisAligned) { sx = dx + hw - 0.5f; sy = dy + hh - 0.5f; }
            else { sx = c * dx + sn * dy + hw - 0.5f; sy = -sn * dx + c * dy + hh - 0.5f; }
            if (sx < -1 || sy < -1 || sx > sp.w || sy > sp.h) continue;
            uint32_t p = samplePremul(sp, sx, sy);
            if (p >> 24) blendOver(row[x], p, alpha);
        }
    }
}

void fillRectF(Surface& s, float l, float t, float r, float b, COLORREF c, float alpha)
{
    if (!s.px || r <= l || b <= t || alpha <= 0) return;
    int x0 = std::max(0, static_cast<int>(std::floor(l))), x1 = std::min(s.w - 1, static_cast<int>(std::ceil(r)) - 1);
    int y0 = std::max(0, static_cast<int>(std::floor(t))), y1 = std::min(s.h - 1, static_cast<int>(std::ceil(b)) - 1);
    uint32_t cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    for (int y = y0; y <= y1; ++y)
    {
        float covY = std::min(b, y + 1.0f) - std::max(t, static_cast<float>(y));
        uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
        for (int x = x0; x <= x1; ++x)
        {
            float covX = std::min(r, x + 1.0f) - std::max(l, static_cast<float>(x));
            float a = covX * covY * alpha;
            if (a <= 0.003f) continue;
            if (a > 1) a = 1;
            uint32_t A = static_cast<uint32_t>(a * 255.0f + 0.5f);
            uint32_t p = (A << 24) | ((cr * A / 255) << 16) | ((cg * A / 255) << 8) | (cb * A / 255);
            blendOver(row[x], p, 1.0f);
        }
    }
}

} // namespace kex
