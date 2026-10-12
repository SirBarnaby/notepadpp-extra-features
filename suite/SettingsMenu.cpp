// SettingsMenu.cpp - the KEX group at the end of Notepad++'s Settings menu: one submenu for each KEX
// Preferences page ("KEX Editing", "KEX Tabs", "KEX Windows", "KEX Highlighting", "KEX Quick Note"),
// holding an on/off switch for every feature on that page and "Preferences..." to open the page.
// The group goes after Notepad++'s own items, so their positions (which Notepad++'s translations use
// to name submenus such as Import) stay as upstream has them.
#include "KexSuite.h"

namespace kex {

namespace {

const wchar_t* const kGroupTitle[PAGE_COUNT] = { L"KEX Editing", L"KEX Tabs", L"KEX Windows", L"KEX Highlighting", L"KEX Quick Note" };
UINT g_first = 0;               // our command ids (NPPM_ALLOCATECMDID): a switch per feature, then "Preferences..." per page
HMENU g_sub[PAGE_COUNT] = {};

HMENU findSettingsMenu()
{
    HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
    for (int i = 0, n = mm ? ::GetMenuItemCount(mm) : 0; i < n; ++i)
        if (HMENU sub = ::GetSubMenu(mm, i))
            if (::GetMenuState(sub, IDM_SETTING_PREFERENCE, MF_BYCOMMAND) != static_cast<UINT>(-1))
                return sub;
    return nullptr;
}

} // namespace

bool buildSettingsMenu()
{
    if (g_first) return true;
    HMENU settingsMenu = findSettingsMenu();
    if (!settingsMenu) { log(L"KEX group: Notepad++'s Settings menu was not found"); return false; }
    int first = 0;
    if (!nppMsg(NPPM_ALLOCATECMDID, M_COUNT + PAGE_COUNT, reinterpret_cast<LPARAM>(&first)) || first <= 0)
    {
        log(L"KEX group: no command ids for the Settings menu");
        return false;
    }
    g_first = static_cast<UINT>(first);
    ::AppendMenuW(settingsMenu, MF_SEPARATOR, 0, nullptr);
    for (int p = 0; p < PAGE_COUNT; ++p)
    {
        HMENU sub = ::CreatePopupMenu();
        for (int i = 0; i < M_COUNT; ++i)
            if (pageOfModule(i) == p) ::AppendMenuW(sub, MF_STRING, g_first + i, modules[i]->title());
        ::AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(sub, MF_STRING, g_first + M_COUNT + p, L"Preferences...");
        ::AppendMenuW(settingsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), kGroupTitle[p]);
        g_sub[p] = sub;
    }
    return true;
}

// WM_INITMENUPOPUP: the switches show which features are running
void settingsMenuPopup(HMENU m)
{
    for (int p = 0; p < PAGE_COUNT; ++p)
    {
        if (!m || m != g_sub[p]) continue;
        for (int i = 0; i < M_COUNT; ++i)
            if (pageOfModule(i) == p)
                ::CheckMenuItem(m, g_first + i, MF_BYCOMMAND | (modules[i]->isEnabled() ? MF_CHECKED : MF_UNCHECKED));
    }
}

// WM_COMMAND: true when the command was one of the group's
bool settingsMenuCommand(UINT id)
{
    if (!g_first || id < g_first || id >= g_first + M_COUNT + PAGE_COUNT) return false;
    const int i = static_cast<int>(id - g_first);
    if (i < M_COUNT) setModuleEnabled(i, !modules[i]->isEnabled(), true);   // stored, as the Preferences tick is
    else nppMsg(NPPM_KEX_SHOWPREFPAGE, static_cast<WPARAM>(i - M_COUNT));
    return true;
}

HMENU settingsSubmenu(int page) { return (page >= 0 && page < PAGE_COUNT) ? g_sub[page] : nullptr; }
UINT settingsSwitchId(int module) { return g_first ? g_first + module : 0; }
UINT settingsPreferencesId(int page) { return g_first ? g_first + M_COUNT + page : 0; }

} // namespace kex
