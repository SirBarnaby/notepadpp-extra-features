// DesktopSync.cpp - "a Notepad++ for each desktop".
//
// Notepad++ runs once: a second notepad++.exe finds the running window (winmain.cpp), brings it to the
// front and hands it its files, and Windows then switches the user to whichever virtual desktop that
// window is on. With this feature a second notepad++.exe started on a desktop that has no Notepad++
// window becomes a new Notepad++ on that desktop instead (as Notepad++'s own -multiInst does), and it
// opens with a copy of the first window's tabs: the same files in the same views, the same active tabs,
// carets and scroll positions, and a copy of every untitled tab. The core writes and reads that copy
// (NPPM_KEX_SAVEDESKTOPCOPY / NPPM_KEX_LOADDESKTOPCOPY); windows never move between desktops.
// That window keeps its own session and backups for its desktop (session-desktop-<id>.xml and
// backup\desktop-<id>\ in the settings folder): unsaved text in it is backed up like in the first window,
// and the next Notepad++ started on that desktop reopens its tabs instead of copying them again.
#include "KexSuite.h"
#include <objbase.h>
#include <shobjidl.h>
#include <cwchar>

namespace kex {

namespace {

const ULONG_PTR COPYDATA_KEX_TABS = 0x4B455854;   // 'KEXT': "write a copy of your tabs into this folder"
std::wstring g_pendingCopy;     // set in a new instance by desktopHandOff: the folder to open at startup
std::wstring g_handOffNote;     // what desktopHandOff did, logged once the suite is set up
std::wstring g_logName = L"kex.log";   // a window for another desktop logs to its own file
std::wstring g_sessionKey;      // a window for another desktop: its desktop's session name
DesktopStats s_stats;

BOOL CALLBACK collectNpp(HWND h, LPARAM l)
{
    wchar_t cls[32] = {};
    if (::IsWindowVisible(h) && ::GetClassNameW(h, cls, 31) && !wcscmp(cls, L"Notepad++"))
        reinterpret_cast<std::vector<HWND>*>(l)->push_back(h);
    return TRUE;
}

ULONGLONG processStartTime(HWND h)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    ULONGLONG t = ~0ull;
    if (HANDLE p = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        FILETIME c, e, k, u;
        if (::GetProcessTimes(p, &c, &e, &k, &u)) t = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
        ::CloseHandle(p);
    }
    return t;
}

std::wstring imagePath(DWORD pid)
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = MAX_PATH * 2 - 1;
    HANDLE p = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    if (!::QueryFullProcessImageNameW(p, 0, buf, &n)) buf[0] = 0;
    ::CloseHandle(p);
    return buf;
}

// %TEMP%\KEX-Notepad++\desktop-copy-<pid of the new instance>
std::wstring copyRoot()
{
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    std::wstring d = tmp;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return d + L"KEX-Notepad++\\desktop-copy-";
}

void removeFolder(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ::DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (::FindNextFileW(f, &fd));
        ::FindClose(f);
    }
    ::RemoveDirectoryW(dir.c_str());
}

// Is one of these windows on the desktop the user is looking at? -1: cannot tell (no virtual desktop
// API, as under Wine). The self-test sets KEX_SELFTEST_OTHER_DESKTOP to 1 (none here) or 0 (all here).
int onCurrentDesktop(const std::vector<HWND>& wins, HWND* here)
{
    wchar_t env[8] = {};
    if (::GetEnvironmentVariableW(L"KEX_SELFTEST_OTHER_DESKTOP", env, 8))
    {
        *here = env[0] == L'0' ? wins.front() : nullptr;
        return *here ? 1 : 0;
    }
    HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = -1;
    IVirtualDesktopManager* vdm = nullptr;
    if (SUCCEEDED(::CoCreateInstance(kCLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, kIID_IVirtualDesktopManager, reinterpret_cast<void**>(&vdm))) && vdm)
    {
        for (HWND h : wins)
        {
            BOOL on = FALSE;
            if (FAILED(vdm->IsWindowOnCurrentVirtualDesktop(h, &on))) continue;
            result = 0;
            if (on) { *here = h; result = 1; break; }
        }
        vdm->Release();
    }
    if (SUCCEEDED(init)) ::CoUninitialize();
    return result;
}

