// SelfTest.cpp - scripted self-test, run only with -pluginMessage="KexSuite_selftest=1".
//
// Timer-driven steps (like the feasibility spike). Every result is logged as PASS/FAIL/INFO to
// <testdir>\selftest-log.txt. "SHOT n name" lines ask the harness (test/run-selftest.sh) for a
// screenshot; the step machine waits for <testdir>\ackN before going on.
// Optional argument: KexSuite_testdir=C:\path (default: <plugin config dir>\KexSuite-selftest).
#include "KexSuite.h"
#include <functional>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cmath>

namespace kex {

namespace {

FILE* g_tlog = nullptr;
std::wstring g_dir;
int g_pass = 0, g_fail = 0, g_shotN = 0, g_waitAck = 0;
std::vector<std::function<int()>> g_steps;
size_t g_idx = 0;
double g_nextAt = 0;
bool g_inStep = false, g_done = false;
std::function<bool()> g_until;
double g_untilDeadline = 0;
bool g_untilOk = false;
int g_globalModified = 0;

HWND g_testWnd = nullptr;        // helper window: "another program" hotkey owner, menu tracking
PROCESS_INFORMATION g_other{};    // Wine's notepad.exe, the "previously active window"
HWND g_otherWnd = nullptr;
PROCESS_INFORMATION g_second{};   // second notepad++.exe
const UINT WM_TEST_TRACKMENU = WM_APP + 501;
HMENU g_kexMenu = nullptr;
int g_origCaretWidth = 1;

void tlog(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 4095, fmt, ap); va_end(ap); buf[4095] = 0;
    if (g_tlog) { fwprintf(g_tlog, L"%ls\n", buf); fflush(g_tlog); }
}

void check(bool ok, const wchar_t* id, const wchar_t* fmt, ...)
{
    wchar_t buf[3000];
    va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 2999, fmt, ap); va_end(ap); buf[2999] = 0;
    if (ok) ++g_pass; else ++g_fail;
    tlog(L"%ls %ls %ls", ok ? L"PASS" : L"FAIL", id, buf);
}

int shot(const wchar_t* name)
{
    g_waitAck = ++g_shotN;
    tlog(L"SHOT %d %ls", g_shotN, name);
    return 0;
}

int until(std::function<bool()> cond, int timeoutMs)
{
    g_until = cond;
    g_untilDeadline = nowMs() + timeoutMs;
    g_untilOk = false;
    return 0;
}

void pump(int ms)
{
    double end = nowMs() + ms;
    while (nowMs() < end)
    {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            if (!::IsDialogMessageW(::GetActiveWindow(), &m)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
        }
        ::Sleep(2);
    }
}

HWND curSci() { return views.sci(views.current()); }

void key(WORD vk, bool down)
{
    INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    switch (vk) { case VK_DELETE: case VK_INSERT: case VK_HOME: case VK_END: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                  case VK_PRIOR: case VK_NEXT: in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; break; }
    ::SendInput(1, &in, sizeof(in));
}
void tap(WORD vk) { key(vk, true); key(vk, false); }
void chord(std::initializer_list<WORD> mods, WORD vk)
{
    for (WORD m : mods) key(m, true);
    tap(vk);
    for (auto it = mods.end(); it != mods.begin();) key(*--it, false);
}
void clickAt(int x, int y)
{
    ::SetCursorPos(x, y);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(2, in, sizeof(INPUT));
}

void writeFile(const std::wstring& p, const std::string& text)
{
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
}
bool fileExists(const std::wstring& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring path(const wchar_t* name) { return g_dir + L"\\" + name; }
void open(const wchar_t* name) { nppMsg(NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path(name).c_str())); }
void switchTo(const wchar_t* name) { nppMsg(NPPM_SWITCHTOFILE, 0, reinterpret_cast<LPARAM>(path(name).c_str())); }
std::wstring curName() { return fileNameOf(bufferPath(static_cast<intptr_t>(nppMsg(NPPM_GETCURRENTBUFFERID)))); }

void focusEditor()
{
    ::SetForegroundWindow(npp._nppHandle);
    HWND s = curSci();
    if (s) ::SetFocus(s);
}

intptr_t posOf(HWND s, int line, int col) { return sci(s, SCI_FINDCOLUMN, line, col); }

int visibleNonBlank(HWND s, intptr_t a, intptr_t b)
{
    // independent count of what should fall: visible, non-blank characters in [a, b)
    RECT rc; ::GetClientRect(s, &rc);
    int margins = 0;
    for (int i = 0; i < static_cast<int>(sci(s, SCI_GETMARGINS)); ++i) margins += static_cast<int>(sci(s, SCI_GETMARGINWIDTHN, i));
    int n = 0;
    for (intptr_t p = a; p < b; p = sci(s, SCI_POSITIONAFTER, p))
    {
        int c = static_cast<int>(sci(s, SCI_GETCHARAT, p)) & 0xFF;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int x = static_cast<int>(sci(s, SCI_POINTXFROMPOSITION, 0, p)), y = static_cast<int>(sci(s, SCI_POINTYFROMPOSITION, 0, p));
        if (x >= margins - 2 && x < rc.right && y >= -40 && y < rc.bottom) ++n;
    }
    return n;
}

std::wstring rectStr(const RECT& r)
{
    wchar_t b[96]; swprintf(b, 96, L"(%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom); return b;
}

int hiddenOf(int style) { return style & ~(CARETSTYLE_INS_MASK | CARETSTYLE_CURSES); }

struct EnumCtx { DWORD pid; HWND found; };
BOOL CALLBACK findTop(HWND h, LPARAM p)
{
    auto* c = reinterpret_cast<EnumCtx*>(p);
    DWORD pid = 0; ::GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && ::IsWindowVisible(h) && !::GetWindow(h, GW_OWNER)) { c->found = h; return FALSE; }
    return TRUE;
}

int countNppWindows()
{
    int n = 0; HWND h = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"Notepad++", nullptr)) != nullptr) ++n;
    return n;
}

HWND findReplaceDlg()
{
    HWND h = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"#32770", nullptr)) != nullptr)
    {
        DWORD pid = 0; ::GetWindowThreadProcessId(h, &pid);
        if (pid == ::GetCurrentProcessId() && ::GetDlgItem(h, 1609) && ::IsWindowVisible(h)) return h;   // IDREPLACEALL
    }
    return nullptr;
}

LRESULT CALLBACK testWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TEST_TRACKMENU)
    {
        ::SetForegroundWindow(npp._nppHandle);
        ::TrackPopupMenuEx(reinterpret_cast<HMENU>(w), TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
            static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)), npp._nppHandle, nullptr);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

