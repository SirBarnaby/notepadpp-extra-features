// TokenStyles.cpp - "Live token styles": Notepad++'s Style All Occurrences of Token, but the
// styling follows the text. Occurrences typed or pasted later are styled too, an occurrence that
// is edited away loses it, and a token can be bold and/or italic as well as coloured.
//
// Colours are Notepad++'s own five token styles (indicators 25..21, "Mark Style 1..5" in the Style
// Configurator), so Search > Jump Up/Down, Copy Styled Text and Clear Style keep working. Our runs
// carry their own indicator value, so "Style One Token" marks are left alone.
//
// Bold and italic need real Scintilla styles. The core wraps the lexer of every Scintilla it sets
// one for (kexsuite::wrapLexer). Whenever Scintilla has a range lexed - after an edit, on
// scrolling, after a language or theme change - the wrapper finds the occurrences in that range
// with Scintilla's own search (so match case and whole word behave exactly like Notepad++'s),
// brings the colour indicators there up to date, and while the real lexer writes its styles swaps
// the style of each character inside a bold or italic occurrence for a variant: a copy of the
// lexer's style plus bold/italic, kept at the top of the 256 styles. The lexer only ever sees its
// own styles (initial style and look-backs are mapped back), so its states and the folding are
// unaffected.
#include "KexSuite.h"
#include "KexSuiteRes.h"
#include "ILexer.h"
#include <map>
#include <cwchar>
#include <cstdio>

