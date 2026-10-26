// merOS Shield - MerEngine : hesaplanmış stil (UA stilleri, cascade, kalıtım, CSS değişkenleri)
#include "style.h"
#include "util.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <initializer_list>
#include <mutex>

namespace mer {

namespace {

struct NamedColor { const wchar_t* n; uint32_t c; };
const NamedColor kColors[] = {
    {L"black", 0x000000}, {L"white", 0xFFFFFF}, {L"red", 0xFF0000}, {L"green", 0x008000}, {L"blue", 0x0000FF},
    {L"yellow", 0xFFFF00}, {L"orange", 0xFFA500}, {L"gray", 0x808080}, {L"grey", 0x808080}, {L"silver", 0xC0C0C0},
    {L"maroon", 0x800000}, {L"purple", 0x800080}, {L"fuchsia", 0xFF00FF}, {L"magenta", 0xFF00FF}, {L"lime", 0x00FF00},
    {L"olive", 0x808000}, {L"navy", 0x000080}, {L"teal", 0x008080}, {L"aqua", 0x00FFFF}, {L"cyan", 0x00FFFF},
    {L"darkgray", 0xA9A9A9}, {L"darkgrey", 0xA9A9A9}, {L"lightgray", 0xD3D3D3}, {L"lightgrey", 0xD3D3D3},
    {L"dimgray", 0x696969}, {L"dimgrey", 0x696969}, {L"whitesmoke", 0xF5F5F5}, {L"gainsboro", 0xDCDCDC},
    {L"darkblue", 0x00008B}, {L"darkred", 0x8B0000}, {L"darkgreen", 0x006400}, {L"brown", 0xA52A2A},
    {L"pink", 0xFFC0CB}, {L"gold", 0xFFD700}, {L"beige", 0xF5F5DC}, {L"ivory", 0xFFFFF0}, {L"lightblue", 0xADD8E6},
    {L"lightyellow", 0xFFFFE0}, {L"lightgreen", 0x90EE90}, {L"crimson", 0xDC143C}, {L"tomato", 0xFF6347},
    {L"coral", 0xFF7F50}, {L"salmon", 0xFA8072}, {L"indigo", 0x4B0082}, {L"violet", 0xEE82EE},
    {L"steelblue", 0x4682B4}, {L"royalblue", 0x4169E1}, {L"skyblue", 0x87CEEB}, {L"slategray", 0x708090},
    {L"slategrey", 0x708090}, {L"darkslategray", 0x2F4F4F}, {L"aliceblue", 0xF0F8FF}, {L"ghostwhite", 0xF8F8FF},
    {L"snow", 0xFFFAFA}, {L"linen", 0xFAF0E6}, {L"wheat", 0xF5DEB3}, {L"tan", 0xD2B48C}, {L"chocolate", 0xD2691E},
    {L"firebrick", 0xB22222}, {L"seagreen", 0x2E8B57}, {L"forestgreen", 0x228B22}, {L"darkorange", 0xFF8C00},
    {L"orangered", 0xFF4500}, {L"midnightblue", 0x191970}, {L"dodgerblue", 0x1E90FF}, {L"lavender", 0xE6E6FA},
    {L"khaki", 0xF0E68C}, {L"honeydew", 0xF0FFF0}, {L"mintcream", 0xF5FFFA}, {L"azure", 0xF0FFFF},
    {L"cornsilk", 0xFFF8DC}, {L"floralwhite", 0xFFFAF0}, {L"oldlace", 0xFDF5E6}, {L"seashell", 0xFFF5EE},
    {L"lightcyan", 0xE0FFFF}, {L"lightpink", 0xFFB6C1}, {L"darkcyan", 0x008B8B}, {L"darkviolet", 0x9400D3},
    {L"rebeccapurple", 0x663399}, {L"cornflowerblue", 0x6495ED}, {L"lightsteelblue", 0xB0C4DE},
    {L"lightslategray", 0x778899}, {L"darkolivegreen", 0x556B2F}, {L"goldenrod", 0xDAA520},
    {L"darkslateblue", 0x483D8B}, {L"slateblue", 0x6A5ACD}, {L"mediumblue", 0x0000CD}, {L"deepskyblue", 0x00BFFF},
    {L"lightskyblue", 0x87CEFA}, {L"cadetblue", 0x5F9EA0}, {L"powderblue", 0xB0E0E6}, {L"turquoise", 0x40E0D0},
    {L"mediumseagreen", 0x3CB371}, {L"limegreen", 0x32CD32}, {L"yellowgreen", 0x9ACD32}, {L"olivedrab", 0x6B8E23},
    {L"darkkhaki", 0xBDB76B}, {L"peru", 0xCD853F}, {L"sienna", 0xA0522D}, {L"saddlebrown", 0x8B4513},
    {L"rosybrown", 0xBC8F8F}, {L"indianred", 0xCD5C5C}, {L"lightcoral", 0xF08080}, {L"darksalmon", 0xE9967A},
    {L"hotpink", 0xFF69B4}, {L"deeppink", 0xFF1493}, {L"mediumvioletred", 0xC71585}, {L"orchid", 0xDA70D6},
    {L"plum", 0xDDA0DD}, {L"thistle", 0xD8BFD8}, {L"mediumpurple", 0x9370DB}, {L"blueviolet", 0x8A2BE2},
    {L"darkmagenta", 0x8B008B}, {L"mistyrose", 0xFFE4E1}, {L"lavenderblush", 0xFFF0F5}, {L"papayawhip", 0xFFEFD5},
    {L"blanchedalmond", 0xFFEBCD}, {L"bisque", 0xFFE4C4}, {L"moccasin", 0xFFE4B5}, {L"navajowhite", 0xFFDEAD},
    {L"peachpuff", 0xFFDAB9}, {L"lemonchiffon", 0xFFFACD}, {L"lightgoldenrodyellow", 0xFAFAD2}, {L"palegreen", 0x98FB98},
    {L"springgreen", 0x00FF7F}, {L"aquamarine", 0x7FFFD4}, {L"mediumaquamarine", 0x66CDAA}, {L"lightseagreen", 0x20B2AA},
    {L"darkturquoise", 0x00CED1}, {L"mediumturquoise", 0x48D1CC}, {L"paleturquoise", 0xAFEEEE}, {L"darkgoldenrod", 0xB8860B},
    {L"palegoldenrod", 0xEEE8AA}, {L"burlywood", 0xDEB887}, {L"sandybrown", 0xF4A460}, {L"antiquewhite", 0xFAEBD7},
    {L"darkseagreen", 0x8FBC8F}, {L"chartreuse", 0x7FFF00}, {L"lawngreen", 0x7CFC00}, {L"greenyellow", 0xADFF2F},
    {L"mediumspringgreen", 0x00FA9A}, {L"mediumorchid", 0xBA55D3}, {L"darkorchid", 0x9932CC}, {L"palevioletred", 0xDB7093},
    {L"mediumslateblue", 0x7B68EE}, {L"darkslategrey", 0x2F4F4F}, {L"lightslategrey", 0x778899},
};

const float kRootFs = 16.f;
// Gorunur alan (vw/vh): stil hesaplanan is parcacigina ozel
thread_local float t_vw = 1280.f, t_vh = 800.f;

bool InList(const std::wstring& n, std::initializer_list<const wchar_t*> l) {
    for (auto x : l) if (n == x) return true;
    return false;
}

bool ParseNumber(const std::wstring& s, float& v, std::wstring& unit) {
    if (s.empty()) return false;
    const wchar_t* b = s.c_str();
    wchar_t* e = nullptr;
    v = wcstof(b, &e);
    if (e == b) return false;
    unit = ToLower(std::wstring(e));
    return std::isfinite(v);
}

// Ust duzey (parantez disi) bosluklara gore bol
std::vector<std::wstring> SplitTop(const std::wstring& s, wchar_t sep = 0) {
    std::vector<std::wstring> out;
    std::wstring cur;
    int paren = 0;
    bool inQ = false;
    wchar_t q = 0;
    for (wchar_t c : s) {
        if (inQ) { cur += c; if (c == q) inQ = false; continue; }
        if (c == L'"' || c == L'\'') { inQ = true; q = c; cur += c; continue; }
        if (c == L'(') paren++;
        else if (c == L')' && paren > 0) paren--;
        bool isSep = sep ? (c == sep) : IsWS(c);
        if (isSep && paren == 0) {
            if (sep || !cur.empty()) out.push_back(sep ? Trim(cur) : cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (sep) out.push_back(Trim(cur));
    else if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---------------- calc() degerlendirici ----------------
// Deger = px + pct% (+ unitless sayi). min()/max() karisik birimlerde 'cap' ile yaklasik.
struct CV {
    float px = 0, pct = 0, num = 0;
    bool isNum = false;     // birimsiz sayi
    bool ok = true;
    float cap = 0;
    uint8_t capKind = 0;
};

class CalcParser {
public:
    CalcParser(const std::wstring& s, float fs) : s_(s), fs_(fs) {}
    CV Parse() { CV v = Expr(); Ws(); if (i_ != s_.size()) v.ok = false; return v; }
    CV Expr() {
        CV a = Term();
        for (;;) {
            Ws();
            if (i_ >= s_.size() || (s_[i_] != L'+' && s_[i_] != L'-')) return a;
            wchar_t op = s_[i_++];
            CV b = Term();
            if (!b.ok || !a.ok) { a.ok = false; return a; }
            if (a.isNum != b.isNum) { a.ok = false; return a; }
            float sg = op == L'+' ? 1.f : -1.f;
            a.px += sg * b.px; a.pct += sg * b.pct; a.num += sg * b.num;
            if (b.capKind) { a.cap = b.cap; a.capKind = b.capKind; }
        }
    }

private:
    const std::wstring& s_;
    float fs_;
    size_t i_ = 0;
    int depth_ = 0;
    void Ws() { while (i_ < s_.size() && IsWS(s_[i_])) i_++; }
    CV Term() {
        CV a = Factor();
        for (;;) {
            Ws();
            if (i_ >= s_.size() || (s_[i_] != L'*' && s_[i_] != L'/')) return a;
            wchar_t op = s_[i_++];
            CV b = Factor();
            if (!a.ok || !b.ok) { a.ok = false; return a; }
            if (op == L'*') {
                if (a.isNum && b.isNum) a.num *= b.num;
                else if (a.isNum) { float k = a.num; a = b; a.px *= k; a.pct *= k; a.cap *= k; }
                else if (b.isNum) { a.px *= b.num; a.pct *= b.num; a.cap *= b.num; }
                else a.ok = false;
            } else {
                if (!b.isNum || std::fabs(b.num) < 1e-6f) { a.ok = false; return a; }
                if (a.isNum) a.num /= b.num;
                else { a.px /= b.num; a.pct /= b.num; a.cap /= b.num; }
            }
        }
    }
    std::vector<CV> Args() {
        std::vector<CV> out;
        for (;;) {
            out.push_back(Expr());
            Ws();
            if (i_ < s_.size() && s_[i_] == L',') { i_++; continue; }
            break;
        }
        Ws();
        if (i_ < s_.size() && s_[i_] == L')') i_++;
        else out.push_back(CV{0, 0, 0, false, false});
        return out;
    }
    static CV MinMax(const std::vector<CV>& a, bool isMin) {
        CV r;
        if (a.empty()) { r.ok = false; return r; }
        for (auto& x : a) if (!x.ok) { r.ok = false; return r; }
        bool anyPct = false;
        for (auto& x : a) if (x.pct != 0 || x.capKind) anyPct = true;
        if (!anyPct) {
            r = a[0];
            for (auto& x : a) {
                if (x.isNum) r.num = isMin ? std::min(r.num, x.num) : std::max(r.num, x.num);
                else r.px = isMin ? std::min(r.px, x.px) : std::max(r.px, x.px);
            }
            return r;
        }
        // karisik: yuzdeli olani temel al, sabit olani sinir yap
        const CV* pctV = nullptr;
        const CV* fixV = nullptr;
        for (auto& x : a) { if (x.pct != 0 && !pctV) pctV = &x; else if (x.pct == 0 && !fixV) fixV = &x; }
        if (!pctV) pctV = &a[0];
        r = *pctV;
        if (fixV && !r.capKind) { r.cap = fixV->px; r.capKind = isMin ? 1 : 2; }
        return r;
    }
    CV Factor() {
        Ws();
        CV v;
        if (i_ >= s_.size() || ++depth_ > 32) { v.ok = false; return v; }
        if (s_[i_] == L'(') {
            i_++;
            v = Expr();
            Ws();
            if (i_ < s_.size() && s_[i_] == L')') i_++; else v.ok = false;
            depth_--;
            return v;
        }
        if (iswalpha(s_[i_])) {
            size_t st = i_;
            while (i_ < s_.size() && (iswalnum(s_[i_]) || s_[i_] == L'-')) i_++;
            std::wstring fn = ToLower(s_.substr(st, i_ - st));
            if (i_ < s_.size() && s_[i_] == L'(') {
                i_++;
                std::vector<CV> a = Args();
                depth_--;
                if (fn == L"calc" || fn == L"-webkit-calc") return a.empty() ? CV{0, 0, 0, false, false} : a[0];
                if (fn == L"min") return MinMax(a, true);
                if (fn == L"max") return MinMax(a, false);
                if (fn == L"clamp" && a.size() == 3) {
                    CV inner = MinMax({a[1], a[2]}, true);
                    return MinMax({a[0], inner}, false);
                }
                if (fn == L"env") { CV z; return a.size() >= 2 ? a[1] : z; }
                v.ok = false;
                return v;
            }
            depth_--;
            if (fn == L"infinity") { v.isNum = true; v.num = 1e6f; return v; }
            if (fn == L"pi") { v.isNum = true; v.num = 3.14159265f; return v; }
            v.ok = false;
            return v;
        }
        const wchar_t* b = s_.c_str() + i_;
        wchar_t* e = nullptr;
        float f = wcstof(b, &e);
        if (e == b || !std::isfinite(f)) { v.ok = false; depth_--; return v; }
        i_ += size_t(e - b);
        size_t st = i_;
        while (i_ < s_.size() && (iswalpha(s_[i_]) || s_[i_] == L'%')) i_++;
        std::wstring u = ToLower(s_.substr(st, i_ - st));
        depth_--;
        if (u.empty()) { v.isNum = true; v.num = f; return v; }
        if (u == L"px") v.px = f;
        else if (u == L"%") v.pct = f;
        else if (u == L"em") v.px = f * fs_;
        else if (u == L"rem") v.px = f * kRootFs;
        else if (u == L"vw" || u == L"dvw" || u == L"svw" || u == L"lvw" || u == L"cqw" || u == L"cqi") v.px = f * t_vw / 100.f;
        else if (u == L"vh" || u == L"dvh" || u == L"svh" || u == L"lvh" || u == L"cqh" || u == L"cqb") v.px = f * t_vh / 100.f;
        else if (u == L"vmin") v.px = f * std::min(t_vw, t_vh) / 100.f;
        else if (u == L"vmax") v.px = f * std::max(t_vw, t_vh) / 100.f;
        else if (u == L"pt") v.px = f * 4.f / 3.f;
        else if (u == L"pc") v.px = f * 16.f;
        else if (u == L"ch" || u == L"ex") v.px = f * fs_ * 0.5f;
        else if (u == L"lh") v.px = f * fs_ * 1.3f;
        else if (u == L"in") v.px = f * 96.f;
        else if (u == L"cm") v.px = f * 37.8f;
        else if (u == L"mm") v.px = f * 3.78f;
        else if (u == L"q") v.px = f * 0.945f;
        else if (u == L"deg" || u == L"s" || u == L"ms" || u == L"fr") { v.isNum = true; v.num = f; }
        else v.ok = false;
        return v;
    }
};

bool CVToLen(const CV& c, Len& out) {
    if (!c.ok) return false;
    if (c.isNum) { if (c.num == 0) { out = {0, Len::Px}; return true; } return false; }
    if (c.pct == 0 && !c.capKind) { out = {c.px, Len::Px}; return true; }
    if (c.px == 0 && !c.capKind) { out = {c.pct, Len::Pct}; return true; }
    out = Len();
    out.u = Len::Calc;
    out.v = c.px;
    out.p = c.pct;
    out.cap = c.cap;
    out.capKind = c.capKind;
    return true;
}

// Uzunluk: px, em, rem, %, pt, vw, vh, ch, ex, in, cm, mm, calc(), min(), max(), clamp()
bool ParseLen(const std::wstring& vIn, float fs, Len& out) {
    std::wstring v = ToLower(Trim(vIn));
    if (v.empty()) return false;
    if (v == L"auto" || v == L"-webkit-fill-available" || v == L"-moz-available" || v == L"stretch" || v == L"fill-available") { out = {0, Len::Auto}; return true; }
    if (v == L"none") { out = {0, Len::None}; return true; }
    if (v == L"fit-content" || v == L"max-content" || v == L"min-content" || v == L"-moz-fit-content" || v == L"-webkit-fit-content" || v == L"-webkit-max-content") { out = {0, Len::Fit}; return true; }
    if (v == L"0") { out = {0, Len::Px}; return true; }
    if (StartsWith(v, L"fit-content(")) { out = {0, Len::Fit}; return true; }
    CalcParser cp(v, fs);
    CV c = cp.Parse();
    if (!c.ok) return false;
    if (c.isNum && c.num != 0) {
        // birimsiz sayi (eski HTML / quirks): piksel kabul et
        if (iswdigit(v[0]) || v[0] == L'.') { out = {c.num, Len::Px}; return true; }
        return false;
    }
    return CVToLen(c, out);
}

float HueToRgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

std::wstring SubstVars(const std::wstring& v, const VarMap* vars, int depth = 0) {
    if (depth > 12) return v;
    size_t p = v.find(L"var(");
    if (p == std::wstring::npos) return v;
    std::wstring out = v.substr(0, p);
    size_t i = p + 4;
    int paren = 1;
    size_t st = i;
    while (i < v.size() && paren > 0) {
        if (v[i] == L'(') paren++;
        else if (v[i] == L')') paren--;
        if (paren > 0) i++;
    }
    std::wstring inner = v.substr(st, i - st);
    std::wstring name = inner, fallback;
    size_t c = inner.find(L',');
    if (c != std::wstring::npos) { name = inner.substr(0, c); fallback = inner.substr(c + 1); }
    name = Trim(name);
    std::wstring rep;
    bool found = false;
    if (vars) {
        auto it = vars->find(name);
        if (it != vars->end()) { rep = it->second; found = true; }
    }
    if (!found) rep = Trim(fallback);
    out += SubstVars(rep, vars, depth + 1);
    if (i < v.size()) out += SubstVars(v.substr(i + 1), vars, depth + 1);
    if (out.size() > 20000) out.resize(20000);
    return out;
}

void SetBoxSides(Len* box, const std::wstring& v, float fs) {
    auto t = SplitTop(v);
    Len l[4];
    int n = 0;
    for (auto& tok : t) { if (n >= 4) break; if (!ParseLen(tok, fs, l[n])) return; n++; }
    if (n == 1) { box[0] = box[1] = box[2] = box[3] = l[0]; }
    else if (n == 2) { box[0] = box[2] = l[0]; box[1] = box[3] = l[1]; }
    else if (n == 3) { box[0] = l[0]; box[1] = box[3] = l[1]; box[2] = l[2]; }
    else if (n == 4) { for (int k = 0; k < 4; k++) box[k] = l[k]; }
}

bool ParseBorderWidth(const std::wstring& t, float fs, float& w) {
    std::wstring s = ToLower(t);
    if (s == L"thin") { w = 1; return true; }
    if (s == L"medium") { w = 3; return true; }
    if (s == L"thick") { w = 5; return true; }
    Len l;
    if (ParseLen(s, fs, l) && l.u == Len::Px) { w = std::max(0.f, l.v); return true; }
    return false;
}

bool IsBorderStyle(const std::wstring& s) {
    return InList(s, {L"none", L"hidden", L"solid", L"dashed", L"dotted", L"double", L"groove", L"ridge", L"inset", L"outset"});
}

void ApplyBorder(Style& s, int side, const std::wstring& v) {
    float w = -1;
    uint32_t col = s.color;
    bool hasCol = false, none = false, hasStyle = false;
    for (auto& tok : SplitTop(v)) {
        std::wstring lt = ToLower(tok);
        if (IsBorderStyle(lt)) { hasStyle = true; if (lt == L"none" || lt == L"hidden") none = true; continue; }
        float bw;
        if (ParseBorderWidth(lt, s.fontSize, bw)) { w = bw; continue; }
        uint32_t c;
        if (ParseColor(tok, c)) { col = c; hasCol = true; }
    }
    if (!hasStyle && w < 0 && !hasCol) return;
    if (none) w = 0;
    else if (w < 0) w = hasStyle ? 3.f : 0.f;   // varsayilan kalinlik: medium
    if (!hasStyle && !none) w = 0;               // stil yazilmamissa kenarlik cizilmez
    int a = side < 0 ? 0 : side, b = side < 0 ? 3 : side;
    for (int k = a; k <= b; k++) { s.bw[k] = std::min(w, 50.f); s.bc[k] = col; }
}

void ApplyFontShorthand(Style& s, const Style& parent, const std::wstring& v);

void ApplyFontSize(Style& s, const Style& parent, const std::wstring& vIn) {
    std::wstring v = ToLower(Trim(vIn));
    float pf = parent.fontSize;
    static const struct { const wchar_t* k; float v; } kw[] = {
        {L"xx-small", 9}, {L"x-small", 10}, {L"small", 13}, {L"medium", 16}, {L"large", 18},
        {L"x-large", 24}, {L"xx-large", 32}, {L"xxx-large", 48}};
    for (auto& k : kw) if (v == k.k) { s.fontSize = k.v; return; }
    if (v == L"smaller") { s.fontSize = pf * 0.83f; return; }
    if (v == L"larger") { s.fontSize = pf * 1.2f; return; }
    if (v == L"inherit") { s.fontSize = pf; return; }
    Len l;
    if (!ParseLen(v, pf, l)) return;
    if (l.u == Len::Px) s.fontSize = l.v;
    else if (l.u == Len::Pct) s.fontSize = pf * l.v / 100.f;
    else if (l.u == Len::Calc) {
        float f = l.v + l.p * pf / 100.f;
        if (l.capKind == 1) f = std::min(f, l.cap);
        else if (l.capKind == 2) f = std::max(f, l.cap);
        s.fontSize = f;
    }
    s.fontSize = std::max(1.f, std::min(s.fontSize, 400.f));
}

uint32_t AvgColor(const std::vector<uint32_t>& cs) {
    if (cs.empty()) return 0;
    float a = 0, r = 0, g = 0, b = 0;
    for (uint32_t c : cs) {
        float al = float(c >> 24) / 255.f;
        a += al;
        r += float((c >> 16) & 255) * al; g += float((c >> 8) & 255) * al; b += float(c & 255) * al;
    }
    if (a <= 0.001f) return 0;
    uint32_t A = uint32_t(std::min(255.f, a / float(cs.size()) * 255.f + 0.5f));
    return (A << 24) | (uint32_t(r / a + 0.5f) << 16) | (uint32_t(g / a + 0.5f) << 8) | uint32_t(b / a + 0.5f);
}

// Degrade: renk duraklarinin ortalamasi (yaklasik arka plan rengi)
bool GradientColor(const std::wstring& tok, uint32_t& out) {
    size_t p = tok.find(L'(');
    if (p == std::wstring::npos || tok.back() != L')') return false;
    std::wstring inner = tok.substr(p + 1, tok.size() - p - 2);
    std::vector<uint32_t> cs;
    for (auto& part : SplitTop(inner, L',')) {
        for (auto& w : SplitTop(part)) { uint32_t c; if (ParseColor(w, c)) { cs.push_back(c); break; } }
    }
    if (cs.empty()) return false;
    out = AvgColor(cs);
    return true;
}

// linear-gradient(...) / radial-gradient(...) / repeating-*
std::shared_ptr<Gradient> ParseGradient(const std::wstring& tok0, float fs) {
    std::wstring tok = ToLower(Trim(tok0));
    size_t p = tok.find(L'(');
    if (p == std::wstring::npos || tok.back() != L')') return nullptr;
    std::wstring fn = tok.substr(0, p);
    if (StartsWith(fn, L"-webkit-") || StartsWith(fn, L"-moz-")) fn = fn.substr(fn.find(L'-', 1) + 1);
    auto g = std::make_shared<Gradient>();
    g->repeating = StartsWith(fn, L"repeating-");
    if (g->repeating) fn = fn.substr(10);
    if (fn == L"radial-gradient") g->radial = true;
    else if (fn != L"linear-gradient") return nullptr;
    auto parts = SplitTop(tok.substr(p + 1, tok.size() - p - 2), L',');
    size_t i = 0;
    if (!parts.empty()) {
        std::wstring f = Trim(parts[0]);
        uint32_t tc;
        auto ft = SplitTop(f);
        bool isColorFirst = !ft.empty() && ParseColor(ft[0], tc);
        if (!isColorFirst) {
            i = 1;
            if (g->radial && f.find(L"circle") != std::wstring::npos) g->circle = true;
            if (!g->radial) {
                if (StartsWith(f, L"to ")) {
                    int cx = 0, cy = 0;
                    for (auto& w : SplitTop(f.substr(3))) {
                        if (w == L"left") cx = -1; else if (w == L"right") cx = 1;
                        else if (w == L"top") cy = -1; else if (w == L"bottom") cy = 1;
                    }
                    if (cx && cy) { g->corner = true; g->cornerX = cx; g->cornerY = cy; }
                    else if (cx) g->angle = cx > 0 ? 90.f : 270.f;
                    else if (cy) g->angle = cy > 0 ? 180.f : 0.f;
                } else {
                    float n; std::wstring u;
                    if (ParseNumber(f, n, u)) {
                        if (u == L"deg") g->angle = n;
                        else if (u == L"turn") g->angle = n * 360.f;
                        else if (u == L"rad") g->angle = n * 57.2957795f;
                        else if (u == L"grad") g->angle = n * 0.9f;
                        else if (u.empty() && n == 0) g->angle = 0;
                    }
                }
            }
        }
    }
    for (; i < parts.size(); i++) {
        auto w = SplitTop(Trim(parts[i]));
        if (w.empty()) continue;
        uint32_t c;
        if (!ParseColor(w[0], c)) {
            // yalnizca konum (renk ipucu) ya da desteklenmeyen: atla
            continue;
        }
        GradStop st; st.color = c;
        for (size_t k = 1; k < w.size() && k < 3; k++) {
            Len l;
            if (!ParseLen(w[k], fs, l)) continue;
            GradStop x = st;
            if (l.u == Len::Pct) { x.unit = 1; x.pos = l.v / 100.f; }
            else if (l.u == Len::Px) { x.unit = 2; x.pos = l.v; }
            else continue;
            if (k == 1) st = x; else g->stops.push_back(st), st = x;
        }
        g->stops.push_back(st);
    }
    if (g->stops.empty()) return nullptr;
    if (g->stops.size() == 1) g->stops.push_back(g->stops[0]);
    return g;
}

// box-shadow: virgulle ayrilmis golgeler
std::shared_ptr<std::vector<BoxShadow>> ParseShadows(const std::wstring& v, float fs, uint32_t currentColor) {
    auto out = std::make_shared<std::vector<BoxShadow>>();
    for (auto& part : SplitTop(v, L',')) {
        BoxShadow b;
        b.color = currentColor;
        std::vector<float> nums;
        bool colorSet = false;
        for (auto& w : SplitTop(Trim(part))) {
            std::wstring l = ToLower(w);
            if (l == L"inset") { b.inset = true; continue; }
            uint32_t c;
            Len ln;
            if (ParseLen(l, fs, ln) && (ln.u == Len::Px)) { nums.push_back(ln.v); continue; }
            if (ParseColor(w, c)) { b.color = c; colorSet = true; continue; }
        }
        (void)colorSet;
        if (nums.size() < 2) continue;
        b.x = nums[0]; b.y = nums[1];
        if (nums.size() > 2) b.blur = std::max(0.f, nums[2]);
        if (nums.size() > 3) b.spread = nums[3];
        if (!(b.color >> 24)) continue;
        out->push_back(b);
    }
    return out;
}

std::wstring UrlInside(const std::wstring& tok) {
    size_t p = tok.find(L'(');
    size_t e = tok.rfind(L')');
    if (p == std::wstring::npos || e == std::wstring::npos || e <= p) return L"";
    std::wstring u = Trim(tok.substr(p + 1, e - p - 1));
    if (!u.empty() && (u[0] == L'"' || u[0] == L'\'')) {
        wchar_t q = u[0];
        std::wstring o;
        for (size_t i = 1; i < u.size() && u[i] != q; i++) { if (u[i] == L'\\' && i + 1 < u.size()) i++; o += u[i]; }
        u = o;
    }
    return u;
}

void ParseBgPosition(const std::vector<std::wstring>& toks, float fs, BgImage& b) {
    std::vector<Len> v;
    bool xSet = false, ySet = false;
    for (auto& t : toks) {
        std::wstring l = ToLower(t);
        if (l == L"left") { b.px = {0, Len::Pct}; xSet = true; }
        else if (l == L"right") { b.px = {100, Len::Pct}; xSet = true; }
        else if (l == L"top") { b.py = {0, Len::Pct}; ySet = true; }
        else if (l == L"bottom") { b.py = {100, Len::Pct}; ySet = true; }
        else if (l == L"center") v.push_back({50, Len::Pct});
        else { Len x; if (ParseLen(l, fs, x)) v.push_back(x); }
    }
    size_t k = 0;
    if (!xSet && k < v.size()) { b.px = v[k++]; xSet = true; }
    if (!ySet && k < v.size()) { b.py = v[k++]; ySet = true; }
    if (!xSet) b.px = {50, Len::Pct};
    if (!ySet) b.py = {50, Len::Pct};
}

void ParseBgSize(const std::wstring& v, float fs, BgImage& b) {
    std::wstring l = ToLower(Trim(v));
    if (l == L"cover") { b.size = 1; return; }
    if (l == L"contain") { b.size = 2; return; }
    auto t = SplitTop(l);
    if (t.empty()) return;
    Len a{0, Len::Auto}, c{0, Len::Auto};
    if (!ParseLen(t[0], fs, a)) return;
    if (t.size() > 1) ParseLen(t[1], fs, c);
    if (a.u == Len::Auto && c.u == Len::Auto) { b.size = 0; return; }
    b.size = 3;
    b.sw = a;
    b.sh = c;
}

uint8_t ParseRepeat(const std::wstring& l) {
    if (l == L"no-repeat") return 1;
    if (l == L"repeat-x") return 2;
    if (l == L"repeat-y") return 3;
    if (l == L"space" || l == L"round" || l == L"repeat") return 0;
    return 255;
}

// background / mask kisaltmasi: renk, resim, konum / boyut, tekrar
void ApplyBackgroundLike(Style& s, const std::wstring& raw, bool mask) {
    auto layers = SplitTop(raw, L',');
    std::shared_ptr<BgImage> img;
    std::shared_ptr<Gradient> grad;
    uint32_t color = 0;
    bool hasColor = false, gradient = false;
    uint32_t gradColor = 0;
    for (size_t li = 0; li < layers.size(); li++) {
        auto toks = SplitTop(layers[li]);
        std::vector<std::wstring> pos;
        std::wstring size;
        bool afterSlash = false;
        BgImage b;
        bool hasImg = false;
        for (size_t i = 0; i < toks.size(); i++) {
            std::wstring t = toks[i];
            std::wstring l = ToLower(t);
            if (StartsWith(l, L"url(")) { b.url = UrlInside(t); hasImg = !b.url.empty(); continue; }
            if (l.find(L"gradient(") != std::wstring::npos) {
                uint32_t g;
                if (GradientColor(l, g)) { gradient = true; gradColor = g; }
                if (!grad && !mask) grad = ParseGradient(t, s.fontSize);
                continue;
            }
            if (l.find(L"image-set(") != std::wstring::npos) {
                size_t u = l.find(L"url(");
                if (u != std::wstring::npos) { b.url = UrlInside(t.substr(u)); hasImg = !b.url.empty(); }
                else {
                    // image-set("a.png" 1x, ...)
                    size_t q = t.find_first_of(L"\"'");
                    if (q != std::wstring::npos) { size_t e = t.find(t[q], q + 1); if (e != std::wstring::npos) { b.url = t.substr(q + 1, e - q - 1); hasImg = true; } }
                }
                continue;
            }
            if (t == L"/") { afterSlash = true; continue; }
            size_t sl = t.find(L'/');
            if (sl != std::wstring::npos && t.find(L'(') == std::wstring::npos) {
                std::wstring a = t.substr(0, sl), c = t.substr(sl + 1);
                if (!a.empty()) pos.push_back(a);
                afterSlash = true;
                if (!c.empty()) size += c + L" ";
                continue;
            }
            uint8_t rp = ParseRepeat(l);
            if (rp != 255) { b.repeat = rp; continue; }
            if (afterSlash) { size += t + L" "; continue; }
            if (l == L"left" || l == L"right" || l == L"top" || l == L"bottom" || l == L"center") { pos.push_back(l); continue; }
            uint32_t c;
            if (!mask && ParseColor(t, c)) { color = c; hasColor = true; continue; }
            Len ln;
            if (ParseLen(l, s.fontSize, ln) && ln.u != Len::Auto && ln.u != Len::None) { pos.push_back(l); continue; }
        }
        if (!pos.empty()) ParseBgPosition(pos, s.fontSize, b);
        if (!size.empty()) ParseBgSize(size, s.fontSize, b);
        if (hasImg && !img) img = std::make_shared<BgImage>(b);
    }
    if (mask) {
        if (img) s.mask = img;
        else if (ToLower(Trim(raw)) == L"none") s.mask.reset();
        return;
    }
    s.bgImage = img;
    s.grad = grad;
    if (hasColor) s.bg = color;
    else if (gradient && !grad) s.bg = gradColor;   // ayristirilamayan degrade: ortalama renk
    else s.bg = 0;
}

std::shared_ptr<BgImage> MutBg(std::shared_ptr<const BgImage>& p) {
    auto m = std::make_shared<BgImage>(p ? *p : BgImage());
    p = m;
    return m;
}

// ---------------- Grid ----------------

bool ParseTrackSize(const std::wstring& t, float fs, TrackSize& out) {
    std::wstring l = ToLower(Trim(t));
    if (l == L"auto") { out = {3, 0}; return true; }
    if (l == L"min-content") { out = {4, 0}; return true; }
    if (l == L"max-content") { out = {5, 0}; return true; }
    if (EndsWith(l, L"fr")) {
        float f; std::wstring u;
        if (ParseNumber(l, f, u) && u == L"fr") { out = {2, std::max(0.f, f)}; return true; }
        return false;
    }
    Len len;
    if (!ParseLen(l, fs, len)) return false;
    if (len.u == Len::Px) { out = {0, len.v}; return true; }
    if (len.u == Len::Pct) { out = {1, len.v}; return true; }
    if (len.u == Len::Calc) { out = {0, len.v}; return true; }
    if (len.u == Len::Fit) { out = {5, 0}; return true; }
    if (len.u == Len::Auto) { out = {3, 0}; return true; }
    return false;
}

bool ParseTrack(const std::wstring& t, float fs, Track& out) {
    std::wstring l = ToLower(Trim(t));
    if (StartsWith(l, L"minmax(") && l.back() == L')') {
        auto args = SplitTop(l.substr(7, l.size() - 8), L',');
        if (args.size() != 2) return false;
        if (!ParseTrackSize(args[0], fs, out.mn) || !ParseTrackSize(args[1], fs, out.mx)) return false;
        if (out.mn.k == 2) out.mn = {3, 0};
        return true;
    }
    if (StartsWith(l, L"fit-content(") && l.back() == L')') {
        TrackSize lim;
        if (!ParseTrackSize(l.substr(12, l.size() - 13), fs, lim)) return false;
        out.mn = {3, 0};
        out.mx = lim.k == 0 ? TrackSize{0, lim.v} : TrackSize{5, 0};
        return true;
    }
    TrackSize ts;
    if (!ParseTrackSize(l, fs, ts)) return false;
    out.mn = ts.k == 2 ? TrackSize{3, 0} : ts;
    out.mx = ts;
    return true;
}

// "200px repeat(3, 1fr) [isim] minmax(0, 1fr)"
bool ParseTrackList(const std::wstring& v, float fs, std::vector<Track>& out, GridSpec* autoRep) {
    out.clear();
    std::wstring l = ToLower(Trim(v));
    if (l == L"none" || l.empty()) return true;
    for (auto& tok : SplitTop(l)) {
        if (tok[0] == L'[') continue;           // adlandirilmis cizgiler
        if (tok.find(L']') != std::wstring::npos) continue;
        if (StartsWith(tok, L"repeat(") && tok.back() == L')') {
            auto args = SplitTop(tok.substr(7, tok.size() - 8), L',');
            if (args.size() < 2) return false;
            std::vector<Track> inner;
            std::wstring rest;
            for (size_t i = 1; i < args.size(); i++) { if (i > 1) rest += L','; rest += args[i]; }
            if (!ParseTrackList(rest, fs, inner, nullptr) || inner.empty()) return false;
            std::wstring cnt = Trim(args[0]);
            if (cnt == L"auto-fill" || cnt == L"auto-fit") {
                if (!autoRep) return false;
                autoRep->autoRepeat = inner;
                autoRep->autoRepeatAt = int(out.size());
                autoRep->autoFit = (cnt == L"auto-fit");
                continue;
            }
            int n = std::max(1, std::min(_wtoi(cnt.c_str()), 100));
            for (int k = 0; k < n && out.size() < 200; k++) out.insert(out.end(), inner.begin(), inner.end());
            continue;
        }
        Track t;
        if (!ParseTrack(tok, fs, t)) return false;
        if (out.size() < 200) out.push_back(t);
    }
    return true;
}

std::vector<std::vector<std::wstring>> ParseAreas(const std::wstring& v) {
    std::vector<std::vector<std::wstring>> rows;
    size_t i = 0;
    while (i < v.size()) {
        if (v[i] == L'"' || v[i] == L'\'') {
            wchar_t q = v[i];
            size_t e = v.find(q, i + 1);
            if (e == std::wstring::npos) break;
            auto names = SplitWS(ToLower(v.substr(i + 1, e - i - 1)));
            if (!names.empty() && rows.size() < 100) rows.push_back(names);
            i = e + 1;
        } else i++;
    }
    return rows;
}

std::shared_ptr<GridSpec> MutGrid(Style& s) {
    auto g = std::make_shared<GridSpec>(s.grid ? *s.grid : GridSpec());
    s.grid = g;
    return g;
}

// "1 / 3", "span 2", "2 / span 3", "1 / -1", "isim"
void ParseGridLine(const std::wstring& v, int16_t& start, int16_t& end, int16_t& span, std::wstring* areaName) {
    std::wstring l = ToLower(Trim(v));
    auto parts = SplitTop(l, L'/');
    auto parseOne = [](const std::wstring& p, int16_t& line, int16_t& sp) -> bool {
        auto t = SplitWS(p);
        if (t.empty() || t[0] == L"auto") return false;
        if (t[0] == L"span") { if (t.size() > 1) sp = int16_t(std::max(1, std::min(_wtoi(t[1].c_str()), 100))); return true; }
        if (iswdigit(t[0][0]) || t[0][0] == L'-') { line = int16_t(std::max(-100, std::min(_wtoi(t[0].c_str()), 100))); return true; }
        return false;
    };
    start = end = 0;
    span = 1;
    if (parts.empty()) return;
    int16_t sp1 = 1, sp2 = 1;
    bool a = parseOne(parts[0], start, sp1);
    if (!a && areaName && !parts[0].empty() && parts[0] != L"auto" && iswalpha(parts[0][0])) *areaName = Trim(parts[0]);
    if (parts.size() > 1) {
        int16_t e = 0;
        if (parseOne(parts[1], e, sp2)) {
            if (e != 0) end = e;
            else span = sp2;
        }
    }
    if (sp1 > 1) span = sp1;
}

// ---------------- Bildirim uygulama ----------------

void ApplyDecl(Style& s, const Style& parent, const std::wstring& prop, const std::wstring& raw) {
    std::wstring val = raw;
    if (val.find(L"var(") != std::wstring::npos) val = SubstVars(val, s.vars.get());
    std::wstring v = ToLower(Trim(val));
    if (v.empty()) return;
    const float fs = s.fontSize;
    const bool inherit = (v == L"inherit");
    if (v == L"initial" || v == L"unset" || v == L"revert" || v == L"revert-layer") {
        // basit yaklasim: kalitimla gecenlerde ebeveyn, digerlerinde atla
        if (prop == L"color") s.color = parent.color;
        return;
    }

    if (prop == L"color") {
        if (inherit || v == L"currentcolor") { s.color = parent.color; return; }
        uint32_t c; if (ParseColor(v, c)) s.color = c;
    } else if (prop == L"background-color") {
        uint32_t c;
        if (v == L"currentcolor") s.bg = s.color;
        else if (ParseColor(v, c)) s.bg = c;
        else if (inherit) s.bg = parent.bg;
    } else if (prop == L"background") {
        if (inherit) { s.bg = parent.bg; s.bgImage = parent.bgImage; return; }
        ApplyBackgroundLike(s, val, false);
    } else if (prop == L"background-image") {
        if (v == L"none") { s.bgImage.reset(); s.grad.reset(); return; }
        if (v.find(L"gradient(") != std::wstring::npos && v.find(L"url(") == std::wstring::npos) {
            for (auto& layer : SplitTop(val, L',')) {
                if (ToLower(layer).find(L"gradient(") == std::wstring::npos) continue;
                s.grad = ParseGradient(layer, fs);
                break;
            }
            uint32_t g;
            if (!s.grad && !(s.bg >> 24) && GradientColor(v, g)) s.bg = g;
            return;
        }
        size_t u = v.find(L"url(");
        if (u != std::wstring::npos) {
            auto b = MutBg(s.bgImage);
            b->url = UrlInside(val.substr(u));
            if (b->url.empty()) s.bgImage.reset();
        }
    } else if (prop == L"background-size" || prop == L"mask-size" || prop == L"-webkit-mask-size") {
        bool mk = prop != L"background-size";
        auto& tgt = mk ? s.mask : s.bgImage;
        if (!tgt) return;
        ParseBgSize(SplitTop(v, L',')[0], fs, *MutBg(tgt));
    } else if (prop == L"background-position" || prop == L"mask-position" || prop == L"-webkit-mask-position") {
        bool mk = prop != L"background-position";
        auto& tgt = mk ? s.mask : s.bgImage;
        if (!tgt) return;
        ParseBgPosition(SplitTop(SplitTop(v, L',')[0]), fs, *MutBg(tgt));
    } else if (prop == L"background-repeat" || prop == L"mask-repeat" || prop == L"-webkit-mask-repeat") {
        bool mk = prop != L"background-repeat";
        auto& tgt = mk ? s.mask : s.bgImage;
        if (!tgt) return;
        auto t = SplitTop(SplitTop(v, L',')[0]);
        uint8_t r = t.empty() ? 0 : ParseRepeat(t[0]);
        if (t.size() == 2) r = (t[0] == L"no-repeat" && t[1] == L"no-repeat") ? 1 : (t[0] == L"repeat" && t[1] == L"no-repeat") ? 2 : (t[0] == L"no-repeat" && t[1] == L"repeat") ? 3 : 0;
        if (r != 255) MutBg(tgt)->repeat = r;
    } else if (prop == L"mask-image" || prop == L"-webkit-mask-image" || prop == L"mask" || prop == L"-webkit-mask") {
        if (v == L"none") { s.mask.reset(); return; }
        if (prop == L"mask" || prop == L"-webkit-mask") { ApplyBackgroundLike(s, val, true); return; }
        size_t u = v.find(L"url(");
        if (u != std::wstring::npos) {
            auto b = MutBg(s.mask);
            b->url = UrlInside(val.substr(u));
        } else if (v.find(L"gradient(") != std::wstring::npos) {
            s.mask.reset(); // degrade maskesi: yok say (icerik gorunur kalsin)
        }
    } else if (prop == L"font-size") {
        ApplyFontSize(s, parent, v);
    } else if (prop == L"font-weight") {
        if (v == L"bold" || v == L"bolder") s.weight = 700;
        else if (v == L"normal") s.weight = 400;
        else if (v == L"lighter") s.weight = 300;
        else if (inherit) s.weight = parent.weight;
        else { int w = _wtoi(v.c_str()); if (w >= 100 && w <= 1000) s.weight = w; }
    } else if (prop == L"font-style") {
        s.italic = (v == L"italic" || StartsWith(v, L"oblique"));
    } else if (prop == L"font-family") {
        s.family = v;
    } else if (prop == L"font") {
        ApplyFontShorthand(s, parent, v);
    } else if (prop == L"text-align") {
        if (v == L"center" || v == L"-webkit-center" || v == L"-moz-center") s.align = 1;
        else if (v == L"right" || v == L"end" || v == L"-webkit-right") s.align = 2;
        else if (inherit) s.align = parent.align;
        else s.align = 0;
    } else if (prop == L"text-decoration" || prop == L"text-decoration-line") {
        s.underline = v.find(L"underline") != std::wstring::npos;
        s.strike = v.find(L"line-through") != std::wstring::npos;
    } else if (prop == L"box-shadow" || prop == L"-webkit-box-shadow") {
        if (v == L"none" || v.empty()) s.shadows.reset();
        else { auto sh = ParseShadows(val, fs, s.color); if (sh->empty()) s.shadows.reset(); else s.shadows = sh; }
    } else if (prop == L"text-overflow") {
        s.ellipsis = v.find(L"ellipsis") != std::wstring::npos;
    } else if (prop == L"-webkit-line-clamp" || prop == L"line-clamp") {
        int k = _wtoi(v.c_str());
        s.lineClamp = uint8_t(std::max(0, std::min(k, 255)));
    } else if (prop == L"text-transform") {
        s.textTransform = v == L"uppercase" ? 1 : v == L"lowercase" ? 2 : v == L"capitalize" ? 3 : 0;
    } else if (prop == L"text-indent") {
        Len l;
        if (ParseLen(v, fs, l) && ((l.u == Len::Px && l.v <= -999) || (l.u == Len::Pct && l.v <= -100))) s.textHidden = true;
        else if (l.u == Len::Px && l.v > -999) s.textHidden = false;
    } else if (prop == L"display") {
        s.flex = 0;
        s.gridCols = 0;
        s.inlineBlock = false;
        s.webkitBox = false;
        if (v == L"flex" || v == L"-webkit-box" || v == L"-webkit-flex" || v == L"-ms-flexbox" || v == L"-moz-box") {
            s.display = Style::Block; s.flex = s.flexDirCol ? 2 : 1; s.webkitBox = (v == L"-webkit-box"); return;
        }
        if (v == L"inline-flex" || v == L"-webkit-inline-box" || v == L"-webkit-inline-flex" || v == L"-ms-inline-flexbox") {
            s.display = Style::Inline; s.inlineBlock = true; s.flex = s.flexDirCol ? 2 : 1; return;
        }
        if (v == L"grid" || v == L"-ms-grid") { s.display = Style::Block; s.gridCols = 1; return; }
        if (v == L"inline-grid" || v == L"-ms-inline-grid") { s.display = Style::Inline; s.inlineBlock = true; s.gridCols = 1; return; }
        if (v == L"none") s.display = Style::None;
        else if (v == L"contents") s.display = Style::Contents;
        else if (v == L"inline-block" || v == L"-moz-inline-stack") { s.display = Style::Inline; s.inlineBlock = true; }
        else if (v == L"inline" || v == L"ruby" || v == L"ruby-text") s.display = Style::Inline;
        else if (v == L"inline-table") { s.display = Style::Table; s.inlineBlock = true; }
        else if (v == L"list-item") s.display = Style::ListItem;
        else if (v == L"table") s.display = Style::Table;
        else if (v == L"table-row") s.display = Style::Row;
        else if (v == L"table-cell") s.display = Style::Cell;
        else if (v == L"table-column" || v == L"table-column-group") s.display = Style::None;
        else if (v == L"flow-root") { s.display = Style::Block; s.bfc = true; }
        else if (v == L"inherit") s.display = parent.display;
        else s.display = Style::Block; // block, table-row-group, table-caption ...
    } else if (prop == L"margin") {
        SetBoxSides(s.m, v, fs);
    } else if (prop == L"padding") {
        SetBoxSides(s.p, v, fs);
    } else if (StartsWith(prop, L"margin-") || StartsWith(prop, L"padding-")) {
        bool mg = prop[0] == L'm';
        std::wstring side = prop.substr(mg ? 7 : 8);
        int k = side == L"top" ? 0 : side == L"right" ? 1 : side == L"bottom" ? 2 : side == L"left" ? 3 : -1;
        if (side == L"inline-start") k = 3; else if (side == L"inline-end") k = 1;
        else if (side == L"block-start") k = 0; else if (side == L"block-end") k = 2;
        Len l;
        if (k >= 0 && ParseLen(v, fs, l)) (mg ? s.m : s.p)[k] = l;
        if (side == L"inline" || side == L"block") {
            auto t = SplitTop(v);
            Len a, b;
            if (!t.empty() && ParseLen(t[0], fs, a)) {
                b = a;
                if (t.size() > 1) ParseLen(t[1], fs, b);
                Len* box = mg ? s.m : s.p;
                if (side == L"inline") { box[3] = a; box[1] = b; } else { box[0] = a; box[2] = b; }
            }
        }
    } else if (prop == L"width" || prop == L"inline-size") {
        Len l; if (ParseLen(v, fs, l)) s.width = l;
    } else if (prop == L"max-width" || prop == L"max-inline-size") {
        Len l; if (ParseLen(v, fs, l)) s.maxWidth = l;
    } else if (prop == L"min-width" || prop == L"min-inline-size") {
        Len l; if (ParseLen(v, fs, l)) s.minWidth = l;
    } else if (prop == L"height" || prop == L"block-size") {
        Len l; if (ParseLen(v, fs, l)) s.height = l;
    } else if (prop == L"min-height" || prop == L"min-block-size") {
        Len l; if (ParseLen(v, fs, l)) s.minHeight = l;
    } else if (prop == L"max-height" || prop == L"max-block-size") {
        Len l; if (ParseLen(v, fs, l)) s.maxHeight = l;
    } else if (prop == L"aspect-ratio") {
        std::wstring t = v;
        if (StartsWith(t, L"auto")) t = Trim(t.substr(4));
        size_t sl = t.find(L'/');
        float a = float(_wtof(t.c_str()));
        float b = sl == std::wstring::npos ? 1.f : float(_wtof(t.c_str() + sl + 1));
        s.aspect = (a > 0 && b > 0) ? a / b : 0;
    } else if (prop == L"border") {
        ApplyBorder(s, -1, val);
    } else if (prop == L"border-top" || prop == L"border-right" || prop == L"border-bottom" || prop == L"border-left" ||
               prop == L"border-block-start" || prop == L"border-block-end" || prop == L"border-inline-start" || prop == L"border-inline-end") {
        int k = (prop == L"border-top" || prop == L"border-block-start") ? 0 : (prop == L"border-right" || prop == L"border-inline-end") ? 1 :
                (prop == L"border-bottom" || prop == L"border-block-end") ? 2 : 3;
        ApplyBorder(s, k, val);
    } else if (prop == L"border-block" || prop == L"border-inline") {
        ApplyBorder(s, prop == L"border-block" ? 0 : 1, val);
        ApplyBorder(s, prop == L"border-block" ? 2 : 3, val);
    } else if (prop == L"border-width") {
        auto t = SplitTop(v);
        float w[4]; int n = 0;
        for (auto& tok : t) { if (n < 4 && ParseBorderWidth(tok, fs, w[n])) n++; }
        if (n == 1) for (int k = 0; k < 4; k++) s.bw[k] = w[0];
        else if (n == 2) { s.bw[0] = s.bw[2] = w[0]; s.bw[1] = s.bw[3] = w[1]; }
        else if (n >= 3) { s.bw[0] = w[0]; s.bw[1] = s.bw[3] = w[1]; s.bw[2] = w[2]; if (n == 4) s.bw[3] = w[3]; }
    } else if (prop == L"border-color") {
        auto t = SplitTop(val);
        uint32_t c[4]; int n = 0;
        for (auto& tok : t) { if (n < 4 && ParseColor(tok, c[n])) n++; }
        if (n == 1) for (int k = 0; k < 4; k++) s.bc[k] = c[0];
        else if (n == 2) { s.bc[0] = s.bc[2] = c[0]; s.bc[1] = s.bc[3] = c[1]; }
        else if (n >= 3) { s.bc[0] = c[0]; s.bc[1] = s.bc[3] = c[1]; s.bc[2] = c[2]; if (n == 4) s.bc[3] = c[3]; }
    } else if (prop == L"border-style") {
        auto t = SplitTop(v);
        if (t.size() == 1 && (v == L"none" || v == L"hidden")) for (int k = 0; k < 4; k++) s.bw[k] = 0;
    } else if (StartsWith(prop, L"border-") && EndsWith(prop, L"-radius")) {
        if (prop == L"border-radius") {
            auto t = SplitTop(v.substr(0, v.find(L'/')));
            if (t.empty()) return;
            Len l;
            if (ParseLen(t[0], fs, l)) {
                if (l.u == Len::Pct) { s.radiusPct = l.v >= 50; s.radius = 0; }
                else if (l.u == Len::Px) { s.radius = std::max(0.f, l.v); s.radiusPct = false; }
            }
        }
    } else if (StartsWith(prop, L"border-") && (EndsWith(prop, L"-color") || EndsWith(prop, L"-width") || EndsWith(prop, L"-style"))) {
        std::wstring side = prop.substr(7, prop.find(L'-', 7) - 7);
        int k = side == L"top" ? 0 : side == L"right" ? 1 : side == L"bottom" ? 2 : side == L"left" ? 3 : -1;
        if (prop.find(L"inline-start") != std::wstring::npos) k = 3;
        else if (prop.find(L"inline-end") != std::wstring::npos) k = 1;
        else if (prop.find(L"block-start") != std::wstring::npos) k = 0;
        else if (prop.find(L"block-end") != std::wstring::npos) k = 2;
        if (k < 0) return;
        if (EndsWith(prop, L"-color")) { uint32_t c; if (ParseColor(v, c)) s.bc[k] = c; }
        else if (EndsWith(prop, L"-width")) { float w; if (ParseBorderWidth(v, fs, w)) s.bw[k] = w; }
        else if (v == L"none" || v == L"hidden") s.bw[k] = 0;
    } else if (prop == L"border-collapse") {
        s.collapse = (v == L"collapse");
        if (s.collapse) s.cellSpacing = 0;
    } else if (prop == L"border-spacing") {
        Len l; auto t = SplitTop(v);
        if (!t.empty() && ParseLen(t[0], fs, l) && l.u == Len::Px) s.cellSpacing = l.v;
    } else if (prop == L"box-sizing") {
        s.borderBox = (v == L"border-box");
    } else if (prop == L"white-space" || prop == L"white-space-collapse" || prop == L"text-wrap-mode") {
        if (prop == L"text-wrap-mode") { s.nowrap = (v == L"nowrap"); return; }
        s.pre = (v == L"pre" || v == L"pre-wrap" || v == L"break-spaces" || v == L"pre-line" || v == L"preserve");
        s.nowrap = (v == L"nowrap" || v == L"pre");
    } else if (prop == L"line-height") {
        if (v == L"normal") { s.lineHeight = 1.3f; s.lineHeightPx = 0; return; }
        if (inherit) { s.lineHeight = parent.lineHeight; s.lineHeightPx = parent.lineHeightPx; return; }
        float f; std::wstring u;
        if (ParseNumber(v, f, u) && u.empty()) { s.lineHeight = f; s.lineHeightPx = 0; return; }
        if (ParseNumber(v, f, u) && u == L"%") { s.lineHeightPx = 0; s.lineHeight = f / 100.f; return; }
        Len l;
        if (ParseLen(v, fs, l) && l.u == Len::Px) s.lineHeightPx = l.v;
    } else if (prop == L"list-style-type" || prop == L"list-style") {
        if (v.find(L"none") != std::wstring::npos) s.listType = 2;
        else if (v.find(L"decimal") != std::wstring::npos || v.find(L"roman") != std::wstring::npos || v.find(L"alpha") != std::wstring::npos || v.find(L"latin") != std::wstring::npos) s.listType = 1;
        else if (v.find(L"circle") != std::wstring::npos) s.listType = 3;
        else if (v.find(L"square") != std::wstring::npos) s.listType = 4;
        else if (v.find(L"disc") != std::wstring::npos) s.listType = 0;
    } else if (prop == L"vertical-align") {
        s.valign = (v == L"top" || v == L"baseline" || v == L"text-top") ? 0 : v == L"bottom" ? 2 : 1;
        s.vAlign = v == L"middle" ? 1 : v == L"top" ? 2 : v == L"bottom" ? 3 : v == L"text-top" ? 4 : v == L"text-bottom" ? 5 :
                   v == L"sub" ? 6 : v == L"super" ? 7 : 0;
    } else if (prop == L"position") {
        s.position = v == L"relative" ? 1 : v == L"absolute" ? 2 : v == L"fixed" ? 3 : (v == L"sticky" || v == L"-webkit-sticky") ? 4 : 0;
    } else if (prop == L"top" || prop == L"right" || prop == L"bottom" || prop == L"left" ||
               prop == L"inset-block-start" || prop == L"inset-block-end" || prop == L"inset-inline-start" || prop == L"inset-inline-end") {
        int k = (prop == L"top" || prop == L"inset-block-start") ? 0 : (prop == L"right" || prop == L"inset-inline-end") ? 1 :
                (prop == L"bottom" || prop == L"inset-block-end") ? 2 : 3;
        Len l;
        if (ParseLen(v, fs, l)) s.inset[k] = l;
    } else if (prop == L"inset" || prop == L"inset-block" || prop == L"inset-inline") {
        Len box[4] = {s.inset[0], s.inset[1], s.inset[2], s.inset[3]};
        if (prop == L"inset") SetBoxSides(box, v, fs);
        else {
            auto t = SplitTop(v);
            Len a;
            if (t.empty() || !ParseLen(t[0], fs, a)) return;
            Len b = a;
            if (t.size() > 1) ParseLen(t[1], fs, b);
            if (prop == L"inset-block") { box[0] = a; box[2] = b; } else { box[3] = a; box[1] = b; }
        }
        for (int k = 0; k < 4; k++) s.inset[k] = box[k];
    } else if (prop == L"transform") {
        s.tx = {0, Len::Px}; s.ty = {0, Len::Px}; s.transformHidden = false;
        if (v == L"none") return;
        size_t i = 0;
        while (i < v.size()) {
            size_t p = v.find(L'(', i);
            if (p == std::wstring::npos) break;
            size_t st = p;
            while (st > i && (iswalpha(v[st - 1]) || v[st - 1] == L'3')) st--;
            std::wstring fn = v.substr(st, p - st);
            size_t e = v.find(L')', p);
            if (e == std::wstring::npos) break;
            auto args = SplitTop(v.substr(p + 1, e - p - 1), L',');
            if (args.size() == 1 && args[0].find(L' ') != std::wstring::npos && args[0].find(L'(') == std::wstring::npos) args = SplitTop(args[0]);
            Len a{0, Len::Px}, b{0, Len::Px};
            if (fn == L"translate" || fn == L"translate3d") {
                if (!args.empty()) ParseLen(args[0], fs, a);
                if (args.size() > 1) ParseLen(args[1], fs, b);
                s.tx = a; s.ty = b;
            } else if (fn == L"translatex") { if (!args.empty() && ParseLen(args[0], fs, a)) s.tx = a; }
            else if (fn == L"translatey") { if (!args.empty() && ParseLen(args[0], fs, a)) s.ty = a; }
            else if (fn == L"scale" || fn == L"scalex" || fn == L"scaley" || fn == L"scale3d") {
                for (auto& x : args) if (std::fabs(_wtof(x.c_str())) < 0.001 && !x.empty() && iswdigit(Trim(x)[0])) s.transformHidden = true;
            }
            i = e + 1;
        }
    } else if (prop == L"translate") {
        auto t = SplitTop(v);
        Len a{0, Len::Px}, b{0, Len::Px};
        if (!t.empty()) ParseLen(t[0], fs, a);
        if (t.size() > 1) ParseLen(t[1], fs, b);
        s.tx = a; s.ty = b;
    } else if (prop == L"overflow" || prop == L"overflow-y" || prop == L"overflow-x" || prop == L"overflow-block") {
        auto t = SplitTop(v);
        std::wstring a = t.empty() ? v : t[0];
        if (prop == L"overflow" || prop == L"overflow-y" || prop == L"overflow-block") s.overflowHidden = (a == L"hidden" || a == L"clip");
        if (a != L"visible" && a != L"clip") s.bfc = true;
    } else if (prop == L"clip" || prop == L"clip-path") {
        if (v.find(L"rect(0") != std::wstring::npos || v.find(L"inset(50%") != std::wstring::npos || v.find(L"rect(1px") != std::wstring::npos ||
            v.find(L"inset(100%") != std::wstring::npos || v.find(L"circle(0") != std::wstring::npos || v.find(L"polygon(0 0, 0 0") != std::wstring::npos)
            s.hiddenClip = true;
    } else if (prop == L"opacity") {
        float f; std::wstring u;
        if (ParseNumber(v, f, u)) s.opacity = std::max(0.f, std::min(u == L"%" ? f / 100.f : f, 1.f));
    } else if (prop == L"visibility") {
        if (v == L"hidden" || v == L"collapse") s.hidden = true;
        else if (v == L"visible") s.hidden = false;
        else if (inherit) s.hidden = parent.hidden;
    } else if (prop == L"flex-direction" || prop == L"-webkit-box-orient") {
        s.flexDirCol = (v == L"column" || v == L"column-reverse" || v == L"vertical");
        s.flexReverse = (v == L"row-reverse" || v == L"column-reverse");
        if (s.flex) s.flex = s.flexDirCol ? 2 : 1;
    } else if (prop == L"flex-flow") {
        s.flexDirCol = v.find(L"column") != std::wstring::npos;
        s.flexReverse = v.find(L"reverse") != std::wstring::npos && v.find(L"wrap-reverse") == std::wstring::npos;
        if (s.flex) s.flex = s.flexDirCol ? 2 : 1;
        s.flexWrap = v.find(L"wrap") != std::wstring::npos && v.find(L"nowrap") == std::wstring::npos;
    } else if (prop == L"flex-wrap") {
        s.flexWrap = (v == L"wrap" || v == L"wrap-reverse");
    } else if (prop == L"justify-content" || prop == L"-webkit-box-pack") {
        if (StartsWith(v, L"safe ") || StartsWith(v, L"unsafe ")) v = v.substr(v.find(L' ') + 1);
        s.justify = (v == L"center") ? 1 : (v == L"flex-end" || v == L"end" || v == L"right") ? 2 :
                    (v == L"space-between" || v == L"justify") ? 3 : (v == L"space-around") ? 4 : (v == L"space-evenly") ? 5 : 0;
    } else if (prop == L"align-items" || prop == L"-webkit-box-align") {
        if (StartsWith(v, L"safe ") || StartsWith(v, L"unsafe ")) v = v.substr(v.find(L' ') + 1);
        s.alignItems = (v == L"center") ? 1 : (v == L"flex-end" || v == L"end" || v == L"self-end") ? 2 :
                       (v == L"flex-start" || v == L"start" || v == L"self-start") ? 3 : (v == L"baseline" || v == L"first baseline" || v == L"last baseline") ? 4 : 0;
    } else if (prop == L"align-self") {
        s.alignSelf = (v == L"auto") ? 0 : (v == L"center") ? 2 : (v == L"flex-end" || v == L"end" || v == L"self-end") ? 3 :
                      (v == L"flex-start" || v == L"start" || v == L"self-start") ? 4 : (v == L"baseline") ? 5 : 1;
    } else if (prop == L"justify-items") {
        s.justifyItems = v == L"center" ? 1 : (v == L"end" || v == L"right" || v == L"flex-end") ? 2 : (v == L"start" || v == L"left" || v == L"flex-start") ? 3 : 0;
    } else if (prop == L"place-items") {
        auto t = SplitTop(v);
        if (t.empty()) return;
        ApplyDecl(s, parent, L"align-items", t[0]);
        ApplyDecl(s, parent, L"justify-items", t.size() > 1 ? t[1] : t[0]);
    } else if (prop == L"place-content") {
        auto t = SplitTop(v);
        if (t.empty()) return;
        ApplyDecl(s, parent, L"justify-content", t.size() > 1 ? t[1] : t[0]);
    } else if (prop == L"gap" || prop == L"grid-gap" || prop == L"row-gap" || prop == L"column-gap" || prop == L"grid-column-gap" || prop == L"grid-row-gap") {
        auto t = SplitTop(v);
        Len a, b;
        if (t.empty() || !ParseLen(t[0], fs, a)) return;
        b = a;
        if (t.size() > 1) ParseLen(t[1], fs, b);
        float av = a.u == Len::Px ? a.v : 0, bv = b.u == Len::Px ? b.v : 0;
        if (prop == L"row-gap" || prop == L"grid-row-gap") s.gapRow = av;
        else if (prop == L"column-gap" || prop == L"grid-column-gap") s.gapCol = av;
        else { s.gapRow = av; s.gapCol = bv; }
    } else if (prop == L"flex" || prop == L"-webkit-flex" || prop == L"-ms-flex") {
        auto t = SplitTop(v);
        if (v == L"none") { s.flexGrow = 0; s.flexShrink = 0; s.flexBasis = {0, Len::Auto}; return; }
        if (v == L"auto") { s.flexGrow = 1; s.flexShrink = 1; s.flexBasis = {0, Len::Auto}; return; }
        if (v == L"initial") { s.flexGrow = 0; s.flexShrink = 1; s.flexBasis = {0, Len::Auto}; return; }
        if (t.empty()) return;
        float g; std::wstring u;
        size_t i = 0;
        if (ParseNumber(t[0], g, u) && u.empty()) {
            s.flexGrow = std::max(0.f, g);
            s.flexShrink = 1;
            s.flexBasis = {0, Len::Px};   // flex: <sayi> -> basis 0
            i = 1;
            if (i < t.size() && ParseNumber(t[i], g, u) && u.empty()) { s.flexShrink = std::max(0.f, g); i++; }
            if (i < t.size()) { Len l; if (ParseLen(t[i], fs, l)) s.flexBasis = l; }
        } else {
            Len l;
            if (ParseLen(t[0], fs, l)) { s.flexBasis = l; s.flexGrow = 1; s.flexShrink = 1; }
        }
    } else if (prop == L"flex-grow" || prop == L"-webkit-box-flex") {
        float g; std::wstring u;
        if (ParseNumber(v, g, u)) s.flexGrow = std::max(0.f, g);
    } else if (prop == L"flex-shrink") {
        float g; std::wstring u;
        if (ParseNumber(v, g, u)) s.flexShrink = std::max(0.f, g);
    } else if (prop == L"flex-basis") {
        Len l;
        if (v == L"content") s.flexBasis = {0, Len::Auto};
        else if (ParseLen(v, fs, l)) s.flexBasis = l;
    } else if (prop == L"order" || prop == L"-webkit-box-ordinal-group") {
        s.order = _wtoi(v.c_str());
    } else if (prop == L"grid-template-columns") {
        auto g = MutGrid(s);
        g->autoRepeat.clear(); g->autoRepeatAt = -1;
        ParseTrackList(v, fs, g->cols, g.get());
    } else if (prop == L"grid-template-rows") {
        auto g = MutGrid(s);
        ParseTrackList(v, fs, g->rows, nullptr);
    } else if (prop == L"grid-template-areas") {
        auto g = MutGrid(s);
        g->areas = v == L"none" ? std::vector<std::vector<std::wstring>>() : ParseAreas(val);
    } else if (prop == L"grid-auto-rows") {
        auto g = MutGrid(s);
        auto t = SplitTop(v);
        g->hasAutoRow = !t.empty() && ParseTrack(t[0], fs, g->autoRow);
    } else if (prop == L"grid-template" || prop == L"grid") {
        auto g = MutGrid(s);
        if (v == L"none") { *g = GridSpec(); return; }
        if (StartsWith(v, L"auto-flow") || v.find(L"auto-flow") != std::wstring::npos) return;
        size_t sl = std::wstring::npos;
        {
            int paren = 0; bool q = false;
            for (size_t i = 0; i < v.size(); i++) {
                wchar_t c = v[i];
                if (c == L'"' || c == L'\'') q = !q;
                if (q) continue;
                if (c == L'(') paren++; else if (c == L')') paren--;
                else if (c == L'/' && paren == 0) { sl = i; break; }
            }
        }
        std::wstring rowsPart = sl == std::wstring::npos ? v : v.substr(0, sl);
        std::wstring rowsRaw = sl == std::wstring::npos ? val : val.substr(0, std::min(val.size(), sl));
        if (sl != std::wstring::npos) {
            g->autoRepeat.clear(); g->autoRepeatAt = -1;
            ParseTrackList(v.substr(sl + 1), fs, g->cols, g.get());
        }
        if (rowsPart.find(L'"') != std::wstring::npos || rowsPart.find(L'\'') != std::wstring::npos) {
            g->areas = ParseAreas(rowsRaw);
            // dizeler disindaki satir boyutlari
            std::wstring sizes;
            bool q = false;
            for (wchar_t c : rowsPart) { if (c == L'"' || c == L'\'') { q = !q; sizes += L' '; continue; } if (!q) sizes += c; }
            ParseTrackList(sizes, fs, g->rows, nullptr);
        } else {
            ParseTrackList(rowsPart, fs, g->rows, nullptr);
        }
    } else if (prop == L"grid-area") {
        auto parts = SplitTop(v, L'/');
        if (parts.size() == 1 && !parts[0].empty() && (iswalpha(parts[0][0]) || parts[0][0] == L'_') && parts[0] != L"auto" && parts[0].find(L"span") != 0) {
            s.gridArea = Trim(parts[0]);
            return;
        }
        s.gridArea.clear();
        std::wstring colPart, rowPart;
        rowPart = parts.size() > 0 ? parts[0] : L"";
        colPart = parts.size() > 1 ? parts[1] : L"";
        if (parts.size() > 2) rowPart += L" / " + parts[2];
        if (parts.size() > 3) colPart += L" / " + parts[3];
        ParseGridLine(rowPart, s.grs, s.gre, s.gspanR, nullptr);
        ParseGridLine(colPart, s.gcs, s.gce, s.gspanC, nullptr);
    } else if (prop == L"grid-column" || prop == L"grid-row") {
        std::wstring name;
        if (prop == L"grid-column") ParseGridLine(v, s.gcs, s.gce, s.gspanC, &name);
        else ParseGridLine(v, s.grs, s.gre, s.gspanR, &name);
    } else if (prop == L"grid-column-start" || prop == L"grid-row-start") {
        int16_t a = 0, b = 0, sp = 1;
        ParseGridLine(v, a, b, sp, nullptr);
        if (prop == L"grid-column-start") { s.gcs = a; if (sp > 1) s.gspanC = sp; } else { s.grs = a; if (sp > 1) s.gspanR = sp; }
    } else if (prop == L"grid-column-end" || prop == L"grid-row-end") {
        int16_t a = 0, b = 0, sp = 1;
        ParseGridLine(v, a, b, sp, nullptr);
        if (prop == L"grid-column-end") { if (a) s.gce = a; else if (sp > 1) s.gspanC = sp; }
        else { if (a) s.gre = a; else if (sp > 1) s.gspanR = sp; }
    } else if (prop == L"float") {
        s.floatSide = (v == L"left" || v == L"inline-start") ? 1 : (v == L"right" || v == L"inline-end") ? 2 : 0;
    } else if (prop == L"clear") {
        s.clear = (v == L"left" || v == L"inline-start") ? 1 : (v == L"right" || v == L"inline-end") ? 2 : v == L"both" ? 3 : 0;
    } else if (prop == L"content") {
        s.content = std::make_shared<std::wstring>(val);
    } else if (prop == L"z-index") {
        if (v == L"auto") { s.hasZIndex = false; s.zIndex = 0; }
        else { s.zIndex = std::max(-100000, std::min(_wtoi(v.c_str()), 100000)); s.hasZIndex = true; }
    } else if (prop == L"cursor") {
        s.cursor = (v.find(L"pointer") != std::wstring::npos || v == L"hand") ? 1 : (v == L"text" || v == L"vertical-text") ? 2 :
                   (v == L"not-allowed" || v == L"no-drop") ? 3 : 0;
    }
}

void ApplyFontShorthand(Style& s, const Style& parent, const std::wstring& v) {
    auto t = SplitTop(v);
    if (t.size() == 1) return; // "font: inherit" / sistem fontlari
    size_t i = 0;
    for (; i < t.size(); i++) {
        const std::wstring& tok = t[i];
        if (tok == L"italic" || tok == L"oblique") s.italic = true;
        else if (tok == L"bold" || tok == L"bolder") s.weight = 700;
        else if (tok == L"normal" || tok == L"small-caps") continue;
        else if (iswdigit(tok[0]) && tok.size() == 3 && tok[1] == L'0' && tok[2] == L'0') s.weight = _wtoi(tok.c_str());
        else break;
    }
    if (i >= t.size()) return;
    std::wstring size = t[i++];
    size_t slash = size.find(L'/');
    std::wstring lh;
    if (slash != std::wstring::npos) { lh = size.substr(slash + 1); size = size.substr(0, slash); }
    else if (i < t.size() && t[i] == L"/" && i + 1 < t.size()) { lh = t[i + 1]; i += 2; }
    else if (i < t.size() && t[i][0] == L'/') { lh = t[i].substr(1); i++; }
    ApplyFontSize(s, parent, size);
    if (!lh.empty()) ApplyDecl(s, parent, L"line-height", lh);
    std::wstring fam;
    for (; i < t.size(); i++) { if (!fam.empty()) fam += L' '; fam += t[i]; }
    if (!fam.empty()) s.family = fam;
}

Style Inherit(const Style& p) {
    Style s = p;
    s.display = Style::Inline;
    s.bg = 0;
    for (int k = 0; k < 4; k++) { s.m[k] = Len(); s.p[k] = Len(); s.bw[k] = 0; s.bc[k] = p.color; s.inset[k] = {0, Len::Auto}; }
    s.width = {0, Len::Auto};
    s.maxWidth = {0, Len::None};
    s.height = {0, Len::Auto};
    s.minWidth = {0, Len::Auto};
    s.minHeight = {0, Len::Auto};
    s.maxHeight = {0, Len::None};
    s.borderBox = false;
    s.valign = 1;
    s.vAlign = 0;
    s.position = 0;
    s.tx = {0, Len::Px};
    s.ty = {0, Len::Px};
    s.overflowHidden = s.hiddenClip = s.offscreen = s.invisible = s.transformHidden = s.bfc = s.inlineBlock = false;
    s.opacity = 1;
    s.clear = 0;
    s.radius = 0;
    s.radiusPct = false;
    s.aspect = 0;
    s.cellSpacing = p.collapse ? 0 : 2;
    s.colspan = 1;
    s.bgImage.reset();
    s.mask.reset();
    s.grad.reset();
    s.shadows.reset();
    s.ellipsis = false;
    s.lineClamp = 0;
    s.webkitBox = false;
    s.pseudo.reset();
    s.content.reset();
    s.flex = 0; s.flexWrap = false; s.flexReverse = false; s.justify = 0; s.alignItems = 0; s.alignSelf = 0; s.justifyItems = 0;
    s.gapRow = s.gapCol = 0;
    s.flexGrow = 0; s.flexShrink = 1; s.flexBasis = {0, Len::Auto}; s.gridCols = 0; s.floatSide = 0; s.order = 0;
    s.flexDirCol = false;
    s.grid.reset();
    s.gridArea.clear();
    s.gcs = s.gce = s.grs = s.gre = 0;
    s.gspanC = s.gspanR = 1;
    s.zIndex = 0;
    s.hasZIndex = false;
    return s;
}

Len EmLen(float em, float fs) { return Len{em * fs, Len::Px}; }

const Node* FindAncestor(const Node* n, const wchar_t* name, int maxUp) {
    for (const Node* p = n->parent; p && maxUp-- > 0; p = p->parent)
        if (p->kind == Node::Elem && p->name == name) return p;
    return nullptr;
}

bool AttrLen(const Node* n, const wchar_t* a, Len& out) {
    const std::wstring* v = n->Attr(a);
    if (!v) return false;
    std::wstring t = Trim(*v);
    float f; std::wstring u;
    if (!ParseNumber(t, f, u) || f < 0) return false;
    out = (u == L"%") ? Len{f, Len::Pct} : Len{f, Len::Px};
    return true;
}

// Tarayici varsayilan stilleri (User-Agent stylesheet) + eski HTML sunum oznitelikleri
void ApplyUA(Style& s, const Node* n, bool scripting) {
    const std::wstring& t = n->name;
    if (scripting && t == L"noscript") { s.display = Style::None; return; }
    if (t == L"slot") { s.display = Style::Contents; return; }   // shadow DOM: slot kutu uretmez
    if (InList(t, {L"head", L"script", L"style", L"title", L"meta", L"link", L"template", L"iframe", L"math",
                   L"object", L"embed", L"canvas", L"audio", L"map", L"area", L"datalist",
                   L"param", L"source", L"track", L"base", L"noembed", L"noframes", L"frameset"})) {
        s.display = Style::None;
        return;
    }
    if (t == L"dialog" && !n->Attr(L"open")) { s.display = Style::None; return; }
    if (n->Attr(L"hidden")) {
        const std::wstring* h = n->Attr(L"hidden");
        if (ToLower(*h) != L"until-found") { s.display = Style::None; return; }
    }

    if (InList(t, {L"html", L"body", L"div", L"p", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"ul", L"ol", L"dl",
                   L"dt", L"dd", L"blockquote", L"pre", L"form", L"fieldset", L"header", L"footer", L"nav", L"main",
                   L"section", L"article", L"aside", L"figure", L"figcaption", L"address", L"hr", L"center",
                   L"details", L"summary", L"noscript", L"legend", L"menu", L"dir", L"hgroup", L"caption",
                   L"thead", L"tbody", L"tfoot", L"listing", L"plaintext", L"xmp", L"search", L"dialog", L"optgroup"}))
        s.display = Style::Block;

    const float fs = s.fontSize;
    auto heading = [&](float f, float mEm) {
        s.fontSize = fs * f;
        s.weight = 700;
        s.m[0] = s.m[2] = EmLen(mEm, s.fontSize);
        s.lineHeight = 1.2f;
    };

    if (t == L"body") { for (int k = 0; k < 4; k++) s.m[k] = Len{8, Len::Px}; }
    else if (t == L"p") { s.m[0] = s.m[2] = EmLen(1, fs); }
    else if (t == L"h1") {
        // bolum icindeki h1 kucuk gosterilir (eski HTML5 kurali)
        bool sec = FindAncestor(n, L"article", 8) || FindAncestor(n, L"section", 8) || FindAncestor(n, L"aside", 8) || FindAncestor(n, L"nav", 8);
        heading(sec ? 1.5f : 2.f, sec ? 0.83f : 0.67f);
    }
    else if (t == L"h2") heading(1.5f, 0.83f);
    else if (t == L"h3") heading(1.17f, 1.f);
    else if (t == L"h4") heading(1.f, 1.33f);
    else if (t == L"h5") heading(0.83f, 1.67f);
    else if (t == L"h6") heading(0.67f, 2.33f);
    else if (t == L"ul" || t == L"menu" || t == L"dir") {
        bool nested = FindAncestor(n, L"ul", 6) || FindAncestor(n, L"ol", 6);
        if (!nested) s.m[0] = s.m[2] = EmLen(1, fs);
        s.p[3] = Len{40, Len::Px};
        s.listType = nested ? 3 : 0;
    } else if (t == L"ol") {
        bool nested = FindAncestor(n, L"ul", 6) || FindAncestor(n, L"ol", 6);
        if (!nested) s.m[0] = s.m[2] = EmLen(1, fs);
        s.p[3] = Len{40, Len::Px};
        s.listType = 1;
    } else if (t == L"li") {
        s.display = Style::ListItem;
    } else if (t == L"dl") { s.m[0] = s.m[2] = EmLen(1, fs); }
    else if (t == L"dd") { s.m[3] = Len{40, Len::Px}; }
    else if (t == L"dt") { s.weight = 700; }
    else if (t == L"blockquote" || t == L"figure") { s.m[0] = s.m[2] = EmLen(1, fs); s.m[1] = s.m[3] = Len{40, Len::Px}; }
    else if (t == L"pre" || t == L"listing" || t == L"xmp" || t == L"plaintext") {
        s.family = L"monospace"; s.fontSize = fs * 0.875f; s.pre = true; s.nowrap = true;
        s.m[0] = s.m[2] = EmLen(1, fs);
    } else if (InList(t, {L"code", L"kbd", L"samp", L"tt"})) {
        s.family = L"monospace"; s.fontSize = fs * 0.875f;
    } else if (t == L"b" || t == L"strong") { s.weight = 700; }
    else if (InList(t, {L"i", L"em", L"cite", L"var", L"dfn", L"address"})) { s.italic = true; }
    else if (t == L"u" || t == L"ins") { s.underline = true; }
    else if (InList(t, {L"s", L"strike", L"del"})) { s.strike = true; }
    else if (t == L"small") { s.fontSize = fs * 0.83f; }
    else if (t == L"big") { s.fontSize = fs * 1.2f; }
    else if (t == L"sub") { s.fontSize = fs * 0.83f; s.vAlign = 6; }
    else if (t == L"sup") { s.fontSize = fs * 0.83f; s.vAlign = 7; }
    else if (t == L"mark") { s.bg = 0xFFFFF176; s.color = 0xFF000000; }
    else if (t == L"hr") {
        s.m[0] = s.m[2] = Len{8, Len::Px};
        s.bw[0] = 1; s.bc[0] = 0xFFC8CCD4;
    } else if (t == L"center") { s.align = 1; }
    else if (t == L"a" && n->Attr(L"href")) { s.color = 0xFF1F57C9; s.underline = true; }
    else if (t == L"table") {
        s.display = Style::Table;
        s.cellSpacing = 2;
        if (auto* cs = n->Attr(L"cellspacing")) s.cellSpacing = std::max(0.f, std::min(float(_wtof(cs->c_str())), 50.f));
        if (auto* b = n->Attr(L"border")) { float bw = float(_wtof(b->c_str())); if (b->empty()) bw = 1; for (int k = 0; k < 4; k++) { s.bw[k] = std::max(0.f, std::min(bw, 10.f)); s.bc[k] = 0xFF999999; } }
        if (auto* al = n->Attr(L"align")) { if (ToLower(*al) == L"center") { s.m[1] = s.m[3] = Len{0, Len::Auto}; } }
    } else if (t == L"tr") { s.display = Style::Row; }
    else if (t == L"td" || t == L"th") {
        s.display = Style::Cell;
        for (int k = 0; k < 4; k++) s.p[k] = Len{1, Len::Px};
        if (t == L"th") { s.weight = 700; s.align = 1; }
        if (const Node* tbl = FindAncestor(n, L"table", 4)) {
            if (auto* cp = tbl->Attr(L"cellpadding")) { float v = std::max(0.f, std::min(float(_wtof(cp->c_str())), 50.f)); for (int k = 0; k < 4; k++) s.p[k] = Len{v, Len::Px}; }
            if (auto* b = tbl->Attr(L"border")) { float v = float(_wtof(b->c_str())); if (b->empty() || v > 0) for (int k = 0; k < 4; k++) { s.bw[k] = 1; s.bc[k] = 0xFF999999; } }
        }
        if (auto* cs = n->Attr(L"colspan")) { int c = _wtoi(cs->c_str()); s.colspan = std::max(1, std::min(c, 64)); }
        if (auto* va = n->Attr(L"valign")) { std::wstring l = ToLower(*va); s.valign = l == L"top" ? 0 : l == L"bottom" ? 2 : 1; }
        if (n->Attr(L"nowrap")) s.nowrap = true;
    } else if (t == L"caption") { s.align = 1; }
    else if (t == L"button") {
        s.bg = 0xFFE9EBF0; s.inlineBlock = true;
        for (int k = 0; k < 4; k++) { s.bw[k] = 1; s.bc[k] = 0xFFA9B1BF; }
        s.p[0] = s.p[2] = Len{2, Len::Px}; s.p[1] = s.p[3] = Len{6, Len::Px};
        s.align = 1;
    }
    else if (t == L"summary") { s.weight = 700; }
    else if (t == L"fieldset") { for (int k = 0; k < 4; k++) { s.bw[k] = 1; s.bc[k] = 0xFFC0C4CC; s.p[k] = Len{8, Len::Px}; } s.m[0] = s.m[2] = Len{8, Len::Px}; }
    else if (t == L"select" || t == L"option" || t == L"optgroup") {
        if (t != L"select") s.display = Style::None;
    } else if (t == L"svg") {
        s.inlineBlock = false;
    } else if (t == L"dialog") {
        s.bg = 0xFFFFFFFF;
        for (int k = 0; k < 4; k++) { s.bw[k] = 1; s.bc[k] = 0xFF333333; s.p[k] = Len{16, Len::Px}; }
    }

    // Sunum oznitelikleri (eski siteler icin)
    if (auto* al = n->Attr(L"align")) {
        std::wstring l = ToLower(*al);
        if (t == L"img" || t == L"table") {
            if (l == L"left") s.floatSide = 1;
            else if (l == L"right") s.floatSide = 2;
        } else s.align = l == L"center" || l == L"middle" ? 1 : l == L"right" ? 2 : 0;
    }
    if (auto* bgc = n->Attr(L"bgcolor")) { uint32_t c; if (ParseColor(*bgc, c)) s.bg = c; }
    if (t == L"body") {
        if (auto* tx = n->Attr(L"text")) { uint32_t c; if (ParseColor(*tx, c)) s.color = c; }
    }
    if (t == L"font") {
        if (auto* c = n->Attr(L"color")) { uint32_t col; if (ParseColor(*c, col)) s.color = col; }
        if (auto* f = n->Attr(L"face")) s.family = ToLower(*f);
        if (auto* sz = n->Attr(L"size")) {
            static const float sizes[8] = {0, 10, 13, 16, 18, 24, 32, 48};
            std::wstring v = Trim(*sz);
            int k = _wtoi(v.c_str());
            if (!v.empty() && (v[0] == L'+' || v[0] == L'-')) k += 3;
            k = std::max(1, std::min(k, 7));
            s.fontSize = sizes[k];
        }
    }
    if (InList(t, {L"table", L"td", L"th", L"img", L"hr", L"div", L"col", L"svg", L"input", L"canvas", L"video"})) {
        Len l;
        if (AttrLen(n, L"width", l)) s.width = l;
        if (AttrLen(n, L"height", l) && (l.u == Len::Px || t == L"svg" || t == L"video")) s.height = l;
    }
}

// Shadow root stil sayfalari: ayni CSS metni (bilesen sablonlari) tek kez ayristirilir.
std::shared_ptr<const StyleSheet> CachedShadowSheet(const std::wstring& css, const StyleSheet& outer) {
    static std::mutex mu;
    static std::unordered_map<std::wstring, std::shared_ptr<const StyleSheet>> cache;
    std::wstring key = std::to_wstring(int(outer.vw)) + L"x" + std::to_wstring(int(outer.vh)) + L"|" + outer.pageBase + L"|" + css;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    auto sh = std::make_shared<StyleSheet>();
    sh->vw = outer.vw;
    sh->vh = outer.vh;
    sh->pageBase = outer.pageBase;
    if (!css.empty()) sh->Add(css, outer.pageBase);
    sh->BuildIndex();
    std::lock_guard<std::mutex> lk(mu);
    if (cache.size() > 256) cache.clear();
    cache[key] = sh;
    return sh;
}

// Shadow host'a uygulanan ic (:host / ::slotted) kurallar dis kurallara yenilir
constexpr int kInnerBias = -50000000;

class Computer {
public:
    Computer(const Document& d, const StyleSheet& sh, std::vector<Style>& out, bool scripting) : doc_(d), sh_(sh), out_(out), scripting_(scripting) {}

    void Run() {
        Style root;
        root.display = Style::Block;
        out_[size_t(doc_.root->id)] = root;
        sheetOf_.assign(out_.size(), nullptr);
        Walk(doc_.root.get(), 0);
    }

private:
    const Document& doc_;
    const StyleSheet& sh_;
    std::vector<Style>& out_;
    bool scripting_;

    struct Match { bool imp; int spec; int order; const Decl* d; };
    std::vector<const StyleSheet*> sheetOf_;   // dugumun ait oldugu agacin stil sayfasi
    std::unordered_map<const Node*, std::shared_ptr<const StyleSheet>> shadowSheets_;

    // Shadow root'taki <style> ogelerinden kapsamli stil sayfasi
    const StyleSheet& ShadowSheet(const Node* root) {
        auto it = shadowSheets_.find(root);
        if (it != shadowSheets_.end()) return *it->second;
        std::wstring css;
        std::vector<const Node*> st;
        for (size_t i = root->kids.size(); i-- > 0;) st.push_back(root->kids[i].get());
        while (!st.empty()) {
            const Node* c = st.back();
            st.pop_back();
            if (c->kind != Node::Elem) continue;
            if (c->name == L"style") {
                const std::wstring* m = c->Attr(L"media");
                if (!m || MediaMatches(*m, sh_.vw, sh_.vh)) { css += TextContent(c); css += L"\n"; }
                continue;
            }
            if (c->name == L"script" || c->name == L"template") continue;
            for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
        }
        auto sheet = CachedShadowSheet(css, sh_);
        shadowSheets_[root] = sheet;
        return *sheet;
    }

    const StyleSheet& SheetOf(const Node* k) {
        const Node* p = k->parent;
        if (!p) return sh_;
        if (p->kind == Node::Shadow) return ShadowSheet(p);
        if (p->id >= 0 && size_t(p->id) < sheetOf_.size() && sheetOf_[size_t(p->id)]) return *sheetOf_[size_t(p->id)];
        return sh_;
    }

    // Cizim (flat) agacinda gez: host -> shadow cocuklari, slot -> atanmis dugumler
    void Walk(const Node* n, size_t depth) {
        if (depth > kMaxDepth + 4) return;
        std::vector<const Node*> flat;
        bool over = HasFlatOverride(n);
        if (over) FlatKids(n, flat);
        const Node* slotRoot = (n->kind == Node::Elem && n->name == L"slot") ? ShadowRootOf(n) : nullptr;
        size_t cnt = over ? flat.size() : n->kids.size();
        for (size_t i = 0; i < cnt; i++) {
            const Node* k = over ? flat[i] : n->kids[i].get();
            if (k->kind != Node::Elem || k->id < 0 || size_t(k->id) >= out_.size()) continue;
            const StyleSheet& sh = SheetOf(k);
            sheetOf_[size_t(k->id)] = &sh;
            const Style& ps = out_[size_t(n->id)];
            Style s = Inherit(ps);
            const StyleSheet* hostSheet = k->shadow ? &ShadowSheet(k->shadow.get()) : nullptr;
            const StyleSheet* slotSheet = (slotRoot && k->parent != n) ? &ShadowSheet(slotRoot) : nullptr;
            Compute(k, s, ps, sh, hostSheet, slotSheet, slotSheet ? n : nullptr);
            out_[size_t(k->id)] = std::move(s);
            if (k->shadow && k->shadow->id >= 0 && size_t(k->shadow->id) < out_.size()) out_[size_t(k->shadow->id)] = out_[size_t(k->id)];
            if (out_[size_t(k->id)].display != Style::None) Walk(k, depth + 1);
        }
    }

    static void Push(const Rule& r, std::vector<Match>* ms, int bias) {
        auto& dst = ms[r.sel.pseudo < 3 ? r.sel.pseudo : 0];
        for (auto& d : *r.decls) dst.push_back({d.important, r.sel.spec + bias, r.order, &d});
    }

    void Collect(const StyleSheet& sh, const std::vector<int>* v, const Node* n, std::vector<Match>* ms) {
        if (!v) return;
        for (int ri : *v) {
            const Rule& r = sh.rules[size_t(ri)];
            if (Matches(r.sel, n)) Push(r, ms, 0);
        }
    }

    template <class M>
    static const std::vector<int>* Find(const M& m, const std::wstring& k) {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }

    static void Sort(std::vector<Match>& ms) {
        std::stable_sort(ms.begin(), ms.end(), [](const Match& a, const Match& b) {
            if (a.imp != b.imp) return !a.imp;
            if (a.spec != b.spec) return a.spec < b.spec;
            return a.order < b.order;
        });
    }

    static void ApplyAll(Style& s, const Style& ps, const std::vector<Match>& ms) {
        // 1) CSS degiskenleri, 2) font boyutu (em icin), 3) geri kalan
        std::shared_ptr<VarMap> nv;
        for (auto& m : ms) {
            if (!StartsWith(m.d->prop, L"--")) continue;
            if (!nv) nv = std::make_shared<VarMap>(s.vars ? *s.vars : VarMap());
            if (nv->size() < 4096) {
                std::wstring val = m.d->value;
                // degiskenin kendi icindeki var() referanslarini simdiki degerlerle coz (dongu onlemi)
                if (val.find(L"var(") != std::wstring::npos) val = SubstVars(val, nv.get());
                (*nv)[m.d->prop] = val;
            }
        }
        if (nv) s.vars = nv;
        for (auto& m : ms)
            if (m.d->prop == L"font-size" || m.d->prop == L"font") ApplyDecl(s, ps, m.d->prop, m.d->value);
        for (auto& m : ms) {
            const std::wstring& p = m.d->prop;
            if (StartsWith(p, L"--") || p == L"font-size" || p == L"font") continue;
            ApplyDecl(s, ps, p, m.d->value);
        }
    }

    void Compute(const Node* n, Style& s, const Style& ps, const StyleSheet& sh, const StyleSheet* hostSheet,
                 const StyleSheet* slotSheet, const Node* slot) {
        ApplyUA(s, n, scripting_);
        if (s.display == Style::None) return;

        std::vector<Match> ms[3];
        if (!n->idAttr.empty()) Collect(sh, Find(sh.byId, n->idAttr), n, ms);
        for (auto& c : n->classes) Collect(sh, Find(sh.byClass, c), n, ms);
        Collect(sh, Find(sh.byTag, n->name), n, ms);
        Collect(sh, &sh.universal, n, ms);
        // :host kurallari (elemanin kendi shadow root'undaki stil)
        if (hostSheet)
            for (int ri : hostSheet->hostRules) {
                const Rule& r = hostSheet->rules[size_t(ri)];
                if (Matches(r.sel, n)) Push(r, ms, kInnerBias);
            }
        // ::slotted() kurallari (atandigi slotun shadow root'undaki stil)
        if (slotSheet && slot)
            for (int ri : slotSheet->slottedRules) {
                const Rule& r = slotSheet->rules[size_t(ri)];
                bool any = false;
                for (auto& x : r.sel.parts.back().slottedArgs) if (Matches(x, n)) { any = true; break; }
                if (any && Matches(r.sel, slot)) Push(r, ms, kInnerBias);
            }

        std::vector<Decl> inl;
        if (auto* st = n->Attr(L"style")) {
            inl = ParseDeclarations(*st);
            for (auto& d : inl) {
                if (!sh_.pageBase.empty() && d.value.find(L"url(") != std::wstring::npos) d.value = ResolveCssUrls(d.value, sh_.pageBase);
            }
            for (auto& d : inl) ms[0].push_back({d.important, 10000000, INT_MAX, &d});
        }
        if (!ms[0].empty()) {
            Sort(ms[0]);
            ApplyAll(s, ps, ms[0]);
        }
        PostFix(s, n);
        if (s.display == Style::None) return;
        if (!ms[1].empty() || !ms[2].empty()) {
            auto pair = std::make_shared<PseudoPair>();
            for (int k = 1; k <= 2; k++) {
                if (ms[k].empty()) continue;
                Sort(ms[k]);
                auto p = std::make_shared<Style>(Inherit(s));
                ApplyAll(*p, s, ms[k]);
                if (MakePseudo(*p, n)) (k == 1 ? pair->before : pair->after) = p;
            }
            if (pair->before || pair->after) s.pseudo = pair;
        }
    }

    // content: "..." attr(x) open-quote ... -> metin. Ikon fontu karakterleri (PUA) atilir.
    static bool MakePseudo(Style& p, const Node* n) {
        if (!p.content || p.display == Style::None) return false;
        std::wstring raw = Trim(*p.content);
        std::wstring low = ToLower(raw);
        if (raw.empty() || low == L"none" || low == L"normal") return false;
        std::wstring text;
        bool hadText = false;
        size_t i = 0;
        while (i < raw.size()) {
            wchar_t c = raw[i];
            if (IsWS(c)) { i++; continue; }
            if (c == L'"' || c == L'\'') {
                size_t j = i + 1;
                while (j < raw.size() && raw[j] != c) {
                    if (raw[j] == L'\\' && j + 1 < raw.size()) {
                        size_t k = j + 1;
                        uint32_t cp = 0;
                        int nd = 0;
                        while (k < raw.size() && nd < 6 && iswxdigit(raw[k])) {
                            wchar_t h = raw[k];
                            cp = cp * 16 + uint32_t(h <= L'9' ? h - L'0' : (towlower(h) - L'a' + 10));
                            k++; nd++;
                        }
                        if (nd > 0) {
                            if (k < raw.size() && raw[k] == L' ') k++;
                            if (cp == 0xA) text += L'\n';
                            else AppendCodepoint(text, cp);
                            j = k;
                        } else { text += raw[j + 1]; j += 2; }
                        continue;
                    }
                    text += raw[j++];
                }
                hadText = true;
                i = j + 1;
                continue;
            }
            size_t j = i;
            while (j < raw.size() && !IsWS(raw[j]) && raw[j] != L'(') j++;
            std::wstring w = ToLower(raw.substr(i, j - i));
            if (j < raw.size() && raw[j] == L'(') {
                int d = 0;
                size_t k = j;
                for (; k < raw.size(); k++) { if (raw[k] == L'(') d++; else if (raw[k] == L')' && --d == 0) break; }
                std::wstring arg = Trim(raw.substr(j + 1, k > j ? k - j - 1 : 0));
                if (w == L"attr") {
                    auto parts = SplitWS(arg);
                    if (!parts.empty()) { if (auto* a = n->Attr(ToLower(parts[0]).c_str())) { text += *a; hadText = true; } }
                }
                i = k + 1;
                continue;
            }
            if (w == L"open-quote") { text += L"“"; hadText = true; }
            else if (w == L"close-quote") { text += L"”"; hadText = true; }
            i = j;
        }
        std::wstring clean;
        bool pua = false;
        for (wchar_t c : text) {
            if (c >= 0xE000 && c <= 0xF8FF) { pua = true; continue; }
            if (c >= 0xD800 && c <= 0xDBFF) { clean += c; continue; }
            clean += c;
        }
        // Bos ya da yalnizca ikon karakteri: kutu olarak anlam tasiyorsa tut (clearfix, dekor)
        if (clean.empty()) {
            bool box = p.clear || p.bg >> 24 || p.bgImage || p.mask || p.bw[0] > 0 || p.bw[2] > 0 ||
                       ((p.display != Style::Inline || p.inlineBlock || p.position >= 2) && (p.width.Def() || p.height.Def()));
            if (!box) return false;
            if (pua && !(p.bg >> 24) && !p.bgImage && !p.mask) return false;
        }
        (void)hadText;
        p.content = std::make_shared<std::wstring>(clean);
        return true;
    }

    // Erisilebilirlik icin gizlenmis ("sr-only") ve ekran disi ogeleri gizle
    static void PostFix(Style& s, const Node* n) {
        if (s.display == Style::None) return;
        if (s.invisible) { s.display = Style::None; return; }
        bool out = s.position == 2 || s.position == 3;
        if (out) {
            bool tiny = (s.width.u == Len::Px && s.width.v <= 1.5f) || (s.height.u == Len::Px && s.height.v <= 1.5f);
            bool farOff = (s.inset[3].u == Len::Px && s.inset[3].v <= -999) || (s.inset[0].u == Len::Px && s.inset[0].v <= -999) ||
                          (s.inset[1].u == Len::Px && s.inset[1].v <= -999) || (s.inset[3].u == Len::Pct && s.inset[3].v <= -100);
            bool slidOut = (s.tx.u == Len::Pct && std::fabs(s.tx.v) >= 100) || (s.ty.u == Len::Pct && s.ty.v <= -100) ||
                           (s.tx.u == Len::Px && std::fabs(s.tx.v) >= 999);
            if (tiny || s.hiddenClip || s.offscreen || farOff || slidOut || s.transformHidden || s.hidden || s.opacity <= 0.01f) {
                s.display = Style::None;
                return;
            }
        } else if (s.hiddenClip && s.overflowHidden) { s.display = Style::None; return; }
        if (s.offscreen && s.overflowHidden) { s.display = Style::None; return; }
        bool zeroH = (s.height.u == Len::Px && s.height.v <= 0.5f) || (s.maxHeight.u == Len::Px && s.maxHeight.v <= 0.5f);
        if (s.overflowHidden && zeroH) { s.display = Style::None; return; }
        if (!out && (s.transformHidden || s.opacity <= 0.01f)) s.hidden = true;
        // display:-webkit-box + line-clamp: icerik blok gibi akar, N satirda kesilir
        if (s.lineClamp > 0 && s.webkitBox) s.flex = 0;
        // absolute/fixed ve float ogeler blok kutu olur (CSS "blockification")
        if ((out || s.floatSide) && s.display == Style::Inline && n->name != L"img" && n->name != L"svg" && n->name != L"video" && n->name != L"input" &&
            n->name != L"select" && n->name != L"textarea" && n->name != L"button") {
            s.display = Style::Block;
            s.inlineBlock = false;
        }
        // <img>/<svg> gibi degistirilen ogeler: satir ici ise atomik kutu
        if (n->name == L"img" || n->name == L"svg" || n->name == L"input" || n->name == L"select" || n->name == L"textarea" || n->name == L"button") {
            if (s.display == Style::Inline) s.inlineBlock = n->name == L"button" || s.inlineBlock;
        }
    }
};

} // namespace

float ResolveLen(const Len& l, float base) {
    switch (l.u) {
    case Len::Px: return l.v;
    case Len::Pct: return l.v * base / 100.f;
    case Len::Calc: {
        float f = l.v + l.p * base / 100.f;
        if (l.capKind == 1) f = std::min(f, l.cap);
        else if (l.capKind == 2) f = std::max(f, l.cap);
        return f;
    }
    default: return 0;
    }
}

bool ParseColor(const std::wstring& vin, uint32_t& out) {
    std::wstring v = ToLower(Trim(vin));
    if (v.empty()) return false;
    if (v == L"transparent") { out = 0; return true; }
    auto hv = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return -1;
    };
    if (v[0] == L'#' || (v.size() == 6 && hv(v[0]) >= 0 && hv(v[5]) >= 0 && hv(v[2]) >= 0)) {
        std::wstring h = v[0] == L'#' ? v.substr(1) : v;
        for (wchar_t c : h) if (hv(c) < 0) return false;
        uint32_t r, g, b, a = 255;
        if (h.size() == 3 || h.size() == 4) {
            r = uint32_t(hv(h[0]) * 17); g = uint32_t(hv(h[1]) * 17); b = uint32_t(hv(h[2]) * 17);
            if (h.size() == 4) a = uint32_t(hv(h[3]) * 17);
        } else if (h.size() == 6 || h.size() == 8) {
            r = uint32_t(hv(h[0]) * 16 + hv(h[1])); g = uint32_t(hv(h[2]) * 16 + hv(h[3])); b = uint32_t(hv(h[4]) * 16 + hv(h[5]));
            if (h.size() == 8) a = uint32_t(hv(h[6]) * 16 + hv(h[7]));
        } else return false;
        out = (a << 24) | (r << 16) | (g << 8) | b;
        return true;
    }
    if (StartsWith(v, L"rgb") || StartsWith(v, L"hsl") || StartsWith(v, L"hwb")) {
        size_t a = v.find(L'('), b = v.rfind(L')');
        if (a == std::wstring::npos) return false;
        if (b == std::wstring::npos || b < a) b = v.size();
        std::wstring in = v.substr(a + 1, b - a - 1);
        if (in.find(L"from ") != std::wstring::npos || in.find(L"calc(") != std::wstring::npos) return false;
        for (auto& c : in) if (c == L',' || c == L'/') c = L' ';
        auto toks = SplitWS(in);
        if (toks.size() < 3) return false;
        float vals[4] = {0, 0, 0, 1};
        bool pct[4] = {false, false, false, false};
        for (size_t i = 0; i < toks.size() && i < 4; i++) {
            float f; std::wstring u;
            if (toks[i] == L"none") { vals[i] = 0; continue; }
            if (!ParseNumber(toks[i], f, u)) return false;
            vals[i] = f;
            pct[i] = (u == L"%");
        }
        float alpha = pct[3] ? vals[3] / 100.f : vals[3];
        alpha = std::max(0.f, std::min(alpha, 1.f));
        float r, g, bl;
        if (v[0] == L'r') {
            r = pct[0] ? vals[0] * 2.55f : vals[0];
            g = pct[1] ? vals[1] * 2.55f : vals[1];
            bl = pct[2] ? vals[2] * 2.55f : vals[2];
        } else if (v[0] == L'h' && v[1] == L'w') {
            float h = std::fmod(vals[0], 360.f) / 360.f;
            if (h < 0) h += 1;
            float wh = vals[1] / 100.f, bk = vals[2] / 100.f;
            if (wh + bk >= 1) { r = g = bl = wh / (wh + bk) * 255; }
            else {
                float rr = HueToRgb(0, 1, h + 1.f / 3), gg = HueToRgb(0, 1, h), bb = HueToRgb(0, 1, h - 1.f / 3);
                r = (rr * (1 - wh - bk) + wh) * 255; g = (gg * (1 - wh - bk) + wh) * 255; bl = (bb * (1 - wh - bk) + wh) * 255;
            }
        } else {
            float h = std::fmod(vals[0], 360.f) / 360.f;
            if (h < 0) h += 1;
            float sat = vals[1] / 100.f, l = vals[2] / 100.f;
            if (sat <= 0) { r = g = bl = l * 255; }
            else {
                float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat;
                float p = 2 * l - q;
                r = HueToRgb(p, q, h + 1.f / 3) * 255;
                g = HueToRgb(p, q, h) * 255;
                bl = HueToRgb(p, q, h - 1.f / 3) * 255;
            }
        }
        auto cl = [](float x) { return uint32_t(std::max(0.f, std::min(x, 255.f)) + 0.5f); };
        out = (uint32_t(alpha * 255 + 0.5f) << 24) | (cl(r) << 16) | (cl(g) << 8) | cl(bl);
        return true;
    }
    if (StartsWith(v, L"color-mix(")) {
        // color-mix(in srgb, A p%, B): renkleri karistir
        size_t a = v.find(L'('), b = v.rfind(L')');
        if (b == std::wstring::npos) return false;
        auto parts = SplitTop(v.substr(a + 1, b - a - 1), L',');
        std::vector<uint32_t> cs;
        for (size_t i = 1; i < parts.size(); i++) {
            for (auto& w : SplitTop(parts[i])) { uint32_t c; if (ParseColor(w, c)) { cs.push_back(c); break; } }
        }
        if (cs.empty()) return false;
        out = AvgColor(cs);
        return true;
    }
    if (v == L"canvas" || v == L"field") { out = 0xFFFFFFFF; return true; }
    if (v == L"canvastext" || v == L"fieldtext" || v == L"buttontext") { out = 0xFF000000; return true; }
    if (v == L"linktext") { out = 0xFF1F57C9; return true; }
    if (v == L"buttonface" || v == L"buttonborder") { out = 0xFFE9EBF0; return true; }
    if (v == L"graytext") { out = 0xFF6D6D6D; return true; }
    if (v == L"highlight") { out = 0xFF3390FF; return true; }
    for (auto& c : kColors) if (v == c.n) { out = 0xFF000000 | c.c; return true; }
    return false;
}

std::vector<Style> ComputeStyles(const Document& doc, const StyleSheet& sheet, bool scripting) {
    StyleContext ctx;
    ctx.vw = sheet.vw;
    ctx.vh = sheet.vh;
    ctx.scripting = scripting;
    return ComputeStyles(doc, sheet, ctx);
}

std::vector<Style> ComputeStyles(const Document& doc, const StyleSheet& sheet, const StyleContext& ctx) {
    t_vw = ctx.vw > 0 ? ctx.vw : 1280.f;
    t_vh = ctx.vh > 0 ? ctx.vh : 800.f;
    std::vector<Style> out(size_t(doc.count) + 1);
    Computer c(doc, sheet, out, ctx.scripting);
    c.Run();
    return out;
}

} // namespace mer
