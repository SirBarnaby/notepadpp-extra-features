// HotkeyDrop.cpp - quick note: a global hotkey (RegisterHotKey, default Ctrl+Alt+M) fades a small
// Notepad++-styled note window in at the centre of the screen (or where it was left), over whatever
// program is in front and whether or not Notepad++ is minimised. Pressing it again fades the note out
// and gives the focus back. The text is saved to kex-quicknote.txt in Notepad++'s settings folder as
// you type. The note has no title bar: its top edge moves it, the other edges and corners resize it.
#include "KexSuite.h"
#include <dwmapi.h>
#include <cmath>
#include <climits>

namespace kex {

namespace {

const int HOTKEY_ID = 0x4B45;
const UINT_PTR TIMER_FADE = 1, TIMER_SAVE = 2, TIMER_TAKEOVER = 3;
const double FADE_MS = 150.0;
const int FRAME96 = 7;                 // the edge around the text: move (top) and resize (the rest)
const wchar_t* NOTE_CLASS = L"KexQuickNote";
HotkeyStats s_stats;

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

int systemDpi()
{
    HDC dc = ::GetDC(nullptr);
    int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ::ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

bool isCloaked(HWND h)
{
    DWORD c = 0;
    return SUCCEEDED(::DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

class QuickNote : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"HotkeyDropDown"; }   // INI section kept from the drop-down
    const wchar_t* title() const override { return L"Quick note (global hotkey)"; }

    void loadOptions() override
    {
        _mods = static_cast<UINT>(settings.getInt(key(), L"Modifiers", MOD_CONTROL | MOD_ALT));
        _vk = static_cast<UINT>(settings.getInt(key(), L"Key", 'M'));
        _keepText = settings.getInt(key(), L"KeepText", 1) != 0;
        _fade = settings.getInt(key(), L"Fade", 1) != 0;
        _w96 = std::max(240, settings.getInt(key(), L"Width", 640));
        _h96 = std::max(140, settings.getInt(key(), L"Height", 420));
        _lineNumbers = settings.getInt(key(), L"LineNumbers", 0) != 0;
        _wrap = settings.getInt(key(), L"WordWrap", 1) != 0;
        _caretLine = settings.getInt(key(), L"CurrentLine", 0) != 0;
        _onTop = settings.getInt(key(), L"OnTop", 1) != 0;
        _remember = settings.getInt(key(), L"RememberPlace", 0) != 0;
        _x = settings.getInt(key(), L"X", INT_MIN);
        _y = settings.getInt(key(), L"Y", INT_MIN);
        _zoom = settings.getInt(key(), L"Zoom", INT_MIN);
    }

    bool selfCheck(std::wstring& why) override
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!::GetClassInfoExW(::GetModuleHandleW(nullptr), L"Scintilla", &wc))
        { why = L"Notepad++'s Scintilla window class is not registered"; return false; }
        wchar_t cls[64] = {};
        ::GetClassNameW(npp._nppHandle, cls, 63);
        if (wcscmp(cls, L"Notepad++")) { why = L"unexpected main window class"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexHotkey")) return false;
        registerKey();
        if (!s_stats.registered && s_stats.byOtherNpp)
            ::SetTimer(hwnd(), TIMER_TAKEOVER, 3000, nullptr);   // take the hotkey over once that window closes
        else if (!s_stats.registered && !selfTestMode)
            showNote(L"KEX: the quick note hotkey " + s_stats.text + L" is already taken by another program. Pick another one in Settings > KEX Features.", 8000);
        return true;
    }

    void disable() override
    {
        if (hwnd()) ::KillTimer(hwnd(), TIMER_TAKEOVER);
        if (hwnd() && s_stats.registered) ::UnregisterHotKey(hwnd(), HOTKEY_ID);
        s_stats.registered = false;
        destroyNote(true);
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
        // With a Notepad++ for each desktop, the first window holds the hotkey and opens the note on
        // whichever desktop the user is on.
        s_stats.byOtherNpp = !s_stats.registered && s_stats.lastError == ERROR_HOTKEY_ALREADY_REGISTERED && otherNotepadPlusPlus();
        s_stats.text = text(_mods, _vk);
        log(L"hotkey %ls: %ls (error %lu)", s_stats.text.c_str(), s_stats.registered ? L"registered" : L"NOT registered", s_stats.lastError);
        return s_stats.registered;
    }

    static bool otherNotepadPlusPlus()
    {
        HWND h = nullptr;
        while ((h = ::FindWindowExW(nullptr, h, L"Notepad++", nullptr)) != nullptr)
        {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid != ::GetCurrentProcessId()) return true;
        }
        return false;
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

    void press()
    {
        ++s_stats.presses;
        if (_note && isCloaked(_note)) destroyNote(true);     // left open on another desktop: start again here
        if (!_note) { open(); return; }
        if (_closing) { fadeTo(255); _closing = false; front(); return; }
        HWND fg = ::GetForegroundWindow();
        if (fg && (fg == _note || ::GetAncestor(fg, GA_ROOT) == _note)) close();
        else front();
    }

    HWND noteWindow() const { return _closing ? nullptr : _note; }
    HWND editor() const { return _closing ? nullptr : _sci; }
    std::wstring file() const { return settings.dir() + L"\\kex-quicknote.txt"; }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_HOTKEY && w == HOTKEY_ID) { log(L"WM_HOTKEY received"); press(); return 0; }
        if (m == WM_TIMER && w == TIMER_TAKEOVER)
        {
            if (registerKey() || !s_stats.byOtherNpp) { ::KillTimer(hwnd(), TIMER_TAKEOVER); refreshPrefPages(); }
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    // ---- the note window ------------------------------------------------------------------------
    void open()
    {
        if (!registerClass()) return;
        HWND fg = ::GetForegroundWindow();
        _prevFg = (fg && fg != _note) ? fg : nullptr;

        POINT pt{}; ::GetCursorPos(&pt);
        MONITORINFO mi{ sizeof(mi) };
        ::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        RECT wa = mi.rcWork;
        int dpi = systemDpi();
        int w = std::min<int>(::MulDiv(_w96, dpi, 96), wa.right - wa.left - 40);
        int h = std::min<int>(::MulDiv(_h96, dpi, 96), wa.bottom - wa.top - 40);
        int x = wa.left + (wa.right - wa.left - w) / 2;
        int y = wa.top + (wa.bottom - wa.top - h) / 2;
        if (_remember && _x != INT_MIN && _y != INT_MIN)
        {
            // where it was left, if that is still on a screen
            POINT tl{ _x, _y }, br{ _x + w - 1, _y + h - 1 };
            if (::MonitorFromPoint(tl, MONITOR_DEFAULTTONULL) && ::MonitorFromPoint(br, MONITOR_DEFAULTTONULL)) { x = _x; y = _y; }
        }

        std::wstring caption = L"Quick note  (" + s_stats.text + L" or Esc to hide)";
        // Layered for its whole life when it fades: switching the style on a shown window is what made
        // the fade skip now and then.
        _layered = _fade && !binaryAlpha();
        DWORD ex = WS_EX_TOOLWINDOW | (_onTop ? WS_EX_TOPMOST : 0) | (_layered ? WS_EX_LAYERED : 0);
        // Unowned, so it shows even while Notepad++ is minimised; created fresh each time, so it opens on
        // the desktop you are looking at. No title bar: the edge around the text moves and sizes it.
        _note = ::CreateWindowExW(ex, NOTE_CLASS, caption.c_str(), WS_POPUP | WS_CLIPCHILDREN,
                                  x, y, w, h, nullptr, nullptr, hInst, this);
        if (!_note) { log(L"quick note: CreateWindowEx failed (%lu)", ::GetLastError()); return; }
        if (nppMsg(NPPM_ISDARKMODEENABLED, 0, 0))
        {
            BOOL dark = TRUE;
            ::DwmSetWindowAttribute(_note, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
        _sci = ::CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                 _note, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!_sci) { log(L"quick note: Scintilla failed (%lu)", ::GetLastError()); destroyNote(false); return; }
        ::SetWindowSubclass(_sci, editorSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
        styleLike(views.sci(views.current()));
        load();
        layout();

        _closing = false;
        if (_layered) { _alpha = 0; ::SetLayeredWindowAttributes(_note, 0, 0, LWA_ALPHA); }
        else _alpha = 255;
        ::ShowWindow(_note, SW_SHOWNA);
        // paint the text before the first visible frame, so the fade shows the note and not a blank box
        ::RedrawWindow(_note, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        front();
        if (_layered) fadeTo(255);
        s_stats.noteOpen = true;
        log(L"quick note: opened at %d,%d %dx%d, previous window %p", x, y, w, h, _prevFg);
    }

    void front()
    {
        if (!_note) return;
        ::SetWindowPos(_note, _onTop ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ::SetForegroundWindow(_note);
        ::SetFocus(_sci);
    }

    void close()
    {
        if (!_note || _closing) return;
        save();
        RECT r{}; ::GetWindowRect(_note, &r);
        int dpi = systemDpi();
        settings.setInt(key(), L"Width", ::MulDiv(r.right - r.left, 96, dpi));
        settings.setInt(key(), L"Height", ::MulDiv(r.bottom - r.top, 96, dpi));
        settings.setInt(key(), L"X", r.left);
        settings.setInt(key(), L"Y", r.top);
        if (_sci) settings.setInt(key(), L"Zoom", static_cast<int>(sci(_sci, SCI_GETZOOM)));
        loadOptions();
        _closing = true;
        s_stats.noteOpen = false;
        if (_prevFg && ::IsWindow(_prevFg) && ::IsWindowVisible(_prevFg) && !::IsIconic(_prevFg)) ::SetForegroundWindow(_prevFg);
        log(L"quick note: closed, focus back to %p", _prevFg);
        if (_layered) fadeTo(0);
        else destroyNote(false);
    }

    void destroyNote(bool saveFirst)
    {
        if (!_note) return;
        if (saveFirst && !_closing) save();
        HWND n = _note;
        _note = nullptr; _sci = nullptr; _closing = false;
        s_stats.noteOpen = false;
        ::DestroyWindow(n);
    }

    void fadeTo(int target)
    {
        _alphaFrom = _alpha; _alphaTo = target; _fadeT0 = nowMs();
        ::SetTimer(_note, TIMER_FADE, 10, nullptr);
        fadeStep();
    }

    void fadeStep()
    {
        double t = std::min(1.0, (nowMs() - _fadeT0) / FADE_MS);
        double e = 1.0 - (1.0 - t) * (1.0 - t);
        _alpha = static_cast<int>(std::lround(_alphaFrom + (_alphaTo - _alphaFrom) * e));
        ::SetLayeredWindowAttributes(_note, 0, static_cast<BYTE>(_alpha), LWA_ALPHA);
        if (t < 1.0) return;
        ::KillTimer(_note, TIMER_FADE);
        if (_alphaTo == 0) destroyNote(false);
    }

    int frame() const { return ::MulDiv(FRAME96, systemDpi(), 96); }

    void layout()
    {
        if (!_note || !_sci) return;
        RECT rc{}; ::GetClientRect(_note, &rc);
        const int f = frame();
        ::MoveWindow(_sci, f, f, std::max(0L, rc.right - 2 * f), std::max(0L, rc.bottom - 2 * f), TRUE);
    }

    // the edge around the text, in the editor's background with a hairline in a quieter text colour
    void paintFrame(HDC dc)
    {
        RECT rc{}; ::GetClientRect(_note, &rc);
        const COLORREF back = _sci ? static_cast<COLORREF>(sci(_sci, SCI_STYLEGETBACK, STYLE_DEFAULT)) : RGB(255, 255, 255);
        const COLORREF fore = _sci ? static_cast<COLORREF>(sci(_sci, SCI_STYLEGETFORE, STYLE_DEFAULT)) : RGB(0, 0, 0);
        auto mix = [](COLORREF a, COLORREF b, int pct) {
            return RGB((GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100, (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100,
                       (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100);
        };
        HBRUSH bb = ::CreateSolidBrush(back);
        ::FillRect(dc, &rc, bb);
        ::DeleteObject(bb);
        HBRUSH lb = ::CreateSolidBrush(mix(back, fore, 35));
        ::FrameRect(dc, &rc, lb);
        ::DeleteObject(lb);
        // a short grip in the middle of the top edge says "drag here"
        const int f = frame(), gw = ::MulDiv(36, systemDpi(), 96);
        RECT g{ (rc.right - gw) / 2, f / 2 - 1, (rc.right + gw) / 2, f / 2 + 1 };
        HBRUSH gb = ::CreateSolidBrush(mix(back, fore, 30));
        ::FillRect(dc, &g, gb);
        ::DeleteObject(gb);
    }

    LRESULT hitTest(HWND h, LPARAM l)
    {
        POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
        ::ScreenToClient(h, &pt);
        RECT rc{}; ::GetClientRect(h, &rc);
        const int f = frame(), c = f * 2;
        const bool left = pt.x < f, right = pt.x >= rc.right - f, top = pt.y < f, bottom = pt.y >= rc.bottom - f;
        const bool nearLeft = pt.x < c, nearRight = pt.x >= rc.right - c, nearTop = pt.y < c, nearBottom = pt.y >= rc.bottom - c;
        if ((top && nearLeft) || (left && nearTop)) return HTTOPLEFT;
        if ((top && nearRight) || (right && nearTop)) return HTTOPRIGHT;
        if ((bottom && nearLeft) || (left && nearBottom)) return HTBOTTOMLEFT;
        if ((bottom && nearRight) || (right && nearBottom)) return HTBOTTOMRIGHT;
        if (top) return HTCAPTION;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    void updateMargins()
    {
        if (!_sci) return;
        if (!_lineNumbers) { sci(_sci, SCI_SETMARGINWIDTHN, 0, 0); return; }
        const LRESULT lines = sci(_sci, SCI_GETLINECOUNT);
        int digits = 1;
        for (LRESULT n = lines; n >= 10; n /= 10) ++digits;
        std::string sample = "_" + std::string(static_cast<size_t>(std::max(2, digits)), '9');
        const LRESULT w = sci(_sci, SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<LPARAM>(sample.c_str()));
        if (w != _marginWidth) { _marginWidth = w; sci(_sci, SCI_SETMARGINWIDTHN, 0, w + 4); }
    }

    // Look like the editor the user is in: font, colours, caret, selection, rendering, zoom.
    void styleLike(HWND src)
    {
        HWND s = _sci;
        sci(s, SCI_SETCODEPAGE, SC_CP_UTF8);
        if (src)
        {
            // DirectWrite drawn through a DC: an HWND render target doesn't show in a layered window
            const LRESULT tech = sci(src, SCI_GETTECHNOLOGY);
            sci(s, SCI_SETTECHNOLOGY, tech == SC_TECHNOLOGY_DEFAULT ? SC_TECHNOLOGY_DEFAULT : SC_TECHNOLOGY_DIRECTWRITEDC);
            char font[128] = {};
            if (sci(src, SCI_STYLEGETFONT, STYLE_DEFAULT, 0) < static_cast<LRESULT>(sizeof(font)))
                sci(src, SCI_STYLEGETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>(font));
            if (font[0]) sci(s, SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>(font));
            sci(s, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, sci(src, SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT));
            sci(s, SCI_STYLESETFORE, STYLE_DEFAULT, sci(src, SCI_STYLEGETFORE, STYLE_DEFAULT));
            sci(s, SCI_STYLESETBACK, STYLE_DEFAULT, sci(src, SCI_STYLEGETBACK, STYLE_DEFAULT));
            sci(s, SCI_STYLECLEARALL);
            sci(s, SCI_SETCARETFORE, sci(src, SCI_GETCARETFORE));
            sci(s, SCI_SETCARETWIDTH, std::max<LRESULT>(1, sci(src, SCI_GETCARETWIDTH)));
            sci(s, SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, sci(src, SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK));
            sci(s, SCI_SETZOOM, _zoom != INT_MIN ? _zoom : sci(src, SCI_GETZOOM));
            sci(s, SCI_STYLESETFORE, STYLE_LINENUMBER, sci(src, SCI_STYLEGETFORE, STYLE_LINENUMBER));
            sci(s, SCI_STYLESETBACK, STYLE_LINENUMBER, sci(src, SCI_STYLEGETBACK, STYLE_LINENUMBER));
            if (_caretLine)
            {
                LRESULT back = sci(src, SCI_GETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK);
                if (!back) back = sci(src, SCI_GETCARETLINEBACK) | 0xFF000000;
                sci(s, SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, back);
            }
        }
        for (int m = 0; m < 5; ++m) sci(s, SCI_SETMARGINWIDTHN, m, 0);
        sci(s, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
        sci(s, SCI_SETMARGINLEFT, 0, _lineNumbers ? 4 : 6);
        sci(s, SCI_SETMARGINRIGHT, 0, 6);
        sci(s, SCI_SETWRAPMODE, _wrap ? SC_WRAP_WORD : SC_WRAP_NONE);
        sci(s, SCI_SETHSCROLLBAR, _wrap ? 0 : 1);
        sci(s, SCI_SETSCROLLWIDTHTRACKING, 1);
        sci(s, SCI_SETSCROLLWIDTH, 1);
        sci(s, SCI_SETCARETLINEVISIBLE, _caretLine ? 1 : 0);
        sci(s, SCI_SETCARETLINEVISIBLEALWAYS, _caretLine ? 1 : 0);
        if (!_caretLine) sci(s, SCI_RESETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK);
        sci(s, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
        _marginWidth = -1;
    }

    void load()
    {
        std::string text;
        if (_keepText)
        {
            HANDLE f = ::CreateFileW(file().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER sz{};
                if (::GetFileSizeEx(f, &sz) && sz.QuadPart < (16 << 20))
                {
                    text.resize(static_cast<size_t>(sz.QuadPart));
                    DWORD got = 0;
                    if (!text.empty() && !::ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
                    text.resize(got);
                }
                ::CloseHandle(f);
            }
        }
        _loading = true;
        sci(_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
        sci(_sci, SCI_EMPTYUNDOBUFFER);
        sci(_sci, SCI_DOCUMENTEND);
        _loading = false;
        _dirty = false;
        updateMargins();
    }

    void save()
    {
        if (_note) ::KillTimer(_note, TIMER_SAVE);
        if (!_keepText || !_sci || !_dirty) return;
        LRESULT len = sci(_sci, SCI_GETLENGTH);
        std::string text(static_cast<size_t>(len) + 1, '\0');
        sci(_sci, SCI_GETTEXT, len + 1, reinterpret_cast<LPARAM>(&text[0]));
        text.resize(static_cast<size_t>(len));
        std::wstring dst = file(), tmp = dst + L".tmp";
        HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { log(L"quick note: can't write %ls (%lu)", tmp.c_str(), ::GetLastError()); return; }
        DWORD put = 0;
        bool ok = text.empty() || (::WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size());
        ::CloseHandle(f);
        if (ok && ::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) _dirty = false;
        else { ::DeleteFileW(tmp.c_str()); log(L"quick note: saving %ls failed (%lu)", dst.c_str(), ::GetLastError()); }
    }

    bool registerClass()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (::GetClassInfoExW(hInst, NOTE_CLASS, &wc)) return true;
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = noteProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hIcon = reinterpret_cast<HICON>(::GetClassLongPtrW(npp._nppHandle, GCLP_HICON));
        wc.hIconSm = reinterpret_cast<HICON>(::GetClassLongPtrW(npp._nppHandle, GCLP_HICONSM));
        wc.lpszClassName = NOTE_CLASS;
        if (::RegisterClassExW(&wc)) return true;
        log(L"quick note: RegisterClassEx failed (%lu)", ::GetLastError());
        return false;
    }

    static LRESULT CALLBACK noteProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_NCCREATE)
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self = reinterpret_cast<QuickNote*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self || self->_note != h) return ::DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_SIZE: self->layout(); return 0;
        case WM_NCHITTEST: return self->hitTest(h, l);
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCAPTION) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            self->paintFrame(dc);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize.x = ::MulDiv(200, systemDpi(), 96);
            mmi->ptMinTrackSize.y = ::MulDiv(100, systemDpi(), 96);
            return 0;
        }
        case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE && self->_sci) ::SetFocus(self->_sci); return 0;
        case WM_CLOSE: self->close(); return 0;
        case WM_COMMAND: if (LOWORD(w) == IDCANCEL) { self->close(); return 0; } break;
        case WM_TIMER:
            if (w == TIMER_FADE) self->fadeStep();
            else if (w == TIMER_SAVE) self->save();
            return 0;
        case WM_NOTIFY:
        {
            auto* n = reinterpret_cast<SCNotification*>(l);
            if (n->nmhdr.hwndFrom == self->_sci && n->nmhdr.code == SCN_MODIFIED && !self->_loading
                && (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)))
            {
                self->_dirty = true;
                ::SetTimer(h, TIMER_SAVE, 800, nullptr);
                self->updateMargins();
            }
            return 0;
        }
        }
        return ::DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK editorSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<QuickNote*>(ref);
        if (m == WM_KEYDOWN && w == VK_ESCAPE) { self->close(); return 0; }
        if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, editorSubclass, 1);
        return ::DefSubclassProc(h, m, w, l);
    }

    UINT _mods = MOD_CONTROL | MOD_ALT, _vk = 'M';
    bool _keepText = true, _fade = true, _closing = false, _dirty = false, _loading = false, _layered = false;
    bool _lineNumbers = false, _wrap = true, _caretLine = false, _onTop = true, _remember = false;
    int _w96 = 640, _h96 = 420, _x = INT_MIN, _y = INT_MIN, _zoom = INT_MIN;
    LRESULT _marginWidth = -1;
    HWND _note = nullptr, _sci = nullptr, _prevFg = nullptr;
    int _alpha = 255, _alphaFrom = 0, _alphaTo = 255;
    double _fadeT0 = 0;
};

QuickNote* s_mod = nullptr;

} // namespace

Module* createHotkeyDrop() { s_mod = new QuickNote(); return s_mod; }
HotkeyStats hotkeyStats() { return s_stats; }
void hotkeyPress() { if (s_mod && s_mod->isEnabled()) s_mod->press(); }
std::wstring hotkeyText(UINT mods, UINT vk) { return QuickNote::text(mods, vk); }
HWND quickNoteWindow() { return s_mod ? s_mod->noteWindow() : nullptr; }
HWND quickNoteEditor() { return s_mod ? s_mod->editor() : nullptr; }
std::wstring quickNoteFile() { return s_mod ? s_mod->file() : std::wstring(); }
// Options dialog: store and (if the feature is on) re-register. Returns true when registered.
bool hotkeyApply(UINT mods, UINT vk)
{
    if (!s_mod) return false;
    settings.setInt(s_mod->key(), L"Modifiers", static_cast<int>(mods));
    settings.setInt(s_mod->key(), L"Key", static_cast<int>(vk));
    s_mod->loadOptions();
    if (!s_mod->isEnabled())
    {
        s_stats.text = QuickNote::text(mods, vk);
        return false;
    }
    return s_mod->registerKey();
}

} // namespace kex