const char* kCpp =
    "// KEX Suite self-test: falling letters keep their syntax colours\r\n"
    "#include <cstdio>\r\n"
    "#include <string>\r\n"
    "\r\n"
    "struct Particle { float x, y, vx, vy; };\r\n"
    "\r\n"
    "static int gravity = 2200;   /* px per second squared */\r\n"
    "\r\n"
    "int main(int argc, char** argv)\r\n"
    "{\r\n"
    "    std::string message = \"Deleted text falls off the screen\";\r\n"
    "    for (int i = 0; i < 10; ++i)\r\n"
    "        printf(\"%d %s\\n\", i * gravity, message.c_str());\r\n"
    "    return argc > 1 ? 0 : 42;\r\n"
    "}\r\n"
    "\r\n"
    "class SmoothCaret { public: double glideMs = 80.0; bool fade = true; };\r\n"
    "\r\n"
    "// line 19 ------------------------\r\n"
    "// line 20 ------------------------\r\n"
    "// line 21 ------------------------\r\n"
    "// line 22 ------------------------\r\n"
    "// line 23 ------------------------\r\n"
    "// line 24\r\n"
    "// line 25: B keeps its caret here\r\n"
    "// line 26\r\n"
    "// line 27\r\n"
    "// line 28\r\n"
    "// line 29\r\n";

std::string bigText()
{
    std::string s;
    for (int i = 0; i < 3000; ++i)
    {
        char b[160];
        snprintf(b, sizeof b, "%04d The quick brown fox jumps over the lazy dog; KEX Suite huge delete line.\r\n", i);
        s += b;
    }
    return s;
}

int pressHotkey()
{
    // Real key events through SendInput; if this Windows/Wine does not turn them into WM_HOTKEY
    // for us, drive the same handler directly. Returns 1 when WM_HOTKEY was delivered.
    int presses = hotkeyStats().presses;
    chord({ VK_CONTROL, VK_MENU }, 'M');
    pump(450);
    if (hotkeyStats().presses > presses) return 1;
    hotkeyPress();
    pump(400);
    return 0;
}

