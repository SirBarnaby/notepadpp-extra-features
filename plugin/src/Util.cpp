// Util.cpp - logging, settings, message windows and the view abstraction.
#include "KexSuite.h"
#include <cstdio>
#include <cstdarg>
#include <cwchar>

namespace kex {

Views views;
Settings settings;

// ---------------------------------------------------------------------------------------------
// logging

static FILE* g_log = nullptr;
extern FILE* selfTestLogFile();   // SelfTest.cpp: non-null while the self-test runs

void log(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 2047, fmt, ap); va_end(ap); buf[2047] = 0;
    if (FILE* t = selfTestLogFile())
    {
        fwprintf(t, L"  [kex] %ls\n", buf); fflush(t);
        return;
    }
    if (!g_log && !settings.dir().empty())
    {
        std::wstring p = settings.dir() + L"\\KexSuite.log";
        g_log = _wfopen(p.c_str(), L"w, ccs=UTF-8");
    }
    if (g_log)
    {
        SYSTEMTIME st; ::GetLocalTime(&st);
        fwprintf(g_log, L"%02d:%02d:%02d.%03d %ls\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(g_log);
    }
}

// ---------------------------------------------------------------------------------------------
// helpers

std::wstring bufferPath(intptr_t bufferId)
{
    if (!bufferId) return L"";
    LRESULT n = nppMsg(NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferId), 0);
    if (n <= 0) return L"";
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    nppMsg(NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferId), reinterpret_cast<LPARAM>(&s[0]));
    s.resize(wcslen(s.c_str()));
    return s;
}

std::wstring fileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

double nowMs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) ::QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool rectsIntersect(const RECT& a, const RECT& b)
{
    RECT r; return ::IntersectRect(&r, &a, &b) != FALSE;
}

// ---------------------------------------------------------------------------------------------
// MsgWindow

static const wchar_t* kMsgClass = L"KexSuiteMsgWindow";

bool MsgWindow::createMsgWindow(const wchar_t* name)
{
    if (_hwnd) return true;
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = hInst;
        wc.lpszClassName = kMsgClass;
        registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    _hwnd = ::CreateWindowExW(0, kMsgClass, name, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, this);
    return _hwnd != nullptr;
}

void MsgWindow::destroyMsgWindow()
{
    if (_hwnd)
    {
        ::SetWindowLongPtrW(_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
}

LRESULT CALLBACK MsgWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<MsgWindow*>(cs->lpCreateParams);
        self->_hwnd = h;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MsgWindow*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self)
    {
        try { return self->onMessage(m, w, l); }
        catch (...) { log(L"exception in message handler (msg 0x%x)", m); return 0; }
    }
    return ::DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Settings

void Settings::init()
{
    wchar_t dir[MAX_PATH * 2] = {};
    nppMsg(NPPM_GETPLUGINSCONFIGDIR, MAX_PATH * 2 - 1, reinterpret_cast<LPARAM>(dir));
    _dir = dir;
    if (_dir.empty())
    {
        wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp); _dir = tmp;
        if (!_dir.empty() && _dir.back() == L'\\') _dir.pop_back();
    }
    ::CreateDirectoryW(_dir.c_str(), nullptr);
    _path = _dir + L"\\KexSuite.ini";
}

int Settings::getInt(const wchar_t* sec, const wchar_t* key, int def) const
{
    return static_cast<int>(::GetPrivateProfileIntW(sec, key, def, _path.c_str()));
}

void Settings::setInt(const wchar_t* sec, const wchar_t* key, int v) const
{
    wchar_t b[32]; swprintf(b, 32, L"%d", v);
    ::WritePrivateProfileStringW(sec, key, b, _path.c_str());
}

std::wstring Settings::getStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) const
{
    std::vector<wchar_t> b(4096);
    DWORD n = ::GetPrivateProfileStringW(sec, key, def, b.data(), static_cast<DWORD>(b.size()), _path.c_str());
    return std::wstring(b.data(), n);
}

void Settings::setStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) const
{
    ::WritePrivateProfileStringW(sec, key, v.c_str(), _path.c_str());
}

void Settings::deleteSection(const wchar_t* sec) const
{
    ::WritePrivateProfileStringW(sec, nullptr, nullptr, _path.c_str());
}

std::vector<std::wstring> Settings::sectionNames() const
{
    std::vector<wchar_t> b(65536);
    DWORD n = ::GetPrivateProfileSectionNamesW(b.data(), static_cast<DWORD>(b.size()), _path.c_str());
    std::vector<std::wstring> out;
    for (DWORD i = 0; i < n && b[i];)
    {
        std::wstring s(&b[i]);
        out.push_back(s);
        i += static_cast<DWORD>(s.size()) + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Views

void Views::init()
{
    LRESULT r = nppMsg(NPPM_KEX_GETINFO);
    _patched = r != 0;
    if (_patched)
    {
        _count = LOWORD(r);
        _patchVersion = HIWORD(r);
        if (_count < 2) _count = 2;
        if (_count > 4) _count = 4;
    }
    else
    {
        _count = 2;
        _patchVersion = 0;
    }
    for (auto& t : _tabCache) t = nullptr;
}

HWND Views::sci(int v) const
{
    if (v < 0 || v >= _count) return nullptr;
    if (_patched) return reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWSCINTILLA, v));
    return v == 0 ? npp._scintillaMainHandle : npp._scintillaSecondHandle;
}

