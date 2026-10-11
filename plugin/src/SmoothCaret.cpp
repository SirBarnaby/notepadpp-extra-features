// SmoothCaret.cpp - a Word-like caret: Scintilla's own caret is hidden (SCI_SETCARETSTYLE with an
// invisible insert style; the exact original value is restored on disable) and our caret is drawn
// on a small layered popup owned by the editor's top-level window. It glides to each new position
// with an ease-out curve and fades instead of blinking hard.
//
// Fail-safe: Scintilla's caret is only hidden while our caret is really on screen (overlay shown and
// not cloaked, e.g. left behind on another virtual desktop). Whenever that can't be confirmed, the
// normal caret comes back, so there is never no caret at all. A watchdog re-checks every 300 ms.
#include "Overlay.h"
#include <dwmapi.h>
#include <cmath>
#include <map>

namespace kex {

namespace {

struct Car { float x = 0, y = 0, w = 0, h = 0; bool block = false, bar = false, main = false; COLORREF color = 0; };

double s_freezeFraction = -1;
bool s_solidBlink = false;
CaretStats s_stats;
std::map<HWND, int> s_origStyle;   // view -> caret style to restore

const UINT WM_KEX_CARET_UPDATE = WM_APP + 31;
const UINT_PTR TIMER_FRAME = 1, TIMER_WATCH = 2;
const UINT_PTR kSciSubclassId = 0x4B455843;   // 'KEXC'

class SmoothCaret;
SmoothCaret* s_self = nullptr;

class SmoothCaret : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"SmoothCaret"; }
    const wchar_t* title() const override { return L"Smooth caret"; }

