// JetBrainsKeys.cpp - "JetBrains shortcuts": the IntelliJ editor keys in Notepad++'s editing views.
// A WH_GETMESSAGE hook on the GUI thread sees each key before Notepad++'s accelerators and
// Scintilla do; a key in the table is swallowed there and its action runs right after (posted, so
// it keeps its place before any keys typed after it). Other keys and other windows are untouched.
#include "KexSuite.h"
#include <cwctype>

namespace kex {

namespace {

enum Mod { C = 1, S = 2, A = 4 };
enum Act {
    A_CMD,          // a Notepad++ menu command
    A_NEXT_OCCURRENCE, A_ALL_OCCURRENCES, A_DUPLICATE, A_DELETE_LINE, A_GROW_SELECTION, A_SHRINK_SELECTION,
    A_TOGGLE_CASE, A_LINE_AFTER, A_LINE_BEFORE
};

struct Binding {
    UINT vk; int mods; Act act; UINT cmd;
    bool npp;               // Notepad++ already uses this key for something else
    const wchar_t* keys; const wchar_t* what;
};

const Binding kBindings[] = {
    { 'J', A, A_NEXT_OCCURRENCE, 0, false, L"Alt+J", L"select the next occurrence" },
    { 'J', A | S, A_CMD, IDM_EDIT_MULTISELECTUNDO, false, L"Shift+Alt+J", L"unselect the last occurrence" },
    { 'J', C | A | S, A_ALL_OCCURRENCES, 0, false, L"Ctrl+Shift+Alt+J", L"select all occurrences" },
    { 'W', C, A_GROW_SELECTION, 0, true, L"Ctrl+W", L"extend selection (word, string, brackets, line, all)" },
    { 'W', C | S, A_SHRINK_SELECTION, 0, true, L"Ctrl+Shift+W", L"shrink selection" },
    { VK_OEM_2, C, A_CMD, IDM_EDIT_BLOCK_COMMENT, false, L"Ctrl+/", L"comment / uncomment line" },
    { VK_DIVIDE, C, A_CMD, IDM_EDIT_BLOCK_COMMENT, false, L"Ctrl+Num /", L"comment / uncomment line" },
    { VK_OEM_2, C | S, A_CMD, IDM_EDIT_STREAM_COMMENT, false, L"Ctrl+Shift+/", L"block comment" },
    { 'D', C, A_DUPLICATE, 0, false, L"Ctrl+D", L"duplicate line or selection" },
    { 'Y', C, A_DELETE_LINE, 0, true, L"Ctrl+Y", L"delete line (redo is Ctrl+Shift+Z)" },
    { VK_UP, A | S, A_CMD, IDM_EDIT_LINE_UP, true, L"Shift+Alt+Up", L"move line up" },
    { VK_DOWN, A | S, A_CMD, IDM_EDIT_LINE_DOWN, true, L"Shift+Alt+Down", L"move line down" },
    { VK_LEFT, A, A_CMD, IDM_VIEW_TAB_PREV, false, L"Alt+Left", L"previous tab" },
    { VK_RIGHT, A, A_CMD, IDM_VIEW_TAB_NEXT, false, L"Alt+Right", L"next tab" },
    { VK_F4, C, A_CMD, IDM_FILE_CLOSE, false, L"Ctrl+F4", L"close tab" },
    { 'J', C | S, A_CMD, IDM_EDIT_JOIN_LINES, false, L"Ctrl+Shift+J", L"join lines" },
    { 'U', C | S, A_TOGGLE_CASE, 0, false, L"Ctrl+Shift+U", L"toggle case" },
    { 'M', C | S, A_CMD, IDM_SEARCH_GOTOMATCHINGBRACE, false, L"Ctrl+Shift+M", L"go to matching brace" },
    { 'R', C, A_CMD, IDM_SEARCH_REPLACE, true, L"Ctrl+R", L"replace" },
    { VK_RETURN, S, A_LINE_AFTER, 0, false, L"Shift+Enter", L"start a new line below" },
    { VK_RETURN, C | A, A_LINE_BEFORE, 0, false, L"Ctrl+Alt+Enter", L"start a new line above" },
    { VK_SUBTRACT, C, A_CMD, IDM_VIEW_FOLD_CURRENT, true, L"Ctrl+Num -", L"fold" },
    { VK_ADD, C, A_CMD, IDM_VIEW_UNFOLD_CURRENT, true, L"Ctrl+Num +", L"unfold" },
    { VK_SUBTRACT, C | S, A_CMD, IDM_VIEW_FOLDALL, false, L"Ctrl+Shift+Num -", L"fold all" },
    { VK_ADD, C | S, A_CMD, IDM_VIEW_UNFOLDALL, false, L"Ctrl+Shift+Num +", L"unfold all" },
    { VK_F12, C, A_CMD, IDM_VIEW_FUNC_LIST, false, L"Ctrl+F12", L"file structure (Function List)" },
    { VK_F11, 0, A_CMD, IDM_SEARCH_TOGGLE_BOOKMARK, true, L"F11", L"toggle bookmark" },
};
const int kBindingCount = static_cast<int>(sizeof(kBindings) / sizeof(kBindings[0]));
const UINT WM_KEX_RUN = WM_APP + 77;
JetBrainsStats s_stats;

int currentMods()
{
    int m = 0;
    if (::GetKeyState(VK_CONTROL) < 0) m |= C;
    if (::GetKeyState(VK_SHIFT) < 0) m |= S;
    if (::GetKeyState(VK_MENU) < 0) m |= A;
    return m;
}

struct Range { intptr_t a = 0, b = 0; };

class JetBrainsKeys;
JetBrainsKeys* s_keys = nullptr;

class JetBrainsKeys : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"JetBrainsKeys"; }
    const wchar_t* title() const override { return L"JetBrains shortcuts"; }

