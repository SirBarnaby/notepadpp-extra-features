// DesktopSync.cpp - "the same Notepad++ on every desktop".
//
// One real window follows the user to every *registered* virtual desktop. A desktop is registered
// when notepad++.exe is started there (the second process sends NPPM_INTERNAL_RESTOREFROMMINIMIZED
// synchronously before SetForegroundWindow, winmain.cpp:817/831, and we move the window to the
// current desktop inside that message) or when the global hotkey is pressed there. Open tabs and
// document contents are shared (it is one instance); each desktop keeps its own active tab per view,
// per-document caret/selection/scroll position and window placement.
#include "KexSuite.h"
#include <map>
#include <set>
#include <cwchar>

namespace kex {

namespace {

const UINT_PTR TIMER_POLL = 1, TIMER_SAVE = 2, TIMER_REAPPLY = 3;
const int MAXV = 4;

struct DocPos { intptr_t anchor = 0, caret = 0, firstLine = 0, xOffset = 0; };

struct Desk {
    bool registered = false;
    ULONGLONG lastUsed = 0;
    bool hasPlacement = false;
    WINDOWPLACEMENT wp{};
    std::wstring active[MAXV];
    int focusView = 0;
    std::map<std::wstring, DocPos> docs[MAXV];
    std::set<std::wstring> pending[MAXV];   // runtime: positions still to restore lazily
};

DesktopStats s_stats;

class DesktopSync;
DesktopSync* s_self = nullptr;

class DesktopSync : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"PerDesktop"; }
    const wchar_t* title() const override { return L"Same Notepad++ on every desktop"; }

    void loadOptions() override
    {
        _pollMs = std::max(50, std::min(2000, settings.getInt(key(), L"PollMs", 150)));
        _closeOnlyHere = settings.getInt(key(), L"CloseOnlyThisDesktop", 1) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        wchar_t cls[64] = {};
        ::GetClassNameW(npp._nppHandle, cls, 63);
        // A second notepad++.exe finds us with FindWindow(L"Notepad++") and sends WM_USER+71.
        if (wcscmp(cls, L"Notepad++")) { why = L"unexpected main window class"; return false; }
        LRESULT ver = nppMsg(NPPM_GETNPPVERSION, 1, 0);
        int major = HIWORD(ver);
        if (major < 7 || major > 9) { why = L"untested Notepad++ version: the second-instance message may have changed"; return false; }
        DesktopProvider* p = desktopProvider();
        if (!p || !p->ok()) { why = p ? p->error() : L"no desktop provider"; return false; }
        GUID g;
        if (!p->windowDesktop(npp._nppHandle, &g) && !p->currentDesktop(&g)) { why = L"cannot read the current virtual desktop"; return false; }
        return true;
    }

    bool enable() override
    {
        s_self = this;
        _prov = desktopProvider();
        if (!_prov || !_prov->ok() || !createMsgWindow(L"KexDesktopSync")) return false;
        load();
        GUID g{};
        if (!_prov->windowDesktop(npp._nppHandle, &g) && !_prov->currentDesktop(&g)) return false;
        _here = guidStr(g);
        // Notepad++ was started here: register this desktop (keep any saved state for later switches).
        Desk& d = _desks[_here];
        d.registered = true;
        d.lastUsed = ::GetTickCount64();
        snapshot(_here);
        GUID cur;
        _lastCur = _prov->currentDesktop(&cur) ? guidStr(cur) : _here;
        mainHooks.onSecondInstance = [] {
            if (!s_self) return;
            bool wasHere = s_self->_prov->isOnCurrent(npp._nppHandle);
            s_self->summon(L"second notepad++.exe launch");
            // The second notepad++.exe activates the window as soon as this returns. If the shell still has
            // it on the old desktop at that moment, Windows switches the user back there; so return only
            // once the move has landed.
            if (!wasHere) s_self->waitUntilHere(1500);
        };
        mainHooks.onSysClose = [] { return s_self ? s_self->onClose() : false; };
        ::SetTimer(hwnd(), TIMER_POLL, _pollMs, nullptr);
        ::SetTimer(hwnd(), TIMER_SAVE, 3000, nullptr);
        _dirty = true;
        updateStats(L"started on " + _here);
        log(L"per-desktop: provider '%ls', Notepad++ is on %ls, %d registered desktop(s)", _prov->name(), _here.c_str(), registeredCount());
        return true;
    }

