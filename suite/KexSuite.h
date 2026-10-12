// KexSuite.h - shared declarations for the KEX suite, built into the KEX Notepad++ (KexSuiteHost.h).
// It drives Notepad++ through the documented plugin messages, like a plugin that is always loaded.
#pragma once
// Notepad++ is compiled for Windows 7; the suite uses Windows 10 APIs (virtual desktops, DWM cloaking)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#undef WINVER
#define WINVER 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "PluginInterface.h"
#include "menuCmdID.h"
#include "KexApi.h"
#include "KexSuiteHost.h"

namespace kex {

extern NppData npp;
extern HINSTANCE hInst;
extern bool selfTestMode;      // set by -pluginMessage="KexSuite_selftest=1"
extern bool underWine;         // ntdll exports wine_get_version

// ---- logging (kex.log in Notepad++'s settings folder; the self-test writes its own log) --------
void log(const wchar_t* fmt, ...);

// ---- small helpers ----------------------------------------------------------------------------
inline LRESULT sci(HWND h, UINT m, WPARAM w = 0, LPARAM l = 0) { return ::SendMessageW(h, m, w, l); }
inline LRESULT nppMsg(UINT m, WPARAM w = 0, LPARAM l = 0) { return ::SendMessageW(npp._nppHandle, m, w, l); }
std::wstring bufferPath(intptr_t bufferId);
std::wstring fileNameOf(const std::wstring& path);
double nowMs();   // QueryPerformanceCounter in ms
bool rectsIntersect(const RECT& a, const RECT& b);

// A hidden message-only window that forwards to a member function.
class MsgWindow {
public:
    virtual ~MsgWindow() { destroyMsgWindow(); }
    bool createMsgWindow(const wchar_t* name);
    void destroyMsgWindow();
    HWND hwnd() const { return _hwnd; }
protected:
    virtual LRESULT onMessage(UINT m, WPARAM w, LPARAM l) { return ::DefWindowProcW(_hwnd, m, w, l); }
private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    HWND _hwnd = nullptr;
};

// ---- views: the two-view plugin API, or the KEX four-view API when the core patch is present ----
class Views {
public:
    void init();                       // call at NPPN_READY
    bool patched() const { return _patched; }
    int patchVersion() const { return _patchVersion; }
    int count() const { return _count; }
    HWND sci(int v) const;             // that view's Scintilla (nullptr if none)
    HWND tabBar(int v) const;          // that view's tab control (patched: KEX msg; else located once)
    int current() const;               // focused view
    bool visible(int v) const;
    int nbFiles(int v) const;
    intptr_t bufferId(int v, int idx) const;
    int currentIndex(int v) const;
    void activate(int v, int idx) const;
    HWND host(int v) const;            // top-level window hosting the view
    int viewOf(HWND sciHwnd) const;    // -1 if not an editing view
    std::vector<HWND> hostWindows() const;   // unique top-level hosts other than the main window
    HWND locateTabBar(int v) const;    // unpatched heuristic (exposed for the self-check)
private:
    bool _patched = false;
    int _count = 2, _patchVersion = 0;
    mutable HWND _tabCache[4] = {};
};
extern Views views;

// ---- settings (kex.ini in Notepad++'s settings folder, NPPM_GETNPPSETTINGSDIRPATH) -------------
class Settings {
public:
    void init();
    const std::wstring& path() const { return _path; }
    const std::wstring& dir() const { return _dir; }
    int getInt(const wchar_t* sec, const wchar_t* key, int def) const;
    void setInt(const wchar_t* sec, const wchar_t* key, int v) const;
    std::wstring getStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def = L"") const;
    void setStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) const;
    void deleteSection(const wchar_t* sec) const;
    std::vector<std::wstring> sectionNames() const;
private:
    std::wstring _path, _dir;
};
extern Settings settings;