    void loadOptions() override
    {
        _all = settings.getInt(key(), L"AlsoNotepadKeys", 1) != 0;
        _caseMode = settings.getInt(key(), L"OccurrenceCase", 0);
        _wordMode = settings.getInt(key(), L"OccurrenceWords", 0);
    }

    bool selfCheck(std::wstring& why) override
    {
        // every Notepad++ command the table sends has to be in the main menu
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        const UINT extra[] = { IDM_EDIT_MULTISELECTNEXT, IDM_EDIT_MULTISELECTNEXTMATCHCASE, IDM_EDIT_MULTISELECTNEXTWHOLEWORD,
                               IDM_EDIT_MULTISELECTNEXTMATCHCASEWHOLEWORD, IDM_EDIT_MULTISELECTALL, IDM_EDIT_MULTISELECTALLMATCHCASE,
                               IDM_EDIT_MULTISELECTALLWHOLEWORD, IDM_EDIT_MULTISELECTALLMATCHCASEWHOLEWORD, IDM_EDIT_UPPERCASE, IDM_EDIT_LOWERCASE };
        std::vector<UINT> ids(extra, extra + sizeof(extra) / sizeof(extra[0]));
        for (const Binding& b : kBindings) if (b.act == A_CMD) ids.push_back(b.cmd);
        for (UINT id : ids)
            if (!mm || ::GetMenuState(mm, id, MF_BYCOMMAND) == static_cast<UINT>(-1))
            {
                wchar_t b[96]; swprintf(b, 96, L"menu command %u not found in this Notepad++", id);
                why = b; return false;
            }
        return true;
    }

    bool enable() override
    {
        loadOptions();
        if (!createMsgWindow(L"KexJetBrainsKeys")) return false;
        _hook = ::SetWindowsHookExW(WH_GETMESSAGE, hookProc, nullptr, ::GetCurrentThreadId());
        if (!_hook) { log(L"JetBrains shortcuts: SetWindowsHookEx failed (%lu)", ::GetLastError()); return false; }
        s_keys = this;
        return true;
    }

    void disable() override
    {
        if (_hook) { ::UnhookWindowsHookEx(_hook); _hook = nullptr; }
        if (s_keys == this) s_keys = nullptr;
        destroyMsgWindow();
        _history.clear();
        _swallowAltUp = false;
    }