    void disable() override
    {
        if (hwnd()) { ::KillTimer(hwnd(), TIMER_POLL); ::KillTimer(hwnd(), TIMER_SAVE); ::KillTimer(hwnd(), TIMER_REAPPLY); }
        if (_prov && !_here.empty()) { snapshot(_here); save(); }
        mainHooks.onSecondInstance = nullptr;
        mainHooks.onSysClose = nullptr;
        destroyMsgWindow();
        _desks.clear();
        s_self = nullptr;
    }

    void onNotify(SCNotification* n) override
    {
        switch (n->nmhdr.code)
        {
        case SCN_UPDATEUI:
        {
            if (_restoring || _here.empty()) break;
            int v = views.viewOf(static_cast<HWND>(n->nmhdr.hwndFrom));
            if (v < 0 || v >= MAXV) break;
            std::wstring p = currentPath(v);
            if (p.empty()) break;
            // still waiting for this desktop's own position (lazy restore): don't overwrite it with
            // the position Notepad++ just restored from another desktop
            if (_desks[_here].pending[v].count(p)) break;
            _desks[_here].docs[v][p] = readPos(views.sci(v));
            _dirty = true;
            break;
        }
        case NPPN_BUFFERACTIVATED:
        {
            if (_here.empty()) break;
            int v = views.current();
            if (v < 0 || v >= MAXV) break;
            std::wstring p = bufferPath(static_cast<intptr_t>(n->nmhdr.idFrom));
            Desk& d = _desks[_here];
            if (!_restoring) { d.active[v] = p; d.focusView = v; }
            if (d.pending[v].count(p) && d.docs[v].count(p))
            {
                // lazy restore: this desktop last saw the document at a different place
                d.pending[v].erase(p);
                DocPos nppPos = readPos(views.sci(v));
                applyPos(views.sci(v), d.docs[v][p]);
                scheduleReapply(v, d.docs[v][p], nppPos);
            }
            _dirty = true;
            break;
        }
        case NPPN_FILERENAMED: case NPPN_FILESAVED: case NPPN_FILECLOSED:
            _pathCache.clear();
            break;
        case NPPN_BEFORESHUTDOWN:
        case NPPN_SHUTDOWN:
            if (!_here.empty()) { snapshot(_here); save(); }
            break;
        }
    }

    // Hotkey or second-instance launch: register the desktop the user is looking at and come here.
    void summon(const wchar_t* why)
    {
        GUID cur;
        if (!_prov || !_prov->currentDesktop(&cur)) { log(L"per-desktop: cannot read current desktop (%ls)", why); return; }
        std::wstring c = guidStr(cur);
        _lastCur = c;
        Desk& d = _desks[c];
        if (!d.registered)
        {
            snapshot(_here);
            Desk copy = _desks[_here];          // new desktops start from the current state
            for (auto& p : copy.pending) p.clear();
            copy.registered = true;
            _desks[c] = copy;
            log(L"per-desktop: registered %ls (%ls), copied state from %ls", c.c_str(), why, _here.c_str());
            save();   // a new desktop is a structural change: persist it right away
        }
        _desks[c].lastUsed = ::GetTickCount64();
        if (c != _here) switchTo(c, why);
        else moveHostsHere(c);
        _dirty = true;
        updateStats(std::wstring(L"summoned (") + why + L") on " + c);
    }

