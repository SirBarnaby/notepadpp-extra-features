// KexTest: scripted self-test of the KEX four-view patch, run inside Notepad++ under Wine.
// Writes C:\kextest\log.txt; "SHOT n" lines ask the driver for a screenshot (acked via C:\kextest\ackN).
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <cstdio>
#include <cwchar>
#include "PluginInterface.h"
#include "menuCmdID.h"
#include "KexApi.h"

static NppData npp;
static HINSTANCE hInst;
static FILE* logf;
static int step = 0, waitingShot = 0, fails = 0;
static UINT_PTR timerId;
static bool legacyCommandRan = false;
static FuncItem funcs[2];

static void logw(const wchar_t* fmt, ...)
{
	wchar_t buf[2048];
	va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 2047, fmt, ap); va_end(ap); buf[2047] = 0;
	if (!logf) logf = _wfopen(L"C:\\kextest\\log.txt", L"w, ccs=UTF-8");
	if (logf) { fwprintf(logf, L"%ls\n", buf); fflush(logf); }
}
static void check(bool ok, const wchar_t* what) { logw(L"  [%ls] %ls", ok ? L"PASS" : L"FAIL", what); if (!ok) ++fails; }
static void shot(int n) { waitingShot = n; logw(L"SHOT %d", n); }
static void drag(int n, POINT a, POINT b) { waitingShot = n; logw(L"DRAG %d %ld %ld %ld %ld", n, a.x, a.y, b.x, b.y); }
static LRESULT S(UINT m, WPARAM w = 0, LPARAM l = 0) { return ::SendMessage(npp._nppHandle, m, w, l); }
static HWND sciOf(int v) { return reinterpret_cast<HWND>(S(NPPM_KEX_GETVIEWSCINTILLA, v)); }
static HWND tabOf(int v) { return reinterpret_cast<HWND>(S(NPPM_KEX_GETVIEWTABBAR, v)); }
static HWND hostOf(int v) { return reinterpret_cast<HWND>(S(NPPM_KEX_GETVIEWHOSTWINDOW, v)); }
static bool vis(int v) { return S(NPPM_KEX_ISVIEWVISIBLE, v) != 0; }
static int nbFiles(int v) { return static_cast<int>(S(NPPM_KEX_GETNBVIEWFILES, v)); }
static std::wstring title(HWND h) { wchar_t t[512] = {}; ::GetWindowTextW(h, t, 511); return t; }
static void open(const wchar_t* f) { S(NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(f)); }
static void cmd(int id) { S(NPPM_MENUCOMMAND, 0, id); }

static RECT clientRect(int v)
{
	RECT r{}; ::GetWindowRect(tabOf(v), &r);
	::MapWindowPoints(nullptr, npp._nppHandle, reinterpret_cast<POINT*>(&r), 2);
	return r;
}

static void dumpViews(const wchar_t* label)
{
	logw(L"%ls", label);
	for (int v = 0; v < 4; ++v)
	{
		RECT r = clientRect(v);
		bool torn = hostOf(v) != npp._nppHandle;
		logw(L"  view %d: visible=%d files=%d torn=%d tabbar=(%ld,%ld)-(%ld,%ld) shown=%d", v + 1, vis(v), nbFiles(v), torn,
			r.left, r.top, r.right, r.bottom, ::IsWindowVisible(tabOf(v)));
	}
}

static int findIndex(int v, const wchar_t* path)
{
	for (int i = 0; i < nbFiles(v); ++i)
	{
		LRESULT id = S(NPPM_KEX_GETVIEWBUFFERID, v, i);
		wchar_t p[MAX_PATH] = {};
		S(NPPM_GETFULLPATHFROMBUFFERID, id, reinterpret_cast<LPARAM>(p));
		if (_wcsicmp(p, path) == 0) return i;
	}
	return -1;
}

static void activate(int v, const wchar_t* path) { S(NPPM_KEX_ACTIVATEDOC, v, findIndex(v, path)); }

static HWND rowSplitter()
{
	HWND best = nullptr, it = nullptr; long bestArea = 0;
	while ((it = ::FindWindowExW(npp._nppHandle, it, L"splitterContainer", nullptr)) != nullptr)
	{
		RECT r; ::GetWindowRect(it, &r); long a = (r.right - r.left) * (r.bottom - r.top);
		if (::IsWindowVisible(it) && a > bestArea) { bestArea = a; best = it; }
	}
	return best;
}

