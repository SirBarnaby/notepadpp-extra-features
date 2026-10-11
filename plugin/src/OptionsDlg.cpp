// OptionsDlg.cpp - Plugins > KEX Suite > Options... (modeless, registered with NPPM_MODELESSDIALOG
// so Tab/Enter work inside Notepad++'s message loop).
#include "KexSuite.h"
#include "resource.h"
#include <cwchar>

namespace kex {

namespace {

HWND g_dlg = nullptr;

struct IntField { int id; int module; const wchar_t* key; int def; };
const IntField kInts[] = {
    { IDC_L_GRAVITY, M_LETTERS, L"Gravity", 2200 },
    { IDC_L_MAX, M_LETTERS, L"MaxLetters", 200 },
    { IDC_L_SPIN, M_LETTERS, L"SpinDegPerSec", 260 },
    { IDC_L_DRIFT, M_LETTERS, L"DriftPxPerSec", 90 },
    { IDC_C_GLIDE, M_CARET, L"GlideMs", 80 },
    { IDC_C_SNAP, M_CARET, L"SnapAfterLines", 12 },
    { IDC_T_ZONE, M_TABS, L"HotZonePx", 6 },
    { IDC_T_DELAY, M_TABS, L"HideDelayMs", 350 },
    { IDC_D_POLL, M_DESKTOP, L"PollMs", 150 },
};
struct BoolField { int id; int module; const wchar_t* key; int def; };
const BoolField kBools[] = {
    { IDC_L_RESPECT, M_LETTERS, L"RespectSystemAnimationSetting", 1 },
    { IDC_L_UNDO, M_LETTERS, L"AnimateUndoRedo", 0 },
    { IDC_C_FADE, M_CARET, L"FadeBlink", 1 },
    { IDC_C_OVB, M_CARET, L"OvertypeBlock", 1 },
    { IDC_H_POSTIT, M_HOTKEY, L"KeepText", 1 },
    { IDC_H_SLIDE, M_HOTKEY, L"Fade", 1 },
    { IDC_D_CLOSE, M_DESKTOP, L"CloseOnlyThisDesktop", 1 },
};
const int kEnable[M_COUNT] = { IDC_L_EN, IDC_C_EN, IDC_H_EN, IDC_T_EN, IDC_D_EN };
const int kStatus[M_COUNT] = { IDC_L_STATUS, IDC_C_STATUS, IDC_H_STATUS, IDC_T_STATUS, IDC_D_STATUS };

void fillStatus(HWND d)
{
    int ok = 0;
    for (int i = 0; i < M_COUNT; ++i)
    {
        Module* m = modules[i];
        if (m->_checkOk) ++ok;
        std::wstring s;
        if (!m->_checkOk) s = L"Unavailable: " + m->_checkMsg;
        else s = m->isEnabled() ? L"Running - self-check OK" : L"Off - self-check OK";
        ::SetDlgItemTextW(d, kStatus[i], s.c_str());
    }
    wchar_t b[96]; swprintf(b, 96, L"KEX Suite: %d/%d features OK", ok, M_COUNT);
    ::SetDlgItemTextW(d, IDC_SUMMARY, b);

    HotkeyStats hs = hotkeyStats();
    std::wstring ks;
    if (modules[M_HOTKEY]->isEnabled())
    {
        if (hs.registered) ks = hs.text + L" is registered";
        else
        {
            wchar_t e[160];
            swprintf(e, 160, L"%ls is taken by another program (error %lu)", hs.text.c_str(), hs.lastError);
            ks = e;
        }
    }
    ::SetDlgItemTextW(d, IDC_H_KEYSTATUS, ks.c_str());

    DesktopStats ds = desktopStats();
    DesktopProvider* p = desktopProvider();
    std::wstring info;
    if (modules[M_DESKTOP]->isEnabled())
        info = L"Registered desktops: " + std::to_wstring(ds.registered) + L"   (" + (p ? p->name() : L"") + L")";
    ::SetDlgItemTextW(d, IDC_D_INFO, info.c_str());
}

void fill(HWND d)
{
    for (int i = 0; i < M_COUNT; ++i)
        ::CheckDlgButton(d, kEnable[i], modules[i]->isEnabled() ? BST_CHECKED : BST_UNCHECKED);
    for (const IntField& f : kInts)
        ::SetDlgItemInt(d, f.id, static_cast<UINT>(settings.getInt(modules[f.module]->key(), f.key, f.def)), FALSE);
    for (const BoolField& f : kBools)
        ::CheckDlgButton(d, f.id, settings.getInt(modules[f.module]->key(), f.key, f.def) ? BST_CHECKED : BST_UNCHECKED);
    UINT mods = static_cast<UINT>(settings.getInt(modules[M_HOTKEY]->key(), L"Modifiers", MOD_CONTROL | MOD_ALT));
    UINT vk = static_cast<UINT>(settings.getInt(modules[M_HOTKEY]->key(), L"Key", 'M'));
    BYTE hk = 0;
    if (mods & MOD_CONTROL) hk |= HOTKEYF_CONTROL;
    if (mods & MOD_ALT) hk |= HOTKEYF_ALT;
    if (mods & MOD_SHIFT) hk |= HOTKEYF_SHIFT;
    ::SendDlgItemMessageW(d, IDC_H_KEY, HKM_SETHOTKEY, MAKEWORD(vk, hk), 0);
    ::CheckDlgButton(d, IDC_H_WIN, (mods & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED);
    fillStatus(d);
}

void apply(HWND d)
{
    for (const IntField& f : kInts)
    {
        BOOL ok = FALSE;
        UINT v = ::GetDlgItemInt(d, f.id, &ok, FALSE);
        if (ok) settings.setInt(modules[f.module]->key(), f.key, static_cast<int>(v));
    }
    for (const BoolField& f : kBools)
        settings.setInt(modules[f.module]->key(), f.key, ::IsDlgButtonChecked(d, f.id) == BST_CHECKED ? 1 : 0);
    WORD hk = static_cast<WORD>(::SendDlgItemMessageW(d, IDC_H_KEY, HKM_GETHOTKEY, 0, 0));
    UINT vk = LOBYTE(hk), flags = HIBYTE(hk), mods = 0;
    if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) mods |= MOD_ALT;
    if (flags & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (::IsDlgButtonChecked(d, IDC_H_WIN) == BST_CHECKED) mods |= MOD_WIN;
    if (vk && mods) hotkeyApply(mods, vk);
    // restart running features so new tunables take effect; switch features on/off as ticked
    for (int i = 0; i < M_COUNT; ++i)
    {
        bool want = ::IsDlgButtonChecked(d, kEnable[i]) == BST_CHECKED;
        if (modules[i]->isEnabled()) setModuleEnabled(i, false, false);
        if (want) setModuleEnabled(i, true, true);
        settings.setInt(modules[i]->key(), L"Enabled", want ? 1 : 0);
    }
    refreshMenuChecks();
    fill(d);
}

void closeDlg()
{
    if (!g_dlg) return;
    nppMsg(NPPM_MODELESSDIALOG, MODELESSDIALOGREMOVE, reinterpret_cast<LPARAM>(g_dlg));
    ::DestroyWindow(g_dlg);
    g_dlg = nullptr;
}

INT_PTR CALLBACK proc(HWND d, UINT m, WPARAM w, LPARAM)
{
    try
    {
        switch (m)
        {
        case WM_INITDIALOG:
            fill(d);
            nppMsg(NPPM_DARKMODESUBCLASSANDTHEME, static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(d));
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case IDOK: apply(d); closeDlg(); return TRUE;
            case IDCANCEL: closeDlg(); return TRUE;
            case IDC_APPLY: apply(d); return TRUE;
            }
            break;
        case WM_CLOSE:
            closeDlg();
            return TRUE;
        }
    }
    catch (...) { log(L"exception in the Options dialog"); }
    return FALSE;
}

} // namespace

HWND optionsWindow() { return g_dlg; }

void openOptions()
{
    if (g_dlg) { ::ShowWindow(g_dlg, SW_SHOW); ::SetForegroundWindow(g_dlg); fill(g_dlg); return; }
    g_dlg = ::CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_OPTIONS), npp._nppHandle, proc, 0);
    if (!g_dlg) { log(L"could not create the Options dialog (%lu)", ::GetLastError()); return; }
    nppMsg(NPPM_MODELESSDIALOG, MODELESSDIALOGADD, reinterpret_cast<LPARAM>(g_dlg));
    RECT nr, dr;
    ::GetWindowRect(npp._nppHandle, &nr);
    ::GetWindowRect(g_dlg, &dr);
    int x = nr.left + ((nr.right - nr.left) - (dr.right - dr.left)) / 2;
    int y = nr.top + ((nr.bottom - nr.top) - (dr.bottom - dr.top)) / 2;
    ::SetWindowPos(g_dlg, nullptr, std::max(0, x), std::max(0, y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ::ShowWindow(g_dlg, SW_SHOW);
}

} // namespace kex
