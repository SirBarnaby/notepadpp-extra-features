// FallingLetters.cpp - CS2D-style falling letters for deleted text.
//
// UI thread: on SCN_MODIFIED/SC_MOD_BEFOREDELETE (requested with NPPM_ADDSCNMODIFIEDFLAGS) we read
// every deleted character that is visible on screen, with its exact position, style colour and
// font, before Scintilla removes it. The batch is handed to a render thread which owns a
// click-through, non-activating, topmost layered window sized to the bounding box of the live
// letters. The window only exists while letters are falling.
#include "Overlay.h"
#include <atomic>
#include <cmath>
#include <map>
#include <deque>

namespace kex {

namespace {

struct FontSpec {
    std::wstring face; int height = 13; int weight = 400; bool italic = false;
    bool operator<(const FontSpec& o) const
    {
        if (face != o.face) return face < o.face;
        if (height != o.height) return height < o.height;
        if (weight != o.weight) return weight < o.weight;
        return italic < o.italic;
    }
};

struct CapRun { std::wstring text; int dx = 0; COLORREF color = 0; int font = 0; };
struct CapItem { int x = 0, y = 0, w = 0, h = 0; std::vector<CapRun> runs; };
struct CapBatch {
    std::vector<FontSpec> fonts;     // fonts[0] = STYLE_DEFAULT font (baseline reference)
    std::vector<CapItem> items;
    RECT monitor{};
    int extraAscent = 0;
    float gravity = 2200, drift = 90, spin = 4.0f, pop = 220;
};

struct Particle {
    Sprite sprite;
    float x0, y0, vx, vy, w, age = 0;
    float x = 0, y = 0, a = 0;
    RECT monitor;
    float gravity;
};

std::atomic<int> s_lastSpawned{ 0 }, s_lastCandidates{ 0 }, s_totalSpawned{ 0 }, s_live{ 0 }, s_frames{ 0 };
std::atomic<int> s_skipUndo{ 0 }, s_skipAnim{ 0 }, s_skipGlobal{ 0 };
std::atomic<int> s_freezeAfter{ -1 };
std::atomic<bool> s_overlayVisible{ false };
CRITICAL_SECTION s_rectLock;
RECT s_overlayRect{};

// ---------------------------------------------------------------------------------------------
// render thread

class Renderer {
public:
    bool start()
    {
        if (_thread) return true;
        _quit = false;
        _event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        _thread = ::CreateThread(nullptr, 0, threadMain, this, 0, nullptr);
        if (!_thread) return false;
        ::WaitForSingleObject(_ready, 3000);
        return _wnd != nullptr;
    }
    void stop()
    {
        if (!_thread) return;
        _quit = true;
        ::SetEvent(_event);
        if (::WaitForSingleObject(_thread, 3000) == WAIT_TIMEOUT) ::TerminateThread(_thread, 0);
        ::CloseHandle(_thread); _thread = nullptr;
        ::CloseHandle(_event); _event = nullptr;
        ::CloseHandle(_ready); _ready = nullptr;
        _queue.clear();
    }
    void submit(CapBatch&& b)
    {
        ::EnterCriticalSection(&_lock);
        _queue.push_back(std::move(b));
        ::LeaveCriticalSection(&_lock);
        ::SetEvent(_event);
    }
    Renderer() { ::InitializeCriticalSection(&_lock); }
    ~Renderer() { stop(); ::DeleteCriticalSection(&_lock); }
    int maxLive = 400;

private:
    static DWORD WINAPI threadMain(LPVOID p) { static_cast<Renderer*>(p)->run(); return 0; }

    HFONT font(const FontSpec& f)
    {
        auto it = _fonts.find(f);
        if (it != _fonts.end()) return it->second;
        LOGFONTW lf{};
        lf.lfHeight = -f.height;
        lf.lfWeight = f.weight;
        lf.lfItalic = f.italic ? TRUE : FALSE;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy(lf.lfFaceName, f.face.c_str(), LF_FACESIZE - 1);
        HFONT h = ::CreateFontIndirectW(&lf);
        _fonts[f] = h;
        return h;
    }

