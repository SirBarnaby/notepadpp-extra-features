// SelfTest.cpp - scripted self-test, run only with -pluginMessage="KexSuite_selftest=1".
//
// Timer-driven steps (like the feasibility spike). Every result is logged as PASS/FAIL/INFO to
// <testdir>\selftest-log.txt. "SHOT n name" lines ask the harness (test/run-selftest.sh) for a
// screenshot; the step machine waits for <testdir>\ackN before going on.
// Optional argument: KexSuite_testdir=C:\path (default: <plugin config dir>\KexSuite-selftest).
#include "KexSuite.h"
#include "KexSuiteRes.h"
#include "preference_rc.h"
#include "SciLexer.h"
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

// The on/off tick of a feature on its Preferences page, clicked the way the user does it.
const int kEnableBox[M_COUNT] = { IDC_L_EN, IDC_C_EN, IDC_H_EN, IDC_T_EN, IDC_D_EN, IDC_S_EN, IDC_J_EN, IDC_G_EN, IDC_K_EN };
HWND enableBox(int module)
{
    if (!prefPage(pageOfModule(module))) { openPreferences(); pump(300); }
    HWND p = prefPage(pageOfModule(module));
    return p ? ::GetDlgItem(p, kEnableBox[module]) : nullptr;
}
void clickEnable(int module)
{
    HWND b = enableBox(module);
    if (!b) return;
    ::SendMessageW(b, BM_SETCHECK, ::SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    ::SendMessageW(::GetParent(b), WM_COMMAND, MAKEWPARAM(kEnableBox[module], BN_CLICKED), reinterpret_cast<LPARAM>(b));
}
bool enableTicked(int module) { HWND b = enableBox(module); return b && ::SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; }
HWND prefDialog() { HWND p = prefPage(0); return p ? ::GetParent(p) : nullptr; }
void showPrefPage(const wchar_t* name)
{
    HWND d = prefDialog();
    if (!d) return;
    HWND list = ::GetDlgItem(d, IDC_LIST_DLGTITLE);
    LRESULT i = ::SendMessageW(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name));
    ::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
    ::SendMessageW(d, WM_COMMAND, MAKEWPARAM(IDC_LIST_DLGTITLE, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
}
std::wstring itemText(HWND page, int id) { wchar_t t[256] = {}; if (page) ::GetDlgItemTextW(page, id, t, 255); return t; }

std::string selText(HWND s)
{
    intptr_t a = sci(s, SCI_GETSELECTIONSTART), b = sci(s, SCI_GETSELECTIONEND);
    if (b <= a) return std::string();
    const char* p = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, a, b - a));
    return p ? std::string(p, static_cast<size_t>(b - a)) : std::string();
}
std::string lineText(HWND s, intptr_t line)
{
    intptr_t a = sci(s, SCI_POSITIONFROMLINE, line), b = sci(s, SCI_GETLINEENDPOSITION, line);
    if (a < 0 || b <= a) return std::string();
    const char* p = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, a, b - a));
    return p ? std::string(p, static_cast<size_t>(b - a)) : std::string();
}

// ---- real mouse input for tab drags --------------------------------------------------------------
void mouseTo(int x, int y)
{
    INPUT in{}; in.type = INPUT_MOUSE;
    const int sw = std::max(1, ::GetSystemMetrics(SM_CXSCREEN) - 1), sh = std::max(1, ::GetSystemMetrics(SM_CYSCREEN) - 1);
    in.mi.dx = ::MulDiv(x, 65535, sw); in.mi.dy = ::MulDiv(y, 65535, sh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    ::SendInput(1, &in, sizeof(in));
}
void mouseButton(bool down)
{
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    ::SendInput(1, &in, sizeof(in));
}
RECT screenRectOf(HWND h) { RECT r{}; if (h) ::GetWindowRect(h, &r); return r; }
RECT viewRect(int v) { return screenRectOf(views.tabBar(v)); }   // the tab control spans the whole pane
bool rectNear(const RECT& a, const RECT& b, int tol)
{
    return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol && std::abs(a.right - b.right) <= tol && std::abs(a.bottom - b.bottom) <= tol;
}
int viewOfFile(const std::wstring& name)
{
    for (int v = 0; v < views.count(); ++v)
        for (int i = 0; views.visible(v) && i < views.nbFiles(v); ++i)
            if (fileNameOf(bufferPath(views.bufferId(v, i))) == name) return v;
    return -1;
}
std::wstring g_dragFile;   // the file whose tab is being dragged
// press on the active tab of view v and pull it off the tab strip
void dragStart(int v)
{
    HWND tab = views.tabBar(v);
    int idx = views.currentIndex(v);
    g_dragFile = fileNameOf(bufferPath(views.bufferId(v, idx)));
    RECT r{};
    ::SendMessageW(tab, TCM_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&r));
    ::MapWindowPoints(tab, nullptr, reinterpret_cast<POINT*>(&r), 2);
    const int x = r.left + 14, y = (r.top + r.bottom) / 2;
    mouseTo(x, y); pump(80);
    POINT c{}; ::GetCursorPos(&c);
    wchar_t cls[64] = {}; ::GetClassNameW(::WindowFromPoint(c), cls, 63);
    mouseButton(true); pump(120);
    // a few small moves on the tab itself start the drag (Windows' tab control captures the mouse
    // on the press; under Wine the tab bar only takes it once it is dragging), then pull it away
    for (int i = 1; i <= 4; ++i) { mouseTo(x + 3 * i, y); pump(40); }
    for (int i = 1; i <= 4; ++i) { mouseTo(x + 12, y + 12 * i); pump(30); }
    wchar_t cap[64] = {}; if (::GetCapture()) ::GetClassNameW(::GetCapture(), cap, 63);
    tlog(L"INFO drag %ls from view %d: tab %d at (%d,%d), cursor (%ld,%ld) over %ls, capture %ls, current view %d",
        g_dragFile.c_str(), v + 1, idx, x, y, c.x, c.y, cls, cap, views.current() + 1);
}
void dragTo(int x, int y)
{
    POINT c{}; ::GetCursorPos(&c);
    for (int i = 1; i <= 6; ++i) { mouseTo(c.x + (x - c.x) * i / 6, c.y + (y - c.y) * i / 6); pump(30); }
    pump(300);   // the preview glides in about 120 ms
}
void dragEnd()
{
    mouseButton(false); pump(400);
    std::wstring where;
    for (int v = 0; v < views.count(); ++v)
    {
        wchar_t b[96]; swprintf(b, 96, L" view%d:%ls%d", v + 1, views.visible(v) ? L"" : L"hidden/", views.visible(v) ? views.nbFiles(v) : 0);
        where += b;
    }
    tlog(L"INFO after drop: %ls in view %d;%ls", g_dragFile.c_str(), viewOfFile(g_dragFile) + 1, where.c_str());
}

// ---- glass text area -----------------------------------------------------------------------------
COLORREF g_glassBack[4] = {};
int g_glassTech[4] = {};
const UINT WM_UPDATESCINTILLAS_ = WM_USER + 5000 + 1;   // WordStyleDlg.h: Style Configurator's "apply"
HWND glassCatcherOf(HWND host)
{
    HWND h = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"KexGlassCatcher", nullptr)) != nullptr)
        if (reinterpret_cast<HWND>(::GetWindowLongPtrW(h, GWLP_USERDATA)) == host) return h;
    return nullptr;
}
bool keyedWith(HWND w, COLORREF* key)
{
    if (!(::GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_LAYERED)) return false;
    COLORREF k = 0; BYTE a = 0; DWORD f = 0;
    if (!::GetLayeredWindowAttributes(w, &k, &a, &f)) return false;
    if (key) *key = k;
    return (f & LWA_COLORKEY) != 0;
}
int stylesWithBack(HWND s, COLORREF c)
{
    int n = 0;
    for (int st = 0; st < 256; ++st)
        if (st != STYLE_CALLTIP && static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, st)) == c) ++n;
    return n;
}
RECT clientOnScreen(HWND h)
{
    RECT r{}; ::GetClientRect(h, &r);
    ::MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
HWND nextVisible(HWND h)
{
    HWND n = ::GetWindow(h, GW_HWNDNEXT);
    while (n && !::IsWindowVisible(n)) n = ::GetWindow(n, GW_HWNDNEXT);
    return n;
}
bool viewsKeyed(COLORREF key)
{
    for (int v = 0; v < views.count(); ++v)
        if (HWND s = views.sci(v))
            if (static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) != key) return false;
    return true;
}
// a screen point on the glass: well right of the end of a short line of view 0
POINT glassPoint(int line)
{
    HWND s = views.sci(0);
    intptr_t end = sci(s, SCI_GETLINEENDPOSITION, line);
    POINT pt{ static_cast<int>(sci(s, SCI_POINTXFROMPOSITION, 0, end)) + 80,
              static_cast<int>(sci(s, SCI_POINTYFROMPOSITION, 0, end)) + static_cast<int>(sci(s, SCI_TEXTHEIGHT, line)) / 2 };
    ::ClientToScreen(s, &pt);
    return pt;
}
LPARAM glassLParam(HWND catcher, POINT screenPt)
{
    ::ScreenToClient(catcher, &screenPt);
    return MAKELPARAM(screenPt.x, screenPt.y);
}
std::wstring classOf(HWND h) { wchar_t c[64] = {}; if (h) ::GetClassNameW(h, c, 63); return c; }

