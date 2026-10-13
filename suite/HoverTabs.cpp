// HoverTabs.cpp - in F12 post-it mode, hovering a thin zone along the top edge of any view shows that
// view's own tab bar over the top of the view (nothing is pushed down), and hides it again when the
// mouse leaves. It is Notepad++'s real tab bar: close buttons, the right-click menu, dragging (and so
// tab snapping and lifting), tab colours, tooltips and dark mode all work as they do outside F12.
//
// Post-it mode hides the tab bars by letting each view cover its own (Notepad++ keeps laying the tab
// bar out above the view either way, see DocTabView::reSizeTo); showing it is bringing it above the view.
#include "KexSuite.h"

namespace kex {

namespace {

const UINT_PTR TIMER_POLL = 1;
const UINT WM_HT_REPLACE = WM_APP + 1;
TabsStats s_stats;

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
        if (!views.tabBar(0)) { why = L"Notepad++'s tab bar was not found"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexHoverTabs")) return false;
        _pollMs = 60;
        ::SetTimer(hwnd(), TIMER_POLL, _pollMs, nullptr);
        return true;
    }

    void disable() override
    {
        hideBar();
        if (hwnd()) ::KillTimer(hwnd(), TIMER_POLL);
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
            ::PostMessageW(hwnd(), WM_HT_REPLACE, 0, 0);   // after Notepad++ has updated the tabs (a new row, a new name)
            break;
        }
    }

    void clickTab(int i)
    {
        if (i < 0 || i >= views.nbFiles(_view)) return;
        views.activate(_view, i);
        if (HWND s = views.sci(_view)) ::SetFocus(s);
        updateStats();
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_TIMER && w == TIMER_POLL) { poll(); return 0; }
        if (m == WM_HT_REPLACE) { if (_shown) { place(); updateStats(); } return 0; }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    void poll()
    {
        HWND n = npp._nppHandle;
        const bool postIt = ::IsWindowVisible(n) && !::IsIconic(n) && isPostItMode();
        // outside F12 mode only watch for it to start, a few times a second
        const UINT wantMs = postIt ? 60 : 400;
        if (wantMs != _pollMs) { _pollMs = wantMs; ::SetTimer(hwnd(), TIMER_POLL, _pollMs, nullptr); }
        if (!postIt) { hideBar(); return; }
        POINT pt; ::GetCursorPos(&pt);
        const int zv = zoneView(pt);
        bool inZone = zv >= 0;
        if (inZone)
        {
            HWND under = kexsuite::windowFromPoint(pt);   // sees through the glass text area
            HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
            if (root != n) inZone = false;                 // another window covers the edge
        }
        if (!_shown)
        {
            if (inZone) showBar(zv);
            return;
        }
        if (!::IsWindow(_bar)) { hideBar(); return; }
        RECT br; ::GetWindowRect(_bar, &br);
        ::InflateRect(&br, 6, 6);
        const bool overBar = ::PtInRect(&br, pt) != FALSE;
        const bool keep = busy();
        if (inZone && zv != _view && !overBar && !keep) { hideBar(); showBar(zv); return; }   // another view's edge
        if (inZone || overBar || keep) _leftAt = 0;
        else if (_leftAt == 0) _leftAt = nowMs();
        else if (nowMs() - _leftAt > _hideDelay) hideBar();
    }

    // a tab being dragged, or a menu open (the tabs' right-click menu): the bar stays
    bool busy() const
    {
        if (::GetCapture() == _bar) return true;
        GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
        return ::GetGUIThreadInfo(::GetCurrentThreadId(), &gi) && (gi.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) != 0;
    }

    // The view whose top edge the mouse is on (-1 if none). Views in the top row also count the strip
    // of window above them, so the very top edge of the screen works for a maximised window.
    int zoneView(POINT pt) const
    {
        HWND n = npp._nppHandle;
        RECT wr; ::GetWindowRect(n, &wr);
        int best = -1; LONG bestDx = LONG_MAX;
        const int focused = views.current();
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s || !views.visible(v) || views.host(v) != n || !::IsWindowVisible(s)) continue;
            RECT r; ::GetWindowRect(s, &r);
            const bool topRow = r.top - wr.top < 40;
            const LONG top = topRow ? std::min(r.top - 2, wr.top - 2) : r.top - 2;
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