    void loadOptions() override
    {
        _glideMs = std::max(0, std::min(1000, settings.getInt(key(), L"GlideMs", 80)));
        _snapLines = std::max(1, settings.getInt(key(), L"SnapAfterLines", 12));
        _fadeBlink = settings.getInt(key(), L"FadeBlink", 1) != 0;
        _overtypeBlock = settings.getInt(key(), L"OvertypeBlock", 1) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s) continue;
            LRESULT st = sci(s, SCI_GETCARETSTYLE);
            if (st < 0 || st > 0x1FF) { why = L"SCI_GETCARETSTYLE gave an unexpected value"; return false; }
            sci(s, SCI_SETCARETSTYLE, st);   // round trip: same value back
            if (sci(s, SCI_GETCARETSTYLE) != st) { why = L"SCI_SETCARETSTYLE does not round-trip"; return false; }
        }
        if (!views.sci(0)) { why = L"main Scintilla view not found"; return false; }
        HWND probe = createOverlayWindow(npp._nppHandle, false, L"probe");
        if (!probe) { why = L"cannot create a layered overlay window"; return false; }
        ::DestroyWindow(probe);
        return true;
    }

    bool enable() override
    {
        s_self = this;
        if (!createMsgWindow(L"KexSmoothCaret")) return false;
        hookViews();
        mainHooks.onMainMoved = [] { if (s_self) s_self->postUpdate(); };
        HWND f = ::GetFocus();
        int v = views.viewOf(f);
        _target = v >= 0 ? f : views.sci(views.current());
        _focused = (v >= 0);
        ::SetTimer(hwnd(), TIMER_WATCH, 300, nullptr);
        update(true);
        return true;
    }

    void disable() override
    {
        if (mainHooks.onMainMoved) mainHooks.onMainMoved = nullptr;
        if (hwnd()) { ::KillTimer(hwnd(), TIMER_FRAME); ::KillTimer(hwnd(), TIMER_WATCH); }
        for (HWND h : _hooked)
            if (::IsWindow(h)) ::RemoveWindowSubclass(h, sciSubclass, kSciSubclassId);
        _hooked.clear();
        for (auto& o : s_origStyle)
            if (::IsWindow(o.first)) sci(o.first, SCI_SETCARETSTYLE, o.second);   // exact original value
        s_origStyle.clear();
        if (_overlay) { ::DestroyWindow(_overlay); _overlay = nullptr; }
        _surf.release();
        destroyMsgWindow();
        _from.clear(); _to.clear();
        s_stats = CaretStats();
        s_self = nullptr;
    }

    void onNotify(SCNotification* n) override
    {
        switch (n->nmhdr.code)
        {
        case SCN_UPDATEUI:
            if (n->nmhdr.hwndFrom == _target) update(false);
            break;
        case SCN_ZOOM:
            if (n->nmhdr.hwndFrom == _target) update(true);
            break;
        case SCN_FOCUSIN:
            if (views.viewOf(static_cast<HWND>(n->nmhdr.hwndFrom)) >= 0) { _target = static_cast<HWND>(n->nmhdr.hwndFrom); _focused = true; update(true); }
            break;
        case SCN_FOCUSOUT:
            if (n->nmhdr.hwndFrom == _target) { _focused = false; hide(); }
            break;
        case NPPN_BUFFERACTIVATED:
        case NPPN_WORDSTYLESUPDATED:
            hookViews();
            postUpdate(true);
            break;
        }
    }

    void postUpdate(bool snap = false)
    {
        if (snap) _snapPending = true;
        if (!_posted && hwnd()) { _posted = true; ::PostMessageW(hwnd(), WM_KEX_CARET_UPDATE, 0, 0); }
    }

    // Called from the Scintilla subclass.
    void onSci(HWND h, UINT m)
    {
        switch (m)
        {
        case WM_SETFOCUS: _target = h; _focused = true; update(true); break;
        case WM_KILLFOCUS: if (h == _target) { _focused = false; hide(); } break;
        case WM_PAINT: case WM_SIZE: case WM_WINDOWPOSCHANGED: case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL:
            if (h == _target) postUpdate();
            break;
        case WM_KEYDOWN: case WM_CHAR: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE:
            if (h == _target) update(false);
            break;
        }
    }

    static LRESULT CALLBACK sciSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
    {
        if (m == WM_NCDESTROY)
        {
            ::RemoveWindowSubclass(h, sciSubclass, kSciSubclassId);
            return ::DefSubclassProc(h, m, w, l);
        }
        LRESULT r = ::DefSubclassProc(h, m, w, l);
        if (s_self)
        {
            if (m == WM_MOUSEMOVE && !(w & MK_LBUTTON)) return r;
            try { s_self->onSci(h, m); } catch (...) {}
        }
        return r;
    }

    CaretStats stats() const { return s_stats; }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_KEX_CARET_UPDATE)
        {
            _posted = false;
            bool snap = _snapPending; _snapPending = false;
            update(snap);
            return 0;
        }
        if (m == WM_TIMER && w == TIMER_FRAME)
        {
            render();
            return 0;
        }
        if (m == WM_TIMER && w == TIMER_WATCH)
        {
            // watchdog: redraw (cheap) so the native caret comes back if our overlay was lost
            if (_focused && _target) render();
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    void hookViews()
    {
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s || std::find(_hooked.begin(), _hooked.end(), s) != _hooked.end()) continue;
            if (::SetWindowSubclass(s, sciSubclass, kSciSubclassId, 0)) _hooked.push_back(s);
        }
    }

    static int hiddenStyleOf(int style) { return style & ~(CARETSTYLE_INS_MASK | CARETSTYLE_CURSES); }

    void hideScintillaCaret(HWND s)
    {
        int cur = static_cast<int>(sci(s, SCI_GETCARETSTYLE));
        auto it = s_origStyle.find(s);
        if (it == s_origStyle.end() || cur != hiddenStyleOf(it->second))
        {
            // First time, or Notepad++ re-applied its caret setting (Preferences > Editing): adopt it as the
            // value to restore, then hide Scintilla's caret again.
            s_origStyle[s] = cur;
            s_stats.lastStyleSaved = cur;
            sci(s, SCI_SETCARETSTYLE, hiddenStyleOf(cur));
        }
    }

    void showScintillaCaret(HWND s)
    {
        auto it = s_origStyle.find(s);
        if (it != s_origStyle.end() && ::IsWindow(s) && sci(s, SCI_GETCARETSTYLE) == hiddenStyleOf(it->second))
            sci(s, SCI_SETCARETSTYLE, it->second);
    }

    static bool cloaked(HWND h)
    {
        DWORD c = 0;
        return SUCCEEDED(::DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
    }

    void hide()
    {
        for (auto& o : s_origStyle) showScintillaCaret(o.first);   // nothing of ours on screen: native caret back
        if (_overlay && ::IsWindowVisible(_overlay)) ::ShowWindow(_overlay, SW_HIDE);
        if (hwnd()) ::KillTimer(hwnd(), TIMER_FRAME);
        _timerMs = 0;
        s_stats.visible = false;
    }

    bool ensureOverlay()
    {
        HWND owner = _target ? ::GetAncestor(_target, GA_ROOT) : npp._nppHandle;
        if (_overlay && owner != _owner) { ::DestroyWindow(_overlay); _overlay = nullptr; }
        if (!_overlay)
        {
            _owner = owner;
            _overlay = createOverlayWindow(owner, false, L"KEX caret");
        }
        return _overlay != nullptr;
    }

    // Caret rectangles in content coordinates (client + scroll offset), like Scintilla's DrawCarets.
    std::vector<Car> compute(HWND s)
    {
        std::vector<Car> out;
        // Notepad++ re-applied its caret setting (Preferences > Editing) while ours was showing: adopt it
        auto known = s_origStyle.find(s);
        int now = static_cast<int>(sci(s, SCI_GETCARETSTYLE));
        if (known != s_origStyle.end() && now != known->second && now != hiddenStyleOf(known->second)) known->second = now;
        int style = s_origStyle.count(s) ? s_origStyle[s] : CARETSTYLE_LINE;
        int width = static_cast<int>(sci(s, SCI_GETCARETWIDTH));
        bool overtype = sci(s, SCI_GETOVERTYPE) != 0;
        bool insBlock = (style & CARETSTYLE_INS_MASK) == CARETSTYLE_BLOCK;
        bool block = overtype ? (_overtypeBlock || (style & CARETSTYLE_OVERSTRIKE_BLOCK)) : insBlock;
        bool bar = overtype && !block;
        bool blockAfter = (style & CARETSTYLE_BLOCK_AFTER) != 0;
        if (width <= 0 && !block && !bar) return out;
        bool addVisible = sci(s, SCI_GETADDITIONALCARETSVISIBLE) != 0;
        int n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        int mainSel = static_cast<int>(sci(s, SCI_GETMAINSELECTION));
        float spaceW = static_cast<float>(sci(s, SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<LPARAM>(" ")));
        float aveW = static_cast<float>(sci(s, SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<LPARAM>("n")));
        Sci_Position docLen = sci(s, SCI_GETLENGTH);
        COLORREF mainColor = caretColor(s, true), addColor = caretColor(s, false);
        // x of column 0 (text start, minus horizontal scroll): Scintilla only shifts a line caret left
        // when it is past column 0, otherwise the caret would vanish under the margin
        float col0 = static_cast<float>(sci(s, SCI_GETMARGINLEFT) - sci(s, SCI_GETXOFFSET));
        for (int m = 0, nm = static_cast<int>(sci(s, SCI_GETMARGINS)); m < nm; ++m) col0 += static_cast<float>(sci(s, SCI_GETMARGINWIDTHN, m));
        for (int i = 0; i < n && out.size() < 400; ++i)
        {
            bool isMain = (i == mainSel);
            if (!isMain && !addVisible) continue;
            Sci_Position caret = sci(s, SCI_GETSELECTIONNCARET, i);
            Sci_Position anchor = sci(s, SCI_GETSELECTIONNANCHOR, i);
            Sci_Position vs = sci(s, SCI_GETSELECTIONNCARETVIRTUALSPACE, i);
            Sci_Position avs = sci(s, SCI_GETSELECTIONNANCHORVIRTUALSPACE, i);
            if (block && !blockAfter && (caret > anchor || (caret == anchor && vs > avs)))
            {
                if (vs > 0) --vs;
                else caret = sci(s, SCI_POSITIONBEFORE, caret);
            }
            Sci_Position line = sci(s, SCI_LINEFROMPOSITION, caret);
            if (!sci(s, SCI_GETLINEVISIBLE, line)) continue;
            Car c;
            float x = static_cast<float>(sci(s, SCI_POINTXFROMPOSITION, 0, caret)) + vs * spaceW;
            float y = static_cast<float>(sci(s, SCI_POINTYFROMPOSITION, 0, caret));
            float h = static_cast<float>(sci(s, SCI_TEXTHEIGHT, line));
            c.main = isMain; c.color = isMain ? mainColor : addColor;
            float charW = aveW;
            if (block || bar)
            {
                Sci_Position le = sci(s, SCI_GETLINEENDPOSITION, line);
                if (vs == 0 && caret < le && caret < docLen)
                {
                    Sci_Position nx = sci(s, SCI_POSITIONAFTER, caret);
                    float x2 = static_cast<float>(sci(s, SCI_POINTXFROMPOSITION, 0, nx));
                    if (x2 > x) charW = x2 - x;
                }
                charW = std::max(charW, 3.0f);
            }
            if (block) { c.x = x; c.w = charW; c.y = y; c.h = h; c.block = true; }
            else if (bar) { c.x = x + 1; c.w = charW - 1; c.y = y + h - 2; c.h = 2; c.bar = true; }
            else { c.x = std::round(x - (x > col0 + 0.5f ? 0.51f : 0)); c.w = static_cast<float>(width); c.y = y; c.h = h; }
            out.push_back(c);
        }
        return out;
    }

    static COLORREF caretColor(HWND s, bool main)
    {
        LRESULT c = sci(s, SCI_GETELEMENTCOLOUR, main ? SC_ELEMENT_CARET : SC_ELEMENT_CARET_ADDITIONAL);
        if (c == 0 && main) c = sci(s, SCI_GETCARETFORE);
        return static_cast<COLORREF>(c & 0xFFFFFF);
    }

    void update(bool snap)
    {
        if (!_target || !::IsWindow(_target)) { hide(); return; }
        if (!_focused || !::IsWindowVisible(_target) || ::IsIconic(::GetAncestor(_target, GA_ROOT))) { hide(); return; }
        if (!s_origStyle.count(_target)) s_origStyle[_target] = static_cast<int>(sci(_target, SCI_GETCARETSTYLE));
        float lh = static_cast<float>(sci(_target, SCI_TEXTHEIGHT, 0));
        float fvl = static_cast<float>(sci(_target, SCI_GETFIRSTVISIBLELINE));
        float xo = static_cast<float>(sci(_target, SCI_GETXOFFSET));
        std::vector<Car> t = compute(_target);
        for (Car& c : t) { c.x += xo; c.y += fvl * lh; }     // content coordinates
        double now = nowMs();
        bool changed = t.size() != _to.size();
        if (!changed)
            for (size_t i = 0; i < t.size(); ++i)
                if (std::fabs(t[i].x - _to[i].x) > 0.1f || std::fabs(t[i].y - _to[i].y) > 0.1f ||
                    std::fabs(t[i].w - _to[i].w) > 0.1f || std::fabs(t[i].h - _to[i].h) > 0.1f || t[i].block != _to[i].block)
                { changed = true; break; }
        bool bigJump = false;
        if (!snap && t.size() == _to.size() && !t.empty())
        {
            float dy = std::fabs(t[0].y - currentOf(0, now).y);
            bigJump = dy > _snapLines * lh;
        }
        if (snap || t.size() != _to.size() || bigJump || lh != _lh || _glideMs == 0)
        {
            _from = t; _to = t; _glideStart = now - 100000;
        }
        else if (changed)
        {
            std::vector<Car> from;
            for (size_t i = 0; i < t.size(); ++i) from.push_back(currentOf(i, now));
            _from = from; _to = t; _glideStart = now;
        }
        if (changed || snap) _blinkStart = now;   // visible right after a move, like Scintilla
        _lh = lh; _fvl = fvl; _xo = xo;
        _period = static_cast<int>(sci(_target, SCI_GETCARETPERIOD));
        _addBlink = sci(_target, SCI_GETADDITIONALCARETSBLINK) != 0;
        render();
    }

    Car currentOf(size_t i, double now) const
    {
        if (i >= _to.size()) return Car();
        if (i >= _from.size()) return _to[i];
        double g = _glideMs > 0 ? (now - _glideStart) / _glideMs : 1.0;
        if (g >= 1.0) return _to[i];
        if (g < 0) g = 0;
        if (s_freezeFraction >= 0 && g > s_freezeFraction) g = s_freezeFraction;
        double e = 1.0 - (1.0 - g) * (1.0 - g) * (1.0 - g);   // ease-out cubic
        Car c = _to[i];
        c.x = static_cast<float>(_from[i].x + (_to[i].x - _from[i].x) * e);
        c.y = static_cast<float>(_from[i].y + (_to[i].y - _from[i].y) * e);
        c.w = static_cast<float>(_from[i].w + (_to[i].w - _from[i].w) * e);
        c.h = static_cast<float>(_from[i].h + (_to[i].h - _from[i].h) * e);
        return c;
    }

    float blinkAlpha(double now, double* nextChangeMs) const
    {
        *nextChangeMs = -1;
        if (s_solidBlink || _period <= 0) return 1.0f;
        double P = _period;
        double F = _fadeBlink ? std::min(140.0, P / 3) : 0.0;
        double ph = std::fmod(now - _blinkStart, 2 * P);
        if (ph < P - F) { *nextChangeMs = (P - F) - ph; return 1.0f; }
        if (ph < P) { *nextChangeMs = 0; return static_cast<float>(1.0 - (ph - (P - F)) / F); }
        if (ph < 2 * P - F) { *nextChangeMs = (2 * P - F) - ph; return 0.0f; }
        *nextChangeMs = 0;
        return F > 0 ? static_cast<float>((ph - (2 * P - F)) / F) : 1.0f;
    }

    void render()
    {
        if (!_target || !_focused) { hide(); return; }
        double now = nowMs();
        // current scroll position: keeps the caret glued to the text while scrolling
        float lh = static_cast<float>(sci(_target, SCI_TEXTHEIGHT, 0));
        float fvl = static_cast<float>(sci(_target, SCI_GETFIRSTVISIBLELINE));
        float xo = static_cast<float>(sci(_target, SCI_GETXOFFSET));
        if (lh != _lh) { update(true); return; }
        RECT client; ::GetClientRect(_target, &client);
        int margins = 0;
        int nm = static_cast<int>(sci(_target, SCI_GETMARGINS));
        for (int i = 0; i < nm; ++i) margins += static_cast<int>(sci(_target, SCI_GETMARGINWIDTHN, i));
        double nextMs;
        float alpha = blinkAlpha(now, &nextMs);
        std::vector<Car> draw;
        bool gliding = false;
        for (size_t i = 0; i < _to.size(); ++i)
        {
            Car c = currentOf(i, now);
            if (_glideMs > 0 && now - _glideStart < _glideMs && !(s_freezeFraction >= 0)) gliding = true;
            c.x -= xo; c.y -= fvl * lh;
            // clip to the text area
            float l = std::max(c.x, static_cast<float>(margins)), r = std::min(c.x + c.w, static_cast<float>(client.right));
            float t = std::max(c.y, 0.0f), b = std::min(c.y + c.h, static_cast<float>(client.bottom));
            if (r <= l || b <= t) continue;
            c.x = l; c.w = r - l; c.y = t; c.h = b - t;
            draw.push_back(c);
        }
        s_stats.carets = static_cast<int>(draw.size());
        s_stats.target = _target;
        if (draw.empty() || !ensureOverlay()) { hide(); scheduleFrames(false, -1); return; }
        float L = 1e9f, T = 1e9f, R = -1e9f, B = -1e9f;
        for (const Car& c : draw) { L = std::min(L, c.x); T = std::min(T, c.y); R = std::max(R, c.x + c.w); B = std::max(B, c.y + c.h); }
        int il = static_cast<int>(std::floor(L)) - 1, it = static_cast<int>(std::floor(T)) - 1;
        int iw = static_cast<int>(std::ceil(R)) + 1 - il, ih = static_cast<int>(std::ceil(B)) + 1 - it;
        if (!_surf.ensure(iw, ih)) { hide(); return; }
        _surf.clear();
        for (const Car& c : draw)
        {
            float a = (c.main || _addBlink) ? alpha : 1.0f;
            if (c.block) a *= 0.55f;          // translucent block so the character stays readable
            fillRectF(_surf, c.x - il, c.y - it, c.x - il + c.w, c.y - it + c.h, c.color, a);
            if (c.main) { s_stats.mainX = c.x; s_stats.mainY = c.y; s_stats.mainH = c.h; s_stats.block = c.block; }
        }
        POINT pos{ il, it };
        ::ClientToScreen(_target, &pos);
        SIZE sz{ iw, ih };
        pushLayered(_overlay, _surf, pos, sz);
        if (!::IsWindowVisible(_overlay))
        {
            // lowest of the owner's popups so Notepad++'s own dialogs (Find, autocomplete) stay above it
            HWND above = ::GetWindow(_owner, GW_HWNDPREV);
            ::SetWindowPos(_overlay, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (!::IsWindowVisible(_overlay) || (cloaked(_overlay) && !cloaked(_owner)))
        {
            // our caret isn't really on screen (e.g. the overlay stayed on another virtual desktop):
            // show the normal caret and start over with a fresh overlay next time
            showScintillaCaret(_target);
            ::DestroyWindow(_overlay); _overlay = nullptr;
            s_stats.visible = false;
            scheduleFrames(false, -1);
            return;
        }
        hideScintillaCaret(_target);
        s_stats.visible = true;
        s_stats.overlay = RECT{ pos.x, pos.y, pos.x + iw, pos.y + ih };
        scheduleFrames(gliding, nextMs);
    }

    void scheduleFrames(bool gliding, double nextBlinkMs)
    {
        if (!hwnd()) return;
        int ms;
        if (gliding || nextBlinkMs == 0) ms = 15;
        else if (nextBlinkMs > 0) ms = static_cast<int>(nextBlinkMs) + 1;
        else ms = 0;
        if (ms == 0) { ::KillTimer(hwnd(), TIMER_FRAME); _timerMs = 0; return; }
        if (ms != _timerMs) { ::SetTimer(hwnd(), TIMER_FRAME, ms, nullptr); _timerMs = ms; }
    }

    HWND _target = nullptr, _overlay = nullptr, _owner = nullptr;
    bool _focused = false, _posted = false, _snapPending = false, _fadeBlink = true, _overtypeBlock = true, _addBlink = true;
    int _glideMs = 80, _snapLines = 12, _period = 500, _timerMs = 0;
    float _lh = 0, _fvl = 0, _xo = 0;
    double _glideStart = 0, _blinkStart = 0;
    std::vector<Car> _from, _to;
    std::vector<HWND> _hooked;
    Surface _surf;
};

} // namespace

Module* createSmoothCaret() { return new SmoothCaret(); }
CaretStats caretStats() { return s_stats; }
void caretTestFreeze(double f, bool solid) { s_freezeFraction = f; s_solidBlink = solid; if (s_self) s_self->postUpdate(); }
int caretOriginalStyle(HWND h) { auto it = s_origStyle.find(h); return it == s_origStyle.end() ? -1 : it->second; }

} // namespace kex