void buildSteps()
{
    auto add = [](std::function<int()> f) { g_steps.push_back(f); };

    // ---- setup ---------------------------------------------------------------------------
    add([] {
        writeFile(path(L"a.cpp"), kCpp);
        writeFile(path(L"b.py"),
            "# Python sample\r\nimport os\r\n\r\ndef falling(letters, gravity=9.81):\r\n    \"\"\"Return letters that fall.\"\"\"\r\n"
            "    return [c for c in letters if not c.isspace()]\r\n\r\nprint(falling('KEX Suite'))\r\n");
        writeFile(path(L"c.txt"), "c.txt - plain text\r\nsecond line of c\r\nthird line of c\r\n");
        writeFile(path(L"d.txt"), "d.txt - plain text\r\nanother file\r\n");
        writeFile(path(L"big.txt"), bigText());
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        tlog(L"INFO Notepad++ version word 0x%08lx; views: %ls, count %d; Wine: %ls",
            static_cast<long>(nppMsg(NPPM_GETNPPVERSION, 1, 0)), views.patched() ? L"KEX patched" : L"standard (NPPM_KEX_GETINFO returned 0)",
            views.count(), underWine ? L"yes" : L"no");
        if (views.patched())
            check(views.count() == 4, L"S1", L"KEX-patched Notepad++: NPPM_KEX_GETINFO reports %d views, using the four-view API", views.count());
        else
            check(views.count() == 2, L"S1", L"unpatched Notepad++: NPPM_KEX_GETINFO=0, using the two-view API");
        int ok = 0;
        for (int i = 0; i < M_COUNT; ++i)
        {
            tlog(L"INFO self-check %ls: %ls", modules[i]->title(), modules[i]->_checkMsg.c_str());
            if (modules[i]->_checkOk) ++ok;
        }
        check(ok == M_COUNT, L"S2", L"startup self-check: KEX Suite: %d/%d features OK (the desktop feature uses fake desktops in self-test mode)", ok, M_COUNT);
        DesktopProvider* real = createRealDesktopProvider();
        tlog(L"INFO real IVirtualDesktopManager here: %ls", real->ok() ? L"available" : real->error().c_str());
        check(true, L"S3", L"real virtual-desktop provider degrades without crashing (available=%d)", real->ok() ? 1 : 0);
        for (int i = 0; i < M_COUNT; ++i)
            check(modules[i]->isEnabled(), L"S4", L"%ls enabled at startup", modules[i]->title());
        open(L"d.txt"); open(L"c.txt"); open(L"b.py"); open(L"a.cpp");
        g_origCaretWidth = static_cast<int>(sci(views.sci(0), SCI_GETCARETWIDTH));
        return 600;
    });

    // ---- menu --------------------------------------------------------------------------------
    add([] {
        HMENU pm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPPLUGINMENU, 0));
        int n = ::GetMenuItemCount(pm);
        for (int i = 0; i < n; ++i)
        {
            wchar_t t[128] = {};
            ::GetMenuStringW(pm, i, t, 127, MF_BYPOSITION);
            if (wcsstr(t, L"KEX Suite")) g_kexMenu = ::GetSubMenu(pm, i);
        }
        check(g_kexMenu != nullptr, L"M1", L"Plugins > KEX Suite submenu present");
        if (g_kexMenu)
        {
            int cnt = ::GetMenuItemCount(g_kexMenu);
            std::wstring items;
            bool checksOk = true;
            for (int i = 0; i < cnt; ++i)
            {
                wchar_t t[128] = {};
                ::GetMenuStringW(g_kexMenu, i, t, 127, MF_BYPOSITION);
                UINT st = ::GetMenuState(g_kexMenu, i, MF_BYPOSITION);
                items += std::wstring(L"[") + ((st & MF_CHECKED) ? L"x] " : L" ] ") + (t[0] ? t : L"----") + L"  ";
                if (i < M_COUNT && ((st & MF_CHECKED) != 0) != modules[i]->isEnabled()) checksOk = false;
            }
            tlog(L"INFO menu items: %ls", items.c_str());
            check(checksOk, L"M2", L"menu check marks match the running features");
        }
        focusEditor();
        ::PostMessageW(npp._nppHandle, WM_SYSCOMMAND, SC_KEYMENU, 'p');   // open Plugins from the menu bar
        return 500;
    });
    add([] { tap('K'); return 300; });          // "KEX Suite"
    add([] { tap(VK_RIGHT); return 500; });     // open its submenu
    add([] { return shot(L"menu-plugins-kexsuite"); });
    add([] { ::EndMenu(); return 400; });
    add([] {
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        ::PostMessageW(g_testWnd, WM_TEST_TRACKMENU, reinterpret_cast<WPARAM>(g_kexMenu), MAKELPARAM(r.left + 470, r.top + 60));
        return 600;
    });
    add([] { return shot(L"menu-kexsuite-submenu"); });
    add([] { ::EndMenu(); return 400; });

    // ---- options dialog ------------------------------------------------------------------------
    add([] {
        openOptions();
        HWND d = optionsWindow();
        check(d && ::IsWindowVisible(d), L"O1", L"Options dialog opens (modeless)");
        if (d)
        {
            wchar_t s[256] = {}; ::GetDlgItemTextW(d, 1001, s, 255);
            tlog(L"INFO options summary line: \"%ls\"", s);
            wchar_t k[256] = {}; ::GetDlgItemTextW(d, 1304, k, 255);
            check(wcsstr(k, L"registered") != nullptr, L"O2", L"Options reports the hotkey state: \"%ls\"", k);
        }
        return 500;
    });
    add([] { return shot(L"options-dialog"); });
    add([] {
        // "another program" owns Ctrl+Alt+M: drop ours, take it with a foreign window, start ours again
        setModuleEnabled(M_HOTKEY, false, false);
        BOOL got = ::RegisterHotKey(g_testWnd, 77, MOD_CONTROL | MOD_ALT, 'M');
        tlog(L"INFO helper window took Ctrl+Alt+M: %d", got);
        setModuleEnabled(M_HOTKEY, true, false);
        HotkeyStats hs = hotkeyStats();
        check(!hs.registered && hs.lastError == ERROR_HOTKEY_ALREADY_REGISTERED, L"O3",
            L"taken hotkey detected: registered=%d error=%lu", hs.registered ? 1 : 0, hs.lastError);
        HWND d = optionsWindow();
        if (d) { ::CheckDlgButton(d, 1300, BST_CHECKED); ::SendMessageW(d, WM_COMMAND, 1002, 0); }   // Apply
        wchar_t k[256] = {}; if (d) ::GetDlgItemTextW(d, 1304, k, 255);
        check(wcsstr(k, L"taken") != nullptr, L"O4", L"Options shows: \"%ls\"", k);
        return 400;
    });
    add([] { return shot(L"options-hotkey-taken"); });
    add([] {
        ::UnregisterHotKey(g_testWnd, 77);
        HWND d = optionsWindow();
        if (d) ::SendMessageW(d, WM_COMMAND, 1002, 0);
        wchar_t k[256] = {}; if (d) ::GetDlgItemTextW(d, 1304, k, 255);
        check(hotkeyStats().registered, L"O5", L"after the other program lets go, Apply registers it again: \"%ls\"", k);
        if (d) ::SendMessageW(d, WM_COMMAND, IDCANCEL, 0);
        check(optionsWindow() == nullptr, L"O6", L"Options dialog closes");
        return 300;
    });

    // ---- falling letters -----------------------------------------------------------------------
    add([] {
        switchTo(L"a.cpp");
        focusEditor();
        HWND s = curSci();
        sci(views.sci(0), SCI_SETCARETWIDTH, 2);   // a wider caret shows up better in screenshots
        sci(views.sci(1), SCI_SETCARETWIDTH, 2);
        intptr_t a = posOf(s, 10, 4), b = sci(s, SCI_GETLINEENDPOSITION, 10);
        sci(s, SCI_SETSEL, a, b);
        int expect = visibleNonBlank(s, a, b);
        tlog(L"INFO deleting the selection on line 11 (%lld chars, %d visible non-blank)", static_cast<long long>(b - a), expect);
        lettersFreezeAfter(230);
        int before = lettersStats().totalSpawned;
        tap(VK_DELETE);   // real keyboard input
        pump(150);
        LettersStats st = lettersStats();
        check(st.totalSpawned - before == expect && expect > 0, L"F1",
            L"Delete key on a selection: %d letters spawned, expected %d", st.totalSpawned - before, expect);
        return 500;
    });
    add([] {
        LettersStats st = lettersStats();
        RECT nr; ::GetWindowRect(npp._nppHandle, &nr);
        check(st.overlayVisible && st.live > 0, L"F2", L"overlay alive with %d letters, bounding box %ls (Notepad++ window %ls)",
            st.live, rectStr(st.overlay).c_str(), rectStr(nr).c_str());
        HWND ov = ::FindWindowW(L"KexSuiteOverlay", L"KEX falling letters");
        LONG_PTR ex = ov ? ::GetWindowLongPtrW(ov, GWL_EXSTYLE) : 0;
        check(ov && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED) && (ex & WS_EX_NOACTIVATE) && (ex & WS_EX_TOPMOST), L"F3",
            L"overlay is layered, click-through, non-activating, topmost (exstyle 0x%lx)", static_cast<long>(ex));
        check(::GetForegroundWindow() == npp._nppHandle && views.viewOf(::GetFocus()) >= 0, L"F4", L"focus stays in the editor while letters fall");
        return shot(L"falling-letters-selection");
    });
    add([] {
        lettersFreezeAfter(-1);
        return until([] { return lettersStats().live == 0 && !lettersStats().overlayVisible; }, 6000);
    });
    add([] {
        check(g_untilOk, L"F5", L"all letters fell off the screen and the overlay hid itself (frames rendered so far: %d)", lettersStats().frames);
        HWND s = curSci();
        sci(s, SCI_GOTOPOS, posOf(s, 6, 18));   // after "gravity" on line 7
        focusEditor();
        lettersFreezeAfter(160);
        int before = lettersStats().totalSpawned;
        tap(VK_BACK); tap(VK_BACK); tap(VK_BACK);
        pump(120);
        check(lettersStats().totalSpawned - before == 3, L"F6", L"Backspace x3: %d letters spawned", lettersStats().totalSpawned - before);
        return 300;
    });
    add([] { return shot(L"falling-letters-backspace"); });
    add([] {
        lettersFreezeAfter(-1);
        pump(50);
        HWND s = curSci();
        intptr_t a = posOf(s, 4, 7), b = posOf(s, 4, 15);   // "Particle"
        sci(s, SCI_SETSEL, a, b);
        int before = lettersStats().totalSpawned;
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_EDIT_CUT);
        pump(80);
        check(lettersStats().totalSpawned - before == 8, L"F7", L"Edit > Cut of \"Particle\": %d letters spawned", lettersStats().totalSpawned - before);
        int su = lettersStats().skippedUndo;
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("UNDO_ME"));
        before = lettersStats().totalSpawned;
        sci(s, SCI_UNDO);
        pump(50);
        check(lettersStats().totalSpawned == before && lettersStats().skippedUndo > su, L"F8",
            L"undo of typed text does not rain letters (skipped undo deletes: %d)", lettersStats().skippedUndo - su);
        sci(s, SCI_GOTOPOS, 0);
        return 200;
    });
    add([] {
        open(L"big.txt");
        HWND s = curSci();
        focusEditor();
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        sci(s, SCI_SETSEL, posOf(s, 2500, 0), posOf(s, 2502, 0));
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        int before = lettersStats().totalSpawned;
        sci(s, SCI_CLEAR);
        pump(50);
        check(lettersStats().totalSpawned == before && lettersStats().lastCandidates == 0, L"F9", L"deleting off-screen lines spawns nothing");
        sci(s, SCI_SELECTALL);
        sci(s, SCI_SETFIRSTVISIBLELINE, 100);
        lettersFreezeAfter(260);
        before = lettersStats().totalSpawned;
        double t0 = nowMs();
        tap(VK_DELETE);
        pump(200);
        LettersStats st = lettersStats();
        check(st.lastCandidates > 200 && st.lastSpawned > 0 && st.lastSpawned <= 200, L"F10",
            L"Select All + Delete of 3000 lines: %d visible letters grouped into %d falling pieces (cap 200); total time incl. 200 ms wait %.0f ms",
            st.lastCandidates, st.lastSpawned, nowMs() - t0);
        return 300;
    });
    add([] { return shot(L"falling-letters-huge-delete"); });
    add([] {
        lettersFreezeAfter(-1);
        HWND s = curSci();
        sci(s, SCI_SETSAVEPOINT);   // so closing big.txt does not ask
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(100);
        switchTo(L"b.py");
        focusEditor();
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_SEARCH_REPLACE);
        return 700;
    });
    add([] {
        // Replace All: Notepad++ switches SCN_MODIFIED off during it and reports NPPN_GLOBALMODIFIED
        HWND dlg = findReplaceDlg();
        if (!dlg) { check(false, L"F11", L"Replace dialog not found"); return 100; }
        ::SetWindowTextW(::GetDlgItem(dlg, 1601), L"letters");   // IDFINDWHAT
        ::SetWindowTextW(::GetDlgItem(dlg, 1602), L"glyphs");    // IDREPLACEWITH
        int before = lettersStats().totalSpawned, gm = g_globalModified;
        ::SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(1609, BN_CLICKED), reinterpret_cast<LPARAM>(::GetDlgItem(dlg, 1609)));
        pump(300);
        std::string txt(static_cast<size_t>(sci(curSci(), SCI_GETLENGTH)) + 1, '\0');
        sci(curSci(), SCI_GETTEXT, txt.size(), reinterpret_cast<LPARAM>(&txt[0]));
        bool replaced = txt.find("glyphs") != std::string::npos;
        check(replaced && lettersStats().totalSpawned == before, L"F11",
            L"Replace All (%ls) spawned %d letters; NPPN_GLOBALMODIFIED seen %d time(s)", replaced ? L"text replaced" : L"NOT replaced",
            lettersStats().totalSpawned - before, g_globalModified - gm);
        ::SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
        ::ShowWindow(dlg, SW_HIDE);
        return 300;
    });
    add([] {
        BOOL was = TRUE;
        ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &was, 0);
        BOOL setOk = ::SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), 0);
        BOOL now = TRUE;
        ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &now, 0);
        HWND s = curSci();
        focusEditor();
        sci(s, SCI_SETSEL, posOf(s, 0, 2), posOf(s, 0, 8));
        int before = lettersStats().totalSpawned, sk = lettersStats().skippedAnimOff;
        tap(VK_DELETE);
        pump(80);
        if (setOk && !now)
            check(lettersStats().totalSpawned == before && lettersStats().skippedAnimOff > sk, L"F12", L"Windows animation effects off: no letters (respected by default)");
        else
            tlog(L"SKIP F12 this system cannot switch SPI_SETCLIENTAREAANIMATION (set=%d, value now=%d)", setOk, now);
        ::SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(was)), 0);
        lettersFreezeAfter(-1);
        pump(100);
        switchTo(L"c.txt");
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_CLONE_TO_ANOTHER_VIEW);
        pump(200);
        HWND s2 = views.sci(1);
        check(views.current() == 1 && views.visible(1), L"F13a", L"c.txt cloned into the second view");
        ::SetFocus(s2);
        sci(s2, SCI_SETSEL, posOf(s2, 1, 0), posOf(s2, 1, 11));
        lettersFreezeAfter(200);
        before = lettersStats().totalSpawned;
        tap(VK_DELETE);
        pump(120);
        check(lettersStats().totalSpawned - before == 10, L"F13", L"second view: %d letters spawned from \"second line\"", lettersStats().totalSpawned - before);
        return 300;
    });
    add([] { return shot(L"falling-letters-second-view"); });
    add([] {
        lettersFreezeAfter(-1);
        return until([] { return lettersStats().live == 0; }, 6000);
    });

    // ---- smooth caret --------------------------------------------------------------------------
    add([] {
        bool ok = true;
        std::wstring detail;
        for (int v = 0; v < 2; ++v)
        {
            HWND s = views.sci(v);
            int orig = caretOriginalStyle(s), cur = static_cast<int>(sci(s, SCI_GETCARETSTYLE));
            wchar_t b[96]; swprintf(b, 96, L"view%d original=0x%x now=0x%x; ", v + 1, orig, cur);
            detail += b;
            if (orig < 0 || cur != hiddenOf(orig)) ok = false;
        }
        check(ok, L"C1", L"Scintilla's caret hidden in both views (%ls)", detail.c_str());
        switchTo(L"a.cpp");
        focusEditor();
        ::SetFocus(views.sci(0));
        caretTestFreeze(-1, true);   // solid (no blink) so screenshots always catch it
        HWND s = views.sci(0);
        sci(s, SCI_GOTOPOS, posOf(s, 2, 0));
        pump(150);
        return 100;
    });
    add([] {
        HWND s = views.sci(0);
        CaretStats a = caretStats();
        intptr_t target = posOf(s, 8, 14);
        float tx = static_cast<float>(sci(s, SCI_POINTXFROMPOSITION, 0, target));
        float ty = static_cast<float>(sci(s, SCI_POINTYFROMPOSITION, 0, target));
        sci(s, SCI_GOTOPOS, target);
        std::wstring traj;
        double t0 = nowMs();
        int between = 0;
        while (nowMs() - t0 < 160)
        {
            pump(12);
            CaretStats c = caretStats();
            wchar_t b[64]; swprintf(b, 64, L"%.0fms:(%.1f,%.1f) ", nowMs() - t0, c.mainX, c.mainY);
            traj += b;
            if (c.mainY > a.mainY + 0.5f && c.mainY < ty - 0.5f) ++between;
        }
        CaretStats e = caretStats();
        tlog(L"INFO caret glide from (%.1f,%.1f) to (%.1f,%.1f): %ls", a.mainX, a.mainY, tx, ty, traj.c_str());
        check(between >= 2 && std::fabs(e.mainX - std::round(tx - 0.51f)) <= 1.0f && std::fabs(e.mainY - ty) < 0.5f, L"C2",
            L"caret glides through %d intermediate positions and lands on the target (%.1f,%.1f)", between, e.mainX, e.mainY);
        caretTestFreeze(0.45, true);
        sci(s, SCI_GOTOPOS, posOf(s, 2, 0));
        pump(120);
        return 200;
    });
    add([] { return shot(L"caret-glide-frozen-midway"); });
    add([] {
        caretTestFreeze(-1, true);
        HWND s = views.sci(0);
        sci(s, SCI_GOTOPOS, posOf(s, 10, 30));
        pump(200);
        CaretStats c = caretStats();
        check(c.visible && c.carets == 1, L"C3", L"custom caret visible, overlay %ls", rectStr(c.overlay).c_str());
        return 100;
    });
    add([] { return shot(L"caret-normal"); });
    add([] {
        HWND s = views.sci(0);
        sci(s, SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE);
        sci(s, SCI_SETRECTANGULARSELECTIONANCHOR, posOf(s, 18, 3));
        sci(s, SCI_SETRECTANGULARSELECTIONCARET, posOf(s, 22, 8));
        pump(200);
        CaretStats c = caretStats();
        check(c.carets == 5, L"C4", L"rectangular selection over 5 lines: %d carets drawn", c.carets);
        return 100;
    });
    add([] { return shot(L"caret-rectangular-selection"); });
    add([] {
        HWND s = views.sci(0);
        sci(s, SCI_CANCEL);
        sci(s, SCI_SETSELECTIONMODE, SC_SEL_STREAM);
        sci(s, SCI_GOTOPOS, posOf(s, 6, 4));
        sci(s, SCI_SETOVERTYPE, 1);
        pump(200);
        CaretStats c = caretStats();
        check(c.block && c.carets == 1, L"C5", L"overtype mode: block caret");
        return 100;
    });
    add([] { return shot(L"caret-overtype-block"); });
    add([] {
        HWND s = views.sci(0);
        sci(s, SCI_SETOVERTYPE, 0);
        int z0 = static_cast<int>(sci(s, SCI_GETZOOM));
        sci(s, SCI_SETZOOM, z0 + 5);
        pump(200);
        CaretStats c = caretStats();
        float lh = static_cast<float>(sci(s, SCI_TEXTHEIGHT, 6));
        check(std::fabs(c.mainH - lh) < 0.5f, L"C6", L"zoom +5: caret height %.0f = line height %.0f", c.mainH, lh);
        sci(s, SCI_SETZOOM, z0);
        pump(150);
        sci(s, SCI_GOTOPOS, posOf(s, 12, 4));
        pump(150);
        CaretStats b = caretStats();
        sci(s, SCI_LINESCROLL, 0, 3);
        pump(30);
        CaretStats a = caretStats();
        float lh2 = static_cast<float>(sci(s, SCI_TEXTHEIGHT, 0));
        check(std::fabs((b.mainY - a.mainY) - 3 * lh2) < 0.5f, L"C7", L"scroll by 3 lines: caret moved %.0f px at once (3 lines = %.0f px)", b.mainY - a.mainY, 3 * lh2);
        sci(s, SCI_LINESCROLL, 0, -3);
        HWND other = ::FindWindowExW(npp._nppHandle, nullptr, L"ReBarWindow32", nullptr);
        ::SetFocus(other ? other : npp._nppHandle);
        pump(80);
        bool hid = !caretStats().visible;
        ::SetFocus(s);
        pump(150);
        check(hid && caretStats().visible, L"C8", L"caret hides on focus loss and comes back on focus");
        HWND s2 = views.sci(1);
        ::SetFocus(s2);
        pump(150);
        check(caretStats().target == s2 && caretStats().visible, L"C9", L"second view focused: caret drawn in view 2");
        ::SetFocus(s);
        pump(100);
        return 100;
    });
    add([] {
        // Notepad++ re-applies its caret setting (Preferences > Editing): adopt it and hide again
        HWND s = views.sci(0);
        int orig = caretOriginalStyle(s);
        sci(s, SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
        sci(s, SCI_GOTOPOS, posOf(s, 3, 0));
        pump(150);
        int adopted = caretOriginalStyle(s);
        check(adopted == CARETSTYLE_BLOCK && sci(s, SCI_GETCARETSTYLE) == hiddenOf(CARETSTYLE_BLOCK), L"C10",
            L"external caret style change adopted (0x%x) and hidden again", adopted);
        sci(s, SCI_SETCARETSTYLE, orig);   // the user's real value again
        sci(s, SCI_GOTOPOS, posOf(s, 3, 2));
        pump(150);
        int o0 = caretOriginalStyle(views.sci(0)), o1 = caretOriginalStyle(views.sci(1));
        ::SendMessageW(npp._nppHandle, WM_COMMAND, menuCmdId(M_CARET), 0);   // the menu item
        int r0 = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE)), r1 = static_cast<int>(sci(views.sci(1), SCI_GETCARETSTYLE));
        HWND ov = ::FindWindowW(L"KexSuiteOverlay", L"KEX caret");
        check(!modules[M_CARET]->isEnabled() && r0 == o0 && r1 == o1 && o0 == orig && !ov, L"C11",
            L"caret off from the menu: style restored exactly (view1 0x%x, view2 0x%x), overlay destroyed", r0, r1);
        return 300;
    });
    add([] { return shot(L"caret-disabled-scintilla-caret-back"); });
    add([] {
        ::SendMessageW(npp._nppHandle, WM_COMMAND, menuCmdId(M_CARET), 0);
        focusEditor();
        pump(100);
        check(modules[M_CARET]->isEnabled() && sci(views.sci(0), SCI_GETCARETSTYLE) == hiddenOf(caretOriginalStyle(views.sci(0))), L"C12",
            L"caret back on from the menu");
        caretTestFreeze(-1, false);
        return 100;
    });

    // ---- per-desktop (fake desktops) ------------------------------------------------------------
    add([] {
        DesktopStats d = desktopStats();
        check(d.registered == 1 && d.here == guidStr(fakeDesktopGuid(0)), L"D1", L"started on fake desktop A %ls: 1 registered desktop", d.here.c_str());
        switchTo(L"c.txt");
        if (views.current() == 1) nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);   // close the clone in view 2
        switchTo(L"a.cpp");
        focusEditor();
        sci(curSci(), SCI_GOTOPOS, posOf(curSci(), 4, 2));
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        pump(150);
        fakeSwitchTo(1);   // the user switches to desktop B (not registered)
        desktopPollNow();
        check(fakeWindowDesktop(npp._nppHandle) == 0, L"D2", L"switch to unregistered desktop B: Notepad++ stays on A (%ls)", desktopStats().lastAction.c_str());
        // the user runs notepad++.exe on desktop B
        wchar_t exe[MAX_PATH]; ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = std::wstring(L"\"") + exe + L"\"";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        BOOL ok = ::CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &g_second);
        tlog(L"INFO started a second notepad++.exe (pid %lu): %d", g_second.dwProcessId, ok);
        return until([] { return g_second.hProcess && ::WaitForSingleObject(g_second.hProcess, 0) == WAIT_OBJECT_0; }, 20000);
    });
    add([] {
        DWORD code = 0;
        if (g_second.hProcess) { ::GetExitCodeProcess(g_second.hProcess, &code); ::CloseHandle(g_second.hProcess); ::CloseHandle(g_second.hThread); }
        DesktopStats d = desktopStats();
        check(g_untilOk && d.registered == 2 && fakeWindowDesktop(npp._nppHandle) == 1 && countNppWindows() == 1, L"D3",
            L"second notepad++.exe on B: it exited (code %lu), B registered (%d desktops), the single window moved to B (%ls)",
            code, d.registered, d.lastAction.c_str());
        switchTo(L"c.txt");
        focusEditor();
        sci(curSci(), SCI_GOTOPOS, posOf(curSci(), 2, 5));
        ::MoveWindow(npp._nppHandle, 120, 90, 820, 520, TRUE);
        switchTo(L"a.cpp");
        sci(curSci(), SCI_GOTOPOS, posOf(curSci(), 24, 3));
        pump(150);   // let Scintilla paint and report the caret move (SCN_UPDATEUI), as it does for a real user
        switchTo(L"c.txt");
        pump(150);
        return 200;
    });
    add([] { return shot(L"desktop-B-state"); });
    add([] {
        fakeSwitchTo(0);
        desktopPollNow();
        pump(300);
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        HWND s = curSci();
        intptr_t caret = sci(s, SCI_GETCURRENTPOS);
        check(fakeWindowDesktop(npp._nppHandle) == 0 && curName() == L"a.cpp" && caret == posOf(s, 4, 2) && r.left == 20 && r.top == 16 && r.right - r.left == 960,
            L"D4", L"back on A: window followed, A's tab (%ls), caret (line %lld) and placement %ls restored",
            curName().c_str(), static_cast<long long>(sci(s, SCI_LINEFROMPOSITION, caret)) + 1, rectStr(r).c_str());
        return 200;
    });
    add([] { return shot(L"desktop-A-state"); });
    add([] {
        fakeSwitchTo(1);
        desktopPollNow();
        pump(300);
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        HWND s = curSci();
        intptr_t caret = sci(s, SCI_GETCURRENTPOS);
        check(fakeWindowDesktop(npp._nppHandle) == 1 && curName() == L"c.txt" && caret == posOf(s, 2, 5) && r.left == 120 && r.top == 90,
            L"D5", L"back on B: B's tab (%ls), caret and placement %ls restored", curName().c_str(), rectStr(r).c_str());
        switchTo(L"a.cpp");
        pump(300);
        Sci_Position line = sci(curSci(), SCI_LINEFROMPOSITION, sci(curSci(), SCI_GETCURRENTPOS));
        check(line == 24, L"D6", L"lazy restore on activation: a.cpp on B is at line %lld (B's own position; A keeps line 5)", static_cast<long long>(line) + 1);
        switchTo(L"c.txt");
        bool would = desktopWouldCloseHere();
        ::SendMessageW(npp._nppHandle, WM_SYSCOMMAND, SC_CLOSE, 0);   // title-bar X
        pump(250);
        DesktopStats d = desktopStats();
        std::wstring ini = settings.getStr((L"Desktop." + guidStr(fakeDesktopGuid(1))).c_str(), L"Registered", L"");
        check(would && ::IsWindow(npp._nppHandle) && d.registered == 1 && fakeWindowDesktop(npp._nppHandle) == 0 && ini.empty(), L"D7",
            L"X on B closed Notepad++ only on B: still running, now on A, %d registered, B's INI section removed", d.registered);
        check(!desktopWouldCloseHere(), L"D8", L"with one registered desktop the X button exits as usual (not intercepted); File > Exit is never intercepted");
        fakeSwitchTo(2);
        desktopPollNow();
        hotkeyPress();
        pump(400);
        d = desktopStats();
        check(d.registered == 2 && fakeWindowDesktop(npp._nppHandle) == 2, L"D9", L"hotkey on unregistered desktop C: registered and moved there (%ls)", d.lastAction.c_str());
        return 200;
    });
    add([] {
        std::wstring regs;
        for (const std::wstring& s : settings.sectionNames()) if (s.compare(0, 8, L"Desktop.") == 0) regs += s + L" ";
        tlog(L"INFO INI sections: %ls", regs.c_str());
        std::wstring a = settings.getStr((L"Desktop." + guidStr(fakeDesktopGuid(0))).c_str(), L"Active0", L"");
        check(regs.find(guidStr(fakeDesktopGuid(0))) != std::wstring::npos && regs.find(guidStr(fakeDesktopGuid(2))) != std::wstring::npos, L"D10",
            L"state persisted per desktop GUID in %ls (A's active tab: %ls)", settings.path().c_str(), a.c_str());
        hotkeyPress();   // in front -> hides
        pump(400);
        fakeSwitchTo(0);
        desktopPollNow();
        hotkeyPress();   // shows on A
        pump(400);
        if (isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(200);
        return 200;
    });

    // ---- global hotkey ---------------------------------------------------------------------------
    add([] {
        check(hotkeyStats().registered, L"H1", L"global hotkey %ls registered", hotkeyStats().text.c_str());
        STARTUPINFOW si{}; si.cb = sizeof(si);
        wchar_t cmd[] = L"notepad.exe";
        BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &g_other);
        tlog(L"INFO started notepad.exe as 'another program': %d", ok);
        return until([] { EnumCtx c{ g_other.dwProcessId, nullptr }; ::EnumWindows(findTop, reinterpret_cast<LPARAM>(&c)); g_otherWnd = c.found; return g_otherWnd != nullptr; }, 15000);
    });
    add([] {
        if (g_otherWnd) ::MoveWindow(g_otherWnd, 340, 220, 620, 420, TRUE);
        ::ShowWindow(npp._nppHandle, SW_MINIMIZE);
        pump(200);
        if (g_otherWnd) ::SetForegroundWindow(g_otherWnd);
        pump(250);
        tlog(L"INFO Notepad++ minimized=%d, foreground is the other program: %d", ::IsIconic(npp._nppHandle), ::GetForegroundWindow() == g_otherWnd);
        int delivered = pressHotkey();
        tlog(L"INFO SendInput Ctrl+Alt+M -> WM_HOTKEY delivered: %ls", delivered ? L"yes" : L"no (handler driven directly)");
        HWND n = npp._nppHandle;
        check(::IsWindowVisible(n) && !::IsIconic(n) && isPostItMode() && ::GetForegroundWindow() == n && views.viewOf(::GetFocus()) >= 0, L"H2",
            L"hotkey while minimized: shown, F12 post-it on, in front, editor focused (WM_HOTKEY from real key events: %ls)", delivered ? L"yes" : L"no");
        return 300;
    });
    add([] { return shot(L"hotkey-dropdown-postit"); });
    add([] {
        int delivered = pressHotkey();
        check(!::IsWindowVisible(npp._nppHandle) && ::GetForegroundWindow() == g_otherWnd, L"H3",
            L"hotkey while in front: hidden, focus back to the previous program (WM_HOTKEY: %ls)", delivered ? L"yes" : L"no");
        return 200;
    });
    add([] { return shot(L"hotkey-hidden-focus-returned"); });
    add([] {
        pressHotkey();
        check(::IsWindowVisible(npp._nppHandle) && ::GetForegroundWindow() == npp._nppHandle, L"H4", L"hotkey while hidden: back in front");
        ::SetForegroundWindow(g_otherWnd);
        ::SetWindowPos(g_otherWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        pump(250);
        pressHotkey();
        check(::IsWindowVisible(npp._nppHandle) && ::GetForegroundWindow() == npp._nppHandle, L"H5", L"hotkey while behind another window: brought to front");
        return 200;
    });

    // ---- F12 hover tabs --------------------------------------------------------------------------
    add([] {
        check(isPostItMode(), L"T0", L"in F12 post-it mode");
        switchTo(L"b.py");
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("\r\n# unsaved edit"));
        switchTo(L"a.cpp");
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        ::SetCursorPos((r.left + r.right) / 2, r.top + 2);
        return until([] { return tabsStats().stripVisible; }, 2000);
    });
    add([] {
        TabsStats t = tabsStats();
        int nb = views.nbFiles(views.current()), cur = views.currentIndex(views.current());
        int dirtyCount = 0; for (int d : t.dirty) dirtyCount += d;
        check(g_untilOk && t.tabCount == nb && t.activeIndex == cur, L"T1", L"hovering the top edge shows the floating strip %ls: %d tabs (view has %d), active tab %d",
            rectStr(t.strip).c_str(), t.tabCount, nb, t.activeIndex);
        check(dirtyCount >= 1, L"T2", L"unsaved marker shown on %d tab(s)", dirtyCount);
        RECT sr = t.strip; RECT sc; ::GetWindowRect(curSci(), &sc);
        check(sr.top <= sc.top + 2 && sr.bottom > sc.top, L"T3", L"strip floats over the text; the editor is not pushed down (editor top %ld, strip %ld..%ld)", sc.top, sr.top, sr.bottom);
        return 100;
    });
    add([] { return shot(L"f12-hover-tabs"); });
    add([] {
        TabsStats t = tabsStats();
        int target = -1;
        for (int i = 0; i < t.tabCount; ++i) if (i != t.activeIndex) { target = i; break; }
        if (target >= 0)
        {
            RECT r = t.tabRects[target];
            clickAt((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            pump(300);
            bool viaMouse = views.currentIndex(views.current()) == target;
            if (!viaMouse) { tabsTestClick(target); pump(200); }
            check(views.currentIndex(views.current()) == target, L"T4", L"click on tab %d switched the document to %ls (%ls)",
                target, curName().c_str(), viaMouse ? L"real mouse click" : L"direct call");
        }
        ::SetCursorPos(500, 500);
        return until([] { return !tabsStats().stripVisible; }, 3000);
    });
    add([] {
        check(g_untilOk, L"T5", L"strip hides when the mouse leaves");
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);   // leave post-it
        pump(300);
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        ::SetCursorPos((r.left + r.right) / 2, r.top + 2);
        pump(400);
        check(!isPostItMode() && !tabsStats().stripVisible, L"T6", L"outside post-it mode the hot zone does nothing");
        ::SetCursorPos(600, 700);
        return 100;
    });

    // ---- every feature off and on again from the menu ------------------------------------------
    add([] {
        HMENU pm = g_kexMenu;
        for (int i = 0; i < M_COUNT; ++i)
        {
            int origStyle0 = caretOriginalStyle(views.sci(0));
            ::SendMessageW(npp._nppHandle, WM_COMMAND, menuCmdId(i), 0);   // same as clicking the menu item
            pump(100);
            bool off = !modules[i]->isEnabled();
            bool unchecked = pm && !(::GetMenuState(pm, menuCmdId(i), MF_BYCOMMAND) & MF_CHECKED);
            std::wstring extra;
            bool restored = true;
            switch (i)
            {
            case M_LETTERS: restored = ::FindWindowW(L"KexSuiteOverlay", L"KEX falling letters") == nullptr; extra = L"render thread and overlay gone"; break;
            case M_CARET:
            {
                int now = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE));
                restored = now == origStyle0 && ::FindWindowW(L"KexSuiteOverlay", L"KEX caret") == nullptr;
                wchar_t b[80]; swprintf(b, 80, L"caret style back to 0x%x (original 0x%x)", now, origStyle0); extra = b;
                break;
            }
            case M_HOTKEY:
            {
                BOOL free = ::RegisterHotKey(g_testWnd, 78, MOD_CONTROL | MOD_ALT, 'M');
                if (free) ::UnregisterHotKey(g_testWnd, 78);
                restored = free != FALSE; extra = L"Ctrl+Alt+M released";
                break;
            }
            case M_TABS: restored = ::FindWindowW(L"KexSuiteTabStrip", nullptr) == nullptr; extra = L"strip window destroyed"; break;
            case M_DESKTOP: restored = mainHooks.onSysClose == nullptr && mainHooks.onSecondInstance == nullptr; extra = L"close/second-instance hooks removed"; break;
            }
            check(off && unchecked && restored, L"E1", L"%ls off from the menu: unchecked, %ls", modules[i]->title(), extra.c_str());
            ::SendMessageW(npp._nppHandle, WM_COMMAND, menuCmdId(i), 0);
            pump(100);
            bool on = modules[i]->isEnabled() && pm && (::GetMenuState(pm, menuCmdId(i), MF_BYCOMMAND) & MF_CHECKED);
            check(on, L"E2", L"%ls back on from the menu", modules[i]->title());
        }
        DWORD_PTR r = 0;
        check(::SendMessageTimeoutW(npp._nppHandle, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &r) != 0, L"E3", L"Notepad++ still responsive");
        int ok = 0; for (int i = 0; i < M_COUNT; ++i) ok += modules[i]->_checkOk ? 1 : 0;
        tlog(L"INFO final summary: KEX Suite: %d/%d features OK", ok, M_COUNT);
        sci(views.sci(0), SCI_SETCARETWIDTH, g_origCaretWidth);
        sci(views.sci(1), SCI_SETCARETWIDTH, g_origCaretWidth);
        return 100;
    });
    // ---- views 3 and 4 of a KEX-patched Notepad++, and snapped-out windows ----
    add([] {
        if (!views.patched()) { tlog(L"SKIP K1-K4: Notepad++ is not KEX-patched"); return 50; }
        switchTo(L"c.txt");
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW + 907);   // IDM_KEX_DOC_CLONE_TO_VIEW3
        pump(200);
        HWND s3 = views.sci(2);
        check(views.current() == 2 && views.visible(2), L"K1", L"c.txt cloned into view 3 (bottom left)");
        ::SetFocus(s3);
        sci(s3, SCI_SETFIRSTVISIBLELINE, 0);
        intptr_t a = posOf(s3, 0, 0), b = sci(s3, SCI_GETLINEENDPOSITION, 0);
        int expect = visibleNonBlank(s3, a, b);
        sci(s3, SCI_SETSEL, a, b);
        lettersFreezeAfter(200);
        int before = lettersStats().totalSpawned;
        tap(VK_DELETE);
        pump(150);
        check(expect > 0 && lettersStats().totalSpawned - before == expect, L"K2", L"view 3: deleting a line spawned %d letters (expected %d)",
            lettersStats().totalSpawned - before, expect);
        pump(100);
        check(caretStats().target == s3 && caretStats().visible, L"K3", L"view 3 focused: smooth caret drawn there (target %p, view 3 %p, visible %d, focus %p, carets %d, main %.0f,%.0f)",
            caretStats().target, s3, caretStats().visible, ::GetFocus(), caretStats().carets, caretStats().mainX, caretStats().mainY);
        return 300;
    });
    add([] { return views.patched() ? shot(L"kex-view3-letters") : 0; });
    add([] {
        if (!views.patched()) return 0;
        lettersFreezeAfter(-1);
        HWND s3 = views.sci(2);
        nppMsg(NPPM_KEX_TEAROUTVIEW, 2, TRUE);
        pump(250);
        HWND host = reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWHOSTWINDOW, 2));
        ::SetForegroundWindow(host);
        ::SetFocus(s3);
        sci(s3, SCI_GOTOPOS, posOf(s3, 1, 3));
        pump(250);
        RECT hr; ::GetWindowRect(host, &hr);
        CaretStats cs = caretStats();
        bool inside = cs.overlay.left >= hr.left && cs.overlay.right <= hr.right && cs.overlay.top >= hr.top && cs.overlay.bottom <= hr.bottom;
        check(host != npp._nppHandle && cs.target == s3 && cs.visible && inside, L"K4", L"view 3 snapped out: caret drawn in its own window, overlay %ls inside %ls",
            rectStr(cs.overlay).c_str(), rectStr(hr).c_str());
        return 200;
    });
    add([] { return views.patched() ? shot(L"kex-view3-snapped-out-caret") : 0; });
    add([] {
        if (!views.patched()) return 0;
        sci(views.sci(2), SCI_UNDO);
        nppMsg(NPPM_KEX_TEAROUTVIEW, 2, FALSE);
        pump(200);
        return 100;
    });
    add([] {
        if (g_other.hProcess) { ::TerminateProcess(g_other.hProcess, 0); ::CloseHandle(g_other.hProcess); ::CloseHandle(g_other.hThread); }
        tlog(L"RESULT %d passed, %d failed", g_pass, g_fail);
        tlog(L"DONE");
        g_done = true;
        return 0;
    });
}