int Views::current() const
{
    if (_patched) return static_cast<int>(nppMsg(NPPM_KEX_GETCURRENTVIEW));
    return static_cast<int>(nppMsg(NPPM_GETCURRENTVIEW));
}

bool Views::visible(int v) const
{
    if (v < 0 || v >= _count) return false;
    if (_patched) return nppMsg(NPPM_KEX_ISVIEWVISIBLE, v) != 0;
    if (v == 0) return ::IsWindowVisible(npp._scintillaMainHandle) != FALSE;
    return nppMsg(NPPM_GETCURRENTDOCINDEX, 0, SUB_VIEW) != -1;
}

int Views::nbFiles(int v) const
{
    if (v < 0 || v >= _count) return 0;
    if (_patched) return static_cast<int>(nppMsg(NPPM_KEX_GETNBVIEWFILES, v));
    return static_cast<int>(nppMsg(NPPM_GETNBOPENFILES, 0, v == 0 ? PRIMARY_VIEW : SECOND_VIEW));
}

intptr_t Views::bufferId(int v, int idx) const
{
    if (v < 0 || v >= _count || idx < 0) return 0;
    if (_patched) return static_cast<intptr_t>(nppMsg(NPPM_KEX_GETVIEWBUFFERID, v, idx));
    LRESULT r = nppMsg(NPPM_GETBUFFERIDFROMPOS, idx, v);
    return r == -1 ? 0 : static_cast<intptr_t>(r);
}

int Views::currentIndex(int v) const
{
    if (v < 0 || v >= _count) return -1;
    if (_patched) return static_cast<int>(nppMsg(NPPM_KEX_GETVIEWCURRENTINDEX, v));
    return static_cast<int>(nppMsg(NPPM_GETCURRENTDOCINDEX, 0, v));
}

void Views::activate(int v, int idx) const
{
    if (v < 0 || v >= _count || idx < 0) return;
    if (_patched) nppMsg(NPPM_KEX_ACTIVATEDOC, v, idx);
    else nppMsg(NPPM_ACTIVATEDOC, v, idx);
}

HWND Views::host(int v) const
{
    if (_patched)
    {
        HWND h = reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWHOSTWINDOW, v));
        if (h) return h;
        HWND s = sci(v);
        if (s) return ::GetAncestor(s, GA_ROOT);
    }
    return npp._nppHandle;
}

int Views::viewOf(HWND h) const
{
    if (!h) return -1;
    for (int v = 0; v < _count; ++v)
        if (sci(v) == h) return v;
    return -1;
}

std::vector<HWND> Views::hostWindows() const
{
    std::vector<HWND> out;
    if (!_patched) return out;
    for (int v = 0; v < _count; ++v)
    {
        HWND h = host(v);
        if (h && h != npp._nppHandle && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    }
    return out;
}

namespace {
struct TabFind { std::vector<HWND> tabs; };
BOOL CALLBACK collectTabs(HWND h, LPARAM p)
{
    wchar_t cls[64] = {};
    ::GetClassNameW(h, cls, 63);
    if (!wcscmp(cls, WC_TABCONTROLW)) reinterpret_cast<TabFind*>(p)->tabs.push_back(h);
    return TRUE;
}
LPARAM tabParam(HWND tab, int i)
{
    TCITEMW it{}; it.mask = TCIF_PARAM;
    if (!::SendMessageW(tab, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(&it))) return 0;
    return it.lParam;
}
}

HWND Views::locateTabBar(int v) const
{
    // Notepad++ stores each tab's BufferID in TCITEM.lParam (DocTabView::addBuffer), so the view's
    // tab control is the SysTabControl32 whose items match that view's buffer list.
    int nb = nbFiles(v);
    if (nb <= 0) return nullptr;
    TabFind f;
    ::EnumChildWindows(npp._nppHandle, collectTabs, reinterpret_cast<LPARAM>(&f));
    HWND best = nullptr; long bestDist = LONG_MAX;
    RECT sr{}; HWND s = sci(v); if (s) ::GetWindowRect(s, &sr);
    for (HWND t : f.tabs)
    {
        int cnt = static_cast<int>(::SendMessageW(t, TCM_GETITEMCOUNT, 0, 0));
        if (cnt != nb) continue;
        bool match = true;
        for (int i = 0; i < cnt && i < 16; ++i)
            if (tabParam(t, i) != static_cast<LPARAM>(bufferId(v, i))) { match = false; break; }
        if (!match) continue;
        RECT tr; ::GetWindowRect(t, &tr);
        long d = labs((tr.left + tr.right) / 2 - (sr.left + sr.right) / 2) + labs(tr.top - sr.top);
        if (d < bestDist) { bestDist = d; best = t; }
    }
    return best;
}

HWND Views::tabBar(int v) const
{
    if (v < 0 || v >= _count) return nullptr;
    if (_patched) return reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWTABBAR, v));
    HWND c = _tabCache[v];
    if (c && ::IsWindow(c))
    {
        int nb = nbFiles(v);
        if (static_cast<int>(::SendMessageW(c, TCM_GETITEMCOUNT, 0, 0)) == nb &&
            (nb == 0 || tabParam(c, 0) == static_cast<LPARAM>(bufferId(v, 0))))
            return c;
    }
    _tabCache[v] = locateTabBar(v);
    return _tabCache[v];
}

} // namespace kex