    // the tab bar's height for its rows of tabs, worked out as Notepad++ does (TabBar::reSizeTo)
    int barHeight() const
    {
        RECT item{};
        if (!::SendMessageW(_bar, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item))) return 0;
        const int rows = std::max(1, static_cast<int>(::SendMessageW(_bar, TCM_GETROWCOUNT, 0, 0)));
        const int marge = rows >= 2 ? (rows - 2) * 3 : 0;
        return rows * static_cast<int>(item.bottom - item.top) + marge + ::GetSystemMetrics(SM_CYEDGE);
    }

    // over the top of the view, above it
    void place()
    {
        HWND s = views.sci(_view);
        if (!::IsWindow(_bar) || !s) return;
        RECT r; ::GetWindowRect(s, &r);
        ::MapWindowPoints(nullptr, ::GetParent(_bar), reinterpret_cast<POINT*>(&r), 2);
        const int h = barHeight();
        if (h <= 0) return;
        ::SetWindowPos(_bar, HWND_TOP, r.left, r.top, r.right - r.left, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::RedrawWindow(_bar, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void showBar(int v)
    {
        HWND bar = views.tabBar(v);
        HWND s = views.sci(v);
        if (!bar || !s || ::GetParent(bar) != ::GetParent(s)) return;
        _view = v;
        _bar = bar;
        _sci = s;
        ::GetWindowRect(_bar, &_barWas);
        ::MapWindowPoints(nullptr, ::GetParent(_bar), reinterpret_cast<POINT*>(&_barWas), 2);
        _barWasVisible = ::IsWindowVisible(_bar) != FALSE;
        // the view must not paint over the bar while the bar is above it
        _sciStyle = ::GetWindowLongPtrW(_sci, GWL_STYLE);
        ::SetWindowLongPtrW(_sci, GWL_STYLE, _sciStyle | WS_CLIPSIBLINGS);
        _shown = true;
        _leftAt = 0;
        place();
        updateStats();
        log(L"hover tabs: tab bar of view %d shown", _view + 1);
    }

    void hideBar()
    {
        if (!_shown) return;
        _shown = false;
        if (::IsWindow(_bar))
        {
            // back under the view, where Notepad++ had it
            ::SetWindowPos(_bar, _sci, _barWas.left, _barWas.top, _barWas.right - _barWas.left, _barWas.bottom - _barWas.top,
                           SWP_NOACTIVATE | (_barWasVisible ? 0 : SWP_HIDEWINDOW));
        }
        if (::IsWindow(_sci))
        {
            ::SetWindowLongPtrW(_sci, GWL_STYLE, _sciStyle);
            ::RedrawWindow(_sci, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        _bar = _sci = nullptr;
        s_stats.stripVisible = false;
        log(L"hover tabs: hidden");
    }

    void updateStats()
    {
        s_stats.stripVisible = _shown;
        s_stats.view = _view;
        s_stats.tabRects.clear(); s_stats.dirty.clear();
        s_stats.tabCount = 0;
        s_stats.activeIndex = -1;
        if (!_shown || !_bar) return;
        ::GetWindowRect(_bar, &s_stats.strip);
        s_stats.tabCount = static_cast<int>(::SendMessageW(_bar, TCM_GETITEMCOUNT, 0, 0));
        s_stats.activeIndex = static_cast<int>(::SendMessageW(_bar, TCM_GETCURSEL, 0, 0));
        for (int i = 0; i < s_stats.tabCount; ++i)
        {
            RECT r{};
            ::SendMessageW(_bar, TCM_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r));
            ::MapWindowPoints(_bar, nullptr, reinterpret_cast<POINT*>(&r), 2);
            s_stats.tabRects.push_back(r);
            TCITEMW it{}; it.mask = TCIF_IMAGE;
            ::SendMessageW(_bar, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(&it));
            s_stats.dirty.push_back(it.iImage == 1 ? 1 : 0);   // DocTabView: UNSAVED_IMG_INDEX
        }
    }

    HWND _bar = nullptr, _sci = nullptr;
    RECT _barWas{};
    bool _barWasVisible = false;
    LONG_PTR _sciStyle = 0;
    bool _shown = false;
    double _leftAt = 0;
    int _zone = 6, _hideDelay = 350, _view = 0;
    UINT _pollMs = 60;
};

HoverTabs* s_mod = nullptr;

} // namespace

Module* createHoverTabs() { s_mod = new HoverTabs(); return s_mod; }
TabsStats tabsStats() { return s_stats; }
void tabsTestClick(int i) { if (s_mod && s_mod->isEnabled()) s_mod->clickTab(i); }

} // namespace kex