void CALLBACK tick(HWND, UINT, UINT_PTR, DWORD)
{
    if (g_inStep || g_done) return;
    if (g_waitAck)
    {
        wchar_t ack[MAX_PATH]; swprintf(ack, MAX_PATH, L"%ls\\ack%d", g_dir.c_str(), g_waitAck);
        if (!fileExists(ack)) return;
        g_waitAck = 0;
    }
    if (g_until)
    {
        bool ok = false;
        try { ok = g_until(); } catch (...) {}
        if (!ok && nowMs() < g_untilDeadline) return;
        g_untilOk = ok;
        g_until = nullptr;
    }
    if (nowMs() < g_nextAt || g_idx >= g_steps.size()) return;
    g_inStep = true;
    int d = 0;
    size_t idx = g_idx++;
    try { d = g_steps[idx](); }
    catch (...) { tlog(L"FAIL X step %u threw an exception", static_cast<unsigned>(idx)); ++g_fail; }
    g_inStep = false;
    g_nextAt = nowMs() + d;
}

} // namespace

FILE* selfTestLogFile() { return g_tlog; }

void selfTestStart(const std::wstring& args)
{
    size_t p = args.find(L"KexSuite_testdir=");
    if (p != std::wstring::npos)
    {
        size_t e = args.find(L';', p);
        g_dir = args.substr(p + 17, e == std::wstring::npos ? std::wstring::npos : e - p - 17);
    }
    if (g_dir.empty()) g_dir = settings.dir() + L"\\KexSuite-selftest";
    ::CreateDirectoryW(g_dir.c_str(), nullptr);
    g_tlog = _wfopen((g_dir + L"\\selftest-log.txt").c_str(), L"w, ccs=UTF-8");
    tlog(L"KEX Suite self-test (plugin message: %ls)", args.c_str());
    WNDCLASSW wc{}; wc.lpfnWndProc = testWndProc; wc.hInstance = hInst; wc.lpszClassName = L"KexSuiteTestWnd";
    ::RegisterClassW(&wc);
    g_testWnd = ::CreateWindowExW(0, L"KexSuiteTestWnd", L"test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    buildSteps();
    g_nextAt = nowMs() + 800;
    ::SetTimer(nullptr, 0, 40, tick);
}

void selfTestOnNotify(SCNotification* n)
{
    if (n->nmhdr.code == NPPN_GLOBALMODIFIED) ++g_globalModified;
}

} // namespace kex
