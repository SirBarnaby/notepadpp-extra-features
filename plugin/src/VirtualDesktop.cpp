// VirtualDesktop.cpp - the desktop provider behind DesktopSync: the real IVirtualDesktopManager
// (documented shell API) and a fake one used by the self-test, because Wine has no virtual desktops.
#include "KexSuite.h"
#include <objbase.h>
#include <shobjidl.h>
#include <map>
#include <cwchar>

namespace kex {

std::wstring guidStr(const GUID& g)
{
    wchar_t b[64];
    swprintf(b, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

bool guidFromStr(const std::wstring& s, GUID* g)
{
    unsigned long d1; unsigned int d2, d3, b[8];
    if (swscanf(s.c_str(), L"{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    g->Data1 = d1; g->Data2 = static_cast<unsigned short>(d2); g->Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i) g->Data4[i] = static_cast<unsigned char>(b[i]);
    return true;
}

namespace {

// {AA509086-5CA9-4C25-8F95-589D3C07B48A}
const CLSID kCLSID_VirtualDesktopManager = { 0xaa509086, 0x5ca9, 0x4c25, { 0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a } };
// {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}
const IID kIID_IVirtualDesktopManager = { 0xa5cd92ff, 0x29be, 0x454c, { 0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b } };

bool isNullGuid(const GUID& g) { static const GUID z{}; return memcmp(&g, &z, sizeof(GUID)) == 0; }

class RealProvider : public DesktopProvider {
public:
    RealProvider()
    {
        HRESULT ci = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        _comInit = SUCCEEDED(ci);
        _hr = ::CoCreateInstance(kCLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, kIID_IVirtualDesktopManager, reinterpret_cast<void**>(&_vdm));
        if (FAILED(_hr)) _vdm = nullptr;
    }
    ~RealProvider() override
    {
        if (_vdm) _vdm->Release();
        if (_comInit) ::CoUninitialize();
    }
    const wchar_t* name() const override { return L"IVirtualDesktopManager"; }
    bool ok() const override { return _vdm != nullptr; }
    std::wstring error() const override
    {
        wchar_t b[160];
        swprintf(b, 160, L"Windows virtual desktop API unavailable (CoCreateInstance(CLSID_VirtualDesktopManager) = 0x%08lX)", static_cast<unsigned long>(_hr));
        return b;
    }
    bool windowDesktop(HWND h, GUID* out) override
    {
        if (!_vdm || !h) return false;
        GUID g{};
        if (FAILED(_vdm->GetWindowDesktopId(h, &g)) || isNullGuid(g)) return false;
        *out = g;
        return true;
    }
    bool isOnCurrent(HWND h) override
    {
        if (!_vdm || !h) return true;
        BOOL b = TRUE;
        if (FAILED(_vdm->IsWindowOnCurrentVirtualDesktop(h, &b))) return true;
        return b != FALSE;
    }
    bool moveWindow(HWND h, const GUID& d) override
    {
        if (!_vdm || !h) return false;
        HRESULT hr = _vdm->MoveWindowToDesktop(h, d);
        if (FAILED(hr)) log(L"MoveWindowToDesktop(%p, %ls) failed 0x%08lX", h, guidStr(d).c_str(), static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }
    bool currentDesktop(GUID* out) override
    {
        if (!_vdm) return false;
        // 1. Notepad++ itself is on the desktop being shown
        if (isOnCurrent(npp._nppHandle) && windowDesktop(npp._nppHandle, out)) return true;
        // 2. the foreground window's desktop (what the user is looking at)
        HWND fg = ::GetForegroundWindow();
        if (fg && isOnCurrent(fg) && windowDesktop(fg, out)) return true;
        // 3. the shell's own record (undocumented but stable since Windows 10)
        return registryCurrent(out);
    }
    bool exists(const GUID& g) override
    {
        std::vector<BYTE> ids;
        if (!readReg(L"VirtualDesktopIDs", ids) || ids.size() < sizeof(GUID)) return true;   // unknown: keep
        for (size_t i = 0; i + sizeof(GUID) <= ids.size(); i += sizeof(GUID))
            if (!memcmp(&ids[i], &g, sizeof(GUID))) return true;
        return false;
    }

private:
    static bool readKey(HKEY root, const std::wstring& path, const wchar_t* value, std::vector<BYTE>& out)
    {
        HKEY k;
        if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        DWORD type = 0, size = 0;
        bool ok = ::RegQueryValueExW(k, value, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_BINARY && size;
        if (ok)
        {
            out.resize(size);
            ok = ::RegQueryValueExW(k, value, nullptr, &type, out.data(), &size) == ERROR_SUCCESS;
        }
        ::RegCloseKey(k);
        return ok;
    }
    static bool readReg(const wchar_t* value, std::vector<BYTE>& out)
    {
        const std::wstring base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\";
        if (readKey(HKEY_CURRENT_USER, base + L"VirtualDesktops", value, out)) return true;   // Windows 11
        DWORD sid = 0;
        ::ProcessIdToSessionId(::GetCurrentProcessId(), &sid);
        wchar_t p[64]; swprintf(p, 64, L"SessionInfo\\%lu\\VirtualDesktops", sid);
        return readKey(HKEY_CURRENT_USER, base + p, value, out);                                // Windows 10
    }
    static bool registryCurrent(GUID* out)
    {
        std::vector<BYTE> b;
        if (!readReg(L"CurrentVirtualDesktop", b) || b.size() != sizeof(GUID)) return false;
        memcpy(out, b.data(), sizeof(GUID));
        return !isNullGuid(*out);
    }

    IVirtualDesktopManager* _vdm = nullptr;
    HRESULT _hr = E_FAIL;
    bool _comInit = false;
};

// Fake desktops for the self-test: same contract, state kept in memory.
class FakeProvider : public DesktopProvider {
public:
    const wchar_t* name() const override { return L"fake desktops (self-test)"; }
    bool ok() const override { return true; }
    bool currentDesktop(GUID* out) override { *out = fakeDesktopGuid(current); return true; }
    bool windowDesktop(HWND h, GUID* out) override { *out = fakeDesktopGuid(deskOf(h)); return true; }
    bool isOnCurrent(HWND h) override { return deskOf(h) == current; }
    bool moveWindow(HWND h, const GUID& d) override
    {
        for (int i = 0; i < 8; ++i) if (fakeDesktopGuid(i) == d) { win[h] = i; return true; }
        return false;
    }
    bool exists(const GUID& g) override
    {
        for (int i = 0; i < 8; ++i) if (fakeDesktopGuid(i) == g) return true;
        return false;
    }
    int deskOf(HWND h)
    {
        auto it = win.find(h);
        if (it == win.end()) { win[h] = current; return current; }   // a window lives where it was first seen
        return it->second;
    }
    int current = 0;
    std::map<HWND, int> win;
};

RealProvider* s_real = nullptr;
FakeProvider* s_fake = nullptr;
bool s_useFake = false;

} // namespace

GUID fakeDesktopGuid(int i)
{
    GUID g = { 0x4B455844u, 0x534B, 0x5450, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, static_cast<unsigned char>(0xA0 + i) } };
    return g;
}

DesktopProvider* createRealDesktopProvider()
{
    if (!s_real) s_real = new RealProvider();
    return s_real;
}

DesktopProvider* desktopProvider()
{
    if (s_useFake) { if (!s_fake) s_fake = new FakeProvider(); return s_fake; }
    return createRealDesktopProvider();
}

void useFakeDesktops(bool on) { s_useFake = on; }
void fakeSwitchTo(int i) { if (!s_fake) s_fake = new FakeProvider(); s_fake->current = i; }
int fakeWindowDesktop(HWND h) { if (!s_fake) return -1; return s_fake->deskOf(h); }

} // namespace kex