// ---- feature modules ------------------------------------------------------------------------
class Module {
public:
    virtual ~Module() {}
    virtual const wchar_t* key() const = 0;       // INI section name
    virtual const wchar_t* title() const = 0;     // menu text
    // Verify the Notepad++ internals this feature relies on. Must not change any state.
    virtual bool selfCheck(std::wstring& why) = 0;
    virtual bool enable() = 0;                    // install hooks; false = could not
    virtual void disable() = 0;                   // remove hooks, restore Notepad++ state exactly
    virtual void loadOptions() {}
    virtual bool defaultOn() const { return true; }   // on until the user switches it off
    virtual void onNotify(SCNotification*) {}
    bool isEnabled() const { return _enabled; }

    bool _enabled = false;
    bool _checkOk = false;
    std::wstring _checkMsg;
    int _cmdIndex = -1;
};

// Implemented in the feature files.
Module* createFallingLetters();
Module* createSmoothCaret();
Module* createHotkeyDrop();
Module* createHoverTabs();
Module* createDesktopSync();
Module* createTabSnap();
Module* createJetBrainsKeys();
Module* createGlassText();
Module* createTokenStyles();

enum ModuleId { M_LETTERS = 0, M_CARET, M_HOTKEY, M_TABS, M_DESKTOP, M_SNAP, M_KEYS, M_GLASS, M_TOKENS, M_COUNT };
extern Module* modules[M_COUNT];

// Core services used across modules.
bool setModuleEnabled(int id, bool on, bool interactive);
void refreshPrefPages();           // after a feature was switched on/off or failed
void showNote(const std::wstring& text, int ms);   // non-modal note in the corner of Notepad++
bool isPostItMode();
enum PrefPageId { PAGE_EDITING = 0, PAGE_TABS, PAGE_WINDOWS, PAGE_HIGHLIGHT, PAGE_NOTE, PAGE_COUNT };
HWND prefPage(int i);              // a KEX Preferences page (PrefPageId), once created
int pageOfModule(int module);      // the page a feature's settings are on
void openPreferences();            // Preferences on the "KEX Editing" page
// The KEX group in the Settings menu (SettingsMenu.cpp): a submenu per Preferences page, with a switch
// for each feature on it and "Preferences..." to open the page.
bool buildSettingsMenu();
void settingsMenuPopup(HMENU m);   // WM_INITMENUPOPUP
bool settingsMenuCommand(UINT id); // WM_COMMAND; true when it was one of the group's
HMENU settingsSubmenu(int page);
UINT settingsSwitchId(int module);
UINT settingsPreferencesId(int page);

// Main-window subclass hooks (owned by the core, consulted by modules).
struct MainHooks {
    // main window moved/sized/activated
    void (*onMainMoved)() = nullptr;
};
extern MainHooks mainHooks;

// "A Notepad++ for each desktop" (DesktopSync.cpp)
void desktopLoadPendingCopy();     // at NPPN_READY: a new window opens the copy of the first window's tabs
LRESULT desktopServeCopy(HWND sender, const COPYDATASTRUCT* cds);   // WM_COPYDATA 'KEXT' from a new window

// Self-test entry points
void selfTestStart(const std::wstring& args);
void selfTestOnNotify(SCNotification* n);

// Wine compatibility: layered windows there have no partial alpha, so use a threshold.
inline bool binaryAlpha() { return underWine; }

// Per-module test hooks (only used by the self-test)
struct LettersStats { int lastSpawned = 0; int lastCandidates = 0; int totalSpawned = 0; int live = 0; int frames = 0;
                      RECT overlay{}; bool overlayVisible = false; int skippedUndo = 0; int skippedAnimOff = 0; int skippedGlobal = 0; };
