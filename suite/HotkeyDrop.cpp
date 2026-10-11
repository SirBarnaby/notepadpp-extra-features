// HotkeyDrop.cpp - quick note: a global hotkey (RegisterHotKey, default Ctrl+Alt+M) fades a small
// Notepad++-styled note window in at the centre of the screen, over whatever program is in front and
// whether or not Notepad++ is minimised. Pressing it again fades the note out and gives the focus
// back. The text is saved to kex-quicknote.txt in Notepad++'s settings folder as you type.
#include "KexSuite.h"
#include <dwmapi.h>
#include <cmath>

namespace kex {

namespace {

const int HOTKEY_ID = 0x4B45;
const UINT_PTR TIMER_FADE = 1, TIMER_SAVE = 2;
const double FADE_MS = 120.0;
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
        if (!s_stats.registered && !selfTestMode)
            showNote(L"KEX: the quick note hotkey " + s_stats.text + L" is already taken by another program. Pick another one in Settings > KEX Features.", 8000);
        return true;
    }

    void disable() override
    {
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

        std::wstring caption = L"Quick note  (" + s_stats.text + L" or Esc to hide)";
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (_fade && !binaryAlpha() ? WS_EX_LAYERED : 0);
        // Unowned, so it shows even while Notepad++ is minimised; created fresh each time, so it opens on
        // the desktop you are looking at.
        _note = ::CreateWindowExW(ex, NOTE_CLASS, caption.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
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
        if (ex & WS_EX_LAYERED) { _alpha = 0; ::SetLayeredWindowAttributes(_note, 0, 0, LWA_ALPHA); }
        else _alpha = 255;
        ::ShowWindow(_note, SW_SHOW);
        front();
        if (ex & WS_EX_LAYERED) fadeTo(255);
        s_stats.noteOpen = true;
        log(L"quick note: opened at %d,%d %dx%d, previous window %p", x, y, w, h, _prevFg);
    }

    void front()
    {
        if (!_note) return;
        ::SetWindowPos(_note, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
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
        loadOptions();
        _closing = true;
        s_stats.noteOpen = false;
        if (_prevFg && ::IsWindow(_prevFg) && ::IsWindowVisible(_prevFg) && !::IsIconic(_prevFg)) ::SetForegroundWindow(_prevFg);
        log(L"quick note: closed, focus back to %p", _prevFg);
        if (_fade && !binaryAlpha())
        {
            LONG_PTR st = ::GetWindowLongPtrW(_note, GWL_EXSTYLE);
            if (!(st & WS_EX_LAYERED))
            {
                ::SetWindowLongPtrW(_note, GWL_EXSTYLE, st | WS_EX_LAYERED);
                ::SetLayeredWindowAttributes(_note, 0, 255, LWA_ALPHA);
                _alpha = 255;
            }
            fadeTo(0);
        }
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
        ::SetTimer(_note, TIMER_FADE, 15, nullptr);
    }

    void fadeStep()
    {
        double t = std::min(1.0, (nowMs() - _fadeT0) / FADE_MS);
        double e = 1.0 - (1.0 - t) * (1.0 - t);
        _alpha = static_cast<int>(std::lround(_alphaFrom + (_alphaTo - _alphaFrom) * e));
        ::SetLayeredWindowAttributes(_note, 0, static_cast<BYTE>(_alpha), LWA_ALPHA);
        if (t < 1.0) return;
        ::KillTimer(_note, TIMER_FADE);
        if (_alphaTo == 0) { destroyNote(false); return; }
        // fully shown: drop the layered style so the editor paints directly (no redirection cost)
        ::SetWindowLongPtrW(_note, GWL_EXSTYLE, ::GetWindowLongPtrW(_note, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        ::RedrawWindow(_note, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    void layout()
    {
        if (!_note || !_sci) return;
        RECT rc{}; ::GetClientRect(_note, &rc);
        ::MoveWindow(_sci, 0, 0, rc.right, rc.bottom, TRUE);
    }

    // Look like the editor the user is in: font, colours, caret, selection, rendering, zoom.
    void styleLike(HWND src)
    {
        HWND s = _sci;
        sci(s, SCI_SETCODEPAGE, SC_CP_UTF8);
        if (src)
        {
            sci(s, SCI_SETTECHNOLOGY, sci(src, SCI_GETTECHNOLOGY));
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
            sci(s, SCI_SETZOOM, sci(src, SCI_GETZOOM));
        }
        for (int m = 0; m < 5; ++m) sci(s, SCI_SETMARGINWIDTHN, m, 0);
        sci(s, SCI_SETMARGINLEFT, 0, 10);
        sci(s, SCI_SETMARGINRIGHT, 0, 10);
        sci(s, SCI_SETWRAPMODE, SC_WRAP_WORD);
        sci(s, SCI_SETHSCROLLBAR, 0);
        sci(s, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
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
    bool _keepText = true, _fade = true, _closing = false, _dirty = false, _loading = false;
    int _w96 = 640, _h96 = 420;
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