    void waitUntilHere(int ms)
    {
        HWND n = npp._nppHandle;
        double t0 = nowMs();
        auto waitSent = [](int forMs) {
            // handle only messages sent to us (the shell may call back into this thread), never input or timers
            double end = nowMs() + forMs;
            while (nowMs() < end)
            {
                ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_SENDMESSAGE, 0);
                MSG m; ::PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            }
        };
        bool here = _prov->isOnCurrent(n);
        while (!here && nowMs() - t0 < ms) { waitSent(10); here = _prov->isOnCurrent(n); }
        if (here) waitSent(60);   // let the shell finish uncloaking before the window is activated
        log(L"per-desktop: window %ls on the current desktop after %.0f ms", here ? L"arrived" : L"did NOT arrive", nowMs() - t0);
    }

    bool wouldCloseHere() const
    {
        auto it = _desks.find(_here);
        return _closeOnlyHere && registeredCount() >= 2 && it != _desks.end() && it->second.registered;
    }

    // Title-bar X / taskbar close: with 2+ registered desktops, close only on this one.
    bool onClose()
    {
        if (!wouldCloseHere()) return false;
        std::wstring gone = _here;
        _desks.erase(gone);
        settings.deleteSection((L"Desktop." + gone).c_str());
        std::wstring target; ULONGLONG best = 0;
        for (auto& d : _desks)
            if (d.second.registered && d.first != gone && d.second.lastUsed >= best) { best = d.second.lastUsed; target = d.first; }
        if (target.empty()) return false;
        GUID g; guidFromStr(target, &g);
        moveAll(g);
        _here = target;
        restore(target);
        _dirty = true;
        save();
        log(L"per-desktop: closed on %ls only; Notepad++ now lives on %ls (%d registered)", gone.c_str(), target.c_str(), registeredCount());
        updateStats(L"closed on " + gone + L", moved to " + target);
        return true;
    }

    void poll()
    {
        GUID cur;
        if (!_prov || !_prov->currentDesktop(&cur)) return;
        std::wstring c = guidStr(cur);
        // The user may have moved the window with Task View: follow that, and register the desktop.
        GUID wg;
        if (_prov->windowDesktop(npp._nppHandle, &wg))
        {
            std::wstring w = guidStr(wg);
            if (w != _here && !_switching)
            {
                log(L"per-desktop: window was moved to %ls by the user", w.c_str());
                snapshot(_here);
                Desk copy = _desks[_here];
                _here = w;
                if (!_desks[w].registered) { copy.registered = true; for (auto& p : copy.pending) p.clear(); _desks[w] = copy; }
                _dirty = true;
            }
        }
        if (c == _lastCur) return;
        _lastCur = c;
        if (c == _here) return;
        auto it = _desks.find(c);
        if (it == _desks.end() || !it->second.registered)
        {
            updateStats(L"switched to unregistered " + c + L": staying put");
            return;
        }
        switchTo(c, L"desktop switch");
    }

    int registeredCount() const
    {
        int n = 0;
        for (auto& d : _desks) if (d.second.registered) ++n;
        return n;
    }

    void updateStats(const std::wstring& action)
    {
        s_stats.registered = registeredCount();
        s_stats.regs.clear();
        for (auto& d : _desks) if (d.second.registered) s_stats.regs.push_back(d.first);
        s_stats.here = _here;
        if (!action.empty()) s_stats.lastAction = action;
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_TIMER)
        {
            if (w == TIMER_POLL) { poll(); return 0; }
            if (w == TIMER_SAVE) { if (_dirty) save(); return 0; }
            if (w == TIMER_REAPPLY)
            {
                ::KillTimer(hwnd(), TIMER_REAPPLY);
                // Notepad++ finishes its own position restore on SCN_PAINTED when wrapping is on
                // (ScintillaEditView::restoreCurrentPosPostStep), which can put back its own position for the
                // document; apply ours again unless the user has moved since (caret at neither place).
                for (auto& r : _reapply)
                {
                    HWND s = views.sci(r.first);
                    if (!s) continue;
                    DocPos now = readPos(s);
                    const DocPos& ours = r.second.first;
                    const DocPos& npps = r.second.second;
                    bool untouched = (now.caret == ours.caret && now.anchor == ours.anchor) || (now.caret == npps.caret && now.anchor == npps.anchor);
                    if (untouched) applyPos(s, ours);
                }
                _reapply.clear();
                return 0;
            }
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    std::wstring currentPath(int v)
    {
        intptr_t id = views.bufferId(v, views.currentIndex(v));
        if (!id) return L"";
        auto it = _pathCache.find(id);
        if (it != _pathCache.end()) return it->second;
        std::wstring p = bufferPath(id);
        _pathCache[id] = p;
        return p;
    }

    static DocPos readPos(HWND s)
    {
        DocPos p;
        if (!s) return p;
        p.anchor = sci(s, SCI_GETANCHOR);
        p.caret = sci(s, SCI_GETCURRENTPOS);
        p.firstLine = sci(s, SCI_DOCLINEFROMVISIBLE, sci(s, SCI_GETFIRSTVISIBLELINE));
        p.xOffset = sci(s, SCI_GETXOFFSET);
        return p;
    }

    static void applyPos(HWND s, const DocPos& p)
    {
        if (!s) return;
        intptr_t len = sci(s, SCI_GETLENGTH);
        intptr_t a = std::min(p.anchor, len), c = std::min(p.caret, len);
        sci(s, SCI_SETSELECTION, c, a);
        sci(s, SCI_SETFIRSTVISIBLELINE, sci(s, SCI_VISIBLEFROMDOCLINE, p.firstLine));
        sci(s, SCI_SETXOFFSET, p.xOffset);
    }

    void scheduleReapply(int v, const DocPos& p, const DocPos& nppPos)
    {
        _reapply[v] = { p, nppPos };
        ::SetTimer(hwnd(), TIMER_REAPPLY, 150, nullptr);
    }

    void snapshot(const std::wstring& id)
    {
        if (id.empty()) return;
        Desk& d = _desks[id];
        d.wp.length = sizeof(d.wp);
        d.hasPlacement = ::GetWindowPlacement(npp._nppHandle, &d.wp) != FALSE;
        d.focusView = views.current();
        for (int v = 0; v < views.count() && v < MAXV; ++v)
        {
            if (!views.visible(v)) { d.active[v].clear(); continue; }
            std::wstring p = currentPath(v);
            d.active[v] = p;
            if (!p.empty()) d.docs[v][p] = readPos(views.sci(v));
        }
    }

    void moveAll(const GUID& g)
    {
        _switching = true;
        _prov->moveWindow(npp._nppHandle, g);
        for (HWND h : views.hostWindows()) _prov->moveWindow(h, g);   // torn-out KEX view windows
        _switching = false;
    }

    void moveHostsHere(const std::wstring& id)
    {
        GUID g; if (!guidFromStr(id, &g)) return;
        if (!_prov->isOnCurrent(npp._nppHandle)) moveAll(g);
        else for (HWND h : views.hostWindows()) if (!_prov->isOnCurrent(h)) _prov->moveWindow(h, g);
    }

    void switchTo(const std::wstring& target, const wchar_t* why)
    {
        snapshot(_here);
        GUID g; if (!guidFromStr(target, &g)) return;
        std::wstring from = _here;
        moveAll(g);
        _here = target;
        _desks[target].lastUsed = ::GetTickCount64();
        restore(target);
        ++s_stats.switches;
        _dirty = true;
        log(L"per-desktop: %ls: %ls -> %ls", why, from.c_str(), target.c_str());
        updateStats(std::wstring(why) + L": " + from + L" -> " + target);
    }

    void restore(const std::wstring& id)
    {
        Desk& d = _desks[id];
        _restoring = true;
        if (d.hasPlacement)
        {
            WINDOWPLACEMENT wp = d.wp;
            wp.length = sizeof(wp);
            // never activate here: activating a window the shell still has on another desktop switches
            // the user to that desktop
            if (!::IsWindowVisible(npp._nppHandle)) wp.showCmd = SW_HIDE;
            else if (wp.showCmd != SW_SHOWMAXIMIZED) wp.showCmd = SW_SHOWNOACTIVATE;
            else if (::IsZoomed(npp._nppHandle)) wp.showCmd = SW_SHOWNA;
            ::SetWindowPlacement(npp._nppHandle, &wp);
        }
        // active tab of each view, the focused view last
        std::vector<int> order;
        for (int v = 0; v < views.count() && v < MAXV; ++v) if (v != d.focusView) order.push_back(v);
        if (d.focusView >= 0 && d.focusView < views.count()) order.push_back(d.focusView);
        for (int v = 0; v < MAXV; ++v) { d.pending[v].clear(); for (auto& kv : d.docs[v]) d.pending[v].insert(kv.first); }
        for (int v : order)
        {
            if (d.active[v].empty() || !views.visible(v)) continue;
            int nb = views.nbFiles(v);
            for (int i = 0; i < nb; ++i)
            {
                intptr_t id2 = views.bufferId(v, i);
                auto it = _pathCache.find(id2);
                std::wstring p = it != _pathCache.end() ? it->second : (_pathCache[id2] = bufferPath(id2));
                if (p == d.active[v])
                {
                    if (i != views.currentIndex(v) || v == d.focusView) views.activate(v, i);
                    break;
                }
            }
        }
        // positions of the documents now in front; the rest are restored when they are activated
        for (int v = 0; v < views.count() && v < MAXV; ++v)
        {
            if (!views.visible(v)) continue;
            std::wstring p = currentPath(v);
            if (d.pending[v].count(p) && d.docs[v].count(p))
            {
                d.pending[v].erase(p);
                DocPos nppPos = readPos(views.sci(v));
                applyPos(views.sci(v), d.docs[v][p]);
                scheduleReapply(v, d.docs[v][p], nppPos);
            }
        }
        _restoring = false;
    }

    // ---- persistence: one INI section per desktop GUID ----
    void save()
    {
        _dirty = false;
        for (auto& kv : _desks)
        {
            const Desk& d = kv.second;
            std::wstring sec = L"Desktop." + kv.first;
            settings.deleteSection(sec.c_str());
            if (!d.registered) continue;
            settings.setInt(sec.c_str(), L"Registered", 1);
            settings.setStr(sec.c_str(), L"LastUsed", std::to_wstring(d.lastUsed));
            if (d.hasPlacement)
            {
                wchar_t b[160];
                swprintf(b, 160, L"%u,%u,%ld,%ld,%ld,%ld", d.wp.flags, d.wp.showCmd, d.wp.rcNormalPosition.left, d.wp.rcNormalPosition.top,
                    d.wp.rcNormalPosition.right, d.wp.rcNormalPosition.bottom);
                settings.setStr(sec.c_str(), L"Placement", b);
            }
            settings.setInt(sec.c_str(), L"FocusView", d.focusView);
            for (int v = 0; v < MAXV; ++v)
            {
                wchar_t k[32];
                swprintf(k, 32, L"Active%d", v);
                if (!d.active[v].empty()) settings.setStr(sec.c_str(), k, d.active[v]);
                int i = 0;
                for (auto& doc : d.docs[v])
                {
                    if (i >= 300) break;
                    wchar_t val[64];
                    swprintf(val, 64, L"|%lld|%lld|%lld|%lld", static_cast<long long>(doc.second.anchor), static_cast<long long>(doc.second.caret),
                        static_cast<long long>(doc.second.firstLine), static_cast<long long>(doc.second.xOffset));
                    swprintf(k, 32, L"Doc%d_%d", v, i++);
                    settings.setStr(sec.c_str(), k, doc.first + val);
                }
                swprintf(k, 32, L"DocCount%d", v);
                settings.setInt(sec.c_str(), k, i);
            }
        }
    }

    void load()
    {
        _desks.clear();
        for (const std::wstring& sec : settings.sectionNames())
        {
            if (sec.compare(0, 8, L"Desktop.") != 0) continue;
            std::wstring id = sec.substr(8);
            GUID g;
            if (!guidFromStr(id, &g) || !_prov->exists(g)) { settings.deleteSection(sec.c_str()); continue; }
            if (!settings.getInt(sec.c_str(), L"Registered", 0)) continue;
            Desk d;
            d.registered = true;
            d.lastUsed = _wcstoui64(settings.getStr(sec.c_str(), L"LastUsed", L"0").c_str(), nullptr, 10);
            std::wstring pl = settings.getStr(sec.c_str(), L"Placement");
            d.wp.length = sizeof(d.wp);
            unsigned fl = 0, sc = 0; long l = 0, t = 0, r = 0, b = 0;
            if (swscanf(pl.c_str(), L"%u,%u,%ld,%ld,%ld,%ld", &fl, &sc, &l, &t, &r, &b) == 6 && r > l && b > t)
            {
                d.hasPlacement = true;
                d.wp.flags = fl; d.wp.showCmd = sc;
                d.wp.rcNormalPosition = RECT{ l, t, r, b };
            }
            d.focusView = settings.getInt(sec.c_str(), L"FocusView", 0);
            for (int v = 0; v < MAXV; ++v)
            {
                wchar_t k[32];
                swprintf(k, 32, L"Active%d", v);
                d.active[v] = settings.getStr(sec.c_str(), k);
                swprintf(k, 32, L"DocCount%d", v);
                int n = settings.getInt(sec.c_str(), k, 0);
                for (int i = 0; i < n && i < 300; ++i)
                {
                    swprintf(k, 32, L"Doc%d_%d", v, i);
                    std::wstring s = settings.getStr(sec.c_str(), k);
                    size_t bar = s.find(L'|');
                    if (bar == std::wstring::npos) continue;
                    long long a = 0, c = 0, f = 0, x = 0;
                    if (swscanf(s.c_str() + bar, L"|%lld|%lld|%lld|%lld", &a, &c, &f, &x) != 4) continue;
                    d.docs[v][s.substr(0, bar)] = DocPos{ static_cast<intptr_t>(a), static_cast<intptr_t>(c), static_cast<intptr_t>(f), static_cast<intptr_t>(x) };
                }
            }
            _desks[id] = d;
        }
    }

    DesktopProvider* _prov = nullptr;
    std::map<std::wstring, Desk> _desks;
    std::map<intptr_t, std::wstring> _pathCache;
    std::map<int, std::pair<DocPos, DocPos>> _reapply;   // view -> (our position, the one Notepad++ restored)
    std::wstring _here, _lastCur;
    bool _restoring = false, _switching = false, _dirty = false, _closeOnlyHere = true;
    int _pollMs = 150;
};

DesktopSync* s_mod = nullptr;

} // namespace

Module* createDesktopSync() { s_mod = new DesktopSync(); return s_mod; }
DesktopStats desktopStats() { if (s_self) s_self->updateStats(L""); return s_stats; }
void desktopPollNow() { if (s_self) s_self->poll(); }
bool desktopWouldCloseHere() { return s_self ? s_self->wouldCloseHere() : false; }

void desktopSummonHere(const wchar_t* why)
{
    if (s_self && s_mod->isEnabled()) { s_self->summon(why); return; }
    desktopMoveHere();
}

bool desktopMoveHere()
{
    DesktopProvider* p = desktopProvider();
    if (!p || !p->ok()) return false;
    if (p->isOnCurrent(npp._nppHandle)) return true;
    GUID cur;
    if (!p->currentDesktop(&cur)) return false;
    bool ok = p->moveWindow(npp._nppHandle, cur);
    for (HWND h : views.hostWindows()) p->moveWindow(h, cur);
    return ok;
}

} // namespace kex