// "desktop-<id of the virtual desktop the user is on>", empty when Windows doesn't say. Explorer keeps the
// current desktop's id in the registry (per logon session on Windows 10); the foreground window's desktop
// is the fallback. The self-test names its fake desktops with KEX_SELFTEST_DESKTOP_ID.
std::wstring currentDesktopKey()
{
    wchar_t env[48] = {};
    if (::GetEnvironmentVariableW(L"KEX_SELFTEST_DESKTOP_ID", env, 48)) return std::wstring(L"desktop-") + env;
    GUID id{};
    bool found = false;
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    const std::wstring keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\" + std::to_wstring(session) + L"\\VirtualDesktops",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops" };
    for (const std::wstring& k : keys)
    {
        DWORD size = sizeof(id);
        if (::RegGetValueW(HKEY_CURRENT_USER, k.c_str(), L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, &id, &size) == ERROR_SUCCESS
            && size == sizeof(id) && id != GUID_NULL) { found = true; break; }
    }
    if (!found)
    {
        HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IVirtualDesktopManager* vdm = nullptr;
        if (SUCCEEDED(::CoCreateInstance(kCLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, kIID_IVirtualDesktopManager, reinterpret_cast<void**>(&vdm))) && vdm)
        {
            if (HWND fg = ::GetForegroundWindow())
                found = SUCCEEDED(vdm->GetWindowDesktopId(fg, &id)) && id != GUID_NULL;
            vdm->Release();
        }
        if (SUCCEEDED(init)) ::CoUninitialize();
    }
    if (!found) return L"";
    wchar_t text[40] = {};
    swprintf(text, 40, L"%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x", id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1],
             id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
    return std::wstring(L"desktop-") + text;
}

// a session file with at least one tab in it
bool hasSavedTabs(const std::wstring& sessionFile)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, sessionFile.c_str(), L"rb") != 0 || !f) return false;
    std::string text;
    char chunk[4096];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0 && text.size() < (4u << 20); ) text.append(chunk, n);
    fclose(f);
    return text.find("<File ") != std::string::npos;
}

bool perDesktopEnabled(const wchar_t* settingsDir)
{
    std::wstring ini = std::wstring(settingsDir) + L"\\kex.ini";
    return ::GetPrivateProfileIntW(L"PerDesktop", L"Enabled", 1, ini.c_str()) != 0;
}

class DesktopSync : public Module {
public:
    const wchar_t* key() const override { return L"PerDesktop"; }
    const wchar_t* title() const override { return L"A Notepad++ for each desktop"; }

    bool selfCheck(std::wstring& why) override
    {
        wchar_t cls[64] = {};
        ::GetClassNameW(npp._nppHandle, cls, 63);
        // a second notepad++.exe finds Notepad++ with FindWindow(L"Notepad++") (winmain.cpp)
        if (wcscmp(cls, L"Notepad++")) { why = L"unexpected main window class"; return false; }
        DesktopProvider* p = desktopProvider();
        if (!p || !p->ok()) { why = p ? p->error() : L"no virtual desktop support"; return false; }
        return true;
    }

    bool enable() override
    {
        // state of the earlier design (one window that followed the user), no longer used
        for (const std::wstring& s : settings.sectionNames())
            if (s.compare(0, 8, L"Desktop.") == 0) settings.deleteSection(s.c_str());
        return true;
    }

    void disable() override {}
};

} // namespace

Module* createDesktopSync() { return new DesktopSync(); }

DesktopStats desktopStats() { return s_stats; }

const std::wstring& logFileName() { return g_logName; }
std::wstring desktopSessionKey() { return g_sessionKey; }

// At NPPN_READY, before the features start: a new instance opens the copy of the first window's tabs.
void desktopLoadPendingCopy()
{
    if (!g_handOffNote.empty()) log(L"per-desktop: %ls", g_handOffNote.c_str());
    if (g_pendingCopy.empty()) return;
    s_stats.newWindow = true;
    s_stats.copiedTabs = static_cast<int>(nppMsg(NPPM_KEX_LOADDESKTOPCOPY, 0, reinterpret_cast<LPARAM>(g_pendingCopy.c_str())));
    log(L"per-desktop: opened %d copied tab(s) from %ls", s_stats.copiedTabs, g_pendingCopy.c_str());
    tokenImportCopy(g_pendingCopy);   // their bold, italic and colours
    removeFolder(g_pendingCopy);
    g_pendingCopy.clear();
}