    void addBatch(CapBatch& b)
    {
        // baseline: Scintilla draws text at line top + maxAscent (+ extra ascent)
        HDC dc = ::CreateCompatibleDC(nullptr);
        TEXTMETRICW tm{};
        HGDIOBJ of = ::SelectObject(dc, font(b.fonts.empty() ? FontSpec{} : b.fonts[0]));
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, of);
        ::DeleteDC(dc);
        int baseline = b.extraAscent + tm.tmAscent;
        for (CapItem& it : b.items)
        {
            Particle p;
            std::vector<TextRun> runs;
            for (CapRun& r : it.runs)
            {
                TextRun t; t.text = r.text; t.dx = r.dx; t.color = r.color;
                t.font = font(b.fonts[r.font < static_cast<int>(b.fonts.size()) ? r.font : 0]);
                runs.push_back(t);
            }
            int pad = 3;
            if (!renderRuns(p.sprite, runs, std::max(1, it.w), std::max(1, it.h), baseline, pad)) continue;
            p.x0 = it.x + it.w * 0.5f;
            p.y0 = it.y + it.h * 0.5f;
            float r1 = rnd(), r2 = rnd(), r3 = rnd();
            p.vx = (r1 * 2 - 1) * b.drift;
            p.vy = -(0.25f + 0.75f * r2) * b.pop;
            p.w = (r3 * 2 - 1) * b.spin;
            p.monitor = b.monitor;
            p.gravity = b.gravity;
            p.x = p.x0; p.y = p.y0;
            _parts.push_back(std::move(p));
        }
        while (static_cast<int>(_parts.size()) > maxLive) _parts.pop_front();
    }

    float rnd() { _seed = _seed * 1664525u + 1013904223u; return static_cast<float>((_seed >> 8) & 0xFFFF) / 65535.0f; }