LettersStats lettersStats();
void lettersFreezeAfter(int ms);   // <0 = run normally
struct CaretStats { int carets = 0; RECT overlay{}; bool visible = false; HWND target = nullptr; int lastStyleSaved = -1;
                    float mainX = 0, mainY = 0, mainH = 0; bool block = false;
                    int pushes = 0;   // times the caret window was redrawn
                    double workMs = 0, computeMs = 0, pushMs = 0;   // time in the caret's code, its caret maths, its redraws
};
CaretStats caretStats();
void caretTestFreeze(double glideFraction, bool solidBlink);   // fraction <0 = off
int caretOriginalStyle(HWND sciHwnd);
struct HotkeyStats { bool registered = false; bool byOtherNpp = false; DWORD lastError = 0; UINT mods = 0, vk = 0; int presses = 0; std::wstring text; bool noteOpen = false; };
HotkeyStats hotkeyStats();
void hotkeyPress();              // run the same code path as WM_HOTKEY
bool hotkeyApply(UINT mods, UINT vk);   // store + re-register; true when registered
std::wstring hotkeyText(UINT mods, UINT vk);
HWND quickNoteWindow();          // the open quick note (nullptr when closed or fading out)
HWND quickNoteEditor();
std::wstring quickNoteFile();
struct TabsStats { bool stripVisible = false; RECT strip{}; int tabCount = 0; int activeIndex = -1; int view = 0; std::vector<RECT> tabRects; std::vector<int> dirty; };
TabsStats tabsStats();
void tabsTestClick(int index);
struct SnapStats { bool visible = false; RECT target{}; RECT window{}; int alpha = 0; int shows = 0; int moves = 0; int cancels = 0;
                   bool lifted = false; RECT lift{}; int liftAlpha = 0; int lifts = 0; };
SnapStats snapStats();
struct JetBrainsStats { int handled = 0; int grows = 0; std::wstring last; };
JetBrainsStats jetBrainsStats();
std::wstring jetBrainsKeyList();   // the table, one "keys<TAB>action" line each
struct GlassStats { bool active = false; COLORREF key = 0; int hosts = 0; int remaps = 0; int placements = 0; int clicks = 0; int wheels = 0;
                    int dragSolids = 0; bool dragSolidNow = false; int accent = 0; int accentFailures = 0; int rendererSwitches = 0;
                    HWND lastTarget = nullptr; std::wstring state; };
GlassStats glassStats();
std::wstring glassInfo();          // the line under the glass settings
void glassTestDragEnter();         // what a drag entering the glass does
struct TokenStats { int wrapperCalls = 0; int liveWrappers = 0; int docs = 0; int lexCalls = 0; int variantsMade = 0; int collisions = 0;
                    int fullPasses = 0; int menus = 0; int augmented = 0; bool hooked = false; std::wstring menuKey; std::wstring last; };
TokenStats tokenStats();
int tokenCommandId(int which);     // our menu commands: 0 Bold, 1 Italic, 2 Clear This Token
std::wstring tokenStoreFile();     // kex-tokens.txt
std::wstring tokenInfo();          // the line on the KEX Highlighting page
int tokenRuleCount(HWND view);     // tokens styled in the document of that view
int tokenVariantBase(HWND view, int style, int* flags);   // lexer style behind a bold/italic variant, -1 if not one
bool tokenDocWrapped(HWND view);   // the document's lexer goes through the suite
bool tokenMenuKeyApply(UINT mods, UINT vk);

// Desktop provider (real IVirtualDesktopManager or the fake used by the self-test)
class DesktopProvider {
public:
    virtual ~DesktopProvider() {}
    virtual const wchar_t* name() const = 0;
    virtual bool ok() const = 0;
    virtual std::wstring error() const { return L""; }
    virtual bool currentDesktop(GUID* out) = 0;
    virtual bool windowDesktop(HWND h, GUID* out) = 0;
    virtual bool isOnCurrent(HWND h) = 0;
    virtual bool moveWindow(HWND h, const GUID& d) = 0;
    virtual bool exists(const GUID&) { return true; }
};
DesktopProvider* createRealDesktopProvider();
class FakeDesktopProvider;
DesktopProvider* desktopProvider();      // the active one (may be null)
void useFakeDesktops(bool on);
// Fake provider control for the self-test.
GUID fakeDesktopGuid(int i);             // deterministic GUID for fake desktop i (0..)
void fakeSwitchTo(int i);
int fakeWindowDesktop(HWND h);
std::wstring guidStr(const GUID& g);
bool guidFromStr(const std::wstring& s, GUID* g);
struct DesktopStats { bool newWindow = false; int copiedTabs = -1; int served = 0; std::wstring lastAction; };
DesktopStats desktopStats();

} // namespace kex
