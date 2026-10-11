// HoverTabs.cpp - in F12 post-it mode, hovering a thin zone along the top edge of any view shows a
// floating tab strip with that view's tabs, over the top of that view (nothing is pushed down).
#include "KexSuite.h"
#include <cmath>

namespace kex {

namespace {

const UINT_PTR TIMER_POLL = 1;
TabsStats s_stats;

struct Tab { std::wstring name; bool dirty = false; bool active = false; RECT rc{}; };

COLORREF mix(COLORREF a, COLORREF b, double t)
{
    return RGB(static_cast<int>(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               static_cast<int>(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               static_cast<int>(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

class HoverTabs : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"HoverTabs"; }
    const wchar_t* title() const override { return L"F12 hover tabs"; }

    void loadOptions() override
    {
        _zone = std::max(2, std::min(40, settings.getInt(key(), L"HotZonePx", 6)));
        _hideDelay = std::max(0, settings.getInt(key(), L"HideDelayMs", 350));
    }

    bool selfCheck(std::wstring& why) override
    {
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        if (!mm || ::GetMenuState(mm, IDM_VIEW_POSTIT, MF_BYCOMMAND) == static_cast<UINT>(-1))
        { why = L"View > Post-It (IDM_VIEW_POSTIT) not found in the main menu"; return false; }
        // We read tab names and unsaved markers from Notepad++'s tab control (BufferID in TCITEM.lParam).
        if (!views.patched() && !views.locateTabBar(0))
        { why = L"Notepad++'s tab bar could not be identified (internal layout changed)"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexHoverTabs")) return false;
        if (!createStrip()) return false;
        ::SetTimer(hwnd(), TIMER_POLL, 60, nullptr);
        return true;
    }

    void disable() override
    {
        if (hwnd()) ::KillTimer(hwnd(), TIMER_POLL);
        if (_strip) { ::DestroyWindow(_strip); _strip = nullptr; }
        if (_font) { ::DeleteObject(_font); _font = nullptr; }
        destroyMsgWindow();
        s_stats = TabsStats();
    }

    void onNotify(SCNotification* n) override
    {
        if (!_shown) return;
        switch (n->nmhdr.code)
        {
        case NPPN_BUFFERACTIVATED: case NPPN_FILEOPENED: case NPPN_FILECLOSED: case NPPN_DOCORDERCHANGED:
        case NPPN_FILESAVED: case SCN_SAVEPOINTLEFT: case SCN_SAVEPOINTREACHED: case NPPN_FILERENAMED:
            refresh();
            break;
        }
    }

    void clickTab(int i)
    {
        if (i < 0 || i >= static_cast<int>(_tabs.size())) return;
        views.activate(_view, i);
        HWND s = views.sci(_view);
        if (s) ::SetFocus(s);
        log(L"hover tabs: activated tab %d in view %d", i, _view);
        refresh();
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_TIMER && w == TIMER_POLL) { poll(); return 0; }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    bool createStrip()
    {
        static bool reg = false;
        if (!reg)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = stripProc; wc.hInstance = hInst; wc.lpszClassName = L"KexSuiteTabStrip";
            wc.hCursor = ::LoadCursor(nullptr, IDC_HAND);
            reg = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        _strip = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"KexSuiteTabStrip", L"KEX tabs",
            WS_POPUP, 0, 0, 10, 10, npp._nppHandle, nullptr, hInst, this);
        if (!_strip) return false;
        ::SetWindowLongPtrW(_strip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        _font = ::CreateFontIndirectW(&ncm.lfMessageFont);
        return true;
    }

    void poll()
    {
        HWND n = npp._nppHandle;
        if (!::IsWindowVisible(n) || ::IsIconic(n) || !isPostItMode()) { hideStrip(); return; }
        POINT pt; ::GetCursorPos(&pt);
        int zv = zoneView(pt);
        bool inZone = zv >= 0;
        if (inZone)
        {
            HWND under = ::WindowFromPoint(pt);
            HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
            if (root != n && under != _strip) inZone = false;   // another window covers the edge
        }
        if (!_shown)
        {
            if (inZone) showStrip(zv);
            return;
        }
        RECT sr; ::GetWindowRect(_strip, &sr);
        ::InflateRect(&sr, 6, 6);
        bool overStrip = ::PtInRect(&sr, pt) != FALSE;
        if (inZone && zv != _view && !overStrip) { _view = zv; refresh(); }   // moved to another view's edge
        if (inZone || overStrip) _leftAt = 0;
        else if (_leftAt == 0) _leftAt = nowMs();
        else if (nowMs() - _leftAt > _hideDelay) hideStrip();
    }

    // The view whose top edge the mouse is on (-1 if none). Views in the top row also count the strip
    // of window above them, so the very top edge of the screen works for a maximised window.
    int zoneView(POINT pt) const
    {
        HWND n = npp._nppHandle;
        RECT wr; ::GetWindowRect(n, &wr);
        int best = -1; LONG bestDx = LONG_MAX;
        int focused = views.current();
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s || !views.visible(v) || views.host(v) != n || !::IsWindowVisible(s)) continue;
            RECT r; ::GetWindowRect(s, &r);
            bool topRow = r.top - wr.top < 40;
            LONG top = topRow ? std::min(r.top - 2, wr.top - 2) : r.top - 2;
            if (pt.y < top || pt.y >= r.top + _zone) continue;
            LONG dx = pt.x < r.left ? r.left - pt.x : pt.x >= r.right ? pt.x - r.right + 1 : 0;
            if (dx == 0) return v;
            if (dx > 16 && !(topRow && pt.x >= wr.left && pt.x < wr.right)) continue;
            // over a splitter or the window edge next to the views: the focused view wins, else the nearest
            if (v == focused) dx = 0;
            if (dx < bestDx) { bestDx = dx; best = v; }
        }
        return best;
    }

    void collect()
    {
        if (_view < 0 || _view >= views.count() || !views.sci(_view)) _view = views.current();
        _tabs.clear();
        int nb = views.nbFiles(_view);
        int cur = views.currentIndex(_view);
        HWND tb = views.tabBar(_view);
        for (int i = 0; i < nb && i < 500; ++i)
        {
            Tab t;
            t.active = (i == cur);
            bool fromTab = false;
            if (tb)
            {
                wchar_t buf[MAX_PATH * 2] = {};
                TCITEMW it{}; it.mask = TCIF_TEXT | TCIF_IMAGE; it.pszText = buf; it.cchTextMax = MAX_PATH * 2 - 1;
                if (::SendMessageW(tb, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(&it)))
                {
                    std::wstring s = buf, o;
                    for (size_t k = 0; k < s.size(); ++k) { o += s[k]; if (s[k] == L'&' && k + 1 < s.size() && s[k + 1] == L'&') ++k; }
                    t.name = o;
                    t.dirty = (it.iImage == 1);   // DocTabView: UNSAVED_IMG_INDEX
                    fromTab = true;
                }
            }
            if (!fromTab)
            {
                t.name = fileNameOf(bufferPath(views.bufferId(_view, i)));
                if (t.active) t.dirty = sci(views.sci(_view), SCI_GETMODIFY) != 0;
            }
            _tabs.push_back(t);
        }
    }

    void layout()
    {
        HWND n = npp._nppHandle;
        HWND s = views.sci(_view);
        RECT area;
        if (s && views.host(_view) == n) ::GetWindowRect(s, &area);
        else ::GetWindowRect(n, &area);
        RECT wr; ::GetWindowRect(n, &wr);
        int width = std::max(120, static_cast<int>(area.right - area.left));
        HDC dc = ::GetDC(_strip);
        HGDIOBJ of = ::SelectObject(dc, _font);
        TEXTMETRICW tm; ::GetTextMetricsW(dc, &tm);
        int th = tm.tmHeight + 10, pad = 12, gap = 3, x = 4, y = 4;
        for (Tab& t : _tabs)
        {
            SIZE sz{}; std::wstring label = (t.dirty ? L"● " : L"") + t.name;
            ::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &sz);
            int w = std::min(static_cast<int>(sz.cx) + 2 * pad, 260);
            if (x + w > width - 4 && x > 4) { x = 4; y += th + gap; }
            t.rc = RECT{ x, y, x + w, y + th };
            x += w + gap;
        }
        ::SelectObject(dc, of);
        ::ReleaseDC(_strip, dc);
        int height = y + th + 5;
        int top = std::max(area.top, wr.top);
        ::SetWindowPos(_strip, HWND_TOPMOST, area.left, top, width, height, SWP_NOACTIVATE);
        s_stats.strip = RECT{ area.left, top, area.left + width, top + height };
    }

    void refresh()
    {
        if (!_shown) return;
        collect();
        layout();
        ::InvalidateRect(_strip, nullptr, FALSE);
        updateStats();
    }

    void showStrip(int v)
    {
        _view = v;
        collect();
        _shown = true;
        _leftAt = 0;
        layout();
        ::ShowWindow(_strip, SW_SHOWNOACTIVATE);
        ::InvalidateRect(_strip, nullptr, FALSE);
        updateStats();
        log(L"hover tabs: shown (%d tabs, view %d)", static_cast<int>(_tabs.size()), _view);
    }

    void hideStrip()
    {
        if (!_shown) return;
        _shown = false;
        ::ShowWindow(_strip, SW_HIDE);
        s_stats.stripVisible = false;
        log(L"hover tabs: hidden");
    }

    void updateStats()
    {
        s_stats.stripVisible = _shown;
        s_stats.tabCount = static_cast<int>(_tabs.size());
        s_stats.view = _view;
        s_stats.activeIndex = -1;
        s_stats.tabRects.clear(); s_stats.dirty.clear();
        RECT sr; ::GetWindowRect(_strip, &sr);
        for (size_t i = 0; i < _tabs.size(); ++i)
        {
            RECT r = _tabs[i].rc; ::OffsetRect(&r, sr.left, sr.top);
            s_stats.tabRects.push_back(r);
            s_stats.dirty.push_back(_tabs[i].dirty ? 1 : 0);
            if (_tabs[i].active) s_stats.activeIndex = static_cast<int>(i);
        }
    }

    void paint(HDC dc)
    {
        RECT rc; ::GetClientRect(_strip, &rc);
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = ::SelectObject(mem, bmp);
        COLORREF bg = static_cast<COLORREF>(nppMsg(NPPM_GETEDITORDEFAULTBACKGROUNDCOLOR));
        COLORREF fg = static_cast<COLORREF>(nppMsg(NPPM_GETEDITORDEFAULTFOREGROUNDCOLOR));
        COLORREF accent = ::GetSysColor(COLOR_HIGHLIGHT);
        COLORREF stripBg = mix(bg, fg, 0.10), tabBg = mix(bg, fg, 0.04), activeBg = mix(bg, accent, 0.35);
        HBRUSH b = ::CreateSolidBrush(stripBg); ::FillRect(mem, &rc, b); ::DeleteObject(b);
        HPEN edge = ::CreatePen(PS_SOLID, 1, mix(bg, fg, 0.30));
        HGDIOBJ op = ::SelectObject(mem, edge);
        ::MoveToEx(mem, 0, rc.bottom - 1, nullptr); ::LineTo(mem, rc.right, rc.bottom - 1);
        HGDIOBJ of = ::SelectObject(mem, _font);
        ::SetBkMode(mem, TRANSPARENT);
        for (size_t i = 0; i < _tabs.size(); ++i)
        {
            const Tab& t = _tabs[i];
            COLORREF c = t.active ? activeBg : (static_cast<int>(i) == _hot ? mix(tabBg, accent, 0.15) : tabBg);
            HBRUSH tb = ::CreateSolidBrush(c);
            HGDIOBJ obr = ::SelectObject(mem, tb);
            HPEN tp = ::CreatePen(PS_SOLID, 1, t.active ? mix(bg, accent, 0.8) : mix(bg, fg, 0.25));
            HGDIOBJ otp = ::SelectObject(mem, tp);
            ::RoundRect(mem, t.rc.left, t.rc.top, t.rc.right, t.rc.bottom, 7, 7);
            ::SelectObject(mem, otp); ::DeleteObject(tp);
            ::SelectObject(mem, obr); ::DeleteObject(tb);
            RECT tr = t.rc; ::InflateRect(&tr, -10, 0);
            if (t.dirty)
            {
                ::SetTextColor(mem, RGB(226, 84, 36));
                RECT dr = tr;
                ::DrawTextW(mem, L"●", 1, &dr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SIZE ds; ::GetTextExtentPoint32W(mem, L"● ", 2, &ds);
                tr.left += ds.cx;
            }
            ::SetTextColor(mem, fg);
            ::DrawTextW(mem, t.name.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        ::SelectObject(mem, of);
        ::SelectObject(mem, op); ::DeleteObject(edge);
        ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, ob); ::DeleteObject(bmp); ::DeleteDC(mem);
    }

    int hitTest(int x, int y) const
    {
        POINT p{ x, y };
        for (size_t i = 0; i < _tabs.size(); ++i) if (::PtInRect(&_tabs[i].rc, p)) return static_cast<int>(i);
        return -1;
    }

    static LRESULT CALLBACK stripProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        auto* self = reinterpret_cast<HoverTabs*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return ::DefWindowProcW(h, m, w, l);
        try
        {
            switch (m)
            {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: { PAINTSTRUCT ps; HDC dc = ::BeginPaint(h, &ps); self->paint(dc); ::EndPaint(h, &ps); return 0; }
            case WM_MOUSEMOVE:
            {
                int hot = self->hitTest(static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)));
                if (hot != self->_hot) { self->_hot = hot; ::InvalidateRect(h, nullptr, FALSE); }
                return 0;
            }
            case WM_LBUTTONUP:
                self->clickTab(self->hitTest(static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))));
                return 0;
            }
        }
        catch (...) {}
        return ::DefWindowProcW(h, m, w, l);
    }

    HWND _strip = nullptr;
    HFONT _font = nullptr;
    bool _shown = false;
    double _leftAt = 0;
    int _zone = 6, _hideDelay = 350, _view = 0, _hot = -1;
    std::vector<Tab> _tabs;
};

HoverTabs* s_mod = nullptr;

} // namespace

Module* createHoverTabs() { s_mod = new HoverTabs(); return s_mod; }
TabsStats tabsStats() { return s_stats; }
void tabsTestClick(int i) { if (s_mod && s_mod->isEnabled()) s_mod->clickTab(i); }

} // namespace kex