    void run()
    {
        _wnd = createOverlayWindow(nullptr, true, L"KEX falling letters");
        ::SetEvent(_ready);
        if (!_wnd) return;
        typedef HRESULT(WINAPI * DwmFlushFn)();
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        DwmFlushFn dwmFlush = dwm ? reinterpret_cast<DwmFlushFn>(::GetProcAddress(dwm, "DwmFlush")) : nullptr;
        bool shown = false;
        double last = nowMs();
        double frozenSince = -1;
        while (!_quit)
        {
            MSG m;
            while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
            std::deque<CapBatch> incoming;
            ::EnterCriticalSection(&_lock);
            incoming.swap(_queue);
            ::LeaveCriticalSection(&_lock);
            for (CapBatch& b : incoming) addBatch(b);
            if (!incoming.empty()) { last = nowMs(); frozenSince = -1; }

            if (_parts.empty())
            {
                if (shown) { ::ShowWindow(_wnd, SW_HIDE); shown = false; s_overlayVisible = false; }
                s_live = 0;
                ::MsgWaitForMultipleObjects(1, &_event, FALSE, INFINITE, QS_ALLINPUT);
                last = nowMs();
                continue;
            }
            double now = nowMs();
            float dt = static_cast<float>((now - last) / 1000.0);
            last = now;
            if (dt > 0.05f) dt = 0.05f;
            int freeze = s_freezeAfter.load();
            if (freeze >= 0 && !_parts.empty() && _parts.back().age * 1000.0f >= freeze) dt = 0;   // test hook

            RECT bb{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
            for (auto it = _parts.begin(); it != _parts.end();)
            {
                Particle& p = *it;
                p.age += dt;
                float t = p.age;
                p.x = p.x0 + p.vx * t;
                p.y = p.y0 + p.vy * t + 0.5f * p.gravity * t * t;
                p.a = p.w * t;
                float r = 0.5f * std::sqrt(static_cast<float>(p.sprite.w * p.sprite.w + p.sprite.h * p.sprite.h)) + 1;
                bool gone = (p.y - r > p.monitor.bottom) || (p.x + r < p.monitor.left - 400) ||
                            (p.x - r > p.monitor.right + 400) || p.age > 8.0f;
                if (gone) { it = _parts.erase(it); continue; }
                bb.left = std::min(bb.left, static_cast<LONG>(std::floor(p.x - r)));
                bb.top = std::min(bb.top, static_cast<LONG>(std::floor(p.y - r)));
                bb.right = std::max(bb.right, static_cast<LONG>(std::ceil(p.x + r)));
                bb.bottom = std::max(bb.bottom, static_cast<LONG>(std::ceil(p.y + r)));
                ++it;
            }
            s_live = static_cast<int>(_parts.size());
            if (_parts.empty()) continue;
            RECT vs{ ::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
            vs.right = vs.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
            vs.bottom = vs.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
            RECT clip;
            if (!::IntersectRect(&clip, &bb, &vs))
            {
                if (shown) { ::ShowWindow(_wnd, SW_HIDE); shown = false; s_overlayVisible = false; }
                waitFrame(dwmFlush, now);
                continue;
            }
            int w = clip.right - clip.left, h = clip.bottom - clip.top;
            if (_surf.ensure(w, h))
            {
                _surf.clear();
                for (Particle& p : _parts)
                    drawSprite(_surf, p.sprite, p.x - clip.left, p.y - clip.top, p.a, 1.0f);
                POINT pos{ clip.left, clip.top }; SIZE sz{ w, h };
                pushLayered(_wnd, _surf, pos, sz);
                if (!shown)
                {
                    ::SetWindowPos(_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    shown = true;
                }
                s_overlayVisible = true;
                ::EnterCriticalSection(&s_rectLock); s_overlayRect = clip; ::LeaveCriticalSection(&s_rectLock);
                ++s_frames;
            }
            (void)frozenSince;
            waitFrame(dwmFlush, now);
        }
        if (_wnd) { ::DestroyWindow(_wnd); _wnd = nullptr; }
        s_overlayVisible = false;
        _parts.clear();
        for (auto& f : _fonts) if (f.second) ::DeleteObject(f.second);
        _fonts.clear();
        _surf.release();
        if (dwm) ::FreeLibrary(dwm);
    }

    void waitFrame(HRESULT(WINAPI* dwmFlush)(), double frameStart)
    {
        if (dwmFlush && !underWine)
        {
            double b = nowMs();
            if (SUCCEEDED(dwmFlush()) && nowMs() - b > 2.0) return;   // vsync-paced by DWM
        }
        double spent = nowMs() - frameStart;
        DWORD wait = spent < 15.0 ? static_cast<DWORD>(16.0 - spent) : 1;
        ::MsgWaitForMultipleObjects(1, &_event, FALSE, wait, QS_ALLINPUT);
    }

    HANDLE _thread = nullptr, _event = nullptr, _ready = nullptr;
    volatile bool _quit = false;
    HWND _wnd = nullptr;
    CRITICAL_SECTION _lock;
    std::deque<CapBatch> _queue;
    std::deque<Particle> _parts;
    std::map<FontSpec, HFONT> _fonts;
    Surface _surf;
    uint32_t _seed = 0x1234567u;
};

// ---------------------------------------------------------------------------------------------
// module

const UINT WM_KEX_SPAWN = WM_APP + 21;

class FallingLetters : public Module, public MsgWindow {
public:
    FallingLetters() { ::InitializeCriticalSection(&s_rectLock); }
    const wchar_t* key() const override { return L"FallingLetters"; }
    const wchar_t* title() const override { return L"Falling letters"; }

    void loadOptions() override
    {
        _gravity = settings.getInt(key(), L"Gravity", 2200);
        _maxParticles = std::max(1, std::min(2000, settings.getInt(key(), L"MaxLetters", 200)));
        _spinDeg = settings.getInt(key(), L"SpinDegPerSec", 260);
        _drift = settings.getInt(key(), L"DriftPxPerSec", 90);
        _pop = settings.getInt(key(), L"PopPxPerSec", 240);
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        _undoToo = settings.getInt(key(), L"AnimateUndoRedo", 0) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        HWND s = views.sci(0);
        if (!s || !::IsWindow(s)) { why = L"main Scintilla view not found"; return false; }
        // NPPM_ADDSCNMODIFIEDFLAGS exists since Notepad++ 8.7.7; an older Notepad++ returns 0.
        if (!_flagsAdded)
        {
            if (!nppMsg(NPPM_ADDSCNMODIFIEDFLAGS, 0, SC_MOD_BEFOREDELETE))
            { why = L"NPPM_ADDSCNMODIFIEDFLAGS not supported by this Notepad++"; return false; }
            _flagsAdded = true;
        }
        if (sci(s, SCI_GETCODEPAGE) < 0) { why = L"Scintilla does not answer"; return false; }
        HWND probe = createOverlayWindow(nullptr, true, L"probe");
        if (!probe) { why = L"cannot create a layered overlay window"; return false; }
        ::DestroyWindow(probe);
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexFallingLetters")) return false;
        _renderer.maxLive = _maxParticles * 2;
        if (!_renderer.start()) { destroyMsgWindow(); return false; }
        return true;
    }

    void disable() override
    {
        _renderer.stop();
        _pending.clear();
        destroyMsgWindow();
        s_live = 0; s_overlayVisible = false;
    }

    void onNotify(SCNotification* n) override
    {
        if (n->nmhdr.code == NPPN_GLOBALMODIFIED)
        {
            // Replace All: Notepad++ switches SCN_MODIFIED off for it (FindReplaceDlg.cpp), and reports it here.
            // Drop anything captured in the same message-loop turn so a Replace All never rains letters.
            if (!_pending.empty()) { _pending.clear(); ++s_skipGlobal; }
            return;
        }
        if (n->nmhdr.code != SCN_MODIFIED || !(n->modificationType & SC_MOD_BEFOREDELETE)) return;
        if (n->length <= 0) return;
        HWND s = static_cast<HWND>(n->nmhdr.hwndFrom);
        if (views.viewOf(s) < 0 || !::IsWindowVisible(s)) return;
        // a document shown in two views notifies from both: letters fall only where the edit was made
        if (views.sci(views.current()) != s) return;
        if ((n->modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) && !_undoToo) { ++s_skipUndo; return; }
        if (_respectAnim)
        {
            BOOL anim = TRUE;
            if (::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) { ++s_skipAnim; return; }
        }
        capture(s, n->position, n->length);
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_KEX_SPAWN)
        {
            _posted = false;
            for (CapBatch& b : _pending) _renderer.submit(std::move(b));
            _pending.clear();
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    struct Cand { Sci_Position pos; int x, y, w; std::wstring ch; int style; };

    static int utf8Len(unsigned char c)
    {
        if (c < 0x80) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    }

    void capture(HWND s, Sci_Position pos, Sci_Position len)
    {
        // a big visible delete means many calls per character: Scintilla's direct function, no messages
        const SciDirect ds(s);
        RECT client; ::GetClientRect(s, &client);
        int margins = 0;
        int nm = static_cast<int>(ds(SCI_GETMARGINS));
        for (int i = 0; i < nm; ++i) margins += static_cast<int>(ds(SCI_GETMARGINWIDTHN, i));
        Sci_Position firstVis = ds(SCI_GETFIRSTVISIBLELINE);
        Sci_Position onScreen = ds(SCI_LINESONSCREEN);
        Sci_Position firstDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis);
        Sci_Position lastDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis + onScreen + 1);
        Sci_Position visStart = ds(SCI_POSITIONFROMLINE, firstDoc);
        Sci_Position visEnd = ds(SCI_GETLINEENDPOSITION, lastDoc);
        Sci_Position a = std::max(pos, visStart), b = std::min(pos + len, visEnd);
        s_lastCandidates = 0; s_lastSpawned = 0;
        if (a >= b) return;
        if (b - a > 400000) b = a + 400000;

        std::string text(static_cast<size_t>(b - a) + 1, '\0');
        Sci_TextRangeFull tr{ { a, b }, &text[0] };
        ds(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&tr));
        text.resize(static_cast<size_t>(b - a));
        int cp = static_cast<int>(ds(SCI_GETCODEPAGE));
        UINT winCp = cp == SC_CP_UTF8 ? CP_UTF8 : (cp == 0 ? CP_ACP : static_cast<UINT>(cp));

        const bool wrapped = ds(SCI_GETWRAPMODE) != SC_WRAP_NONE;
        std::vector<Cand> cands;
        Sci_Position lineA = ds(SCI_LINEFROMPOSITION, a), lineB = ds(SCI_LINEFROMPOSITION, b);
        size_t i = 0;
        for (Sci_Position line = lineA; line <= lineB; ++line)
        {
            Sci_Position ls = std::max(a, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, line)));
            Sci_Position le = std::min(b, static_cast<Sci_Position>(ds(SCI_GETLINEENDPOSITION, line)));
            Sci_Position next = std::min(b, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, line + 1)));
            if (!ds(SCI_GETLINEVISIBLE, line)) { i = static_cast<size_t>(next - a); continue; }
            i = static_cast<size_t>(ls - a);
            int prevX = 0; bool havePrev = false; Cand* prev = nullptr;
            // unwrapped, the whole line has one y
            const int lineY = wrapped ? 0 : static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, ls));
            while (static_cast<Sci_Position>(i) + a < le)
            {
                unsigned char c = static_cast<unsigned char>(text[i]);
                int n = 1;
                if (winCp == CP_UTF8) n = utf8Len(c);
                else if (c >= 0x80 && ::IsDBCSLeadByteEx(winCp, c)) n = 2;
                if (i + n > text.size()) n = static_cast<int>(text.size() - i);
                Sci_Position p = a + static_cast<Sci_Position>(i);
                int x = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, p));
                int y = wrapped ? static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, p)) : lineY;
                if (prev && havePrev && prev->y == y && x > prevX) prev->w = x - prevX;
                prev = nullptr; havePrev = false;
                bool blank = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c < 0x20);
                if (!blank)
                {
                    wchar_t wbuf[4] = {};
                    int wn = ::MultiByteToWideChar(winCp, 0, &text[i], n, wbuf, 4);
                    bool onScreenH = x >= margins - 2 && x < client.right && y >= -40 && y < client.bottom;
                    if (wn > 0 && onScreenH)
                    {
                        Cand cd{ p, x, y, 0, std::wstring(wbuf, wn), static_cast<int>(ds(SCI_GETSTYLEINDEXAT, p)) };
                        cands.push_back(cd);
                        prev = &cands.back(); prevX = x; havePrev = true;
                    }
                }
                i += n;
            }
            if (prev && havePrev)
            {
                // last char of the line segment: measure it with the end of line position
                Sci_Position p2 = a + static_cast<Sci_Position>(i);
                int x2 = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, p2));
                int y2 = static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, p2));
                prev->w = (y2 == prev->y && x2 > prev->x) ? x2 - prev->x : 0;
            }
            i = static_cast<size_t>(next - a);
        }
        s_lastCandidates = static_cast<int>(cands.size());
        if (cands.empty()) return;

        CapBatch batch;
        batch.gravity = static_cast<float>(_gravity);
        batch.drift = static_cast<float>(_drift);
        batch.spin = static_cast<float>(_spinDeg) * 3.14159265f / 180.0f;
        batch.pop = static_cast<float>(_pop);
        batch.extraAscent = static_cast<int>(sci(s, SCI_GETEXTRAASCENT));
        HMONITOR mon = ::MonitorFromWindow(s, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(mon, &mi);
        batch.monitor = mi.rcMonitor;
        int lineH = static_cast<int>(sci(s, SCI_TEXTHEIGHT, sci(s, SCI_LINEFROMPOSITION, cands[0].pos)));
        int zoom = static_cast<int>(sci(s, SCI_GETZOOM));
        int dpi = dpiOf(s);

        std::map<int, std::pair<int, COLORREF>> styleCache;   // style -> (font index, colour)
        std::map<FontSpec, int> fontIdx;
        auto styleInfo = [&](int style) -> std::pair<int, COLORREF> {
            auto it = styleCache.find(style);
            if (it != styleCache.end()) return it->second;
            FontSpec f = fontFor(s, style, zoom, dpi);
            int idx;
            auto fi = fontIdx.find(f);
            if (fi == fontIdx.end()) { idx = static_cast<int>(batch.fonts.size()); batch.fonts.push_back(f); fontIdx[f] = idx; }
            else idx = fi->second;
            std::pair<int, COLORREF> r{ idx, static_cast<COLORREF>(sci(s, SCI_STYLEGETFORE, style)) };
            styleCache[style] = r;
            return r;
        };
        styleInfo(STYLE_DEFAULT);   // fonts[0]: baseline reference

        // Too many letters for the cap: let neighbours on a line fall together as chunks.
        // Chunks never span lines, so widen them until the pieces fit the cap.
        size_t chunk = (cands.size() + _maxParticles - 1) / _maxParticles;
        if (chunk < 1) chunk = 1;
        auto countPieces = [&](size_t ch) {
            size_t pieces = 0;
            for (size_t k = 0; k < cands.size(); ++pieces)
            {
                const Cand& first = cands[k];
                int right = first.x; size_t taken = 0;
                while (k < cands.size() && taken < ch)
                {
                    const Cand& c = cands[k];
                    if (taken && (c.y != first.y || c.x < right - 1 || c.x - right > 3 * lineH)) break;
                    right = c.x + (c.w > 0 ? c.w : lineH / 2);
                    ++k; ++taken;
                }
            }
            return pieces;
        };
        while (chunk < cands.size() && countPieces(chunk) > static_cast<size_t>(_maxParticles))
            chunk += std::max<size_t>(1, chunk / 4);
        for (size_t k = 0; k < cands.size() && batch.items.size() < static_cast<size_t>(_maxParticles);)
        {
            CapItem item;
            const Cand& first = cands[k];
            POINT org{ first.x, first.y }; ::ClientToScreen(s, &org);
            item.x = org.x; item.y = org.y; item.h = lineH;
            int right = first.x;
            size_t taken = 0;
            while (k < cands.size() && taken < chunk)
            {
                const Cand& c = cands[k];
                if (taken && (c.y != first.y || c.x < right - 1 || c.x - right > 3 * lineH)) break;
                auto si = styleInfo(c.style);
                CapRun r; r.text = c.ch; r.dx = c.x - first.x; r.color = si.second; r.font = si.first;
                item.runs.push_back(r);
                int cw = c.w > 0 ? c.w : lineH / 2;
                right = c.x + cw;
                ++k; ++taken;
            }
            item.w = std::max(1, right - first.x);
            batch.items.push_back(std::move(item));
        }
        s_lastSpawned = static_cast<int>(batch.items.size());
        s_totalSpawned += s_lastSpawned;
        _pending.push_back(std::move(batch));
        if (!_posted && hwnd()) { _posted = true; ::PostMessageW(hwnd(), WM_KEX_SPAWN, 0, 0); }
    }

    static FontSpec fontFor(HWND s, int style, int zoom, int dpi)
    {
        FontSpec f;
        char name[128] = {};
        sci(s, SCI_STYLEGETFONT, style, reinterpret_cast<LPARAM>(name));
        wchar_t wname[128] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 127);
        f.face = wname[0] ? wname : L"Courier New";
        double pts = static_cast<double>(sci(s, SCI_STYLEGETSIZEFRACTIONAL, style)) / SC_FONT_SIZE_MULTIPLIER + zoom;
        if (pts < 2) pts = 2;
        f.height = static_cast<int>(pts * dpi / 72.0 + 0.5);
        f.weight = static_cast<int>(sci(s, SCI_STYLEGETWEIGHT, style));
        f.italic = sci(s, SCI_STYLEGETITALIC, style) != 0;
        return f;
    }

    Renderer _renderer;
    std::vector<CapBatch> _pending;
    bool _posted = false, _flagsAdded = false;
    int _gravity = 2200, _maxParticles = 200, _spinDeg = 260, _drift = 90, _pop = 240;
    bool _respectAnim = true, _undoToo = false;
};

} // namespace

Module* createFallingLetters() { return new FallingLetters(); }

LettersStats lettersStats()
{
    LettersStats st;
    st.lastSpawned = s_lastSpawned; st.lastCandidates = s_lastCandidates; st.totalSpawned = s_totalSpawned;
    st.live = s_live; st.frames = s_frames; st.overlayVisible = s_overlayVisible;
    st.skippedUndo = s_skipUndo; st.skippedAnimOff = s_skipAnim; st.skippedGlobal = s_skipGlobal;
    ::EnterCriticalSection(&s_rectLock); st.overlay = s_overlayRect; ::LeaveCriticalSection(&s_rectLock);
    return st;
}

void lettersFreezeAfter(int ms) { s_freezeAfter = ms; }

} // namespace kex