    static std::wstring list(bool all)
    {
        std::wstring s;
        for (const Binding& b : kBindings)
        {
            s += b.keys; s += L"\t"; s += b.what;
            if (b.npp && !all) s += L"   (off: Notepad++ keeps this key)";
            s += L"\n";
        }
        return s;
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_KEX_RUN)
        {
            try { run(static_cast<int>(w), reinterpret_cast<HWND>(l)); }
            catch (...) { log(L"JetBrains shortcuts: exception in action %d", static_cast<int>(w)); }
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    static LRESULT CALLBACK hookProc(int code, WPARAM w, LPARAM l)
    {
        if (code == HC_ACTION && w == PM_REMOVE && s_keys)
        {
            MSG* msg = reinterpret_cast<MSG*>(l);
            try { s_keys->onKey(msg); } catch (...) {}
        }
        return ::CallNextHookEx(nullptr, code, w, l);
    }

    void onKey(MSG* msg)
    {
        const UINT m = msg->message;
        if (m == WM_SYSKEYUP || m == WM_KEYUP)
        {
            // Alt released after an Alt shortcut: keep Windows from opening the menu bar
            if (_swallowAltUp && msg->wParam == VK_MENU) { _swallowAltUp = false; msg->message = WM_NULL; }
            return;
        }
        if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN) return;
        const UINT vk = static_cast<UINT>(msg->wParam);
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) return;
        const int mods = currentMods();
        const Binding* hit = nullptr;
        for (int i = 0; i < kBindingCount; ++i)
            if (kBindings[i].vk == vk && kBindings[i].mods == mods && (_all || !kBindings[i].npp)) { hit = &kBindings[i]; break; }
        if (!hit) return;
        if (views.viewOf(msg->hwnd) == -1) return;   // only Notepad++'s editing views
        const int index = static_cast<int>(hit - kBindings);
        msg->message = WM_NULL;                       // nothing else sees the key
        if (mods & A) _swallowAltUp = true;
        ::PostMessageW(hwnd(), WM_KEX_RUN, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(msg->hwnd));
        ++s_stats.handled;
        s_stats.last = hit->keys;
    }

    void command(UINT id) { nppMsg(WM_COMMAND, id, 0); }

    // Alt+J matches what Smart Highlighting shows, unless the KEX Editing page picks otherwise:
    // case 0 like Smart Highlighting, 1 match case, 2 ignore case; whole words 0 like Smart
    // Highlighting, 1 from a bare caret (JetBrains), 2 always, 3 never
    void decideMatching(HWND s)
    {
        bool hlCase = false, hlWord = true;
        kexsuite::smartHighlightOptions(&hlCase, &hlWord);
        _matchCase = _caseMode == 1 ? true : _caseMode == 2 ? false : hlCase;
        _wholeWord = _wordMode == 1 ? sci(s, SCI_GETSELECTIONEMPTY) != 0 : _wordMode == 2 ? true : _wordMode == 3 ? false : hlWord;
    }

    void run(int index, HWND s)
    {
        if (index < 0 || index >= kBindingCount || !::IsWindow(s)) return;
        const Binding& b = kBindings[index];
        switch (b.act)
        {
        case A_CMD: command(b.cmd); break;
        case A_NEXT_OCCURRENCE:
        case A_ALL_OCCURRENCES:
        {
            // the matching is settled on the first press and kept while occurrences are added
            if (sci(s, SCI_GETSELECTIONS) == 1) decideMatching(s);
            const bool next = b.act == A_NEXT_OCCURRENCE;
            const UINT id = _matchCase ? (_wholeWord ? (next ? IDM_EDIT_MULTISELECTNEXTMATCHCASEWHOLEWORD : IDM_EDIT_MULTISELECTALLMATCHCASEWHOLEWORD)
                                                     : (next ? IDM_EDIT_MULTISELECTNEXTMATCHCASE : IDM_EDIT_MULTISELECTALLMATCHCASE))
                                       : (_wholeWord ? (next ? IDM_EDIT_MULTISELECTNEXTWHOLEWORD : IDM_EDIT_MULTISELECTALLWHOLEWORD)
                                                     : (next ? IDM_EDIT_MULTISELECTNEXT : IDM_EDIT_MULTISELECTALL));
            command(id);
            break;
        }
        case A_DUPLICATE:
            sci(s, sci(s, SCI_GETSELECTIONEMPTY) ? SCI_LINEDUPLICATE : SCI_SELECTIONDUPLICATE);
            break;
        case A_DELETE_LINE:
            sci(s, SCI_LINEDELETE);
            break;
        case A_GROW_SELECTION: grow(s); break;
        case A_SHRINK_SELECTION: shrink(s); break;
        case A_TOGGLE_CASE:
        {
            if (sci(s, SCI_GETSELECTIONEMPTY))
            {
                intptr_t p = sci(s, SCI_GETCURRENTPOS);
                intptr_t a = sci(s, SCI_WORDSTARTPOSITION, p, TRUE), e = sci(s, SCI_WORDENDPOSITION, p, TRUE);
                if (e <= a) break;
                sci(s, SCI_SETSEL, a, e);
            }
            intptr_t n = sci(s, SCI_GETSELTEXT, 0, 0);
            std::string t(static_cast<size_t>(n) + 1, '\0');
            sci(s, SCI_GETSELTEXT, 0, reinterpret_cast<LPARAM>(&t[0]));
            int wn = ::MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
            std::wstring wt(static_cast<size_t>(wn > 0 ? wn : 1), L'\0');
            if (wn > 0) ::MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, &wt[0], wn);
            bool anyLower = false;
            for (wchar_t c : wt) if (std::iswlower(c)) { anyLower = true; break; }
            command(anyLower ? IDM_EDIT_UPPERCASE : IDM_EDIT_LOWERCASE);
            break;
        }
        case A_LINE_AFTER:
            sci(s, SCI_LINEEND);
            sci(s, SCI_NEWLINE);   // Notepad++ indents the new line as usual
            break;
        case A_LINE_BEFORE:
        {
            sci(s, SCI_BEGINUNDOACTION);
            intptr_t line = sci(s, SCI_LINEFROMPOSITION, sci(s, SCI_GETCURRENTPOS));
            int indent = static_cast<int>(sci(s, SCI_GETLINEINDENTATION, line));
            sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, line));
            sci(s, SCI_NEWLINE);
            sci(s, SCI_SETLINEINDENTATION, line, indent);
            sci(s, SCI_GOTOPOS, sci(s, SCI_GETLINEINDENTPOSITION, line));
            sci(s, SCI_ENDUNDOACTION);
            break;
        }
        }
    }

    // ---- extend / shrink selection ---------------------------------------------------------------
    static bool strictlyContains(const Range& outer, const Range& in)
    {
        return outer.a <= in.a && outer.b >= in.b && (outer.a < in.a || outer.b > in.b);
    }

    static Range wordAt(HWND s, intptr_t p)
    {
        return Range{ sci(s, SCI_WORDSTARTPOSITION, p, TRUE), sci(s, SCI_WORDENDPOSITION, p, TRUE) };
    }

    // innermost quoted string on the line around cur: its inside and the string with its quotes
    static void quoteCandidates(HWND s, const Range& cur, std::vector<Range>& out)
    {
        intptr_t line = sci(s, SCI_LINEFROMPOSITION, cur.a);
        if (sci(s, SCI_LINEFROMPOSITION, cur.b) != line) return;
        intptr_t ls = sci(s, SCI_POSITIONFROMLINE, line), le = sci(s, SCI_GETLINEENDPOSITION, line);
        if (le - ls > 20000) return;
        const char* t = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, ls, le - ls));
        if (!t) return;
        char q = 0; intptr_t open = -1;
        for (intptr_t i = 0; i < le - ls; ++i)
        {
            char c = t[i];
            if (q)
            {
                if (c == '\\') { ++i; continue; }
                if (c == q)
                {
                    Range in{ ls + open + 1, ls + i }, whole{ ls + open, ls + i + 1 };
                    if (in.a <= cur.a && in.b >= cur.b) { out.push_back(in); out.push_back(whole); }
                    else if (whole.a <= cur.a && whole.b >= cur.b) out.push_back(whole);
                    q = 0;
                }
            }
            else if (c == '"' || c == '\'' || c == '`') { q = c; open = i; }
        }
    }

    // innermost bracket pair around cur: its inside and the pair itself
    static void bracketCandidates(HWND s, const Range& cur, std::vector<Range>& out)
    {
        const intptr_t limit = 200000;
        intptr_t from = std::max<intptr_t>(0, cur.a - limit);
        if (cur.a <= from) return;
        const char* t = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, from, cur.a - from));
        if (!t) return;
        int depth[3] = {};
        const char* opens = "([{";
        const char* closes = ")]}";
        for (intptr_t i = cur.a - 1; i >= from; --i)
        {
            char c = t[i - from];
            for (int k = 0; k < 3; ++k)
            {
                if (c == closes[k]) ++depth[k];
                else if (c == opens[k])
                {
                    if (depth[k] > 0) { --depth[k]; continue; }
                    intptr_t m = sci(s, SCI_BRACEMATCH, i, 0);
                    if (m >= cur.b)
                    {
                        out.push_back(Range{ i + 1, m });
                        out.push_back(Range{ i, m + 1 });
                        return;
                    }
                }
            }
        }
    }

    void grow(HWND s)
    {
        Range cur{ sci(s, SCI_GETSELECTIONSTART), sci(s, SCI_GETSELECTIONEND) };
        std::vector<Range>& hist = _history[s];
        if (!(cur.a == _last.a && cur.b == _last.b && _lastWnd == s)) hist.clear();   // the user selected something else
        std::vector<Range> cand;
        if (cur.a == cur.b) cand.push_back(wordAt(s, cur.a));
        else { Range w{ sci(s, SCI_WORDSTARTPOSITION, cur.a, TRUE), sci(s, SCI_WORDENDPOSITION, cur.b, TRUE) }; cand.push_back(w); }
        quoteCandidates(s, cur, cand);
        bracketCandidates(s, cur, cand);
        intptr_t la = sci(s, SCI_LINEFROMPOSITION, cur.a), lb = sci(s, SCI_LINEFROMPOSITION, cur.b);
        cand.push_back(Range{ sci(s, SCI_GETLINEINDENTPOSITION, la), sci(s, SCI_GETLINEENDPOSITION, lb) });
        cand.push_back(Range{ sci(s, SCI_POSITIONFROMLINE, la), sci(s, SCI_POSITIONFROMLINE, lb + 1) });
        cand.push_back(Range{ 0, sci(s, SCI_GETLENGTH) });
        const Range* best = nullptr;
        for (const Range& r : cand)
            if (r.b > r.a && strictlyContains(r, cur) && (!best || r.b - r.a < best->b - best->a)) best = &r;
        if (!best) return;
        hist.push_back(cur);
        _last = *best; _lastWnd = s;
        sci(s, SCI_SETSEL, best->a, best->b);
        ++s_stats.grows;
    }

    void shrink(HWND s)
    {
        Range cur{ sci(s, SCI_GETSELECTIONSTART), sci(s, SCI_GETSELECTIONEND) };
        std::vector<Range>& hist = _history[s];
        if (hist.empty() || !(cur.a == _last.a && cur.b == _last.b && _lastWnd == s)) return;
        Range prev = hist.back(); hist.pop_back();
        _last = prev; _lastWnd = s;
        sci(s, SCI_SETSEL, prev.a, prev.b);
    }

    HHOOK _hook = nullptr;
    bool _all = true, _swallowAltUp = false, _wholeWord = true, _matchCase = false;
    int _caseMode = 0, _wordMode = 0;
    struct Hist { HWND wnd; std::vector<Range> ranges; };
    struct HistMap {
        std::vector<Hist> items;
        std::vector<Range>& operator[](HWND w) { for (Hist& h : items) if (h.wnd == w) return h.ranges; items.push_back(Hist{ w, {} }); return items.back().ranges; }
        void clear() { items.clear(); }
    } _history;
    Range _last{ -1, -1 };
    HWND _lastWnd = nullptr;
};

} // namespace

Module* createJetBrainsKeys() { return new JetBrainsKeys(); }
JetBrainsStats jetBrainsStats() { return s_stats; }
std::wstring jetBrainsKeyList() { return JetBrainsKeys::list(settings.getInt(L"JetBrainsKeys", L"AlsoNotepadKeys", 1) != 0); }

} // namespace kex