// ---- live token styles ------------------------------------------------------------------------------
const int kTokValue = 0x4B54;    // TokenStyles.cpp: the indicator value of its runs
const char* kTokCpp =
    "// token test\r\n"
    "int alpha = 1;\r\n"
    "int beta = alpha + 2; // alpha in a comment\r\n"
    "/* block alpha */\r\n"
    "void gamma() { return; }\r\n"
    "int alphabet = 3; // not a whole word\r\n";

std::string docText(HWND s)
{
    const char* p = reinterpret_cast<const char*>(sci(s, SCI_GETCHARACTERPOINTER));
    return p ? std::string(p, static_cast<size_t>(sci(s, SCI_GETLENGTH))) : std::string();
}
// whole-word occurrences, found independently of Scintilla
std::vector<intptr_t> wordsIn(HWND s, const std::string& w)
{
    std::vector<intptr_t> out;
    const std::string t = docText(s);
    auto wordc = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    for (size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1))
        if ((p == 0 || !wordc(t[p - 1])) && (p + w.size() >= t.size() || !wordc(t[p + w.size()]))) out.push_back(static_cast<intptr_t>(p));
    return out;
}
bool tokOurs(HWND s, int ind, intptr_t a, intptr_t n)
{
    return sci(s, SCI_INDICATORVALUEAT, ind, a) == kTokValue && sci(s, SCI_INDICATOREND, ind, a) >= a + n;
}
int tokRuns(HWND s, int ind)
{
    int n = 0;
    const intptr_t len = sci(s, SCI_GETLENGTH);
    for (intptr_t p = 0; p < len;)
    {
        intptr_t e = sci(s, SCI_INDICATOREND, ind, p);
        if (e <= p) break;
        if (sci(s, SCI_INDICATORVALUEAT, ind, p) == kTokValue) ++n;
        p = e;
    }
    return n;
}
int allOurs(HWND s, int ind, const std::vector<intptr_t>& at, intptr_t n)
{
    int k = 0;
    for (intptr_t a : at) if (tokOurs(s, ind, a, n)) ++k;
    return k;
}
// SCI_GETSTYLEAT hands styles above 127 back negative (a signed char)
int styleAt(HWND s, intptr_t pos) { return static_cast<int>(sci(s, SCI_GETSTYLEAT, pos)) & 0xFF; }
// every character of the occurrences drawn with a bold/italic variant of `base` (-1: any base)
bool styledAs(HWND s, const std::vector<intptr_t>& at, intptr_t n, int flags, int base, std::wstring* why)
{
    for (intptr_t a : at)
        for (intptr_t i = a; i < a + n; ++i)
        {
            const int st = styleAt(s, i);
            int f = 0;
            const int b = tokenVariantBase(s, st, &f);
            const bool ok = flags ? (b >= 0 && f == flags && (base < 0 || b == base)) : b < 0;
            if (!ok)
            {
                if (why) { wchar_t w[160]; swprintf(w, 160, L"position %d: style %d (base %d, flags %d)", static_cast<int>(i), st, b, f); *why = w; }
                return false;
            }
        }
    return true;
}
// what a failed restore step saw
std::wstring tokDiag(HWND s)
{
    const std::vector<intptr_t> be = wordsIn(s, "beta"), ga = wordsIn(s, "gamma");
    int f = 0;
    const int gs = ga.empty() ? -1 : styleAt(s, ga[0]);
    const int gb = ga.empty() ? -1 : tokenVariantBase(s, gs, &f);
    wchar_t w[400];
    swprintf(w, 400, L"[%ls, %d rules, wrapped %d, beta x%d value %d, gamma x%d style %d base %d flags %d, lexed to %d of %d]", curName().c_str(), tokenRuleCount(s),
             tokenDocWrapped(s) ? 1 : 0, static_cast<int>(be.size()), be.empty() ? -1 : static_cast<int>(sci(s, SCI_INDICATORVALUEAT, 24, be[0])),
             static_cast<int>(ga.size()), gs, gb, f, static_cast<int>(sci(s, SCI_GETENDSTYLED)), static_cast<int>(sci(s, SCI_GETLENGTH)));
    return w;
}
void caretOn(HWND s, intptr_t pos) { sci(s, SCI_SETEMPTYSELECTION, pos + 1); }
void tokCommand(UINT id) { ::SendMessageW(npp._nppHandle, WM_COMMAND, id, 0); }
// let the views paint (that is when Scintilla lexes) and the posted work run
void settle(int ms = 250) { for (int v = 0; v < views.count(); ++v) if (HWND s = views.sci(v)) ::UpdateWindow(s); pump(ms); }
bool menuHasId(HMENU m, UINT id) { return m && ::GetMenuState(m, id, MF_BYCOMMAND) != static_cast<UINT>(-1); }
HMENU subWith(HMENU m, UINT id)
{
    if (!m) return nullptr;
    for (int i = 0, n = ::GetMenuItemCount(m); i < n; ++i)
        if (::GetMenuItemID(m, i) == id) return m;
    for (int i = 0, n = ::GetMenuItemCount(m); i < n; ++i)
        if (HMENU r = subWith(::GetSubMenu(m, i), id)) return r;
    return nullptr;
}
std::vector<int> g_tokBase;    // the lexer's styles of k.cpp before any bold/italic
std::vector<int> stylesOf(HWND s)
{
    std::vector<int> v;
    for (intptr_t i = 0, n = sci(s, SCI_GETLENGTH); i < n; ++i) v.push_back(styleAt(s, i));
    return v;
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
            check(modules[i]->isEnabled() == modules[i]->defaultOn(), L"S4", L"%ls %ls at startup, its default", modules[i]->title(), modules[i]->defaultOn() ? L"on" : L"off");
        open(L"d.txt"); open(L"c.txt"); open(L"b.py"); open(L"a.cpp");
        g_origCaretWidth = static_cast<int>(sci(views.sci(0), SCI_GETCARETWIDTH));
        return 600;
    });

    // ---- Settings > KEX Features... and the two Preferences pages ------------------------------
    add([] {
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        wchar_t t[64] = {};
        ::GetMenuStringW(mm, IDM_SETTING_KEX, t, 63, MF_BYCOMMAND);
        check(::GetMenuState(mm, IDM_SETTING_KEX, MF_BYCOMMAND) != static_cast<UINT>(-1), L"M1", L"Settings menu has \"%ls\"", t);
        HMENU pm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPPLUGINMENU, 0));
        bool inPlugins = false;
        for (int i = 0, n = ::GetMenuItemCount(pm); i < n; ++i)
        {
            wchar_t p[128] = {}; ::GetMenuStringW(pm, i, p, 127, MF_BYPOSITION);
            if (wcsstr(p, L"KEX")) inPlugins = true;
        }
        check(!inPlugins, L"M2", L"nothing KEX left in the Plugins menu (the suite is built in)");
        focusEditor();
        ::PostMessageW(npp._nppHandle, WM_SYSCOMMAND, SC_KEYMENU, 't');   // open Settings from the menu bar
        return 500;
    });
    add([] { return shot(L"menu-settings-kex"); });
    add([] { ::EndMenu(); return 300; });
    add([] {
        openPreferences();
        pump(300);
        HWND p = prefPage(0), d = prefDialog();
        bool ticks = true;
        for (int i = 0; i < M_COUNT; ++i) if (enableTicked(i) != modules[i]->isEnabled()) ticks = false;
        check(d && ::IsWindowVisible(d) && p && ::IsWindowVisible(p), L"P1", L"Settings > KEX Features opens Preferences on the \"KEX Editing\" page");
        check(ticks, L"P2", L"the on/off ticks on both pages match the running features");
        tlog(L"INFO page summary: \"%ls\"", itemText(p, IDC_SUMMARY).c_str());
        return 300;
    });
    add([] { return shot(L"preferences-kex-editing"); });
    add([] {
        showPrefPage(L"KEX Windows");
        pump(200);
        HWND p = prefPage(PAGE_WINDOWS);
        std::wstring k = itemText(p, IDC_H_KEYSTATUS);
        check(p && ::IsWindowVisible(p) && !::IsWindowVisible(prefPage(PAGE_EDITING)), L"P3", L"\"KEX Windows\" page shows when picked in the list");
        check(k.find(L"works") != std::wstring::npos, L"P4", L"the page reports the quick note hotkey: \"%ls\"", k.c_str());
        return 300;
    });
    add([] { return shot(L"preferences-kex-windows"); });
    add([] {
        showPrefPage(L"KEX Tabs");
        pump(200);
        HWND p = prefPage(PAGE_TABS);
        check(p && ::IsWindowVisible(p) && !::IsWindowVisible(prefPage(PAGE_WINDOWS)) && ::IsDlgButtonChecked(p, IDC_S_EN) == BST_CHECKED,
            L"P3b", L"\"KEX Tabs\" page shows tab snapping (on) and F12 hover tabs: \"%ls\"", itemText(p, IDC_S_STATUS).c_str());
        return 300;
    });
    add([] { return shot(L"preferences-kex-tabs"); });
    add([] {
        // "another program" owns Ctrl+Alt+M: drop ours, take it with a foreign window, start ours again
        setModuleEnabled(M_HOTKEY, false, false);
        BOOL got = ::RegisterHotKey(g_testWnd, 77, MOD_CONTROL | MOD_ALT, 'M');
        tlog(L"INFO helper window took Ctrl+Alt+M: %d", got);
        setModuleEnabled(M_HOTKEY, true, false);
        HotkeyStats hs = hotkeyStats();
        check(!hs.registered && hs.lastError == ERROR_HOTKEY_ALREADY_REGISTERED, L"P5",
            L"taken hotkey detected: registered=%d error=%lu", hs.registered ? 1 : 0, hs.lastError);
        std::wstring k = itemText(prefPage(PAGE_WINDOWS), IDC_H_KEYSTATUS);
        check(k.find(L"taken") != std::wstring::npos, L"P6", L"the page shows: \"%ls\"", k.c_str());
        return 400;
    });
    add([] { return shot(L"preferences-hotkey-taken"); });
    add([] {
        ::UnregisterHotKey(g_testWnd, 77);
        clickEnable(M_HOTKEY);   // off
        clickEnable(M_HOTKEY);   // and on again
        std::wstring k = itemText(prefPage(PAGE_WINDOWS), IDC_H_KEYSTATUS);
        check(hotkeyStats().registered && k.find(L"works") != std::wstring::npos, L"P7",
            L"after the other program lets go, switching the quick note off and on takes the key back: \"%ls\"", k.c_str());
        HWND p1 = prefPage(PAGE_TABS);
        ::SetDlgItemInt(p1, IDC_T_DELAY, 420, FALSE);   // typed number: applied half a second later
        pump(800);
        check(settings.getInt(modules[M_TABS]->key(), L"HideDelayMs", 0) == 420 && modules[M_TABS]->isEnabled(), L"P8",
            L"a number typed on the page is saved and the feature restarted with it");
        ::SetDlgItemInt(p1, IDC_T_DELAY, 350, FALSE);
        pump(800);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        pump(200);
        check(!::IsWindowVisible(prefDialog()), L"P9", L"Preferences closes");
        focusEditor();
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
            // hidden only where the smooth caret is drawn, so a lost overlay can never leave you without a caret
            bool drawnHere = caretStats().target == s && caretStats().visible;
            if (orig < 0 || cur != (drawnHere ? hiddenOf(orig) : orig)) ok = false;
        }
        check(ok, L"C1", L"Scintilla's caret hidden only in the view the smooth caret is drawn in (%ls)", detail.c_str());
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
        clickEnable(M_CARET);   // its tick on the KEX Editing page
        int r0 = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE)), r1 = static_cast<int>(sci(views.sci(1), SCI_GETCARETSTYLE));
        HWND ov = ::FindWindowW(L"KexSuiteOverlay", L"KEX caret");
        check(!modules[M_CARET]->isEnabled() && r0 == o0 && r1 == o1 && o0 == orig && !ov, L"C11",
            L"caret off from Preferences: style restored exactly (view1 0x%x, view2 0x%x), overlay destroyed", r0, r1);
        return 300;
    });
    add([] { return shot(L"caret-disabled-scintilla-caret-back"); });
    add([] {
        clickEnable(M_CARET);
        focusEditor();
        pump(100);
        check(modules[M_CARET]->isEnabled() && sci(views.sci(0), SCI_GETCARETSTYLE) == hiddenOf(caretOriginalStyle(views.sci(0))), L"C12",
            L"caret back on from Preferences");
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
        ::SendMessageW(npp._nppHandle, NPPM_INTERNAL_RESTOREFROMMINIMIZED_, 0, 0);   // what a second notepad++.exe sends
        pump(400);
        d = desktopStats();
        check(d.registered == 2 && fakeWindowDesktop(npp._nppHandle) == 2, L"D9", L"second instance on unregistered desktop C: registered and moved there (%ls)", d.lastAction.c_str());
        return 200;
    });
    add([] {
        std::wstring regs;
        for (const std::wstring& s : settings.sectionNames()) if (s.compare(0, 8, L"Desktop.") == 0) regs += s + L" ";
        tlog(L"INFO INI sections: %ls", regs.c_str());
        std::wstring a = settings.getStr((L"Desktop." + guidStr(fakeDesktopGuid(0))).c_str(), L"Active0", L"");
        check(regs.find(guidStr(fakeDesktopGuid(0))) != std::wstring::npos && regs.find(guidStr(fakeDesktopGuid(2))) != std::wstring::npos, L"D10",
            L"state persisted per desktop GUID in %ls (A's active tab: %ls)", settings.path().c_str(), a.c_str());
        fakeSwitchTo(0);
        desktopPollNow();   // A is registered: the window follows
        pump(300);
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
        ::DeleteFileW(quickNoteFile().c_str());
        ::ShowWindow(npp._nppHandle, SW_MINIMIZE);
        pump(200);
        if (g_otherWnd) ::SetForegroundWindow(g_otherWnd);
        ::SetCursorPos(420, 300);
        pump(250);
        tlog(L"INFO Notepad++ minimized=%d, foreground is the other program: %d", ::IsIconic(npp._nppHandle), ::GetForegroundWindow() == g_otherWnd);
        int delivered = pressHotkey();
        tlog(L"INFO SendInput Ctrl+Alt+M -> WM_HOTKEY delivered: %ls", delivered ? L"yes" : L"no (handler driven directly)");
        HWND q = quickNoteWindow();
        RECT r{}; if (q) ::GetWindowRect(q, &r);
        POINT pt{ 420, 300 };
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        int dx = (r.left + r.right) / 2 - (mi.rcWork.left + mi.rcWork.right) / 2, dy = (r.top + r.bottom) / 2 - (mi.rcWork.top + mi.rcWork.bottom) / 2;
        check(q && ::IsWindowVisible(q) && std::abs(dx) <= 2 && std::abs(dy) <= 2, L"Q1",
            L"hotkey from another program while Notepad++ is minimized: quick note %ls centred on the screen (off by %d,%d; WM_HOTKEY from real key events: %ls)",
            rectStr(r).c_str(), dx, dy, delivered ? L"yes" : L"no");
        check(q && ::GetForegroundWindow() == q && ::GetFocus() == quickNoteEditor() && ::IsIconic(npp._nppHandle), L"Q2",
            L"the note is in front with the typing focus; Notepad++ stays minimized");
        tap('N'); tap('O'); tap('T'); tap('E');
        return 300;
    });
    add([] { return shot(L"quick-note"); });
    add([] {
        HWND ed = quickNoteEditor();
        char buf[64] = {};
        if (ed) sci(ed, SCI_GETTEXT, sizeof(buf), reinterpret_cast<LPARAM>(buf));
        check(strcmp(buf, "note") == 0, L"Q3", L"typing goes into the note: \"%hs\"", buf);
        int delivered = pressHotkey();
        pump(200);
        std::string saved;
        if (FILE* f = _wfopen(quickNoteFile().c_str(), L"rb")) { char b[64] = {}; size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); saved.assign(b, n); }
        check(!quickNoteWindow() && !::FindWindowW(L"KexQuickNote", nullptr) && ::GetForegroundWindow() == g_otherWnd && saved == "note", L"Q4",
            L"hotkey again: note gone, focus back to the other program, text saved to %ls (\"%hs\"; WM_HOTKEY: %ls)",
            quickNoteFile().c_str(), saved.c_str(), delivered ? L"yes" : L"no");
        return 200;
    });
    add([] {
        pressHotkey();
        HWND ed = quickNoteEditor();
        char buf[64] = {};
        if (ed) sci(ed, SCI_GETTEXT, sizeof(buf), reinterpret_cast<LPARAM>(buf));
        check(ed && strcmp(buf, "note") == 0 && sci(ed, SCI_GETCURRENTPOS) == 4, L"Q5", L"reopened with the text kept, caret at the end: \"%hs\"", buf);
        tap(VK_ESCAPE);
        pump(300);
        check(!quickNoteWindow() && ::GetForegroundWindow() == g_otherWnd, L"Q6", L"Esc hides the note and gives the focus back");
        return 100;
    });
    add([] {
        pressHotkey();
        ::SetForegroundWindow(g_otherWnd);
        pump(250);
        HWND q = quickNoteWindow();
        bool lostFocus = ::GetForegroundWindow() == g_otherWnd;
        pressHotkey();
        check(q && lostFocus && quickNoteWindow() == q && ::GetForegroundWindow() == q, L"Q7", L"hotkey while the note is open but not in front: brought to front, not hidden");
        pressHotkey();
        check(!quickNoteWindow(), L"Q8", L"and the next press hides it");
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        focusEditor();
        pump(200);
        return 200;
    });

    // ---- F12 hover tabs --------------------------------------------------------------------------
    add([] {
        if (!isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(300);
        check(isPostItMode(), L"T0", L"in F12 post-it mode");
        switchTo(L"b.py");
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("\r\n# unsaved edit"));
        switchTo(L"a.cpp");
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        RECT sr; ::GetWindowRect(curSci(), &sr);
        ::SetCursorPos((sr.left + sr.right) / 2, r.top + 2);
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

    // ---- every feature off and on again from Preferences ----------------------------------------
    add([] {
        for (int i = 0; i < M_COUNT; ++i)
        {
            if (!modules[i]->defaultOn()) continue;   // off by default: its own tests switch it on and off
            int origStyle0 = caretOriginalStyle(views.sci(0));
            clickEnable(i);
            pump(100);
            bool off = !modules[i]->isEnabled();
            bool unchecked = !enableTicked(i) && settings.getInt(modules[i]->key(), L"Enabled", 1) == 0;
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
            case M_SNAP: restored = ::FindWindowW(L"KexSuiteSnapPreview", nullptr) == nullptr && !kexsuite::tabSnapOn(); extra = L"preview window destroyed, the core is back to the drop menu"; break;
            }
            check(off && unchecked && restored, L"E1", L"%ls off from Preferences: unticked and remembered, %ls", modules[i]->title(), extra.c_str());
            clickEnable(i);
            pump(100);
            bool on = modules[i]->isEnabled() && enableTicked(i) && settings.getInt(modules[i]->key(), L"Enabled", 0) == 1;
            check(on, L"E2", L"%ls back on from Preferences", modules[i]->title());
        }
        DWORD_PTR r = 0;
        check(::SendMessageTimeoutW(npp._nppHandle, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &r) != 0, L"E3", L"Notepad++ still responsive");
        int ok = 0; for (int i = 0; i < M_COUNT; ++i) ok += modules[i]->_checkOk ? 1 : 0;
        tlog(L"INFO final summary: KEX: %d/%d features OK", ok, M_COUNT);
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
        if (!isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(300);
        RECT r; ::GetWindowRect(views.sci(2), &r);
        ::SetCursorPos((r.left + r.right) / 2, r.top + 2);
        return until([] { return tabsStats().stripVisible; }, 2000);
    });
    add([] {
        if (!views.patched()) return 0;
        TabsStats t = tabsStats();
        RECT r; ::GetWindowRect(views.sci(2), &r);
        check(g_untilOk && t.view == 2 && t.strip.top >= r.top - 2 && t.strip.top <= r.top + 4 && t.strip.left >= r.left - 2 && t.strip.right <= r.right + 2, L"K5",
            L"F12: hovering the top of view 3 shows view 3's tabs over view 3 (strip %ls, view 3 %ls, view %d)", rectStr(t.strip).c_str(), rectStr(r).c_str(), t.view + 1);
        return 100;
    });
    add([] { return views.patched() ? shot(L"kex-view3-hover-tabs") : 0; });
    add([] {
        if (!views.patched()) return 0;
        ::SetCursorPos(10, 10);
        pump(600);
        if (isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(300);
        return 100;
    });
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
    // ---- JetBrains shortcuts ----------------------------------------------------------------------
    add([] {
        writeFile(path(L"j.cpp"), "int value = 1;\r\nint other = value + value;\r\ncall(alpha, \"a string here\", beta);\r\n// fourth line\r\n");
        open(L"j.cpp");
        pump(200);
        if (!modules[M_KEYS]->isEnabled()) setModuleEnabled(M_KEYS, true, false);
        focusEditor();
        HWND s = curSci();
        sci(s, SCI_GOTOPOS, 6);   // inside "value"
        pump(100);
        chord({ VK_MENU }, 'J'); pump(150);
        std::string first = selText(s);
        chord({ VK_MENU }, 'J'); pump(150);
        chord({ VK_MENU }, 'J'); pump(150);
        int n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        check(first == "value" && n == 3, L"J1", L"Alt+J from the caret selects the word, then each next occurrence: \"%hs\", %d selections", first.c_str(), n);
        chord({ VK_SHIFT, VK_MENU }, 'J'); pump(150);
        n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
        ::GetGUIThreadInfo(::GetCurrentThreadId(), &gi);
        check(n == 2 && !(gi.flags & GUI_INMENUMODE), L"J2", L"Shift+Alt+J drops the last one (%d left); releasing Alt does not open the menu bar", n);
        sci(s, SCI_GOTOPOS, 6); pump(50);
        chord({ VK_CONTROL, VK_SHIFT, VK_MENU }, 'J'); pump(150);
        n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        check(n == 3, L"J3", L"Ctrl+Shift+Alt+J selects all %d occurrences", n);
        sci(s, SCI_GOTOPOS, 6);
        return 100;
    });
    add([] {
        HWND s = curSci();
        intptr_t l2 = sci(s, SCI_POSITIONFROMLINE, 2);
        sci(s, SCI_GOTOPOS, l2 + 16);   // inside "string"
        pump(50);
        const char* expect[] = { "string", "a string here", "\"a string here\"", "alpha, \"a string here\", beta", "(alpha, \"a string here\", beta)",
                                 "call(alpha, \"a string here\", beta)", "call(alpha, \"a string here\", beta);" };
        bool ok = true;
        std::string got;
        for (const char* e : expect)
        {
            chord({ VK_CONTROL }, 'W'); pump(120);
            got = selText(s);
            if (got != e) { ok = false; break; }
        }
        check(ok, L"J4", L"Ctrl+W extends the selection word > string > quotes > brackets > call > line (stopped at \"%hs\")", got.c_str());
        chord({ VK_CONTROL, VK_SHIFT }, 'W'); pump(120);
        got = selText(s);
        check(got == "call(alpha, \"a string here\", beta)", L"J5", L"Ctrl+Shift+W shrinks it back: \"%hs\"", got.c_str());
        check(::IsWindow(s) && curName() == L"j.cpp", L"J6", L"Ctrl+W no longer closes the tab while JetBrains keys are on");
        return 100;
    });
    add([] {
        HWND s = curSci();
        intptr_t lines = sci(s, SCI_GETLINECOUNT);
        sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, 3));
        chord({ VK_CONTROL }, 'Y'); pump(150);
        bool deleted = sci(s, SCI_GETLINECOUNT) == lines - 1 && lineText(s, 3).empty();
        chord({ VK_CONTROL }, 'Z'); pump(150);
        check(deleted && sci(s, SCI_GETLINECOUNT) == lines, L"J7", L"Ctrl+Y deletes the line (Ctrl+Z brings it back)");
        sci(s, SCI_GOTOPOS, 0);
        chord({ VK_CONTROL }, VK_OEM_2); pump(150);
        std::string commented = lineText(s, 0);
        chord({ VK_CONTROL }, VK_OEM_2); pump(150);
        check(commented.rfind("//", 0) == 0 && lineText(s, 0) == "int value = 1;", L"J8", L"Ctrl+/ comments the line and uncomments it: \"%hs\"", commented.c_str());
        sci(s, SCI_GOTOPOS, 2);
        chord({ VK_SHIFT, VK_MENU }, VK_DOWN); pump(150);
        bool moved = lineText(s, 1) == "int value = 1;" && lineText(s, 0) == "int other = value + value;";
        chord({ VK_SHIFT, VK_MENU }, VK_UP); pump(150);
        check(moved && lineText(s, 0) == "int value = 1;", L"J9", L"Shift+Alt+Down / Up move the line");
        sci(s, SCI_GOTOPOS, 4);
        chord({ VK_SHIFT }, VK_RETURN); pump(150);
        intptr_t caretLine = sci(s, SCI_LINEFROMPOSITION, sci(s, SCI_GETCURRENTPOS));
        bool newLine = lineText(s, 0) == "int value = 1;" && caretLine == 1 && lineText(s, 1).empty();
        chord({ VK_CONTROL }, 'Z'); pump(150);
        check(newLine, L"J10", L"Shift+Enter starts a new line below without splitting this one");
        std::wstring before = curName();
        chord({ VK_MENU }, VK_RIGHT); pump(200);
        std::wstring after = curName();
        chord({ VK_MENU }, VK_LEFT); pump(200);
        check(after != before && curName() == before, L"J11", L"Alt+Right / Alt+Left switch tabs (%ls > %ls > %ls)", before.c_str(), after.c_str(), curName().c_str());
        return 100;
    });
    add([] {
        // keeping Notepad++'s own keys: Ctrl+Y is redo again, Alt+J still works
        settings.setInt(L"JetBrainsKeys", L"AlsoNotepadKeys", 0);
        setModuleEnabled(M_KEYS, false, false); setModuleEnabled(M_KEYS, true, false);
        focusEditor();
        HWND s = curSci();
        sci(s, SCI_EMPTYUNDOBUFFER);   // so Notepad++'s redo has nothing to redo
        intptr_t lines = sci(s, SCI_GETLINECOUNT);
        sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, 3));
        chord({ VK_CONTROL }, 'Y'); pump(150);
        bool kept = sci(s, SCI_GETLINECOUNT) == lines;
        sci(s, SCI_GOTOPOS, 6);
        chord({ VK_MENU }, 'J'); pump(150);
        check(kept && selText(s) == "value", L"J12", L"\"Also the keys Notepad++ uses\" off: Ctrl+Y left to Notepad++, Alt+J still works");
        settings.setInt(L"JetBrainsKeys", L"AlsoNotepadKeys", 1);
        setModuleEnabled(M_KEYS, false, false);
        sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, 3));
        chord({ VK_MENU }, 'J'); pump(150);
        bool inert = sci(s, SCI_GETSELECTIONEMPTY) != 0;
        ::EndMenu(); tap(VK_ESCAPE); pump(100);
        setModuleEnabled(M_KEYS, true, false);
        check(inert && modules[M_KEYS]->isEnabled(), L"J13", L"switched off, Alt+J does nothing; back on afterwards (%d keys handled in all)", jetBrainsStats().handled);
        return 100;
    });
    // ---- tab snapping: drag a tab to the edge of a view ------------------------------------------
    add([] {
        if (!views.patched()) { tlog(L"SKIP N1-N9: Notepad++ is not KEX-patched"); return 50; }
        // start from one view: every tab back in the main view
        for (int v = 1; v < views.count(); ++v)
            for (int guard = 0; views.visible(v) && views.nbFiles(v) > 0 && guard < 20; ++guard)
            {
                views.activate(v, views.nbFiles(v) - 1);
                pump(50);
                nppMsg(NPPM_MENUCOMMAND, 0, IDM_KEX_DOC_TO_VIEW1);
                pump(80);
            }
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        ::SetForegroundWindow(npp._nppHandle);
        switchTo(L"b.py");
        pump(300);
        check(views.visible(0) && !views.visible(1) && !views.visible(2) && !views.visible(3) && views.nbFiles(0) >= 4, L"N0",
            L"one view with %d tabs to drag from", views.nbFiles(0));
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        RECT rv = viewRect(0);
        const int w = rv.right - rv.left;
        dragStart(0);
        dragTo(rv.right - w / 10, (rv.top + rv.bottom) / 2);
        SnapStats st = snapStats();
        const int mid = rv.left + w / 2;
        bool rightHalf = st.target.left > mid - 12 && st.target.left < mid + 12 && std::abs(st.target.right - rv.right) <= 3
                      && std::abs(st.target.top - rv.top) <= 3 && std::abs(st.target.bottom - rv.bottom) <= 3;
        bool inside = st.window.left >= st.target.left && st.window.right <= st.target.right && st.window.top >= st.target.top && st.window.bottom <= st.target.bottom;
        check(st.visible && rightHalf && inside, L"N1", L"dragging %ls to the right edge previews the right half: %ls (view %ls, preview window %ls)",
            g_dragFile.c_str(), rectStr(st.target).c_str(), rectStr(rv).c_str(), rectStr(st.window).c_str());
        HWND pw = ::FindWindowW(L"KexSuiteSnapPreview", nullptr);
        LONG_PTR ex = pw ? ::GetWindowLongPtrW(pw, GWL_EXSTYLE) : 0;
        check(pw && ::IsWindowVisible(pw) && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED) && (ex & WS_EX_NOACTIVATE), L"N2",
            L"the preview is a click-through layered window that never takes focus");
        return 50;
    });
    add([] { return views.patched() ? shot(L"tab-snap-preview-right") : 0; });
    add([] {
        if (!views.patched()) return 0;
        RECT predicted = snapStats().target;
        dragEnd();
        RECT r1 = viewRect(1);
        check(views.visible(1) && viewOfFile(g_dragFile) == 1 && views.nbFiles(1) == 1 && views.current() == 1 && !snapStats().visible, L"N3",
            L"dropped there: %ls opened in a new view on the right, preview gone", g_dragFile.c_str());
        check(rectNear(r1, predicted, 6), L"N4", L"the new view sits where the preview was: %ls vs preview %ls", rectStr(r1).c_str(), rectStr(predicted).c_str());
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // two views side by side: the bottom of the left one opens a view below it (a quarter)
        switchTo(L"c.txt");
        pump(150);
        RECT rv = viewRect(0);
        const int h = rv.bottom - rv.top;
        dragStart(0);
        dragTo((rv.left + rv.right) / 2, rv.bottom - h / 10);
        SnapStats st = snapStats();
        const int mid = rv.top + h / 2;
        bool bottomHalf = st.visible && st.target.top > mid - 14 && st.target.top < mid + 14 && std::abs(st.target.bottom - rv.bottom) <= 3
                       && std::abs(st.target.left - rv.left) <= 3 && std::abs(st.target.right - rv.right) <= 3;
        check(bottomHalf, L"N5", L"the bottom edge of the left view previews its bottom half: %ls (view %ls)", rectStr(st.target).c_str(), rectStr(rv).c_str());
        return 50;
    });
    add([] { return views.patched() ? shot(L"tab-snap-preview-quarter") : 0; });
    add([] {
        if (!views.patched()) return 0;
        RECT predicted = snapStats().target;
        dragEnd();
        RECT r3 = viewRect(2);
        check(views.visible(2) && viewOfFile(g_dragFile) == 2 && rectNear(r3, predicted, 6), L"N6",
            L"dropped: %ls opened bottom left at %ls (preview %ls)", g_dragFile.c_str(), rectStr(r3).c_str(), rectStr(predicted).c_str());
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // the middle of another view: the tab moves into it, the preview covers that view
        switchTo(L"d.txt");
        pump(150);
        RECT r1 = viewRect(1);
        int before = views.nbFiles(1);
        dragStart(viewOfFile(L"d.txt"));
        dragTo((r1.left + r1.right) / 2, (r1.top + r1.bottom) / 2);
        SnapStats st = snapStats();
        check(st.visible && rectNear(st.target, r1, 3), L"N7", L"over the middle of the right view the preview covers it: %ls (view %ls)",
            rectStr(st.target).c_str(), rectStr(r1).c_str());
        dragEnd();
        check(viewOfFile(L"d.txt") == 1 && views.nbFiles(1) == before + 1, L"N8", L"dropped: d.txt moved into the right view");
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // Esc while dragging cancels: nothing moves
        switchTo(L"a.cpp");
        pump(150);
        const int from = viewOfFile(L"a.cpp");
        RECT r1 = viewRect(1);
        int cancels = snapStats().cancels;
        dragStart(from);
        dragTo((r1.left + r1.right) / 2, r1.bottom - (r1.bottom - r1.top) / 10);
        bool shown = snapStats().visible;
        key(VK_ESCAPE, true); pump(200); key(VK_ESCAPE, false); pump(100);
        bool hidden = !snapStats().visible && snapStats().cancels == cancels + 1;
        dragEnd();
        check(shown && hidden && viewOfFile(L"a.cpp") == from && !views.visible(3), L"N9",
            L"Esc during a drag hides the preview and drops nothing (a.cpp still in view %d)", viewOfFile(L"a.cpp") + 1);
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // switched off: no preview, the classic drop handling (a drop on another view still moves it)
        setModuleEnabled(M_SNAP, false, false);
        switchTo(L"a.cpp");
        pump(150);
        const int from = viewOfFile(L"a.cpp");
        const int to = (from == 1) ? 0 : 1;
        RECT rt = viewRect(to);
        int shows = snapStats().shows;
        dragStart(from);
        dragTo((rt.left + rt.right) / 2, (rt.top + rt.bottom) / 2);
        bool noPreview = !snapStats().visible && snapStats().shows == shows;
        dragEnd();
        check(noPreview && viewOfFile(L"a.cpp") == to, L"N10", L"tab snapping off: no preview, dropping on another view still moves the tab (view %d)", viewOfFile(L"a.cpp") + 1);
        setModuleEnabled(M_SNAP, true, false);
        check(kexsuite::tabSnapOn(), L"N11", L"tab snapping back on");
        return 200;
    });
    // ---- glass text area: colour key, click catcher, theme changes, off again ---------------------
    add([] {
        // Wine has no DWM blur, so the test engine keys the views and places the catcher without it
        settings.setStr(L"Glass", L"Engine", L"keyonly");
        std::string text;
        for (int i = 1; i <= 300; ++i) text += "glass line " + std::to_string(i) + "\r\n";
        writeFile(path(L"g.txt"), text);
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        views.activate(0, 0);
        pump(100);
        open(L"g.txt");
        focusEditor();
        if (views.sci(1)) sci(views.sci(1), SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);   // a Direct2D view, if this Wine has Direct2D
        pump(200);
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            g_glassBack[v] = s ? static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) : 0;
            g_glassTech[v] = s ? static_cast<int>(sci(s, SCI_GETTECHNOLOGY)) : -1;
        }
        tlog(L"INFO glass: g.txt in view %d, theme background %06lx, renderers %d %d %d %d", viewOfFile(L"g.txt") + 1,
             static_cast<unsigned long>(g_glassBack[0]), g_glassTech[0], g_glassTech[1], g_glassTech[2], g_glassTech[3]);
        clickEnable(M_GLASS);
        pump(400);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        pump(200);
        GlassStats g = glassStats();
        COLORREF key = 0;
        const bool keyed = keyedWith(npp._nppHandle, &key);
        check(modules[M_GLASS]->isEnabled() && g.active && keyed && key == (g_glassBack[0] ^ 0x010001) && settings.getInt(L"Glass", L"Enabled", 0) == 1, L"G1",
              L"glass on from Preferences: the main window is colour-keyed with %06lx (theme background %06lx)", static_cast<unsigned long>(key), static_cast<unsigned long>(g_glassBack[0]));
        int left = 0;
        for (int v = 0; v < views.count(); ++v) if (HWND s = views.sci(v)) left += stylesWithBack(s, g_glassBack[v]);
        HWND s0 = views.sci(0);
        check(viewsKeyed(key) && left == 0 && static_cast<COLORREF>(sci(s0, SCI_STYLEGETBACK, STYLE_LINENUMBER)) == key, L"G2",
              L"all four views' backgrounds and the line-number margin are the key (%d styles left on the old background)", left);
        bool tech = true;
        std::wstring ts;
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s) continue;
            const int o = g_glassTech[v], now = static_cast<int>(sci(s, SCI_GETTECHNOLOGY));
            const int want = (o == SC_TECHNOLOGY_DIRECTWRITE || o == SC_TECHNOLOGY_DIRECTWRITERETAIN || o == SC_TECHNOLOGY_DIRECT_WRITE_1) ? SC_TECHNOLOGY_DIRECTWRITEDC : o;
            if (now != want) tech = false;
            ts += std::to_wstring(o) + L">" + std::to_wstring(now) + L" ";
        }
        check(tech, L"G3", L"Direct2D window renderers drawn through a DC while the glass is on (view renderers %ls)", ts.c_str());
        HWND c = glassCatcherOf(npp._nppHandle);
        RECT cr = clientOnScreen(npp._nppHandle), kr = screenRectOf(c);
        check(c && ::IsWindowVisible(c) && ::EqualRect(&cr, &kr) && nextVisible(npp._nppHandle) == c && g.hosts >= 1, L"G4",
              L"a click catcher %ls sits directly behind the window, over its client area %ls", rectStr(kr).c_str(), rectStr(cr).c_str());
        return 200;
    });
    add([] { return shot(L"glass-on"); });
    add([] {
        HWND s = views.sci(0);
        HWND c = glassCatcherOf(npp._nppHandle);
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        sci(s, SCI_GOTOPOS, 0);
        const int line = 3;
        POINT pt = glassPoint(line);
        const int clicks = glassStats().clicks;
        ::SendMessageW(c, WM_LBUTTONDOWN, MK_LBUTTON, glassLParam(c, pt));
        ::SendMessageW(c, WM_LBUTTONUP, 0, glassLParam(c, pt));
        pump(150);
        const intptr_t caret = sci(s, SCI_GETCURRENTPOS), end = sci(s, SCI_GETLINEENDPOSITION, line);
        check(caret == end && glassStats().clicks == clicks + 1 && glassStats().lastTarget == s && ::GetFocus() == s, L"G5",
              L"a click on the glass right of line %d goes to the view under it: caret %d, line end %d", line + 1, static_cast<int>(caret), static_cast<int>(end));
        HWND real = ::WindowFromPoint(pt);
        check(kexsuite::windowFromPoint(pt) == s, L"G6", L"the core's windowFromPoint finds the view there (plain WindowFromPoint gives %ls)", classOf(real).c_str());
        const int wheels = glassStats().wheels;
        ::SendMessageW(c, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-3 * WHEEL_DELTA)), MAKELPARAM(pt.x, pt.y));
        pump(150);
        const intptr_t first = sci(s, SCI_GETFIRSTVISIBLELINE);
        check(first > 0 && glassStats().wheels == wheels + 1, L"G7", L"the mouse wheel on the glass scrolls the view under it (first line %d)", static_cast<int>(first) + 1);
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        pt = glassPoint(line);
        ::PostMessageW(c, WM_RBUTTONDOWN, MK_RBUTTON, glassLParam(c, pt));
        ::PostMessageW(c, WM_RBUTTONUP, 0, glassLParam(c, pt));
        return 500;
    });
    add([] {
        HWND menu = ::FindWindowW(L"#32768", nullptr);
        const bool shown = menu && ::IsWindowVisible(menu);
        ::EndMenu();
        check(shown, L"G8", L"a right-click on the glass opens the editor's context menu");
        return 300;
    });
    add([] {
        COLORREF key = glassStats().key;
        int remaps = glassStats().remaps;
        ::SendMessageW(npp._nppHandle, WM_UPDATESCINTILLAS_, FALSE, 0);
        int left = stylesWithBack(views.sci(0), g_glassBack[0]);
        check(viewsKeyed(key) && left == 0 && glassStats().remaps > remaps, L"G9",
              L"Notepad++ re-applying the theme (Style Configurator) keeps the glass (%d styles left on the old background)", left);
        focusEditor();
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_PYTHON);
        pump(100);
        left = stylesWithBack(views.sci(0), g_glassBack[0]);
        bool lang = viewsKeyed(key) && left == 0;
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_TEXT);
        pump(100);
        check(lang && viewsKeyed(key), L"G10", L"switching g.txt to Python and back keeps the glass (%d styles left)", left);
        glassTestDragEnter();
        const bool solid = !keyedWith(npp._nppHandle, nullptr) && glassStats().dragSolidNow;
        check(solid, L"G11", L"dragging something over the glass turns it solid, so the view's own drop handling gets the drop");
        return 400;
    });
    add([] {
        check(keyedWith(npp._nppHandle, nullptr) && !glassStats().dragSolidNow, L"G12", L"the glass comes back once the mouse button is up");
        HWND c = glassCatcherOf(npp._nppHandle);
        if (g_otherWnd && ::IsWindow(g_otherWnd)) { ::SetForegroundWindow(g_otherWnd); pump(250); }
        ::SetForegroundWindow(npp._nppHandle);
        pump(250);
        ::MoveWindow(npp._nppHandle, 60, 40, 900, 640, TRUE);
        pump(400);
        RECT cr = clientOnScreen(npp._nppHandle), kr = screenRectOf(c);
        check(nextVisible(npp._nppHandle) == c && ::EqualRect(&cr, &kr), L"G13",
              L"after another program was in front and the window moved, the catcher is still right behind it at %ls", rectStr(kr).c_str());
        openPreferences();
        pump(300);
        showPrefPage(L"KEX Windows");
        pump(200);
        HWND p = prefPage(PAGE_WINDOWS);
        std::wstring st = itemText(p, IDC_G_STATUS), info = itemText(p, IDC_G_INFO), tint = itemText(p, IDC_G_TINT);
        check(st == L"Running" && info.rfind(L"On:", 0) == 0 && tint == L"65", L"G14", L"KEX Windows page: \"%ls\", \"%ls\", tint %ls%%", st.c_str(), info.c_str(), tint.c_str());
        return 200;
    });
    add([] { return shot(L"preferences-kex-windows-glass"); });
    add([] {
        clickEnable(M_GLASS);
        pump(300);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        pump(200);
        const bool layered = (::GetWindowLongPtrW(npp._nppHandle, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        bool back = true, tech = true;
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v))
            {
                if (static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) != g_glassBack[v]) back = false;
                if (static_cast<int>(sci(s, SCI_GETTECHNOLOGY)) != g_glassTech[v]) tech = false;
            }
        check(!modules[M_GLASS]->isEnabled() && !layered && back && tech && !glassCatcherOf(npp._nppHandle) && settings.getInt(L"Glass", L"Enabled", 1) == 0, L"G15",
              L"glass off: the window is not layered, the theme's backgrounds and renderers are back, the catcher is gone");
        if (views.sci(1)) sci(views.sci(1), SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DEFAULT);
        return 200;
    });
    // ---- live token styles: Style All Occurrences of Token that follows the text, bold/italic, menus -----
    add([] {
        writeFile(path(L"k.cpp"), kTokCpp);
        ::DeleteFileW(tokenStoreFile().c_str());
        views.activate(0, 0);
        pump(100);
        open(L"k.cpp");
        focusEditor();
        pump(300);
        HWND s = curSci();
        TokenStats ts = tokenStats();
        check(modules[M_TOKENS]->isEnabled() && ts.hooked && tokenDocWrapped(s) && ts.wrapperCalls > 0, L"L1",
              L"live token styles on by default; k.cpp's lexer goes through the suite (%d lexers wrapped, %d alive)", ts.wrapperCalls, ts.liveWrappers);
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        caretOn(s, al[0]);
        tokCommand(IDM_SEARCH_MARKALLEXT1);   // Search > Style All Occurrences of Token > Using 1st Style
        settle();
        check(al.size() == 4 && allOurs(s, 25, al, 5) == 4 && tokRuns(s, 25) == 4 && tokenRuleCount(s) == 1, L"L2",
              L"Style All Occurrences > 1st style on \"alpha\": %d of %d whole-word occurrences styled live (not \"alphabet\"), %d runs",
              allOurs(s, 25, al, 5), static_cast<int>(al.size()), tokRuns(s, 25));
        return 200;
    });
    add([] {
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("int delta = alpha * 2;\r\n"));
        pump(300);   // painting lexes the new line
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        check(al.size() == 5 && tokOurs(s, 25, al.back(), 5), L"L3", L"an \"alpha\" typed later is styled too (%d of %d)",
              allOurs(s, 25, al, 5), static_cast<int>(al.size()));
        // edit the first one into "alphx": it loses the style
        intptr_t a = al[0];
        sci(s, SCI_SETTARGETRANGE, a + 4, a + 5);
        sci(s, SCI_REPLACETARGET, 1, reinterpret_cast<LPARAM>("x"));
        settle();
        check(!tokOurs(s, 25, a, 4) && sci(s, SCI_INDICATORVALUEAT, 25, a) == 0 && tokRuns(s, 25) == 4, L"L4",
              L"an occurrence edited into \"alphx\" loses the style; the other %d keep it", tokRuns(s, 25));
        sci(s, SCI_SETTARGETRANGE, a + 4, a + 5);
        sci(s, SCI_REPLACETARGET, 1, reinterpret_cast<LPARAM>("a"));
        settle();
        g_tokBase = stylesOf(s);
        return 100;
    });
    add([] {
        HWND s = curSci();
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        caretOn(s, al[1]);
        tokCommand(static_cast<UINT>(tokenCommandId(0)));   // Bold
        settle(400);
        const intptr_t code = al[1];
        const int st = styleAt(s, code);
        int f = 0;
        const int base = tokenVariantBase(s, st, &f);
        check(base == SCE_C_IDENTIFIER && f == 1 && sci(s, SCI_STYLEGETWEIGHT, st) >= SC_WEIGHT_BOLD &&
              sci(s, SCI_STYLEGETFORE, st) == sci(s, SCI_STYLEGETFORE, SCE_C_IDENTIFIER) && sci(s, SCI_STYLEGETSIZEFRACTIONAL, st) == sci(s, SCI_STYLEGETSIZEFRACTIONAL, SCE_C_IDENTIFIER),
              L"L5", L"Bold: \"alpha\" in code is drawn with style %d, a bold copy of the identifier style %d (weight %d)", st, base, static_cast<int>(sci(s, SCI_STYLEGETWEIGHT, st)));
        std::wstring why;
        const bool all = styledAs(s, al, 5, 1, -1, &why);
        int fc = 0, fb = 0;
        const int bc = tokenVariantBase(s, styleAt(s, al[2]), &fc);
        const int bb = tokenVariantBase(s, styleAt(s, al[3]), &fb);
        // apart from the swapped styles, the lexer's output is exactly what it was
        std::vector<int> now = stylesOf(s);
        int diff = -1;
        for (size_t i = 0; i < now.size() && i < g_tokBase.size(); ++i)
        {
            const int b = tokenVariantBase(s, now[i], nullptr);
            if ((b >= 0 ? b : now[i]) != g_tokBase[i]) { diff = static_cast<int>(i); break; }
        }
        check(all && bc == SCE_C_COMMENTLINE && bb == SCE_C_COMMENT && now.size() == g_tokBase.size() && diff < 0,
              L"L6", L"all 5 are bold, each on its own lexer style (comment line %d, block comment %d); every other character keeps the lexer's style (first difference: %d) %ls", bc, bb, diff, why.c_str());
        return 200;
    });
    add([] { return shot(L"tokens-live"); });
    add([] {
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("// more alpha here\r\n"));
        settle(400);
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        std::wstring why;
        check(al.size() == 6 && styledAs(s, std::vector<intptr_t>{ al.back() }, 5, 1, SCE_C_COMMENTLINE, &why) && tokOurs(s, 25, al.back(), 5), L"L7",
              L"a new \"alpha\" typed in a comment comes out bold and coloured straight away (%d found) %ls", static_cast<int>(al.size()), why.c_str());
        caretOn(s, al[1]);
        tokCommand(static_cast<UINT>(tokenCommandId(1)));   // Italic
        settle(400);
        const int st = styleAt(s, al[1]);
        check(styledAs(s, al, 5, 3, -1, &why) && sci(s, SCI_STYLEGETITALIC, st) && sci(s, SCI_STYLEGETWEIGHT, st) >= SC_WEIGHT_BOLD, L"L8",
              L"Italic too: style %d is bold italic %ls", st, why.c_str());
        ::SendMessageW(npp._nppHandle, WM_UPDATESCINTILLAS_, FALSE, 0);   // Style Configurator's apply
        settle(400);
        const int st2 = styleAt(s, al[1]);
        check(sci(s, SCI_STYLEGETITALIC, st2) && sci(s, SCI_STYLEGETWEIGHT, st2) >= SC_WEIGHT_BOLD && tokOurs(s, 25, al[1], 5), L"L9",
              L"after Notepad++ applies the theme again, the bold italic and the colour are still there (style %d)", st2);
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_PYTHON);
        settle(300);
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_CPP);
        settle(400);
        al = wordsIn(s, "alpha");
        const size_t vp = docText(s).find("void");
        const int vs = styleAt(s, static_cast<intptr_t>(vp)), vb = vp < g_tokBase.size() ? g_tokBase[vp] : -1;
        check(styledAs(s, al, 5, 3, -1, &why) && vs == vb && vs != 0, L"L10",
              L"switching the language to Python and back keeps them bold italic, and C++ colouring is back (\"void\" style %d, was %d) %ls (%d variants made, %d lexer collisions)",
              vs, vb, why.c_str(), tokenStats().variantsMade, tokenStats().collisions);
        return 200;
    });
    add([] {
        HWND s = curSci();
        caretOn(s, wordsIn(s, "beta")[0]);
        focusEditor();
        pump(100);
        chord({ VK_CONTROL, VK_SHIFT }, 'H');
        return 600;
    });
    add([] {
        HWND menu = ::FindWindowW(L"#32768", nullptr);
        check(menu && ::IsWindowVisible(menu) && tokenStats().menus >= 1, L"L11", L"Ctrl+Shift+H in the editor pops the token menu up at the caret");
        return shot(L"tokens-menu");
    });
    add([] {
        tap('2');     // "&2nd style"
        return 500;
    });
    add([] {
        HWND s = curSci();
        std::vector<intptr_t> be = wordsIn(s, "beta");
        check(!::FindWindowW(L"#32768", nullptr) && be.size() == 1 && tokOurs(s, 24, be[0], 4) && tokenRuleCount(s) == 2, L"L12",
              L"picking \"2nd style\" there styles \"beta\" with Mark Style 2 (%d tokens styled now)", tokenRuleCount(s));
        caretOn(s, be[0]);
        ::PostMessageW(npp._nppHandle, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(s), MAKELPARAM(-1, -1));
        return 500;
    });
    add([] {
        HWND menu = ::FindWindowW(L"#32768", nullptr);
        HMENU top = menu ? reinterpret_cast<HMENU>(::SendMessageW(menu, MN_GETHMENU, 0, 0)) : nullptr;
        HMENU sub = subWith(top, IDM_SEARCH_MARKALLEXT1);
        if (sub) ::SendMessageW(npp._nppHandle, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), 0);   // as Windows does when it opens
        const bool bold = menuHasId(sub, static_cast<UINT>(tokenCommandId(0))) && menuHasId(sub, static_cast<UINT>(tokenCommandId(1))) && menuHasId(sub, static_cast<UINT>(tokenCommandId(2)));
        const bool checked = sub && (::GetMenuState(sub, IDM_SEARCH_MARKALLEXT2, MF_BYCOMMAND) & MF_CHECKED) && !(::GetMenuState(sub, IDM_SEARCH_MARKALLEXT1, MF_BYCOMMAND) & MF_CHECKED);
        ::EndMenu();
        check(menu && sub && bold && checked, L"L13", L"right-click > Style All Occurrences of Token has Bold, Italic and Clear This Token, with \"Using 2nd Style\" ticked for \"beta\"");
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        HMENU msub = subWith(mm, IDM_SEARCH_MARKALLEXT1);
        if (msub) ::SendMessageW(npp._nppHandle, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(msub), 0);
        check(menuHasId(msub, static_cast<UINT>(tokenCommandId(0))), L"L14", L"and so does Search > Style All Occurrences of Token");
        return 300;
    });
    add([] {
        HWND s = curSci();
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        caretOn(s, al[2] + 2);
        tokCommand(static_cast<UINT>(tokenCommandId(2)));   // Clear This Token
        settle(400);
        al = wordsIn(s, "alpha");
        std::wstring why;
        check(tokRuns(s, 25) == 0 && styledAs(s, al, 5, 0, -1, &why) && tokenRuleCount(s) == 1 && tokOurs(s, 24, wordsIn(s, "beta")[0], 4), L"L15",
              L"Clear This Token takes the colour, bold and italic off every \"alpha\" and leaves \"beta\" %ls", why.c_str());
        // remembered per file: close k.cpp and open it again
        FILE* f = _wfopen(tokenStoreFile().c_str(), L"rb");
        char buf[2048] = {}; size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0; if (f) fclose(f);
        std::string stored(buf, n);
        check(stored.find("k.cpp") != std::string::npos && stored.find("\tbeta") != std::string::npos && stored.find("\talpha") == std::string::npos, L"L16",
              L"kex-tokens.txt remembers \"beta\" for k.cpp (%d bytes)", static_cast<int>(n));
        caretOn(s, wordsIn(s, "gamma")[0]);
        tokCommand(static_cast<UINT>(tokenCommandId(0)));   // gamma: bold, no colour
        settle(300);
        nppMsg(NPPM_SAVECURRENTFILE);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(300);
        open(L"k.cpp");
        settle(500);
        return 200;
    });
    add([] {
        HWND s = curSci();
        std::wstring why;
        const std::vector<intptr_t> ga = wordsIn(s, "gamma");
        check(curName() == L"k.cpp" && tokenRuleCount(s) == 2 && tokOurs(s, 24, wordsIn(s, "beta")[0], 4) && styledAs(s, ga, 5, 1, -1, &why), L"L17",
              L"closed and opened again, k.cpp has its styled tokens back: \"beta\" coloured, \"gamma\" bold %ls %ls", why.c_str(), tokDiag(s).c_str());
        openPreferences();
        pump(300);
        showPrefPage(L"KEX Highlighting");
        pump(200);
        HWND p = prefPage(PAGE_HIGHLIGHT);
        const WORD hk = static_cast<WORD>(::SendDlgItemMessageW(p, IDC_K_KEY, HKM_GETHOTKEY, 0, 0));
        const std::wstring st = itemText(p, IDC_K_STATUS), info = itemText(p, IDC_K_INFO);
        check(p && ::IsWindowVisible(p) && st == L"Running" && LOBYTE(hk) == 'H' && HIBYTE(hk) == (HOTKEYF_CONTROL | HOTKEYF_SHIFT) && info.find(L"2 tokens") != std::wstring::npos,
              L"L18", L"KEX Highlighting page: \"%ls\", menu key Ctrl+Shift+H, \"%ls\"", st.c_str(), info.c_str());
        return 200;
    });
    add([] { return shot(L"preferences-kex-highlighting"); });
    add([] {
        HWND s = curSci();
        clickEnable(M_TOKENS);   // off
        pump(300);
        settle(400);
        std::wstring why;
        const bool plain = styledAs(s, wordsIn(s, "gamma"), 5, 0, -1, &why);
        check(!modules[M_TOKENS]->isEnabled() && !tokenStats().hooked && tokRuns(s, 24) == 0 && plain && settings.getInt(L"TokenStyles", L"Enabled", 1) == 0, L"L19",
              L"switched off: the colours and the bold are gone %ls", why.c_str());
        clickEnable(M_TOKENS);   // on again: the file's tokens come back from kex-tokens.txt
        pump(300);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        settle(400);
        check(modules[M_TOKENS]->isEnabled() && tokOurs(s, 24, wordsIn(s, "beta")[0], 4) && styledAs(s, wordsIn(s, "gamma"), 5, 1, -1, &why), L"L20",
              L"and on again: k.cpp's tokens are back %ls %ls", why.c_str(), tokDiag(s).c_str());
        tokCommand(IDM_SEARCH_CLEARALLMARKS);   // Search > Clear Style > Clear all Styles
        settle(400);
        FILE* f = _wfopen(tokenStoreFile().c_str(), L"rb");
        char buf[2048] = {}; size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0; if (f) fclose(f);
        check(tokenRuleCount(s) == 0 && tokRuns(s, 24) == 0 && styledAs(s, wordsIn(s, "gamma"), 5, 0, -1, &why) && std::string(buf, n).find("k.cpp") == std::string::npos,
              L"L21", L"Clear all Styles clears the live tokens too, and k.cpp is forgotten %ls", why.c_str());
        focusEditor();
        return 200;
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
