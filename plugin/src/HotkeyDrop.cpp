// HotkeyDrop.cpp - global hotkey (RegisterHotKey, default Ctrl+Alt+M) that drops Notepad++ down in
// F12 "post-it" mode on the current desktop, and hides it again when it is already in front.
#include "KexSuite.h"
#include <cmath>

namespace kex {

namespace {

const int HOTKEY_ID = 0x4B45;
const UINT_PTR TIMER_SLIDE = 1;
HotkeyStats s_stats;
bool s_hid = false;

std::wstring keyName(UINT vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) { wchar_t b[8]; swprintf(b, 8, L"F%u", vk - VK_F1 + 1); return b; }
    UINT sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t b[64] = {};
    LONG lp = static_cast<LONG>(sc << 16);
    switch (vk) { case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
                  case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: lp |= 1 << 24; break; }
    if (::GetKeyNameTextW(lp, b, 63) > 0) return b;
    swprintf(b, 64, L"VK 0x%02X", vk);
    return b;
}

class HotkeyDrop : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"HotkeyDropDown"; }
    const wchar_t* title() const override { return L"Global hotkey drop-down"; }

    void loadOptions() override
    {
        _mods = static_cast<UINT>(settings.getInt(key(), L"Modifiers", MOD_CONTROL | MOD_ALT));
        _vk = static_cast<UINT>(settings.getInt(key(), L"Key", 'M'));
        _postIt = settings.getInt(key(), L"SwitchOnPostIt", 1) != 0;
        _slide = settings.getInt(key(), L"SlideAnimation", 1) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        if (!mm || ::GetMenuState(mm, IDM_VIEW_POSTIT, MF_BYCOMMAND) == static_cast<UINT>(-1))
        { why = L"View > Post-It (IDM_VIEW_POSTIT) not found in the main menu"; return false; }
        wchar_t cls[64] = {};
        ::GetClassNameW(npp._nppHandle, cls, 63);
        if (wcscmp(cls, L"Notepad++")) { why = L"unexpected main window class"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexHotkey")) return false;
        registerKey();
        if (!s_stats.registered && !selfTestMode)
            showNote(L"KEX Suite: the hotkey " + s_stats.text + L" is already taken by another program. Pick another one in Plugins > KEX Suite > Options.", 8000);
        return true;
    }

    void disable() override
    {
        if (hwnd())
        {
            if (s_stats.registered) ::UnregisterHotKey(hwnd(), HOTKEY_ID);
            ::KillTimer(hwnd(), TIMER_SLIDE);
        }
        s_stats.registered = false;
        if (_sliding) finishSlide();
        if (s_hid) { ::ShowWindow(npp._nppHandle, SW_SHOW); s_hid = false; }   // never leave Notepad++ invisible
        destroyMsgWindow();
    }

    bool registerKey()
    {
        if (!hwnd()) return false;
        if (s_stats.registered) ::UnregisterHotKey(hwnd(), HOTKEY_ID);
        s_stats.registered = ::RegisterHotKey(hwnd(), HOTKEY_ID, _mods | MOD_NOREPEAT, _vk) != FALSE;
        s_stats.lastError = s_stats.registered ? 0 : ::GetLastError();
        if (!s_stats.registered && s_stats.lastError == ERROR_INVALID_PARAMETER)
        {
            // MOD_NOREPEAT needs Windows 7; retry without it
            s_stats.registered = ::RegisterHotKey(hwnd(), HOTKEY_ID, _mods, _vk) != FALSE;
            s_stats.lastError = s_stats.registered ? 0 : ::GetLastError();
        }
        s_stats.mods = _mods; s_stats.vk = _vk;
        s_stats.text = text(_mods, _vk);
        log(L"hotkey %ls: %ls (error %lu)", s_stats.text.c_str(), s_stats.registered ? L"registered" : L"NOT registered", s_stats.lastError);
        return s_stats.registered;
    }

    static std::wstring text(UINT mods, UINT vk)
    {
        std::wstring t;
        if (mods & MOD_CONTROL) t += L"Ctrl+";
        if (mods & MOD_ALT) t += L"Alt+";
        if (mods & MOD_SHIFT) t += L"Shift+";
        if (mods & MOD_WIN) t += L"Win+";
        return t + keyName(vk);
    }

    void setKey(UINT mods, UINT vk) { _mods = mods; _vk = vk; }

    void press()
    {
        ++s_stats.presses;
        HWND n = npp._nppHandle;
        HWND fg = ::GetForegroundWindow();
        bool ours = false;
        if (fg)
        {
            HWND root = ::GetAncestor(fg, GA_ROOTOWNER);
            ours = (root == n || fg == n);
            for (HWND h : views.hostWindows()) if (root == h || fg == h) ours = true;
        }
        bool visible = ::IsWindowVisible(n) && !::IsIconic(n);
        DesktopProvider* p = desktopProvider();
        bool here = !(p && p->ok()) || p->isOnCurrent(n);
        if (visible && ours && here && !_sliding) dropUp();
        else dropDown(ours ? nullptr : fg);
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_HOTKEY && w == HOTKEY_ID) { log(L"WM_HOTKEY received"); press(); return 0; }
        if (m == WM_TIMER && w == TIMER_SLIDE) { slideStep(); return 0; }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    void dropDown(HWND previous)
    {
        HWND n = npp._nppHandle;
        if (previous && previous != n) _prevFg = previous;
        if (_sliding) finishSlide();
        desktopSummonHere(L"hotkey");                 // current desktop first (registers it for the per-desktop feature)
        if (!::IsWindowVisible(n) && s_hid) { ::ShowWindow(n, SW_SHOWNA); }
        s_hid = false;
        restoreNppWindow();                             // from the tray or minimized, Notepad++'s own way
        if (_postIt && !isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        if (!::IsWindowVisible(n)) ::ShowWindow(n, SW_SHOW);
        ::SetForegroundWindow(n);
        HWND s = views.sci(views.current());
        if (s) ::SetFocus(s);
        if (_slide) startSlide(true);
        log(L"hotkey: dropped down (post-it=%d)", isPostItMode() ? 1 : 0);
    }

    void dropUp()
    {
        if (_slide) { startSlide(false); return; }
        hideNow();
    }

    void hideNow()
    {
        ::ShowWindow(npp._nppHandle, SW_HIDE);
        s_hid = true;
        if (_prevFg && ::IsWindow(_prevFg) && ::IsWindowVisible(_prevFg)) ::SetForegroundWindow(_prevFg);
        log(L"hotkey: hidden, focus back to %p", _prevFg);
    }

    void startSlide(bool down)
    {
        HWND n = npp._nppHandle;
        ::GetWindowRect(n, &_final);
        HMONITOR mon = ::MonitorFromWindow(n, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(mon, &mi);
        int h = _final.bottom - _final.top;
        _hiddenTop = mi.rcMonitor.top - h;
        _down = down;
        _sliding = true;
        _t0 = nowMs();
        if (down) ::SetWindowPos(n, nullptr, _final.left, _hiddenTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ::SetTimer(hwnd(), TIMER_SLIDE, 15, nullptr);
    }

    void slideStep()
    {
        double t = (nowMs() - _t0) / 140.0;
        if (t >= 1.0) { finishSlide(); return; }
        double e = 1.0 - (1.0 - t) * (1.0 - t);
        int y = _down ? static_cast<int>(_hiddenTop + (_final.top - _hiddenTop) * e)
                      : static_cast<int>(_final.top + (_hiddenTop - _final.top) * e);
        ::SetWindowPos(npp._nppHandle, nullptr, _final.left, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void finishSlide()
    {
        ::KillTimer(hwnd(), TIMER_SLIDE);
        _sliding = false;
        if (_down)
            ::SetWindowPos(npp._nppHandle, nullptr, _final.left, _final.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        else
        {
            hideNow();
            // put it back where it was, still hidden, so the next drop-down lands in the same place
            ::SetWindowPos(npp._nppHandle, nullptr, _final.left, _final.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    UINT _mods = MOD_CONTROL | MOD_ALT, _vk = 'M';
    bool _postIt = true, _slide = true, _sliding = false, _down = true;
    HWND _prevFg = nullptr;
    RECT _final{};
    int _hiddenTop = 0;
    double _t0 = 0;
};

HotkeyDrop* s_mod = nullptr;

} // namespace

Module* createHotkeyDrop() { s_mod = new HotkeyDrop(); return s_mod; }
HotkeyStats hotkeyStats() { return s_stats; }
void hotkeyPress() { if (s_mod && s_mod->isEnabled()) s_mod->press(); }
bool hotkeyHidWindow() { return s_hid; }
void hotkeyForgetHidden() { s_hid = false; }
std::wstring hotkeyText(UINT mods, UINT vk) { return HotkeyDrop::text(mods, vk); }
// Options dialog: store and (if the feature is on) re-register. Returns true when registered.
bool hotkeyApply(UINT mods, UINT vk)
{
    if (!s_mod) return false;
    settings.setInt(s_mod->key(), L"Modifiers", static_cast<int>(mods));
    settings.setInt(s_mod->key(), L"Key", static_cast<int>(vk));
    s_mod->loadOptions();
    if (!s_mod->isEnabled())
    {
        s_stats.text = HotkeyDrop::text(mods, vk);
        return false;
    }
    return s_mod->registerKey();
}

} // namespace kex