namespace kex {

namespace {

using Scintilla::IDocument;
using Scintilla::ILexer5;

const int kOurValue = 0x4B54;                                   // indicator value of our runs
const int kIndicator[6] = { 0, 25, 24, 23, 22, 21 };            // style 1..5: SCE_UNIVERSAL_FOUND_STYLE_EXT1..5
const UINT kMarkAll[6] = { 0, IDM_SEARCH_MARKALLEXT1, IDM_SEARCH_MARKALLEXT2, IDM_SEARCH_MARKALLEXT3,
                           IDM_SEARCH_MARKALLEXT4, IDM_SEARCH_MARKALLEXT5 };
const UINT kUnmarkAll[6] = { 0, IDM_SEARCH_UNMARKALLEXT1, IDM_SEARCH_UNMARKALLEXT2, IDM_SEARCH_UNMARKALLEXT3,
                             IDM_SEARCH_UNMARKALLEXT4, IDM_SEARCH_UNMARKALLEXT5 };
const wchar_t* const kOrdinal[6] = { L"", L"1st", L"2nd", L"3rd", L"4th", L"5th" };
enum { F_BOLD = 1, F_ITALIC = 2 };
const size_t kMaxToken = 400;                                   // bytes
const size_t kMaxStoredFiles = 500;
const UINT WM_TOK_MENU = WM_APP + 80;    // lParam: the view the menu key was pressed in
const UINT WM_TOK_VIEW = WM_APP + 81;    // lParam: a Scintilla that just got its styles from Notepad++
const UINT WM_TOK_DOC = WM_APP + 82;     // lParam: a document with new variant styles to define
const UINT_PTR kSubclassId = 0x4B45544B; // 'KETK'
enum { CMD_BOLD = 0, CMD_ITALIC, CMD_CLEAR, CMD_COUNT };

struct Rule {
    std::wstring text;
    std::string bytes;      // in the document's encoding
    int style = 0;          // 1..5, 0 = no colour
    int flags = 0;          // F_BOLD | F_ITALIC
    bool matchCase = false, wholeWord = true;
};

struct Span { intptr_t a, b; int style, flags; };

struct Doc {
    intptr_t buffer = 0;
    std::wstring path;
    std::vector<Rule> rules;
    int codePage = -1;                    // the rules' bytes are in this encoding
    // variant styles: a variant is a copy of a lexer style (base) plus bold/italic
    unsigned char variant[256][4] = {};   // [base][flags], 0 = none yet
    unsigned char base[256];              // variant -> its base, identity otherwise
    unsigned char vflags[256] = {};       // variant -> its flags, 0 for lexer styles
    bool defined[256] = {};               // its definition has been sent to the views
    bool reserved[256] = {};              // the lexer writes it: never a variant
    int variants = 0;
    int floor = 40;                       // first style the lexer leaves alone
    std::string lexer;
    intptr_t restyleFrom = -1;            // posted: define new variants, then restyle from here
    intptr_t relexFrom = INTPTR_MAX;      // bold/italic taken away: lex again from here
    bool posted = false;
    Doc() { for (int i = 0; i < 256; ++i) base[i] = static_cast<unsigned char>(i); }
    int styleMask() const { int m = 0; for (const Rule& r : rules) if (r.style) m |= 1 << r.style; return m; }
    void resetVariants()
    {
        for (int i = 0; i < 256; ++i)
        {
            base[i] = static_cast<unsigned char>(i); vflags[i] = 0; defined[i] = false; reserved[i] = false;
            for (int f = 0; f < 4; ++f) variant[i][f] = 0;
        }
        variants = 0;
    }
};

std::map<sptr_t, Doc> g_docs;
// live wrapped lexers per document. Never destroyed: documents (and so their lexers) can still be
// released while the static objects are torn down at exit.
std::map<sptr_t, int>& g_wrappers = *new std::map<sptr_t, int>();
std::vector<HWND> g_scis;             // every Scintilla a lexer was wrapped for (views, document map, ...)
HWND g_post = nullptr;                // the module's message window, while it runs
HWND g_hidden = nullptr;              // a Scintilla of our own, for documents no window shows
TokenStats s_stats;
UINT g_ids[CMD_COUNT] = {};           // our menu command ids (NPPM_ALLOCATECMDID)

// ---- Scintilla access ---------------------------------------------------------------------------

struct Sci {
    HWND h = nullptr; SciFnDirect fn = nullptr; sptr_t p = 0;
    explicit Sci(HWND w) : h(w)
    {
        if (h)
        {
            fn = reinterpret_cast<SciFnDirect>(::SendMessageW(h, SCI_GETDIRECTFUNCTION, 0, 0));
            p = static_cast<sptr_t>(::SendMessageW(h, SCI_GETDIRECTPOINTER, 0, 0));
        }
    }
    sptr_t operator()(UINT m, uptr_t w = 0, sptr_t l = 0) const
    {
        return fn ? fn(p, m, w, l) : static_cast<sptr_t>(::SendMessageW(h, m, w, l));
    }
};

sptr_t docOf(HWND h) { return h ? static_cast<sptr_t>(::SendMessageW(h, SCI_GETDOCPOINTER, 0, 0)) : 0; }

// The Scintillas that show a document now, editing views first.
std::vector<HWND> scisShowing(sptr_t doc)
{
    std::vector<HWND> out;
    auto consider = [&](HWND h)
    {
        if (h && h != g_hidden && ::IsWindow(h) && std::find(out.begin(), out.end(), h) == out.end() && docOf(h) == doc)
            out.push_back(h);
    };
    for (int v = 0; v < views.count(); ++v) consider(views.sci(v));
    for (HWND h : g_scis) consider(h);
    return out;
}

// Run f with a Scintilla showing the document: one that does, or our hidden one.
template <class F> void withDoc(sptr_t doc, F f)
{
    std::vector<HWND> shown = scisShowing(doc);
    if (!shown.empty()) { f(shown[0]); return; }
    if (!g_hidden)
        g_hidden = ::CreateWindowExW(0, L"Scintilla", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
    if (!g_hidden) return;
    ::SendMessageW(g_hidden, SCI_SETDOCPOINTER, 0, doc);
    f(g_hidden);
    ::SendMessageW(g_hidden, SCI_SETDOCPOINTER, 0, 0);
}

std::string toBytes(const std::wstring& w, int codePage)
{
    UINT cp = codePage == SC_CP_UTF8 ? CP_UTF8 : (codePage == 0 ? CP_ACP : static_cast<UINT>(codePage));
    if (w.empty()) return std::string();
    BOOL lossy = FALSE;
    int n = ::WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, cp == CP_UTF8 ? nullptr : &lossy);
    if (n <= 0 || lossy) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s, int codePage)
{
    UINT cp = codePage == SC_CP_UTF8 ? CP_UTF8 : (codePage == 0 ? CP_ACP : static_cast<UINT>(codePage));
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void ensureBytes(Doc& d, const Sci& s)
{
    const int cp = static_cast<int>(s(SCI_GETCODEPAGE));
    if (cp == d.codePage) return;
    d.codePage = cp;
    for (Rule& r : d.rules) r.bytes = toBytes(r.text, cp);
}

// Occurrences of the rules that touch [a, b), found the way Notepad++'s Mark All finds them.
void findSpans(const Sci& s, Doc& d, intptr_t a, intptr_t b, std::vector<Span>& out)
{
    ensureBytes(d, s);
    const intptr_t len = static_cast<intptr_t>(s(SCI_GETLENGTH));
    const sptr_t ts = s(SCI_GETTARGETSTART), te = s(SCI_GETTARGETEND), fl = s(SCI_GETSEARCHFLAGS);
    for (const Rule& r : d.rules)
    {
        const intptr_t n = static_cast<intptr_t>(r.bytes.size());
        if (n == 0) continue;
        intptr_t from = std::max<intptr_t>(0, a - n + 1);
        const intptr_t to = std::min<intptr_t>(len, b + n - 1);
        s(SCI_SETSEARCHFLAGS, (r.matchCase ? SCFIND_MATCHCASE : 0) | (r.wholeWord ? SCFIND_WHOLEWORD : 0));
        while (from < to)
        {
            s(SCI_SETTARGETRANGE, from, to);
            const sptr_t at = s(SCI_SEARCHINTARGET, r.bytes.size(), reinterpret_cast<sptr_t>(r.bytes.data()));
            if (at < 0) break;
            const sptr_t end = s(SCI_GETTARGETEND);
            if (end > at && at < b && end > a) out.push_back({ at, end, r.style, r.flags });
            from = end > at ? end : at + 1;
        }
    }
    s(SCI_SETTARGETRANGE, ts, te);
    s(SCI_SETSEARCHFLAGS, fl);
    std::sort(out.begin(), out.end(), [](const Span& x, const Span& y) { return x.a < y.a; });
}

// Make our runs of the colour indicators inside [a, b) exactly the coloured spans (whole spans are
// filled, even past b). Only real changes are made: each one makes Notepad++ repaint the view.
void syncIndicators(const Sci& s, int mask, const std::vector<Span>& spans, intptr_t a, intptr_t b)
{
    const sptr_t cur = s(SCI_GETINDICATORCURRENT), val = s(SCI_GETINDICATORVALUE);
    auto fill = [&](int ind, intptr_t pos, int value, intptr_t n)
    {
        if (n <= 0) return;
        s(SCI_SETINDICATORCURRENT, ind);
        if (value) { s(SCI_SETINDICATORVALUE, value); s(SCI_INDICATORFILLRANGE, pos, n); }
        else s(SCI_INDICATORCLEARRANGE, pos, n);
    };
    for (int st = 1; st <= 5; ++st)
    {
        if (!(mask & (1 << st))) continue;
        const int ind = kIndicator[st];
        std::vector<std::pair<intptr_t, intptr_t>> want;
        for (const Span& sp : spans)
        {
            if (sp.style != st) continue;
            if (!want.empty() && sp.a <= want.back().second) want.back().second = std::max(want.back().second, sp.b);
            else want.push_back({ sp.a, sp.b });
        }
        for (intptr_t pos = a; pos < b;)
        {
            const intptr_t end = static_cast<intptr_t>(s(SCI_INDICATOREND, ind, pos));
            if (end <= pos) break;
            if (s(SCI_INDICATORVALUEAT, ind, pos) == kOurValue)
            {
                intptr_t x = pos;
                const intptr_t y = std::min(end, b);
                for (const auto& w : want)
                {
                    if (w.second <= x) continue;
                    if (w.first >= y) break;
                    if (w.first > x) fill(ind, x, 0, w.first - x);
                    x = std::max(x, w.second);
                    if (x >= y) break;
                }
                if (x < y) fill(ind, x, 0, y - x);
            }
            pos = end;
        }
        for (const auto& w : want)
            if (!(s(SCI_INDICATORVALUEAT, ind, w.first) == kOurValue && s(SCI_INDICATOREND, ind, w.first) >= w.second))
                fill(ind, w.first, kOurValue, w.second - w.first);
    }
    s(SCI_SETINDICATORCURRENT, cur);
    s(SCI_SETINDICATORVALUE, val);
}

// Remove every run of ours from the given indicators of the whole document.
void clearIndicators(const Sci& s, int mask)
{
    std::vector<Span> none;
    syncIndicators(s, mask, none, 0, static_cast<intptr_t>(s(SCI_GETLENGTH)));
}

void postDocWork(sptr_t doc, Doc& d)
{
    if (d.posted || !g_post) return;
    d.posted = true;
    ::PostMessageW(g_post, WM_TOK_DOC, 0, doc);
}

// The variant for a lexer style plus flags. 0 = not usable yet: a new one gets defined in the views
// first (posted), and the range is styled again after that.
int variantFor(Doc& d, int base, int flags, intptr_t pos)
{
    if (!flags) return base;
    int v = d.variant[base][flags];
    if (!v)
    {
        for (int s = 255; s >= std::max(d.floor, 40); --s)
            if (!d.vflags[s] && !d.reserved[s] && !d.variant[s][0]) { v = s; break; }
        if (!v) return 0;
        d.variant[base][flags] = static_cast<unsigned char>(v);
        d.variant[v][0] = 1;                                 // marks the slot as taken
        d.base[v] = static_cast<unsigned char>(base);
        d.vflags[v] = static_cast<unsigned char>(flags);
        d.defined[v] = false;
        ++d.variants;
        ++s_stats.variantsMade;
    }
    if (!d.defined[v])
    {
        if (d.restyleFrom < 0 || pos < d.restyleFrom) d.restyleFrom = pos;
        return 0;
    }
    return v;
}

// ---- the IDocument the wrapped lexer sees ----------------------------------------------------------

class Proxy : public IDocument {
public:
    Proxy(IDocument* real, Doc& d) : _r(real), _d(d) {}
    std::vector<Span> spans;      // bold/italic occurrences of the range being lexed
    bool collision = false;
    bool started = false;         // the lexer called StartStyling
    intptr_t pos() const { return _pos; }

    int SCI_METHOD Version() const override { return _r->Version(); }
    void SCI_METHOD SetErrorStatus(int status) override { _r->SetErrorStatus(status); }
    Sci_Position SCI_METHOD Length() const override { return _r->Length(); }
    void SCI_METHOD GetCharRange(char* buffer, Sci_Position position, Sci_Position n) const override { _r->GetCharRange(buffer, position, n); }
    char SCI_METHOD StyleAt(Sci_Position position) const override
    {
        return static_cast<char>(_d.base[static_cast<unsigned char>(_r->StyleAt(position))]);
    }
    Sci_Position SCI_METHOD LineFromPosition(Sci_Position position) const override { return _r->LineFromPosition(position); }
    Sci_Position SCI_METHOD LineStart(Sci_Position line) const override { return _r->LineStart(line); }
    int SCI_METHOD GetLevel(Sci_Position line) const override { return _r->GetLevel(line); }
    int SCI_METHOD SetLevel(Sci_Position line, int level) override { return _r->SetLevel(line, level); }
    int SCI_METHOD GetLineState(Sci_Position line) const override { return _r->GetLineState(line); }
    int SCI_METHOD SetLineState(Sci_Position line, int state) override { return _r->SetLineState(line, state); }
    void SCI_METHOD StartStyling(Sci_Position position) override { _pos = position; started = true; _r->StartStyling(position); }
    bool SCI_METHOD SetStyleFor(Sci_Position n, char style) override { return write(n, nullptr, style); }
    bool SCI_METHOD SetStyles(Sci_Position n, const char* styles) override { return write(n, styles, 0); }
    void SCI_METHOD DecorationSetCurrentIndicator(int indicator) override { _r->DecorationSetCurrentIndicator(indicator); }
    void SCI_METHOD DecorationFillRange(Sci_Position position, int value, Sci_Position n) override { _r->DecorationFillRange(position, value, n); }
    void SCI_METHOD ChangeLexerState(Sci_Position start, Sci_Position end) override { _r->ChangeLexerState(start, end); }
    int SCI_METHOD CodePage() const override { return _r->CodePage(); }
    bool SCI_METHOD IsDBCSLeadByte(char ch) const override { return _r->IsDBCSLeadByte(ch); }
    const char* SCI_METHOD BufferPointer() override { return _r->BufferPointer(); }
    int SCI_METHOD GetLineIndentation(Sci_Position line) override { return _r->GetLineIndentation(line); }
    Sci_Position SCI_METHOD LineEnd(Sci_Position line) const override { return _r->LineEnd(line); }
    Sci_Position SCI_METHOD GetRelativePosition(Sci_Position start, Sci_Position offset) const override { return _r->GetRelativePosition(start, offset); }
    int SCI_METHOD GetCharacterAndWidth(Sci_Position position, Sci_Position* pWidth) const override { return _r->GetCharacterAndWidth(position, pWidth); }

private:
    bool write(Sci_Position n, const char* styles, char single)
    {
        const intptr_t p0 = _pos, p1 = _pos + n;
        if (_d.variants)
        {
            // the lexer writing one of our variants means it uses that style itself
            if (styles) { for (Sci_Position i = 0; i < n; ++i) if (_d.vflags[static_cast<unsigned char>(styles[i])]) { note(styles[i]); break; } }
            else if (_d.vflags[static_cast<unsigned char>(single)]) note(single);
        }
        bool touched = false;
        for (const Span& sp : spans) if (sp.a < p1 && sp.b > p0) { touched = true; break; }
        bool ok;
        if (!touched) ok = styles ? _r->SetStyles(n, styles) : _r->SetStyleFor(n, single);
        else
        {
            _buf.assign(static_cast<size_t>(n), single);
            if (styles) std::copy(styles, styles + n, _buf.begin());
            for (const Span& sp : spans)
            {
                if (sp.b <= p0 || sp.a >= p1) continue;
                for (intptr_t i = std::max(sp.a, p0), e = std::min(sp.b, p1); i < e; ++i)
                {
                    const unsigned char c = static_cast<unsigned char>(_buf[static_cast<size_t>(i - p0)]);
                    const int v = variantFor(_d, _d.base[c], _d.vflags[c] | sp.flags, i);
                    if (v) _buf[static_cast<size_t>(i - p0)] = static_cast<char>(v);
                }
            }
            ok = _r->SetStyles(n, _buf.data());
        }
        if (ok) _pos += n;
        return ok;
    }
    void note(char style)
    {
        const unsigned char c = static_cast<unsigned char>(style);
        _d.reserved[c] = true;
        collision = true;
    }
    IDocument* _r;
    Doc& _d;
    intptr_t _pos = 0;
    std::vector<char> _buf;
};

// ---- the wrapped lexer ------------------------------------------------------------------------

int floorOf(ILexer5* l)
{
    int f = 40;
    try
    {
        const int named = l->NamedStyles(), second = l->DistanceToSecondaryStyles();
        f = std::max(f, named + (second > 0 ? second : 0));
    }
    catch (...) {}
    return std::min(f, 192);
}

class Wrapper : public ILexer5 {
public:
    Wrapper(ILexer5* inner, sptr_t doc) : _in(inner), _doc(doc)
    {
        _floor = floorOf(inner);
        const char* n = nullptr;
        try { n = inner->GetName(); } catch (...) {}
        _name = n ? n : "";
        ++g_wrappers[doc];
    }

    int SCI_METHOD Version() const override { return Scintilla::lvRelease5; }
    void SCI_METHOD Release() override
    {
        ILexer5* in = _in;
        const sptr_t doc = _doc;
        delete this;
        in->Release();
        auto it = g_wrappers.find(doc);
        if (it != g_wrappers.end() && --it->second <= 0) g_wrappers.erase(it);
    }
    const char* SCI_METHOD PropertyNames() override { return _in->PropertyNames(); }
    int SCI_METHOD PropertyType(const char* name) override { return _in->PropertyType(name); }
    const char* SCI_METHOD DescribeProperty(const char* name) override { return _in->DescribeProperty(name); }
    Sci_Position SCI_METHOD PropertySet(const char* key, const char* val) override { return _in->PropertySet(key, val); }
    const char* SCI_METHOD DescribeWordListSets() override { return _in->DescribeWordListSets(); }
    Sci_Position SCI_METHOD WordListSet(int n, const char* wl) override { return _in->WordListSet(n, wl); }
    void* SCI_METHOD PrivateCall(int operation, void* pointer) override { return _in->PrivateCall(operation, pointer); }
    int SCI_METHOD LineEndTypesSupported() override { return _in->LineEndTypesSupported(); }
    int SCI_METHOD AllocateSubStyles(int styleBase, int numberStyles) override { return _in->AllocateSubStyles(styleBase, numberStyles); }
    int SCI_METHOD SubStylesStart(int styleBase) override { return _in->SubStylesStart(styleBase); }
    int SCI_METHOD SubStylesLength(int styleBase) override { return _in->SubStylesLength(styleBase); }
    int SCI_METHOD StyleFromSubStyle(int subStyle) override { return _in->StyleFromSubStyle(subStyle); }
    int SCI_METHOD PrimaryStyleFromStyle(int style) override { return _in->PrimaryStyleFromStyle(style); }
    void SCI_METHOD FreeSubStyles() override { _in->FreeSubStyles(); }
    void SCI_METHOD SetIdentifiers(int style, const char* identifiers) override { _in->SetIdentifiers(style, identifiers); }
    int SCI_METHOD DistanceToSecondaryStyles() override { return _in->DistanceToSecondaryStyles(); }
    const char* SCI_METHOD GetSubStyleBases() override { return _in->GetSubStyleBases(); }
    int SCI_METHOD NamedStyles() override { return _in->NamedStyles(); }
    const char* SCI_METHOD NameOfStyle(int style) override { return _in->NameOfStyle(style); }
    const char* SCI_METHOD TagsOfStyle(int style) override { return _in->TagsOfStyle(style); }
    const char* SCI_METHOD DescriptionOfStyle(int style) override { return _in->DescriptionOfStyle(style); }
    const char* SCI_METHOD GetName() override { return _in->GetName(); }
    int SCI_METHOD GetIdentifier() override { return _in->GetIdentifier(); }
    const char* SCI_METHOD PropertyGet(const char* key) override { return _in->PropertyGet(key); }

    void SCI_METHOD Lex(Sci_PositionU startPos, Sci_Position length, int initStyle, IDocument* pAccess) override
    {
        auto it = g_docs.find(_doc);
        if (it == g_docs.end()) { _in->Lex(startPos, length, initStyle, pAccess); return; }
        Doc& d = it->second;
        adopt(d);
        Proxy px(pAccess, d);
        ++s_stats.lexCalls;
        try
        {
            if (!d.rules.empty())
            {
                std::vector<HWND> shown = scisShowing(_doc);
                if (!shown.empty())
                {
                    Sci s(shown[0]);
                    const intptr_t a = static_cast<intptr_t>(startPos), b = a + length;
                    std::vector<Span> spans;
                    findSpans(s, d, a, b, spans);
                    syncIndicators(s, d.styleMask(), spans, a, b);
                    for (const Span& sp : spans) if (sp.flags) px.spans.push_back(sp);
                }
            }
        }
        catch (...) { px.spans.clear(); }
        _in->Lex(startPos, length, d.base[static_cast<unsigned char>(initStyle)], &px);
        if (!px.collision && (d.variants || !px.spans.empty()))
            settleUnwritten(pAccess, d, px.spans, static_cast<intptr_t>(startPos), static_cast<intptr_t>(startPos) + length,
                            px.started ? px.pos() : static_cast<intptr_t>(startPos));
        if (px.collision)
        {
            // the lexer uses a style we gave out: the variants move elsewhere and the document is styled again
            ++s_stats.collisions;
            bool keep[256]; std::copy(d.reserved, d.reserved + 256, keep);
            d.resetVariants();
            std::copy(keep, keep + 256, d.reserved);
            d.restyleFrom = 0;
        }
        if (d.restyleFrom >= 0) postDocWork(_doc, d);
    }

    void SCI_METHOD Fold(Sci_PositionU startPos, Sci_Position length, int initStyle, IDocument* pAccess) override
    {
        auto it = g_docs.find(_doc);
        if (it == g_docs.end() || !it->second.variants) { _in->Fold(startPos, length, initStyle, pAccess); return; }
        Proxy px(pAccess, it->second);
        _in->Fold(startPos, length, it->second.base[static_cast<unsigned char>(initStyle)], &px);
    }

    // Some lexers leave characters alone: the null lexer of plain text styles only the last one of
    // the range. Those characters still get their bold/italic, or lose one the token no longer has.
    static void settleUnwritten(IDocument* r, Doc& d, const std::vector<Span>& spans, intptr_t a, intptr_t b, intptr_t endStyled)
    {
        a = std::max<intptr_t>(a, 0);
        b = std::min<intptr_t>(b, static_cast<intptr_t>(r->Length()));
        bool wrote = false;
        intptr_t runStart = -1;
        int runStyle = -1;
        auto flush = [&](intptr_t end)
        {
            if (runStart < 0) return;
            r->StartStyling(runStart);
            r->SetStyleFor(end - runStart, static_cast<char>(runStyle));
            wrote = true;
            runStart = -1;
        };
        if (b <= a) return;
        std::vector<unsigned char> flagsAt(static_cast<size_t>(b - a), 0);
        for (const Span& sp : spans)
            for (intptr_t i = std::max(sp.a, a), e = std::min(sp.b, b); i < e; ++i) flagsAt[static_cast<size_t>(i - a)] |= static_cast<unsigned char>(sp.flags);
        for (intptr_t i = a; i < b; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(r->StyleAt(i));
            const int flags = flagsAt[static_cast<size_t>(i - a)];
            int want = c;
            if (flags) { const int v = variantFor(d, d.base[c], flags, i); want = v ? v : d.base[c]; }
            else if (d.vflags[c]) want = d.base[c];
            if (want == c) { flush(i); continue; }
            if (runStart >= 0 && want != runStyle) flush(i);
            if (runStart < 0) { runStart = i; runStyle = want; }
        }
        flush(b);
        if (wrote) r->StartStyling(endStyled);   // where the lexer left the document's styling
    }

    // this lexer now styles the document: variants made for another lexer have to go
    void adopt(Doc& d)
    {
        if (d.lexer == _name && d.floor == _floor) return;
        const bool had = !d.lexer.empty() && d.variants;
        d.lexer = _name;
        d.floor = _floor;
        if (had) { d.resetVariants(); d.restyleFrom = 0; }
    }

private:
    ILexer5* _in;
    sptr_t _doc;
    int _floor = 40;
    std::string _name;
};

// ---- defining the variant styles in a view ------------------------------------------------------

void defineVariant(HWND h, int v, int b, int flags)
{
    Sci s(h);
    char font[128] = {};
    s(SCI_STYLEGETFONT, b, reinterpret_cast<sptr_t>(font));
    s(SCI_STYLESETFONT, v, reinterpret_cast<sptr_t>(font));
    s(SCI_STYLESETSIZEFRACTIONAL, v, s(SCI_STYLEGETSIZEFRACTIONAL, b));
    s(SCI_STYLESETFORE, v, s(SCI_STYLEGETFORE, b));
    s(SCI_STYLESETBACK, v, s(SCI_STYLEGETBACK, b));
    s(SCI_STYLESETUNDERLINE, v, s(SCI_STYLEGETUNDERLINE, b));
    s(SCI_STYLESETEOLFILLED, v, s(SCI_STYLEGETEOLFILLED, b));
    s(SCI_STYLESETCHARACTERSET, v, s(SCI_STYLEGETCHARACTERSET, b));
    s(SCI_STYLESETCASE, v, s(SCI_STYLEGETCASE, b));
    s(SCI_STYLESETVISIBLE, v, s(SCI_STYLEGETVISIBLE, b));
    s(SCI_STYLESETCHANGEABLE, v, s(SCI_STYLEGETCHANGEABLE, b));
    s(SCI_STYLESETHOTSPOT, v, s(SCI_STYLEGETHOTSPOT, b));
    const int weight = static_cast<int>(s(SCI_STYLEGETWEIGHT, b));
    s(SCI_STYLESETWEIGHT, v, (flags & F_BOLD) ? std::max(weight, static_cast<int>(SC_WEIGHT_BOLD)) : weight);
    s(SCI_STYLESETITALIC, v, (flags & F_ITALIC) ? 1 : s(SCI_STYLEGETITALIC, b));
}

void defineAll(HWND h, const Doc& d, bool onlyNew)
{
    for (int v = 0; v < 256; ++v)
        if (d.vflags[v] && (!onlyNew || !d.defined[v])) defineVariant(h, v, d.base[v], d.vflags[v]);
}

// Have the document lexed again from the line of `from` on, in every window that shows it.
void restyle(sptr_t doc, intptr_t from)
{
    withDoc(doc, [&](HWND h)
    {
        Sci s(h);
        const intptr_t pos = static_cast<intptr_t>(s(SCI_POSITIONFROMLINE, s(SCI_LINEFROMPOSITION, std::max<intptr_t>(0, from))));
        if (pos < static_cast<intptr_t>(s(SCI_GETENDSTYLED))) s(SCI_STARTSTYLING, pos, 0);
    });
    for (HWND h : scisShowing(doc)) ::InvalidateRect(h, nullptr, FALSE);
}

// ---- the module -------------------------------------------------------------------------------

struct Token {
    std::string bytes;
    std::wstring text;
    int rule = -1;          // the rule it belongs to, if any
    bool multiline = false;
    bool tooLong = false;
};

struct Stored { std::wstring path; std::vector<Rule> rules; };

class TokenStyles;
TokenStyles* s_mod = nullptr;
HHOOK s_hook = nullptr;

int currentMods()
{
    int m = 0;
    if (::GetKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (::GetKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    if (::GetKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    return m;
}

bool sameText(const Rule& r, const std::wstring& t)
{
    if (r.matchCase) return r.text == t;
    return ::CompareStringOrdinal(r.text.c_str(), static_cast<int>(r.text.size()), t.c_str(), static_cast<int>(t.size()), TRUE) == CSTR_EQUAL;
}

std::wstring escape(const std::wstring& t)
{
    std::wstring o;
    for (wchar_t c : t)
    {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\t') o += L"\\t";
        else if (c == L'\r') o += L"\\r";
        else if (c == L'\n') o += L"\\n";
        else o += c;
    }
    return o;
}

std::wstring unescape(const std::wstring& t)
{
    std::wstring o;
    for (size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] != L'\\' || i + 1 == t.size()) { o += t[i]; continue; }
        wchar_t c = t[++i];
        o += c == L't' ? L'\t' : c == L'r' ? L'\r' : c == L'n' ? L'\n' : c;
    }
    return o;
}

bool isRealPath(const std::wstring& p) { return p.find(L'\\') != std::wstring::npos || p.find(L'/') != std::wstring::npos; }

class TokenStyles : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"TokenStyles"; }
    const wchar_t* title() const override { return L"Live token styles"; }

    void loadOptions() override
    {
        _mods = static_cast<UINT>(settings.getInt(key(), L"MenuModifiers", MOD_CONTROL | MOD_SHIFT));
        _vk = static_cast<UINT>(settings.getInt(key(), L"MenuKey", 'H'));
        _remember = settings.getInt(key(), L"Remember", 1) != 0;
        s_stats.menuKey = hotkeyText(_mods, _vk);
    }

    bool selfCheck(std::wstring& why) override
    {
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        for (int s = 1; s <= 5; ++s)
            if (!mm || ::GetMenuState(mm, kMarkAll[s], MF_BYCOMMAND) == static_cast<UINT>(-1) ||
                ::GetMenuState(mm, kUnmarkAll[s], MF_BYCOMMAND) == static_cast<UINT>(-1))
            {
                why = L"Search > Style All Occurrences of Token is not in this Notepad++";
                return false;
            }
        return true;
    }

    bool enable() override
    {
        loadOptions();
        if (!createMsgWindow(L"KexTokenStyles")) return false;
        if (!g_ids[0])
        {
            int first = 0;
            if (!nppMsg(NPPM_ALLOCATECMDID, CMD_COUNT, reinterpret_cast<LPARAM>(&first)) || first <= 0) return false;
            for (int i = 0; i < CMD_COUNT; ++i) g_ids[i] = static_cast<UINT>(first + i);
        }
        if (!::SetWindowSubclass(npp._nppHandle, mainProc, kSubclassId, 0)) return false;
        _subclassed = true;
        s_hook = ::SetWindowsHookExW(WH_GETMESSAGE, hookProc, nullptr, ::GetCurrentThreadId());
        if (!s_hook) { log(L"Live token styles: SetWindowsHookEx failed (%lu)", ::GetLastError()); return false; }
        g_post = hwnd();
        s_mod = this;
        _shutdown = false;
        loadStore();
        for (int v = 0; v < views.count(); ++v)
            if (HWND h = views.sci(v)) attach(h, views.bufferId(v, views.currentIndex(v)));
        return true;
    }

    void disable() override
    {
        if (s_hook) { ::UnhookWindowsHookEx(s_hook); s_hook = nullptr; }
        if (_subclassed) { ::RemoveWindowSubclass(npp._nppHandle, mainProc, kSubclassId); _subclassed = false; }
        unaugmentMainMenu();
        if (!_shutdown)
        {
            // the colours and the bold/italic go; the lexer styles the documents again (lazily)
            std::vector<sptr_t> docs;
            for (auto& kv : g_docs) docs.push_back(kv.first);
            for (sptr_t doc : docs)
            {
                Doc& d = g_docs[doc];
                const bool restyleIt = d.variants > 0;
                d.rules.clear();
                withDoc(doc, [&](HWND h)
                {
                    Sci s(h);
                    clearIndicators(s, 0x3E);
                    if (restyleIt) s(SCI_STARTSTYLING, 0, 0);
                });
                for (HWND h : scisShowing(doc)) ::InvalidateRect(h, nullptr, FALSE);
            }
        }
        g_docs.clear();
        g_post = nullptr;
        if (s_mod == this) s_mod = nullptr;
        destroyMsgWindow();
        if (g_hidden) { ::DestroyWindow(g_hidden); g_hidden = nullptr; }
    }

    void onNotify(SCNotification* n) override
    {
        switch (n->nmhdr.code)
        {
        case NPPN_SHUTDOWN:
            _shutdown = true;
            break;
        case NPPN_BUFFERACTIVATED:
        {
            const int v = views.current();
            if (HWND h = views.sci(v)) attach(h, static_cast<intptr_t>(n->nmhdr.idFrom));
            break;
        }
        case NPPN_FILECLOSED:
            for (auto it = g_docs.begin(); it != g_docs.end(); ++it)
                if (it->second.buffer == static_cast<intptr_t>(n->nmhdr.idFrom)) { g_docs.erase(it); break; }
            break;
        case NPPN_FILESAVED:
        case NPPN_FILERENAMED:
            for (auto& kv : g_docs)
                if (kv.second.buffer == static_cast<intptr_t>(n->nmhdr.idFrom))
                {
                    const std::wstring p = bufferPath(kv.second.buffer);
                    if (p != kv.second.path) { forget(kv.second.path); kv.second.path = p; remember(kv.second); }
                }
            break;
        }
    }

    // ---- options page ----
    bool applyMenuKey(UINT mods, UINT vk)
    {
        settings.setInt(key(), L"MenuModifiers", static_cast<int>(mods));
        settings.setInt(key(), L"MenuKey", static_cast<int>(vk));
        loadOptions();
        return true;
    }

    std::wstring info()
    {
        HWND h = views.sci(views.current());
        auto it = g_docs.find(docOf(h));
        const size_t n = it == g_docs.end() ? 0 : it->second.rules.size();
        std::wstring s = L"Menu key " + s_stats.menuKey + L" in the editing views. ";
        if (n) s += std::to_wstring(n) + (n == 1 ? L" token is" : L" tokens are") + L" styled in this file.";
        else s += L"No token is styled in this file.";
        return s;
    }

    int ruleCount(HWND h) { auto it = g_docs.find(docOf(h)); return it == g_docs.end() ? 0 : static_cast<int>(it->second.rules.size()); }
    int variantBase(HWND h, int style, int* flags)
    {
        auto it = g_docs.find(docOf(h));
        if (it == g_docs.end() || style < 0 || style > 255 || !it->second.vflags[style]) return -1;
        if (flags) *flags = it->second.vflags[style];
        return it->second.base[style];
    }
    std::wstring storeFile() const { return settings.dir() + L"\\kex-tokens.txt"; }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        try
        {
            if (m == WM_TOK_MENU) { showMenu(reinterpret_cast<HWND>(l)); return 0; }
            if (m == WM_TOK_VIEW)
            {
                HWND h = reinterpret_cast<HWND>(l);
                auto it = g_docs.find(docOf(h));
                if (::IsWindow(h) && it != g_docs.end() && it->second.variants) defineAll(h, it->second, false);
                return 0;
            }
            if (m == WM_TOK_DOC) { docWork(static_cast<sptr_t>(l)); return 0; }
        }
        catch (...) { log(L"Live token styles: exception in message %u", m); }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    // ---- documents ----
    void attach(HWND h, intptr_t buffer)
    {
        const sptr_t doc = docOf(h);
        if (!doc) return;
        const std::wstring path = buffer ? bufferPath(buffer) : std::wstring();
        auto it = g_docs.find(doc);
        if (it != g_docs.end())
        {
            if (buffer) it->second.buffer = buffer;
            if (buffer && path != it->second.path) { forget(it->second.path); it->second.path = path; remember(it->second); }
            return;
        }
        if (!_remember || !isRealPath(path)) return;
        for (const Stored& st : _store)
            if (::CompareStringOrdinal(st.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                Doc& d = g_docs[doc];
                d.buffer = buffer;
                d.path = path;
                d.rules = st.rules;
                log(L"Live token styles: %d styled tokens restored for %ls", static_cast<int>(d.rules.size()), path.c_str());
                changed(doc, d, 0x3E, true);
                return;
            }
    }

    Doc& docFor(HWND h)
    {
        const sptr_t doc = docOf(h);
        auto it = g_docs.find(doc);
        if (it != g_docs.end()) return it->second;
        Doc& d = g_docs[doc];
        d.buffer = static_cast<intptr_t>(nppMsg(NPPM_GETCURRENTBUFFERID));
        d.path = bufferPath(d.buffer);
        return d;
    }

    // After the rules changed: colours over the whole document now, bold/italic by lexing again.
    void changed(sptr_t doc, Doc& d, int styleMask, bool relex)
    {
        intptr_t first = -1;
        withDoc(doc, [&](HWND h)
        {
            Sci s(h);
            std::vector<Span> spans;
            findSpans(s, d, 0, static_cast<intptr_t>(s(SCI_GETLENGTH)), spans);
            syncIndicators(s, styleMask, spans, 0, static_cast<intptr_t>(s(SCI_GETLENGTH)));
            ++s_stats.fullPasses;
            for (const Span& sp : spans) if (sp.flags) { first = sp.a; break; }
        });
        if (relex)
        {
            if (!g_wrappers.count(doc))
            {
                // a document whose lexer was set before the suite started: have Notepad++ set them again
                ::SendMessageW(npp._nppHandle, WM_USER + 5000 + 1, FALSE, 0);   // WM_UPDATESCINTILLAS (Style Configurator's apply)
            }
            const intptr_t from = std::min(first >= 0 ? first : INTPTR_MAX, d.relexFrom);
            if (from != INTPTR_MAX) restyle(doc, from);
        }
        d.relexFrom = INTPTR_MAX;
        remember(d);
        for (HWND h : scisShowing(doc)) ::InvalidateRect(h, nullptr, FALSE);
        refreshPrefPages();
    }

    void docWork(sptr_t doc)
    {
        auto it = g_docs.find(doc);
        if (it == g_docs.end()) return;
        Doc& d = it->second;
        d.posted = false;
        for (HWND h : scisShowing(doc)) defineAll(h, d, true);
        for (int v = 0; v < 256; ++v) if (d.vflags[v]) d.defined[v] = true;
        if (d.restyleFrom >= 0)
        {
            const intptr_t from = d.restyleFrom;
            d.restyleFrom = -1;
            restyle(doc, from);
        }
    }

    // ---- the token under the caret ----
    Token tokenAt(HWND h, Doc* d)
    {
        Token t;
        Sci s(h);
        intptr_t a = static_cast<intptr_t>(s(SCI_GETSELECTIONSTART)), b = static_cast<intptr_t>(s(SCI_GETSELECTIONEND));
        if (a == b)
        {
            const intptr_t caret = static_cast<intptr_t>(s(SCI_GETCURRENTPOS));
            if (d && !d->rules.empty())
            {
                // on an occurrence of a styled token: that token, even if it isn't one word
                std::vector<Span> spans;
                findSpans(s, *d, caret, caret + 1, spans);
                if (caret > 0) findSpans(s, *d, caret - 1, caret, spans);
                for (const Span& sp : spans)
                    if (sp.a <= caret && caret <= sp.b && sp.b - sp.a > b - a) { a = sp.a; b = sp.b; }
            }
            if (a == b)
            {
                a = static_cast<intptr_t>(s(SCI_WORDSTARTPOSITION, caret, 1));
                b = static_cast<intptr_t>(s(SCI_WORDENDPOSITION, caret, 1));
            }
        }
        if (b <= a) return t;
        if (static_cast<size_t>(b - a) > kMaxToken) { t.tooLong = true; return t; }
        const char* p = reinterpret_cast<const char*>(s(SCI_GETRANGEPOINTER, a, b - a));
        if (!p) return t;
        t.bytes.assign(p, static_cast<size_t>(b - a));
        t.multiline = t.bytes.find_first_of("\r\n") != std::string::npos;
        t.text = toWide(t.bytes, static_cast<int>(s(SCI_GETCODEPAGE)));
        if (d)
            for (size_t i = 0; i < d->rules.size(); ++i)
                if (sameText(d->rules[i], t.text)) { t.rule = static_cast<int>(i); break; }
        return t;
    }

    enum Action { A_STYLE, A_TOGGLE_STYLE, A_BOLD, A_ITALIC, A_CLEAR, A_CLEAR_ALL, A_UNSTYLE };

    // true = done; false = left to Notepad++ (a token over several lines: its own static styling)
    bool act(Action a, int style)
    {
        HWND h = views.sci(views.current());
        if (!h) return false;
        const sptr_t doc = docOf(h);
        auto found = g_docs.find(doc);
        Doc* existing = found == g_docs.end() ? nullptr : &found->second;
        if (a == A_CLEAR_ALL || a == A_UNSTYLE)
        {
            if (!existing) return true;
            Doc& d = *existing;
            int mask = 0; bool relex = false;
            if (a == A_CLEAR_ALL) noteRelex(h, d);
            for (Rule& r : d.rules)
                if (a == A_CLEAR_ALL || r.style == style)
                {
                    if (r.style) mask |= 1 << r.style;
                    if (a == A_CLEAR_ALL && r.flags) relex = true;
                    r.style = 0;
                    if (a == A_CLEAR_ALL) r.flags = 0;
                }
            prune(d);
            s_stats.last = a == A_CLEAR_ALL ? L"cleared all tokens" : L"cleared style " + std::to_wstring(style);
            changed(doc, d, mask | 0x3E, relex);
            return true;
        }
        Token t = tokenAt(h, existing);
        if (t.bytes.empty() || t.text.empty())
        {
            if (a == A_STYLE) return false;   // Notepad++ does what it does without a token
            if (t.tooLong) showNote(L"KEX: the selection is too long to style live.", 4000);
            return true;
        }
        if (t.multiline)
        {
            if (a == A_STYLE) return false;   // Notepad++ styles it the old way
            showNote(L"KEX: live token styles work within one line.", 4000);
            return true;
        }
        Doc& d = docFor(h);
        Rule* r = t.rule >= 0 ? &d.rules[static_cast<size_t>(t.rule)] : nullptr;
        if (!r)
        {
            if (a == A_CLEAR) return true;
            Rule nr;
            nr.text = t.text;
            nr.bytes = t.bytes;
            kexsuite::styleTokenOptions(&nr.matchCase, &nr.wholeWord);
            d.rules.push_back(nr);
            r = &d.rules.back();
            d.codePage = -1;   // bytes recomputed for every rule on the next search
        }
        int mask = 0; bool relex = false;
        if (r->style) mask |= 1 << r->style;
        if (a == A_BOLD || a == A_ITALIC || a == A_CLEAR) noteRelex(h, d);
        switch (a)
        {
        case A_STYLE: r->style = style; break;
        case A_TOGGLE_STYLE: r->style = r->style == style ? 0 : style; break;
        case A_BOLD: r->flags ^= F_BOLD; relex = true; break;
        case A_ITALIC: r->flags ^= F_ITALIC; relex = true; break;
        case A_CLEAR: relex = r->flags != 0; r->style = 0; r->flags = 0; break;
        default: break;
        }
        if (r->style) mask |= 1 << r->style;
        s_stats.last = L"\"" + t.text + L"\": style " + std::to_wstring(r->style) + ((r->flags & F_BOLD) ? L" bold" : L"") + ((r->flags & F_ITALIC) ? L" italic" : L"");
        prune(d);
        changed(doc, d, mask, relex);
        return true;
    }

    // bold/italic are being taken away: lex again from the first occurrence there is now
    void noteRelex(HWND h, Doc& d)
    {
        Sci s(h);
        std::vector<Span> spans;
        findSpans(s, d, 0, static_cast<intptr_t>(s(SCI_GETLENGTH)), spans);
        for (const Span& sp : spans) if (sp.flags) { d.relexFrom = std::min(d.relexFrom, sp.a); break; }
    }

    void prune(Doc& d)
    {
        d.rules.erase(std::remove_if(d.rules.begin(), d.rules.end(), [](const Rule& r) { return !r.style && !r.flags; }), d.rules.end());
    }

    // ---- remembering them per file (kex-tokens.txt in Notepad++'s settings folder) ----
    void loadStore()
    {
        _store.clear();
        FILE* f = _wfopen(storeFile().c_str(), L"rb");
        if (!f) return;
        std::string all;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
        fclose(f);
        std::wstring w = toWide(all, SC_CP_UTF8);
        size_t pos = 0;
        while (pos < w.size())
        {
            size_t e = w.find(L'\n', pos);
            if (e == std::wstring::npos) e = w.size();
            std::wstring line = w.substr(pos, e - pos);
            pos = e + 1;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.size() < 2 || line[1] != L'\t') continue;
            if (line[0] == L'F') { _store.push_back({ line.substr(2), {} }); continue; }
            if (line[0] != L'T' || _store.empty()) continue;
            std::vector<std::wstring> f5;
            size_t p = 2;
            for (int i = 0; i < 4; ++i)
            {
                size_t t = line.find(L'\t', p);
                if (t == std::wstring::npos) break;
                f5.push_back(line.substr(p, t - p));
                p = t + 1;
            }
            if (f5.size() != 4) continue;
            Rule r;
            r.style = std::max(0, std::min(5, _wtoi(f5[0].c_str())));
            r.flags = _wtoi(f5[1].c_str()) & 3;
            r.matchCase = _wtoi(f5[2].c_str()) != 0;
            r.wholeWord = _wtoi(f5[3].c_str()) != 0;
            r.text = unescape(line.substr(p));
            if (!r.text.empty() && (r.style || r.flags)) _store.back().rules.push_back(r);
        }
        _store.erase(std::remove_if(_store.begin(), _store.end(), [](const Stored& s) { return s.rules.empty(); }), _store.end());
    }

    void saveStore()
    {
        std::wstring w = L"# KEX live token styles, one block per file (Settings > Preferences > KEX Highlighting)\n";
        for (const Stored& st : _store)
        {
            w += L"F\t" + st.path + L"\n";
            for (const Rule& r : st.rules)
                w += L"T\t" + std::to_wstring(r.style) + L"\t" + std::to_wstring(r.flags) + L"\t" + (r.matchCase ? L"1" : L"0") + L"\t"
                   + (r.wholeWord ? L"1" : L"0") + L"\t" + escape(r.text) + L"\n";
        }
        const std::string s = toBytes(w, SC_CP_UTF8);
        const std::wstring tmp = storeFile() + L".new";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return;
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
        ::MoveFileExW(tmp.c_str(), storeFile().c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void remember(const Doc& d)
    {
        if (!_remember || !isRealPath(d.path)) return;
        forget(d.path, false);
        if (!d.rules.empty())
        {
            Stored st{ d.path, d.rules };
            for (Rule& r : st.rules) r.bytes.clear();
            _store.push_back(st);
            if (_store.size() > kMaxStoredFiles) _store.erase(_store.begin(), _store.begin() + (_store.size() - kMaxStoredFiles));
        }
        saveStore();
    }

    void forget(const std::wstring& path, bool save = true)
    {
        if (!_remember || path.empty()) return;
        const size_t before = _store.size();
        _store.erase(std::remove_if(_store.begin(), _store.end(), [&](const Stored& s)
            { return ::CompareStringOrdinal(s.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL; }), _store.end());
        if (save && _store.size() != before) saveStore();
    }

    // ---- menus ----
    static bool menuHas(HMENU m, UINT id)
    {
        for (int i = 0, n = ::GetMenuItemCount(m); i < n; ++i)
            if (::GetMenuItemID(m, i) == id) return true;
        return false;
    }

    static HMENU submenuWith(HMENU m, UINT id, int depth = 0)
    {
        if (!m || depth > 4) return nullptr;
        if (menuHas(m, id)) return m;
        for (int i = 0, n = ::GetMenuItemCount(m); i < n; ++i)
            if (HMENU sub = ::GetSubMenu(m, i))
                if (HMENU r = submenuWith(sub, id, depth + 1)) return r;
        return nullptr;
    }

    const Rule* ruleAtCaret()
    {
        HWND h = views.sci(views.current());
        auto it = g_docs.find(docOf(h));
        if (!h || it == g_docs.end()) return nullptr;
        Token t = tokenAt(h, &it->second);
        return t.rule >= 0 ? &it->second.rules[static_cast<size_t>(t.rule)] : nullptr;
    }

    // Notepad++'s own "Style All Occurrences of Token" submenu: Bold, Italic, Clear This Token below its five
    // styles, and checks for the token at the caret.
    void augment(HMENU m)
    {
        if (::GetMenuState(m, g_ids[CMD_BOLD], MF_BYCOMMAND) == static_cast<UINT>(-1))
        {
            ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(m, MF_STRING, g_ids[CMD_BOLD], L"Bold");
            ::AppendMenuW(m, MF_STRING, g_ids[CMD_ITALIC], L"Italic");
            ::AppendMenuW(m, MF_STRING, g_ids[CMD_CLEAR], L"Clear This Token");
        }
        const Rule* r = ruleAtCaret();
        for (int s = 1; s <= 5; ++s)
            ::CheckMenuItem(m, kMarkAll[s], MF_BYCOMMAND | (r && r->style == s ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(m, g_ids[CMD_BOLD], MF_BYCOMMAND | (r && (r->flags & F_BOLD) ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(m, g_ids[CMD_ITALIC], MF_BYCOMMAND | (r && (r->flags & F_ITALIC) ? MF_CHECKED : MF_UNCHECKED));
        ::EnableMenuItem(m, g_ids[CMD_CLEAR], MF_BYCOMMAND | (r ? MF_ENABLED : MF_GRAYED));
        ++s_stats.augmented;
    }

    // a context menu without that submenu (contextMenu.xml changed) gets one of its own
    void addOwnSubmenu(HMENU top)
    {
        HMENU sub = ::CreatePopupMenu();
        for (int s = 1; s <= 5; ++s)
            ::AppendMenuW(sub, MF_STRING, kMarkAll[s], (std::wstring(L"Using ") + kOrdinal[s] + L" Style").c_str());
        augment(sub);
        ::AppendMenuW(top, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(top, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Style All Occurrences of Token");
    }

    void unaugmentMainMenu()
    {
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        HMENU m = submenuWith(mm, IDM_SEARCH_MARKALLEXT1);
        if (!m || ::GetMenuState(m, g_ids[CMD_BOLD], MF_BYCOMMAND) == static_cast<UINT>(-1)) return;
        for (int i = ::GetMenuItemCount(m) - 1; i >= 0; --i)
        {
            const UINT id = ::GetMenuItemID(m, i);
            const bool ours = id == g_ids[CMD_BOLD] || id == g_ids[CMD_ITALIC] || id == g_ids[CMD_CLEAR];
            if (ours) ::DeleteMenu(m, static_cast<UINT>(i), MF_BYPOSITION);
        }
        int last = ::GetMenuItemCount(m) - 1;
        MENUITEMINFOW mi{ sizeof(mi) }; mi.fMask = MIIM_FTYPE;
        if (last >= 0 && ::GetMenuItemInfoW(m, static_cast<UINT>(last), TRUE, &mi) && (mi.fType & MFT_SEPARATOR))
            ::DeleteMenu(m, static_cast<UINT>(last), MF_BYPOSITION);
        for (int s = 1; s <= 5; ++s) ::CheckMenuItem(m, kMarkAll[s], MF_BYCOMMAND | MF_UNCHECKED);
    }

    static HBITMAP swatch(COLORREF c, int size)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return bmp;
        auto* px = static_cast<DWORD*>(bits);
        const BYTE r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
        const DWORD fill = 0xFF000000u | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(g) << 8) | b;
        const DWORD edge = 0xFF000000u | (static_cast<DWORD>(r * 3 / 5) << 16) | (static_cast<DWORD>(g * 3 / 5) << 8) | (b * 3 / 5);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const bool in = x >= 1 && y >= 1 && x < size - 1 && y < size - 1;
                const bool corner = (x <= 1 || x >= size - 2) && (y <= 1 || y >= size - 2);
                const bool border = x == 1 || y == 1 || x == size - 2 || y == size - 2;
                px[y * size + x] = !in || corner ? 0 : (border ? edge : fill);
            }
        return bmp;
    }

    // The menu key: the same choices as a small menu at the caret.
    void showMenu(HWND h)
    {
        if (!h || views.viewOf(h) < 0) return;
        const sptr_t doc = docOf(h);
        auto found = g_docs.find(doc);
        Token t = tokenAt(h, found == g_docs.end() ? nullptr : &found->second);
        const Rule* r = (t.rule >= 0 && found != g_docs.end()) ? &found->second.rules[static_cast<size_t>(t.rule)] : nullptr;
        const bool usable = !t.text.empty() && !t.multiline;
        const bool anyRules = found != g_docs.end() && !found->second.rules.empty();

        HMENU m = ::CreatePopupMenu();
        std::wstring head;
        if (usable)
        {
            std::wstring shown = t.text.size() > 40 ? t.text.substr(0, 40) + L"..." : t.text;
            for (wchar_t& c : shown) if (c == L'\t') c = L' ';
            std::wstring amp;
            for (wchar_t c : shown) { amp += c; if (c == L'&') amp += L'&'; }
            head = L"“" + amp + L"”";
        }
        else head = t.multiline ? L"Live styles work within one line" : L"No word at the caret";
        MENUITEMINFOW mi{ sizeof(mi) };
        mi.fMask = MIIM_STRING | MIIM_STATE | MIIM_ID;
        mi.wID = 99; mi.fState = MFS_DISABLED | MFS_DEFAULT; mi.dwTypeData = const_cast<wchar_t*>(head.c_str());
        ::InsertMenuItemW(m, 0, TRUE, &mi);
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        HDC screen = ::GetDC(nullptr);
        const int dpi = screen ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
        if (screen) ::ReleaseDC(nullptr, screen);
        const int size = std::max(12, 14 * dpi / 96);
        HBITMAP sw[6] = {};
        for (int s = 1; s <= 5; ++s)
        {
            sw[s] = swatch(static_cast<COLORREF>(::SendMessageW(h, SCI_INDICGETFORE, kIndicator[s], 0)), size);
            std::wstring text = std::wstring(L"&") + kOrdinal[s] + L" style";
            MENUITEMINFOW it{ sizeof(it) };
            it.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_BITMAP | MIIM_FTYPE;
            it.fType = MFT_STRING | MFT_RADIOCHECK;
            it.wID = static_cast<UINT>(s);
            it.fState = (usable ? MFS_ENABLED : MFS_DISABLED) | (r && r->style == s ? MFS_CHECKED : MFS_UNCHECKED);
            it.hbmpItem = sw[s];
            it.dwTypeData = const_cast<wchar_t*>(text.c_str());
            ::InsertMenuItemW(m, static_cast<UINT>(::GetMenuItemCount(m)), TRUE, &it);
        }
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(m, MF_STRING | (usable ? 0 : MF_GRAYED) | (r && (r->flags & F_BOLD) ? MF_CHECKED : 0), 10, L"&Bold");
        ::AppendMenuW(m, MF_STRING | (usable ? 0 : MF_GRAYED) | (r && (r->flags & F_ITALIC) ? MF_CHECKED : 0), 11, L"&Italic");
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(m, MF_STRING | (r ? 0 : MF_GRAYED), 12, L"&Clear this token");
        ::AppendMenuW(m, MF_STRING | (anyRules ? 0 : MF_GRAYED), 13, L"Clear &all tokens");

        Sci s(h);
        const intptr_t caret = static_cast<intptr_t>(s(SCI_GETCURRENTPOS));
        POINT pt{ static_cast<LONG>(s(SCI_POINTXFROMPOSITION, 0, caret)),
                  static_cast<LONG>(s(SCI_POINTYFROMPOSITION, 0, caret) + s(SCI_TEXTHEIGHT, s(SCI_LINEFROMPOSITION, caret))) };
        RECT rc; ::GetClientRect(h, &rc);
        pt.x = std::max<LONG>(0, std::min(pt.x, rc.right)); pt.y = std::max<LONG>(0, std::min(pt.y, rc.bottom));
        ::ClientToScreen(h, &pt);
        ++s_stats.menus;
        HWND owner = views.host(views.viewOf(h));
        ::SetForegroundWindow(owner);
        const UINT cmd = static_cast<UINT>(::TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr));
        ::DestroyMenu(m);
        for (HBITMAP b : sw) if (b) ::DeleteObject(b);
        ::SetFocus(h);
        if (cmd >= 1 && cmd <= 5) act(A_TOGGLE_STYLE, static_cast<int>(cmd));
        else if (cmd == 10) act(A_BOLD, 0);
        else if (cmd == 11) act(A_ITALIC, 0);
        else if (cmd == 12) act(A_CLEAR, 0);
        else if (cmd == 13) act(A_CLEAR_ALL, 0);
    }

    // ---- hooks ----
    static LRESULT CALLBACK hookProc(int code, WPARAM w, LPARAM l)
    {
        if (code == HC_ACTION && w == PM_REMOVE && s_mod && s_mod->_vk)
        {
            MSG* msg = reinterpret_cast<MSG*>(l);
            if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) && msg->wParam == s_mod->_vk &&
                static_cast<UINT>(currentMods()) == s_mod->_mods && views.viewOf(msg->hwnd) >= 0)
            {
                ::PostMessageW(s_mod->hwnd(), WM_TOK_MENU, 0, reinterpret_cast<LPARAM>(msg->hwnd));
                msg->message = WM_NULL;
            }
        }
        return ::CallNextHookEx(s_hook, code, w, l);
    }

    static LRESULT CALLBACK mainProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
    {
        TokenStyles* self = s_mod;
        if (!self) return ::DefSubclassProc(h, m, w, l);
        try
        {
            switch (m)
            {
            case WM_COMMAND:
            {
                const UINT id = LOWORD(w);
                if (id == g_ids[CMD_BOLD]) { self->act(A_BOLD, 0); return 0; }
                if (id == g_ids[CMD_ITALIC]) { self->act(A_ITALIC, 0); return 0; }
                if (id == g_ids[CMD_CLEAR]) { self->act(A_CLEAR, 0); return 0; }
                for (int s = 1; s <= 5; ++s)
                {
                    if (id == kMarkAll[s]) { if (self->act(A_STYLE, s)) return 0; break; }
                    if (id == kUnmarkAll[s]) { self->act(A_UNSTYLE, s); break; }   // Notepad++ clears the rest of that style
                }
                if (id == IDM_SEARCH_CLEARALLMARKS) self->act(A_CLEAR_ALL, 0);
                break;
            }
            case WM_CONTEXTMENU:
                if (views.viewOf(reinterpret_cast<HWND>(w)) >= 0)
                {
                    self->_editorMenu = true; self->_editorMenuSeen = false;
                    LRESULT r = ::DefSubclassProc(h, m, w, l);
                    self->_editorMenu = false;
                    return r;
                }
                break;
            case WM_INITMENUPOPUP:
            {
                LRESULT r = ::DefSubclassProc(h, m, w, l);
                HMENU menu = reinterpret_cast<HMENU>(w);
                if (!HIWORD(l))
                {
                    if (menuHas(menu, IDM_SEARCH_MARKALLEXT1)) self->augment(menu);
                    else if (self->_editorMenu && !self->_editorMenuSeen)
                    {
                        self->_editorMenuSeen = true;
                        if (!submenuWith(menu, IDM_SEARCH_MARKALLEXT1)) self->addOwnSubmenu(menu);
                    }
                }
                return r;
            }
            case WM_NCDESTROY:
                ::RemoveWindowSubclass(h, mainProc, kSubclassId);
                self->_subclassed = false;
                break;
            }
        }
        catch (...) { log(L"Live token styles: exception in message %u", m); }
        return ::DefSubclassProc(h, m, w, l);
    }

public:
    UINT _mods = MOD_CONTROL | MOD_SHIFT, _vk = 'H';
    bool _remember = true;
private:
    bool _subclassed = false, _shutdown = false;
    bool _editorMenu = false, _editorMenuSeen = false;
    std::vector<Stored> _store;
};

} // namespace

Module* createTokenStyles() { return new TokenStyles(); }

TokenStats tokenStats()
{
    TokenStats s = s_stats;
    s.docs = static_cast<int>(g_docs.size());
    s.liveWrappers = 0;
    for (auto& kv : g_wrappers) s.liveWrappers += kv.second;
    s.hooked = s_hook != nullptr;
    return s;
}
int tokenCommandId(int which) { return (which >= 0 && which < CMD_COUNT) ? static_cast<int>(g_ids[which]) : 0; }
std::wstring tokenStoreFile() { return settings.dir() + L"\\kex-tokens.txt"; }
std::wstring tokenInfo() { return s_mod ? s_mod->info() : std::wstring(); }
int tokenRuleCount(HWND view) { return s_mod ? s_mod->ruleCount(view) : 0; }
int tokenVariantBase(HWND view, int style, int* flags) { return s_mod ? s_mod->variantBase(view, style & 0xFF, flags) : -1; }
bool tokenDocWrapped(HWND view) { return g_wrappers.count(docOf(view)) != 0; }
bool tokenMenuKeyApply(UINT mods, UINT vk)
{
    if (s_mod) return s_mod->applyMenuKey(mods, vk);
    settings.setInt(L"TokenStyles", L"MenuModifiers", static_cast<int>(mods));
    settings.setInt(L"TokenStyles", L"MenuKey", static_cast<int>(vk));
    return true;
}

} // namespace kex

// ---------------------------------------------------------------------------------------------
// entry point for the core (KexSuiteHost.h)

namespace kexsuite {

Scintilla::ILexer5* wrapLexer(Scintilla::ILexer5* lexer, HWND view)
{
    using namespace kex;
    if (!lexer || !view || !modules[0]) return lexer;
    try
    {
        const sptr_t doc = docOf(view);
        if (!doc) return lexer;
        ++s_stats.wrapperCalls;
        g_scis.erase(std::remove_if(g_scis.begin(), g_scis.end(), [](HWND h) { return !::IsWindow(h); }), g_scis.end());
        if (std::find(g_scis.begin(), g_scis.end(), view) == g_scis.end()) g_scis.push_back(view);
        // Notepad++ sets this view's styles right after the lexer: our variants are defined again then
        auto it = g_docs.find(doc);
        if (g_post && it != g_docs.end() && it->second.variants) ::PostMessageW(g_post, WM_TOK_VIEW, 0, reinterpret_cast<LPARAM>(view));
        return new Wrapper(lexer, doc);
    }
    catch (...) { return lexer; }
}

} // namespace kexsuite
