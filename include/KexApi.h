// KexApi.h - messages added to Notepad++ by the KEX four-view patch.
//
// The patch gives Notepad++ four editing views (0 = main, 1 = second, 2 = third, 3 = fourth)
// laid out as a 2x2 grid, and lets any view be torn out into its own top-level window.
// On an unpatched Notepad++ every message below returns 0, so a plugin calls NPPM_KEX_GETINFO
// first and falls back to the standard two-view API when it returns 0.
//
// Legacy plugin messages keep their two-view meaning (NPPM_GETCURRENTVIEW etc. only ever
// report 0 or 1), so existing plugins never see an index they don't expect.
//
// A plugin whose commands should also run while view 3 or 4 has focus exports
//     extern "C" __declspec(dllexport) BOOL kexAware() { return TRUE; }
// Without that export the patched Notepad++ blocks its menu commands in views 3/4 (with a
// message box) and sends it Scintilla notifications from views 0/1 only.
#pragma once
#include <windows.h>

#define KEX_USER (WM_USER + 9000)   // Notepad++ uses WM_USER+0..7999 ranges; 9000+ is free

// LRESULT: MAKELONG(viewCount, patchVersion). 0 = unpatched Notepad++.
#define NPPM_KEX_GETINFO              (KEX_USER + 1)
// LRESULT: index (0..3) of the view that has focus.
#define NPPM_KEX_GETCURRENTVIEW       (KEX_USER + 2)
// wParam: view. LRESULT: that view's Scintilla HWND (NULL if view >= count).
#define NPPM_KEX_GETVIEWSCINTILLA     (KEX_USER + 3)
// wParam: view. LRESULT: that view's tab bar HWND.
#define NPPM_KEX_GETVIEWTABBAR        (KEX_USER + 4)
// wParam: view. LRESULT: TRUE if the view is shown (it has at least one tab).
#define NPPM_KEX_ISVIEWVISIBLE        (KEX_USER + 5)
// wParam: view. LRESULT: number of tabs open in that view.
#define NPPM_KEX_GETNBVIEWFILES       (KEX_USER + 6)
// wParam: view, lParam: tab index. LRESULT: BufferID (0 if out of range).
#define NPPM_KEX_GETVIEWBUFFERID      (KEX_USER + 7)
// wParam: view. LRESULT: index of the active tab in that view, -1 if none.
#define NPPM_KEX_GETVIEWCURRENTINDEX  (KEX_USER + 8)
// wParam: view, lParam: tab index. Activates that tab in that view (and focuses the view).
#define NPPM_KEX_ACTIVATEDOC          (KEX_USER + 9)
// wParam: view, lParam: TRUE = tear the view out into its own window, FALSE = snap it back.
#define NPPM_KEX_TEAROUTVIEW          (KEX_USER + 10)
// wParam: view. LRESULT: top-level HWND currently hosting the view
// (the Notepad++ main window, or the view's own window when torn out).
#define NPPM_KEX_GETVIEWHOSTWINDOW    (KEX_USER + 11)
