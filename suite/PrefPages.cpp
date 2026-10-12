// PrefPages.cpp - Settings > Preferences > "KEX Editing", "KEX Tabs" and "KEX Windows" (the core
// creates the pages inside Notepad++'s Preferences dialog and hands their messages to prefPageProc).
// Like the other Preferences pages, every change takes effect at once; numbers apply half a second
// after the last keystroke.
#include "KexSuite.h"
#include "KexSuiteRes.h"
#include <cwchar>

namespace kex {

namespace {

const UINT_PTR TIMER_APPLY = 1;
HWND g_pages[PAGE_COUNT] = {};
const wchar_t* const kPageTitle[PAGE_COUNT] = { L"KEX Editing", L"KEX Tabs", L"KEX Windows" };
const wchar_t* const kPageName[PAGE_COUNT] = { L"KexEditing", L"KexTabs", L"KexWindows" };
bool g_filling = false;

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
    { IDC_G_TINT, M_GLASS, L"TintPercent", 65 },
};
struct BoolField { int id; int module; const wchar_t* key; int def; };
const BoolField kBools[] = {
    { IDC_L_RESPECT, M_LETTERS, L"RespectSystemAnimationSetting", 1 },
    { IDC_L_UNDO, M_LETTERS, L"AnimateUndoRedo", 0 },
    { IDC_C_FADE, M_CARET, L"FadeBlink", 1 },
    { IDC_C_OVB, M_CARET, L"OvertypeBlock", 1 },
    { IDC_H_KEEP, M_HOTKEY, L"KeepText", 1 },
    { IDC_H_FADE, M_HOTKEY, L"Fade", 1 },
    { IDC_D_CLOSE, M_DESKTOP, L"CloseOnlyThisDesktop", 1 },
    { IDC_S_ANIM, M_SNAP, L"Animate", 1 },
    { IDC_J_ALL, M_KEYS, L"AlsoNotepadKeys", 1 },
    { IDC_G_MARGINS, M_GLASS, L"Margins", 1 },
};
const int kEnable[M_COUNT] = { IDC_L_EN, IDC_C_EN, IDC_H_EN, IDC_T_EN, IDC_D_EN, IDC_S_EN, IDC_J_EN, IDC_G_EN };
const int kStatus[M_COUNT] = { IDC_L_STATUS, IDC_C_STATUS, IDC_H_STATUS, IDC_T_STATUS, IDC_D_STATUS, IDC_S_STATUS, IDC_J_STATUS, IDC_G_STATUS };

bool has(HWND d, int id) { return ::GetDlgItem(d, id) != nullptr; }

void fillStatus(HWND d)
{
    int ok = 0;
    for (int i = 0; i < M_COUNT; ++i)
    {
        Module* m = modules[i];
        if (m->_checkOk) ++ok;
        if (!has(d, kStatus[i])) continue;
        std::wstring s;
        if (!m->_checkOk) s = L"Unavailable: " + m->_checkMsg;
        else s = m->isEnabled() ? L"Running" : L"Off";
        ::SetDlgItemTextW(d, kStatus[i], s.c_str());
    }
    wchar_t b[96]; swprintf(b, 96, L"%d of %d KEX features passed their start-up self-check.", ok, M_COUNT);
    ::SetDlgItemTextW(d, IDC_SUMMARY, b);

    if (has(d, IDC_H_KEYSTATUS))
    {
        HotkeyStats hs = hotkeyStats();
        std::wstring ks;
        if (modules[M_HOTKEY]->isEnabled())
        {
            if (hs.registered) ks = L"works from any program";
            else
            {
                wchar_t e[160];
                swprintf(e, 160, L"taken by another program (error %lu)", hs.lastError);
                ks = e;
            }
        }
        ::SetDlgItemTextW(d, IDC_H_KEYSTATUS, ks.c_str());
    }
    if (has(d, IDC_D_INFO))
    {
        DesktopStats ds = desktopStats();
        std::wstring info;
        if (modules[M_DESKTOP]->isEnabled())
            info = L"Notepad++ is on " + std::to_wstring(ds.registered) + (ds.registered == 1 ? L" desktop. " : L" desktops. ")
                 + L"Open it on another desktop to add that one.";
        ::SetDlgItemTextW(d, IDC_D_INFO, info.c_str());
    }
    if (has(d, IDC_G_INFO)) ::SetDlgItemTextW(d, IDC_G_INFO, glassInfo().c_str());
}

void fill(HWND d)
{
    g_filling = true;
    for (int i = 0; i < M_COUNT; ++i)
        if (has(d, kEnable[i])) ::CheckDlgButton(d, kEnable[i], modules[i]->isEnabled() ? BST_CHECKED : BST_UNCHECKED);
    for (const IntField& f : kInts)
        if (has(d, f.id)) ::SetDlgItemInt(d, f.id, static_cast<UINT>(settings.getInt(modules[f.module]->key(), f.key, f.def)), FALSE);
    for (const BoolField& f : kBools)
        if (has(d, f.id)) ::CheckDlgButton(d, f.id, settings.getInt(modules[f.module]->key(), f.key, f.def) ? BST_CHECKED : BST_UNCHECKED);
    if (has(d, IDC_H_KEY))
    {
        UINT mods = static_cast<UINT>(settings.getInt(modules[M_HOTKEY]->key(), L"Modifiers", MOD_CONTROL | MOD_ALT));
        UINT vk = static_cast<UINT>(settings.getInt(modules[M_HOTKEY]->key(), L"Key", 'M'));
        BYTE hk = 0;
        if (mods & MOD_CONTROL) hk |= HOTKEYF_CONTROL;
        if (mods & MOD_ALT) hk |= HOTKEYF_ALT;
        if (mods & MOD_SHIFT) hk |= HOTKEYF_SHIFT;
        ::SendDlgItemMessageW(d, IDC_H_KEY, HKM_SETHOTKEY, MAKEWORD(vk, hk), 0);
        ::CheckDlgButton(d, IDC_H_WIN, (mods & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED);
    }
    fillStatus(d);
    g_filling = false;
}

// A running feature reads its settings when it starts, so restart it to pick up a change.
void restart(int module)
{
    if (!modules[module]->isEnabled()) return;
    setModuleEnabled(module, false, false);
    setModuleEnabled(module, true, false);
}

void applyNumbers(HWND d)
{
    bool changed[M_COUNT] = {};
    for (const IntField& f : kInts)
    {
        if (!has(d, f.id)) continue;
        BOOL ok = FALSE;
        UINT v = ::GetDlgItemInt(d, f.id, &ok, FALSE);
        if (!ok || static_cast<int>(v) == settings.getInt(modules[f.module]->key(), f.key, f.def)) continue;
        settings.setInt(modules[f.module]->key(), f.key, static_cast<int>(v));
        changed[f.module] = true;
    }
    for (int i = 0; i < M_COUNT; ++i) if (changed[i]) restart(i);
}

void applyHotkey(HWND d)
{
    WORD hk = static_cast<WORD>(::SendDlgItemMessageW(d, IDC_H_KEY, HKM_GETHOTKEY, 0, 0));
    UINT vk = LOBYTE(hk), flags = HIBYTE(hk), mods = 0;
    if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) mods |= MOD_ALT;
    if (flags & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (::IsDlgButtonChecked(d, IDC_H_WIN) == BST_CHECKED) mods |= MOD_WIN;
    if (vk && mods) hotkeyApply(mods, vk);
}

void onCommand(HWND d, int id, int code)
{
    if (g_filling) return;
    if (code == BN_CLICKED)
    {
        for (int i = 0; i < M_COUNT; ++i)
            if (id == kEnable[i])
            {
                bool want = ::IsDlgButtonChecked(d, id) == BST_CHECKED;
                setModuleEnabled(i, want, true);
                if (!want) settings.setInt(modules[i]->key(), L"Enabled", 0);
                refreshPrefPages();
                return;
            }
        for (const BoolField& f : kBools)
            if (id == f.id)
            {
                settings.setInt(modules[f.module]->key(), f.key, ::IsDlgButtonChecked(d, id) == BST_CHECKED ? 1 : 0);
                restart(f.module);
                refreshPrefPages();
                return;
            }
        if (id == IDC_H_WIN) { applyHotkey(d); refreshPrefPages(); }
        if (id == IDC_J_LIST)
            ::MessageBoxW(::GetParent(d), jetBrainsKeyList().c_str(), L"JetBrains shortcuts", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (code == EN_CHANGE)
    {
        if (id == IDC_H_KEY) { applyHotkey(d); fillStatus(d); return; }
        for (const IntField& f : kInts)
            if (id == f.id) { ::SetTimer(d, TIMER_APPLY, 500, nullptr); return; }
    }
}

int pageIndex(HWND d) { return has(d, IDC_L_EN) ? PAGE_EDITING : (has(d, IDC_S_EN) ? PAGE_TABS : PAGE_WINDOWS); }

} // namespace

HWND prefPage(int i) { return (i >= 0 && i < PAGE_COUNT) ? g_pages[i] : nullptr; }

int pageOfModule(int module)
{
    switch (module)
    {
    case M_LETTERS: case M_CARET: case M_KEYS: return PAGE_EDITING;
    case M_TABS: case M_SNAP: return PAGE_TABS;
    case M_GLASS: return PAGE_WINDOWS;
    default: return PAGE_WINDOWS;
    }
}

void refreshPrefPages()
{
    for (HWND d : g_pages)
        if (d && ::IsWindow(d)) fill(d);
}

void openPreferences() { nppMsg(NPPM_MENUCOMMAND, 0, IDM_SETTING_KEX); }

} // namespace kex

namespace kexsuite {

using namespace kex;

int prefPageCount() { return PAGE_COUNT; }
const wchar_t* prefPageTitle(int page) { return (page >= 0 && page < PAGE_COUNT) ? kPageTitle[page] : L"KEX"; }
const wchar_t* prefPageName(int page) { return (page >= 0 && page < PAGE_COUNT) ? kPageName[page] : L"Kex"; }

intptr_t prefPageProc(HWND d, UINT m, WPARAM w, LPARAM)
{
    if (!modules[0]) return FALSE;
    try
    {
        switch (m)
        {
        case WM_INITDIALOG:
            g_pages[pageIndex(d)] = d;
            fill(d);
            return TRUE;
        case WM_SHOWWINDOW:
            if (w) fill(d);
            break;
        case WM_TIMER:
            if (w == TIMER_APPLY) { ::KillTimer(d, TIMER_APPLY); applyNumbers(d); refreshPrefPages(); return TRUE; }
            break;
        case WM_COMMAND:
            onCommand(d, LOWORD(w), HIWORD(w));
            return TRUE;
        case WM_DESTROY:
            ::KillTimer(d, TIMER_APPLY);
            if (g_pages[pageIndex(d)] == d) g_pages[pageIndex(d)] = nullptr;
            break;
        }
    }
    catch (...) { log(L"exception on a KEX Preferences page"); }
    return FALSE;
}

} // namespace kexsuite