static BOOL CALLBACK findDlg(HWND h, LPARAM p)
{
	wchar_t cls[64] = {}; ::GetClassNameW(h, cls, 63);
	if (::IsWindowVisible(h) && wcscmp(cls, L"#32770") == 0) { *reinterpret_cast<HWND*>(p) = h; return FALSE; }
	return TRUE;
}

static BOOL CALLBACK dlgText(HWND h, LPARAM p)
{
	wchar_t cls[64] = {}; ::GetClassNameW(h, cls, 63);
	if (wcscmp(cls, L"Static") == 0) { std::wstring t = title(h); if (t.size() > 20) *reinterpret_cast<std::wstring*>(p) = t; }
	return TRUE;
}

static void legacyCommand() { legacyCommandRan = true; }
static void restart() { step = 0; }

static void CALLBACK tick(HWND, UINT, UINT_PTR, DWORD)
{
	HWND n = npp._nppHandle;
	if (waitingShot)
	{
		wchar_t ack[64]; swprintf(ack, 64, L"C:\\kextest\\ack%d", waitingShot);
		if (::GetFileAttributesW(ack) == INVALID_FILE_ATTRIBUTES) return;
		waitingShot = 0;
	}
	static bool busy = false;
	if (busy) return; // a modal box from this step is running its own loop
	busy = true;
	switch (step++)
	{
	case 0:
	{
		::MoveWindow(n, 4, 4, 1100, 760, TRUE);
		LRESULT info = S(NPPM_KEX_GETINFO);
		logw(L"T0 NPPM_KEX_GETINFO views=%d patch=%d", LOWORD(info), HIWORD(info));
		check(LOWORD(info) == 4, L"patched Notepad++ reports four views");
		for (const wchar_t* f : { L"C:\\kextest\\a.txt", L"C:\\kextest\\b.txt", L"C:\\kextest\\c.txt", L"C:\\kextest\\d.txt", L"C:\\kextest\\e.txt" })
			open(f);
		dumpViews(L"T0 five files opened");
		check(vis(0) && !vis(1) && !vis(2) && !vis(3), L"only view 1 is shown at start");
		break;
	}
	case 1:
	{
		activate(0, L"C:\\kextest\\b.txt"); cmd(IDM_KEX_DOC_TO_VIEW2);
		activate(0, L"C:\\kextest\\c.txt"); cmd(IDM_KEX_DOC_TO_VIEW3);
		activate(0, L"C:\\kextest\\d.txt"); cmd(IDM_KEX_DOC_TO_VIEW4);
		dumpViews(L"T1 moved b, c, d to views 2, 3, 4");
		check(vis(0) && vis(1) && vis(2) && vis(3), L"all four views shown");
		check(nbFiles(0) == 2 && nbFiles(1) == 1 && nbFiles(2) == 1 && nbFiles(3) == 1, L"files: 2/1/1/1");
		RECT a = clientRect(0), b = clientRect(1), c = clientRect(2), d = clientRect(3);
		check(a.left < b.left && a.top < c.top && c.left < d.left && b.top < d.top && a.right <= b.left && a.bottom <= c.bottom, L"2x2 grid: 1 top-left, 2 top-right, 3 bottom-left, 4 bottom-right");
		check(abs(a.left - c.left) < 3 && abs(b.left - d.left) < 3 && abs(a.top - b.top) < 3, L"grid lines up");
		shot(1);
		break;
	}
	case 2:
	{
		activate(3, L"C:\\kextest\\d.txt");
		logw(L"T2 view 4 focused: NPPM_KEX_GETCURRENTVIEW=%d, legacy NPPM_GETCURRENTVIEW=%d", (int)S(NPPM_KEX_GETCURRENTVIEW), (int)S(NPPM_GETCURRENTVIEW));
		check(S(NPPM_KEX_GETCURRENTVIEW) == 3, L"KEX API reports view 4 (index 3)");
		check(S(NPPM_GETCURRENTVIEW) == 1, L"legacy API reports the second view (index 1), never 3");
		check(::GetFocus() == sciOf(3), L"keyboard focus is in view 4");
		activate(2, L"C:\\kextest\\c.txt");
		legacyCommandRan = false;
		::PostMessage(n, WM_COMMAND, funcs[1]._cmdID, 0);
		logw(L"T3 posted a command of a two-view plugin while view 3 has focus");
		break;
	}
	case 3:
	{
		HWND dlg = nullptr; ::EnumThreadWindows(::GetCurrentThreadId(), findDlg, reinterpret_cast<LPARAM>(&dlg));
		std::wstring text; if (dlg) ::EnumChildWindows(dlg, dlgText, reinterpret_cast<LPARAM>(&text));
		logw(L"  message box: %ls \"%ls\"", dlg ? L"shown" : L"none", text.c_str());
		check(dlg != nullptr && !legacyCommandRan, L"two-view plugin command is blocked in view 3 with a message");
		if (dlg) { shot(2); ::PostMessage(dlg, WM_COMMAND, IDOK, 0); }
		break;
	}
	case 4:
	{
		activate(0, L"C:\\kextest\\a.txt");
		::SendMessage(n, WM_COMMAND, funcs[1]._cmdID, 0);
		check(legacyCommandRan, L"the same command runs normally in view 1");
		BOOL ok = static_cast<BOOL>(S(NPPM_KEX_TEAROUTVIEW, 3, TRUE));
		HWND host = hostOf(3);
		dumpViews(L"T4 tore view 4 out");
		check(ok && host != n && ::GetParent(sciOf(3)) == host && ::IsWindowVisible(host), L"view 4 lives in its own visible window");
		check(::GetAncestor(host, GA_ROOT) == host && ::GetWindow(host, GW_OWNER) == nullptr, L"that window is top-level and unowned (own taskbar button)");
		RECT b = clientRect(1), a = clientRect(0), c = clientRect(2);
		check(b.bottom >= c.bottom - 3 && b.top <= a.top + 3, L"view 2 now fills the right column");
		::SetWindowPos(host, HWND_TOP, 620, 300, 460, 380, SWP_NOACTIVATE);
		shot(3);
		break;
	}
	case 5:
	{
		activate(3, L"C:\\kextest\\d.txt");
		::SendMessage(sciOf(3), SCI_DOCUMENTEND, 0, 0);
		::SendMessage(sciOf(3), SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("\r\ntyped in the snapped-out view 4"));
		logw(L"T5 typed in torn-out view 4. Notepad++ title=\"%ls\"", title(n).c_str());
		check(title(n).find(L"*") == 0, L"Notepad++ marks the document as modified");
		break;
	}
	case 6:
	{
		logw(L"T5b torn-out window title=\"%ls\"", title(hostOf(3)).c_str());
		check(title(hostOf(3)).find(L"*d.txt") == 0, L"the snapped-out window title shows the document and its modified mark");
		// all four views are in use, so "Move to New Window" has nowhere to go
		activate(0, L"C:\\kextest\\a.txt");
		cmd(IDM_KEX_DOC_TEAROUT);
		check(nbFiles(0) == 2, L"Move to New Window does nothing while all four views are in use");
		// close c.txt: view 3 becomes empty and is hidden
		activate(2, L"C:\\kextest\\c.txt");
		cmd(IDM_FILE_CLOSE);
		dumpViews(L"T6 closed c.txt (view 3's only file)");
		check(!vis(2), L"view 3 hides when its last file closes");
		activate(0, L"C:\\kextest\\a.txt");
		cmd(IDM_KEX_DOC_TEAROUT);
		dumpViews(L"T6b Move to New Window on a.txt");
		check(vis(2) && hostOf(2) != n && findIndex(2, L"C:\\kextest\\a.txt") != -1 && findIndex(0, L"C:\\kextest\\a.txt") == -1, L"a.txt moved into a new window (view 3, snapped out)");
		::SetWindowPos(hostOf(2), HWND_TOP, 40, 380, 460, 330, SWP_NOACTIVATE);
		shot(4);
		break;
	}
	case 7:
	{
		cmd(IDM_KEX_SNAPBACK_ALL);
		dumpViews(L"T7 snapped all views back in");
		bool home = true;
		for (int v = 0; v < 4; ++v) home = home && hostOf(v) == n && ::GetParent(sciOf(v)) == n;
		check(home, L"every view is back inside Notepad++");
		RECT a = clientRect(0), b = clientRect(1), c = clientRect(2), d = clientRect(3);
		check(a.left < b.left && a.top < c.top && c.left < d.left && b.top < d.top, L"the 2x2 grid is restored");
		shot(5);
		break;
	}
	case 8:
	{
		HWND row = rowSplitter();
		::SendMessage(row, WM_COMMAND, 2001, 0); // "Rotate to right" on the middle splitter
		dumpViews(L"T8 rotated the grid to the right");
		RECT a = clientRect(0), b = clientRect(1), c = clientRect(2), d = clientRect(3);
		check(c.left < a.left && c.top < d.top && d.left < b.left && a.top < b.top, L"whole grid rotated clockwise: 3 1 / 4 2");
		shot(6);
		::SendMessage(row, WM_COMMAND, 2000, 0); // and back
		a = clientRect(0); b = clientRect(1); c = clientRect(2); d = clientRect(3);
		check(a.left < b.left && a.top < c.top && c.left < d.left && b.top < d.top, L"rotating left restores 1 2 / 3 4");
		break;
	}
	case 9:
	{
		S(NPPM_KEX_TEAROUTVIEW, 3, TRUE);
		cmd(IDM_FILE_SAVEALL);
		BOOL saved = S(NPPM_SAVECURRENTSESSION, 0, reinterpret_cast<LPARAM>(L"C:\\kextest\\session.xml")) != 0;
		check(saved, L"session saved");
		cmd(IDM_FILE_CLOSEALL);
		dumpViews(L"T9 closed everything");
		int nbVisible = 0; bool docked = true;
		for (int v = 0; v < 4; ++v) { nbVisible += vis(v) ? 1 : 0; docked = docked && hostOf(v) == n; }
		check(nbVisible == 1 && docked, L"after Close All one view (the active one, as upstream) is left and no window is snapped out");
		break;
	}
	case 10:
	{
		S(NPPM_LOADSESSION, 0, reinterpret_cast<LPARAM>(L"C:\\kextest\\session.xml"));
		dumpViews(L"T10 reloaded the session");
		check(vis(0) && vis(1) && vis(2) && vis(3), L"four views restored");
		check(findIndex(1, L"C:\\kextest\\b.txt") != -1 && findIndex(2, L"C:\\kextest\\a.txt") != -1 && findIndex(3, L"C:\\kextest\\d.txt") != -1, L"each view got its files back");
		check(hostOf(3) != n && ::IsWindowVisible(hostOf(3)), L"view 4 comes back snapped out");
		shot(7);
		break;
	}
	case 11:
	{
		// drag the snapped-out window by its title bar onto view 2's tab strip
		HWND host = hostOf(3);
		::SetWindowPos(host, HWND_TOP, 600, 380, 460, 300, 0);
		::SetForegroundWindow(host);
		RECT w{}; ::GetWindowRect(host, &w);
		RECT t{}; ::GetWindowRect(tabOf(1), &t);
		drag(20, POINT{ w.left + 120, w.top + ::GetSystemMetrics(SM_CYFRAME) + ::GetSystemMetrics(SM_CYCAPTION) / 2 }, POINT{ t.left + 300, t.top + 10 });
		break;
	}
	case 12:
	{
		dumpViews(L"T11 dragged the snapped-out view 4 onto view 2's tab bar");
		check(hostOf(3) == n && ::GetParent(sciOf(3)) == n, L"dropping the window on a tab bar snaps it back in");
		// now drag view 1's only tab out of Notepad++
		RECT r{}; TabCtrl_GetItemRect(tabOf(0), 0, &r);
		::MapWindowPoints(tabOf(0), nullptr, reinterpret_cast<POINT*>(&r), 2);
		::SetForegroundWindow(n);
		drag(21, POINT{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, POINT{ 1190, 600 });
		break;
	}
	case 13:
	{
		dumpViews(L"T12 dragged view 1's tab outside Notepad++");
		check(hostOf(0) != n && ::IsWindowVisible(hostOf(0)) && findIndex(0, L"C:\\kextest\\e.txt") != -1, L"a tab dropped outside Notepad++ snaps out into its own window");
		shot(8);
		break;
	}
	default:
		logw(L"RESULT %d failure(s)", fails);
		logw(L"DONE");
		::KillTimer(nullptr, timerId);
		break;
	}
	busy = false;
}

BOOL APIENTRY DllMain(HANDLE h, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) hInst = static_cast<HINSTANCE>(h);
	return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData d) { npp = d; }
extern "C" __declspec(dllexport) const wchar_t* getName() { return L"KexTest"; }
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* n)
{
	wcscpy(funcs[0]._itemName, L"Restart self-test");
	funcs[0]._pFunc = restart;
	wcscpy(funcs[1]._itemName, L"Two-view command");
	funcs[1]._pFunc = legacyCommand;
	*n = 2;
	return funcs;
}
extern "C" __declspec(dllexport) void beNotified(SCNotification* n)
{
	if (n->nmhdr.code == NPPN_READY)
		timerId = ::SetTimer(nullptr, 0, 900, tick);
}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