// In the running instance (main-window subclass): a new instance on another desktop asks for the tabs.
// Returns tabs + 1, 0 when refused.
LRESULT desktopServeCopy(HWND sender, const COPYDATASTRUCT* cds)
{
    if (!cds || !cds->lpData || cds->cbData < sizeof(wchar_t) || cds->cbData > 4096) return 0;
    std::wstring dir(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
    while (!dir.empty() && dir.back() == L'\0') dir.pop_back();
    // only another process of this same notepad++.exe, and only into our own temporary folder
    DWORD pid = 0;
    if (!sender || !::GetWindowThreadProcessId(sender, &pid) || pid == ::GetCurrentProcessId()) return 0;
    std::wstring self = imagePath(::GetCurrentProcessId()), other = imagePath(pid);
    if (self.empty() || _wcsicmp(self.c_str(), other.c_str())) return 0;
    const std::wstring root = copyRoot();
    if (dir.size() <= root.size() || _wcsnicmp(dir.c_str(), root.c_str(), root.size())
        || dir.find_first_not_of(L"0123456789", root.size()) != std::wstring::npos)
        return 0;
    int n = static_cast<int>(nppMsg(NPPM_KEX_SAVEDESKTOPCOPY, 0, reinterpret_cast<LPARAM>(dir.c_str())));
    if (n >= 0) tokenExportCopy(dir);
    ++s_stats.served;
    s_stats.lastAction = L"copied " + std::to_wstring(n) + L" tab(s) for a new window on another desktop";
    log(L"per-desktop: %ls (pid %lu)", s_stats.lastAction.c_str(), pid);
    return n < 0 ? 0 : n + 1;
}

} // namespace kex

// Called by a second notepad++.exe from winmain.cpp, before the suite is set up (KexSuiteHost.h).
HWND kexsuite::desktopHandOff(HWND running, const wchar_t* settingsDir, wchar_t* desktopKey, int keyLen)
{
    using namespace kex;
    if (desktopKey && keyLen > 0) desktopKey[0] = 0;
    if (!running || !settingsDir || !perDesktopEnabled(settingsDir)) return running;
    std::vector<HWND> wins;
    ::EnumWindows(collectNpp, reinterpret_cast<LPARAM>(&wins));
    if (wins.empty()) return running;   // hidden in the tray: Notepad++ brings it back as usual
    HWND here = nullptr;
    int on = onCurrentDesktop(wins, &here);
    if (on < 0) return running;          // cannot tell: Notepad++'s usual behaviour
    if (on > 0) return here;             // one on this desktop: it gets the files

    // Notepad++ is open only on other desktops: this process becomes a new Notepad++ here, with this
    // desktop's own session. When this desktop has none yet, it starts with a copy of the first one's tabs.
    const std::wstring key = currentDesktopKey();
    g_sessionKey = key;
    g_logName = L"kex-" + (key.empty() ? std::wstring(L"desktop") : key) + L".log";
    if (!key.empty() && desktopKey && static_cast<int>(key.size()) < keyLen)
    {
        wcscpy_s(desktopKey, keyLen, key.c_str());
        if (hasSavedTabs(std::wstring(settingsDir) + L"\\session-" + key + L".xml"))
        {
            g_handOffNote = L"no Notepad++ on this desktop: new window with this desktop's own session (" + key + L")";
            return nullptr;
        }
    }
    HWND source = wins.front();
    ULONGLONG oldest = processStartTime(source);
    for (HWND h : wins)
    {
        ULONGLONG t = processStartTime(h);
        if (t < oldest) { oldest = t; source = h; }
    }
    std::wstring dir = copyRoot();
    ::CreateDirectoryW(dir.substr(0, dir.rfind(L'\\')).c_str(), nullptr);
    dir += std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(dir.c_str(), nullptr);
    // the receiver checks the sender is this same notepad++.exe, as Notepad++ does for its own WM_COPYDATA
    HWND sender = ::CreateWindowExW(0, L"Static", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    COPYDATASTRUCT cds{};
    cds.dwData = COPYDATA_KEX_TABS;
    cds.cbData = static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(dir.c_str());
    DWORD_PTR result = 0;
    LRESULT sent = ::SendMessageTimeoutW(source, WM_COPYDATA, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&cds),
                                         SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &result);
    if (sender) ::DestroyWindow(sender);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(source, &pid);
    if (sent && result > 0)
    {
        g_pendingCopy = dir;
        g_handOffNote = L"no Notepad++ on this desktop: new window with " + std::to_wstring(static_cast<long long>(result) - 1)
                      + L" tab(s) copied from pid " + std::to_wstring(pid) + L" (session " + (key.empty() ? std::wstring(L"none") : key) + L")";
    }
    else
    {
        removeFolder(dir);
        g_handOffNote = L"no Notepad++ on this desktop: new window; pid " + std::to_wstring(pid) + L" sent no tabs";
    }
    return nullptr;
}
