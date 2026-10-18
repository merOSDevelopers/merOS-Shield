// merOS Shield - MerEngine : hesaplanmış stil (UA stilleri, cascade, kalıtım, CSS değişkenleri)
#include "style.h"
#include "util.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

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
};

const float kRootFs = 16.f;

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

// Uzunluk: px, em, rem, %, pt, vw, vh, ch, ex, in, cm, mm
bool ParseLen(const std::wstring& vIn, float fs, Len& out) {
    std::wstring v = ToLower(Trim(vIn));
    if (v == L"auto") { out = {0, Len::Auto}; return true; }
    if (v == L"none") { out = {0, Len::None}; return true; }
    if (v == L"0") { out = {0, Len::Px}; return true; }
    if (StartsWith(v, L"calc(") || StartsWith(v, L"min(") || StartsWith(v, L"max(")) {
        // Basit calc destegi: "calc(100% - 20px)" -> yuzde kismini kullan
        size_t pct = v.find(L'%');
        if (pct != std::wstring::npos) {
            size_t st = pct;
            while (st > 0 && (iswdigit(v[st - 1]) || v[st - 1] == L'.')) st--;
            float f = 0; std::wstring u;
            if (ParseNumber(v.substr(st, pct - st), f, u)) { out = {f, Len::Pct}; return true; }
        }
        return false;
    }
    if (StartsWith(v, L"clamp(")) {
        size_t c = v.find(L',');
        if (c == std::wstring::npos) return false;
        return ParseLen(v.substr(6, c - 6), fs, out); // alt sinir
    }
    float f; std::wstring u;
    if (!ParseNumber(v, f, u)) return false;
    if (u.empty() || u == L"px") out = {f, Len::Px};
    else if (u == L"%") out = {f, Len::Pct};
    else if (u == L"em") out = {f * fs, Len::Px};
    else if (u == L"rem") out = {f * kRootFs, Len::Px};
    else if (u == L"pt") out = {f * 4.f / 3.f, Len::Px};
    else if (u == L"vw") out = {f * 12.8f, Len::Px};
    else if (u == L"vh") out = {f * 8.f, Len::Px};
    else if (u == L"ch" || u == L"ex") out = {f * fs * 0.5f, Len::Px};
    else if (u == L"in") out = {f * 96.f, Len::Px};
    else if (u == L"cm") out = {f * 37.8f, Len::Px};
    else if (u == L"mm") out = {f * 3.78f, Len::Px};
    else return false;
    return true;
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
    if (depth > 8) return v;
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
    return out;
}

std::wstring ExtractUrl(const std::wstring& s) {
    size_t u = s.find(L"url(");
    if (u == std::wstring::npos) return L"";
    size_t start = u + 4;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'"' || s[start] == L'\'')) start++;
    size_t end = s.find(L')', start);
    if (end == std::wstring::npos) end = s.size();
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'"' || s[end - 1] == L'\'')) end--;
    return s.substr(start, end - start);
}

void ParseBgPosition(const std::wstring& v, float fs, Len& px, Len& py) {
    auto t = SplitWS(v);
    if (t.empty()) return;
    auto parseOne = [fs](const std::wstring& tok, bool isY) -> Len {
        if (tok == L"left") return Len{0, Len::Pct};
        if (tok == L"right") return Len{100, Len::Pct};
        if (tok == L"top") return Len{0, Len::Pct};
        if (tok == L"bottom") return Len{100, Len::Pct};
        if (tok == L"center") return Len{50, Len::Pct};
        Len l;
        if (ParseLen(tok, fs, l)) return l;
        return Len{isY ? 50.f : 0.f, Len::Pct};
    };
    if (t.size() == 1) {
        if (t[0] == L"top" || t[0] == L"bottom") {
            px = Len{50, Len::Pct};
            py = parseOne(t[0], true);
        } else {
            px = parseOne(t[0], false);
            py = Len{50, Len::Pct};
        }
    } else {
        px = parseOne(t[0], false);
        py = parseOne(t[1], true);
    }
}

void SetBoxSides(Len* box, const std::wstring& v, float fs) {
    auto t = SplitWS(v);
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
    if (ParseLen(s, fs, l) && l.u == Len::Px) { w = l.v; return true; }
    return false;
}

bool IsBorderStyle(const std::wstring& s) {
    return InList(s, {L"none", L"hidden", L"solid", L"dashed", L"dotted", L"double", L"groove", L"ridge", L"inset", L"outset"});
}

void ApplyBorder(Style& s, int side, const std::wstring& v) {
    float w = -1;
    uint32_t col = s.color;
    bool hasCol = false, none = false, hasStyle = false;
    for (auto& tok : SplitWS(v)) {
        std::wstring lt = ToLower(tok);
        if (IsBorderStyle(lt)) { hasStyle = true; if (lt == L"none" || lt == L"hidden") none = true; continue; }
        float bw;
        if (ParseBorderWidth(lt, s.fontSize, bw)) { w = bw; continue; }
        uint32_t c;
        if (ParseColor(tok, c)) { col = c; hasCol = true; }
    }
    if (!hasStyle && w < 0 && !hasCol) return;
    if (none) w = 0;
    else if (w < 0) w = hasStyle ? 1.f : 0.f;
    if (!hasStyle && !none) { /* stil yazilmamissa CSS'te kenarlik cizilmez */ w = 0; }
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
    s.fontSize = std::max(1.f, std::min(s.fontSize, 400.f));
}

void ApplyDecl(Style& s, const Style& parent, const std::wstring& prop, const std::wstring& raw) {
    std::wstring val = raw;
    if (val.find(L"var(") != std::wstring::npos) val = SubstVars(val, s.vars.get());
    std::wstring v = ToLower(Trim(val));
    if (v.empty()) return;
    const float fs = s.fontSize;
    const bool inherit = (v == L"inherit");

    if (prop == L"color") {
        if (inherit || v == L"currentcolor") { s.color = parent.color; return; }
        uint32_t c; if (ParseColor(v, c)) s.color = c;
    } else if (prop == L"background-color") {
        uint32_t c; if (ParseColor(v, c)) s.bg = c;
        else if (inherit) s.bg = parent.bg;
    } else if (prop == L"background") {
        if (v == L"none" || v == L"transparent") { s.bg = 0; s.bgImage.clear(); return; }
        std::wstring u = ExtractUrl(val);
        if (!u.empty()) s.bgImage = u;
        if (v.find(L"no-repeat") != std::wstring::npos) s.bgRepeat = 1;
        else if (v.find(L"repeat-x") != std::wstring::npos) s.bgRepeat = 2;
        else if (v.find(L"repeat-y") != std::wstring::npos) s.bgRepeat = 3;

        std::vector<std::wstring> posTokens;
        for (auto& tok : SplitWS(v)) {
            if (StartsWith(tok, L"url(") || tok.find(L')') != std::wstring::npos) continue;
            if (tok == L"no-repeat" || tok == L"repeat" || tok == L"repeat-x" || tok == L"repeat-y") continue;
            if (tok == L"scroll" || tok == L"fixed" || tok == L"local") continue;
            uint32_t c;
            if (ParseColor(tok, c)) { s.bg = c; continue; }
            posTokens.push_back(tok);
        }
        if (!posTokens.empty()) {
            std::wstring posStr;
            for (auto& pt : posTokens) { if (!posStr.empty()) posStr += L' '; posStr += pt; }
            ParseBgPosition(posStr, fs, s.bgPosX, s.bgPosY);
        }
    } else if (prop == L"background-image") {
        if (v == L"none") s.bgImage.clear();
        else {
            std::wstring u = ExtractUrl(val);
            if (!u.empty()) s.bgImage = u;
        }
    } else if (prop == L"background-position") {
        ParseBgPosition(v, fs, s.bgPosX, s.bgPosY);
    } else if (prop == L"background-repeat") {
        if (v == L"no-repeat") s.bgRepeat = 1;
        else if (v == L"repeat-x") s.bgRepeat = 2;
        else if (v == L"repeat-y") s.bgRepeat = 3;
        else s.bgRepeat = 0;
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
        if (v == L"center" || v == L"-webkit-center") s.align = 1;
        else if (v == L"right" || v == L"end") s.align = 2;
        else s.align = 0;
    } else if (prop == L"text-decoration" || prop == L"text-decoration-line") {
        s.underline = v.find(L"underline") != std::wstring::npos;
        s.strike = v.find(L"line-through") != std::wstring::npos;
    } else if (prop == L"text-transform") {
        s.textTransform = v == L"uppercase" ? 1 : v == L"lowercase" ? 2 : v == L"capitalize" ? 3 : 0;
    } else if (prop == L"display") {
        s.flex = 0;
        s.gridCols = 0;
        if (v == L"flex" || v == L"inline-flex") { s.display = Style::Block; s.flex = s.flexDirCol ? 2 : 1; return; }
        if (v == L"grid" || v == L"inline-grid") { s.display = Style::Block; s.gridCols = std::max(1, s.gridTemplate); return; }
        if (v == L"none") s.display = Style::None;
        else if (v == L"inline" || v == L"inline-block" || v == L"inline-flex" || v == L"inline-grid" || v == L"contents" || v == L"inline-table") s.display = Style::Inline;
        else if (v == L"list-item") s.display = Style::ListItem;
        else if (v == L"table") s.display = Style::Table;
        else if (v == L"table-row") s.display = Style::Row;
        else if (v == L"table-cell") s.display = Style::Cell;
        else s.display = Style::Block; // block, flex, grid, flow-root, table-row-group ...
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
            auto t = SplitWS(v);
            Len a, b;
            if (!t.empty() && ParseLen(t[0], fs, a)) {
                b = a;
                if (t.size() > 1) ParseLen(t[1], fs, b);
                Len* box = mg ? s.m : s.p;
                if (side == L"inline") { box[3] = a; box[1] = b; } else { box[0] = a; box[2] = b; }
            }
        }
    } else if (prop == L"width") {
        Len l; if (ParseLen(v, fs, l)) s.width = l;
    } else if (prop == L"max-width") {
        Len l; if (ParseLen(v, fs, l)) s.maxWidth = l;
    } else if (prop == L"height" || prop == L"min-height") {
        Len l; if (ParseLen(v, fs, l)) { if (prop == L"height" || l.u == Len::Px) s.height = l; }
    } else if (prop == L"border") {
        ApplyBorder(s, -1, val);
    } else if (prop == L"border-top" || prop == L"border-right" || prop == L"border-bottom" || prop == L"border-left") {
        int k = prop == L"border-top" ? 0 : prop == L"border-right" ? 1 : prop == L"border-bottom" ? 2 : 3;
        ApplyBorder(s, k, val);
    } else if (prop == L"border-width") {
        auto t = SplitWS(v);
        float w[4]; int n = 0;
        for (auto& tok : t) { if (n < 4 && ParseBorderWidth(tok, fs, w[n])) n++; }
        if (n == 1) for (int k = 0; k < 4; k++) s.bw[k] = w[0];
        else if (n == 2) { s.bw[0] = s.bw[2] = w[0]; s.bw[1] = s.bw[3] = w[1]; }
        else if (n >= 3) { s.bw[0] = w[0]; s.bw[1] = s.bw[3] = w[1]; s.bw[2] = w[2]; if (n == 4) s.bw[3] = w[3]; }
    } else if (prop == L"border-color") {
        auto t = SplitWS(val);
        uint32_t c;
        if (!t.empty() && ParseColor(t[0], c)) for (int k = 0; k < 4; k++) s.bc[k] = c;
    } else if (prop == L"border-style") {
        if (v == L"none" || v == L"hidden") for (int k = 0; k < 4; k++) s.bw[k] = 0;
    } else if (StartsWith(prop, L"border-") && (EndsWith(prop, L"-color") || EndsWith(prop, L"-width"))) {
        std::wstring side = prop.substr(7, prop.find(L'-', 7) - 7);
        int k = side == L"top" ? 0 : side == L"right" ? 1 : side == L"bottom" ? 2 : side == L"left" ? 3 : -1;
        if (k < 0) return;
        if (EndsWith(prop, L"-color")) { uint32_t c; if (ParseColor(v, c)) s.bc[k] = c; }
        else { float w; if (ParseBorderWidth(v, fs, w)) s.bw[k] = w; }
    } else if (prop == L"border-collapse") {
        s.collapse = (v == L"collapse");
        if (s.collapse) s.cellSpacing = 0;
    } else if (prop == L"border-spacing") {
        Len l; auto t = SplitWS(v);
        if (!t.empty() && ParseLen(t[0], fs, l) && l.u == Len::Px) s.cellSpacing = l.v;
    } else if (prop == L"box-sizing") {
        s.borderBox = (v == L"border-box");
    } else if (prop == L"white-space") {
        s.pre = (v == L"pre" || v == L"pre-wrap" || v == L"break-spaces" || v == L"pre-line");
        s.nowrap = (v == L"nowrap" || v == L"pre");
    } else if (prop == L"line-height") {
        if (v == L"normal") { s.lineHeight = 1.3f; s.lineHeightPx = 0; return; }
        float f; std::wstring u;
        if (!ParseNumber(v, f, u)) return;
        if (u.empty()) { s.lineHeight = f; s.lineHeightPx = 0; }
        else if (u == L"%") { s.lineHeightPx = 0; s.lineHeight = f / 100.f; }
        else { Len l; if (ParseLen(v, fs, l) && l.u == Len::Px) s.lineHeightPx = l.v; }
    } else if (prop == L"list-style-type" || prop == L"list-style") {
        if (v.find(L"none") != std::wstring::npos) s.listType = 2;
        else if (v.find(L"decimal") != std::wstring::npos || v.find(L"roman") != std::wstring::npos || v.find(L"alpha") != std::wstring::npos) s.listType = 1;
        else if (v.find(L"circle") != std::wstring::npos) s.listType = 3;
        else if (v.find(L"square") != std::wstring::npos) s.listType = 4;
        else if (v.find(L"disc") != std::wstring::npos) s.listType = 0;
    } else if (prop == L"vertical-align") {
        s.valign = v == L"top" ? 0 : v == L"bottom" ? 2 : 1;
    } else if (prop == L"position") {
        s.position = v == L"relative" ? 1 : v == L"absolute" ? 2 : v == L"fixed" ? 3 : v == L"sticky" ? 1 : 0;
    } else if (prop == L"overflow" || prop == L"overflow-y") {
        s.overflowHidden = (v == L"hidden" || v == L"clip");
    } else if (prop == L"clip" || prop == L"clip-path") {
        if (v.find(L"rect(0") != std::wstring::npos || v.find(L"inset(50%") != std::wstring::npos || v.find(L"rect(1px") != std::wstring::npos)
            s.hiddenClip = true;
    } else if (prop == L"top") {
        Len l; if (ParseLen(v, fs, l)) s.top = l;
        if (l.u == Len::Px && l.v <= -999) s.offscreen = true;
    } else if (prop == L"bottom") {
        Len l; if (ParseLen(v, fs, l)) s.bottom = l;
    } else if (prop == L"left") {
        Len l; if (ParseLen(v, fs, l)) s.left = l;
        if (l.u == Len::Px && l.v <= -999) s.offscreen = true;
    } else if (prop == L"right") {
        Len l; if (ParseLen(v, fs, l)) s.right = l;
        if (l.u == Len::Px && l.v <= -999) s.offscreen = true;
    } else if (prop == L"z-index") {
        if (v == L"auto") { s.hasZIndex = false; s.zIndex = 0; }
        else {
            s.zIndex = _wtoi(v.c_str());
            s.hasZIndex = true;
        }
    } else if (prop == L"clear") {
        if (v == L"left") s.clear = 1;
        else if (v == L"right") s.clear = 2;
        else if (v == L"both") s.clear = 3;
        else s.clear = 0;
    } else if (prop == L"border-radius") {
        Len l; if (ParseLen(v, fs, l) && l.u == Len::Px) s.borderRadius = std::max(0.f, l.v);
    } else if (prop == L"text-indent") {
        Len l;
        if (ParseLen(v, fs, l) && l.u == Len::Px && l.v <= -999) s.textIndentOffscreen = true;
    } else if (prop == L"flex-direction") {
        s.flexDirCol = (v == L"column" || v == L"column-reverse");
        if (s.flex) s.flex = s.flexDirCol ? 2 : 1;
    } else if (prop == L"flex-flow") {
        s.flexDirCol = v.find(L"column") != std::wstring::npos;
        if (s.flex) s.flex = s.flexDirCol ? 2 : 1;
        s.flexWrap = v.find(L"wrap") != std::wstring::npos && v.find(L"nowrap") == std::wstring::npos;
    } else if (prop == L"flex-wrap") {
        s.flexWrap = (v == L"wrap" || v == L"wrap-reverse");
    } else if (prop == L"justify-content") {
        s.justify = (v == L"center") ? 1 : (v == L"flex-end" || v == L"end" || v == L"right") ? 2 :
                    (v == L"space-between") ? 3 : (v == L"space-around" || v == L"space-evenly") ? 4 : 0;
    } else if (prop == L"align-items") {
        s.alignItems = (v == L"center") ? 1 : (v == L"flex-end" || v == L"end") ? 2 : 0;
    } else if (prop == L"gap" || prop == L"grid-gap" || prop == L"row-gap" || prop == L"column-gap" || prop == L"grid-column-gap" || prop == L"grid-row-gap") {
        auto t = SplitWS(v);
        Len a, b;
        if (t.empty() || !ParseLen(t[0], fs, a)) return;
        b = a;
        if (t.size() > 1) ParseLen(t[1], fs, b);
        float av = a.u == Len::Px ? a.v : 0, bv = b.u == Len::Px ? b.v : 0;
        if (prop == L"row-gap" || prop == L"grid-row-gap") s.gapRow = av;
        else if (prop == L"column-gap" || prop == L"grid-column-gap") s.gapCol = av;
        else { s.gapRow = av; s.gapCol = bv; }
    } else if (prop == L"flex") {
        auto t = SplitWS(v);
        if (v == L"none") { s.flexGrow = 0; return; }
        if (v == L"auto") { s.flexGrow = 1; s.flexBasis = {0, Len::Auto}; return; }
        if (!t.empty()) {
            float g; std::wstring u;
            if (ParseNumber(t[0], g, u) && u.empty()) {
                s.flexGrow = g;
                s.flexBasis = {0, Len::Px};
                if (t.size() >= 2) { Len l; if (ParseLen(t.back(), fs, l) && t.back() != L"0" && t.size() == 3) s.flexBasis = l; else if (t.size() == 2) { float sh; std::wstring u2; if (!(ParseNumber(t[1], sh, u2) && u2.empty())) { if (ParseLen(t[1], fs, l)) s.flexBasis = l; } } }
                if (t.size() == 1) s.flexBasis = {0, Len::Px};
            } else {
                Len l;
                if (ParseLen(t[0], fs, l)) { s.flexBasis = l; s.flexGrow = 1; }
            }
        }
    } else if (prop == L"flex-grow") {
        float g; std::wstring u;
        if (ParseNumber(v, g, u)) s.flexGrow = std::max(0.f, g);
    } else if (prop == L"flex-basis") {
        Len l; if (ParseLen(v, fs, l)) s.flexBasis = l;
    } else if (prop == L"order") {
        s.order = _wtoi(v.c_str());
    } else if (prop == L"grid-template-columns") {
        int n = 0;
        if (StartsWith(v, L"repeat(")) {
            int r = _wtoi(v.c_str() + 7);
            n = r > 0 ? r : (v.find(L"auto-fill") != std::wstring::npos || v.find(L"auto-fit") != std::wstring::npos) ? 3 : 1;
        } else {
            n = int(SplitWS(v).size());
        }
        s.gridTemplate = std::max(1, std::min(n, 24));
        if (s.gridCols) s.gridCols = s.gridTemplate;
    } else if (prop == L"float") {
        s.floatSide = v == L"left" ? 1 : v == L"right" ? 2 : 0;
    } else if (prop == L"visibility") {
        s.invisible = (v == L"hidden" || v == L"collapse");
    }
}

void ApplyFontShorthand(Style& s, const Style& parent, const std::wstring& v) {
    auto t = SplitWS(v);
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
    for (int k = 0; k < 4; k++) { s.m[k] = Len(); s.p[k] = Len(); s.bw[k] = 0; s.bc[k] = p.color; }
    s.width = {0, Len::Auto};
    s.maxWidth = {0, Len::None};
    s.height = {0, Len::Auto};
    s.borderBox = false;
    s.valign = 1;
    s.position = 0;
    s.overflowHidden = s.hiddenClip = s.offscreen = s.invisible = false;
    s.cellSpacing = p.collapse ? 0.f : 2.f;
    s.colspan = 1;
    s.flex = 0; s.flexWrap = false; s.justify = 0; s.alignItems = 0; s.gapRow = s.gapCol = 0;
    s.flexGrow = 0; s.flexBasis = {0, Len::Auto}; s.gridCols = 0; s.floatSide = 0; s.order = 0;
    s.flexDirCol = false; s.gridTemplate = 0;
    s.clear = 0;
    s.bgImage.clear();
    s.bgPosX = {0, Len::Px}; s.bgPosY = {0, Len::Px}; s.bgRepeat = 0;
    s.top = {0, Len::Auto}; s.right = {0, Len::Auto}; s.bottom = {0, Len::Auto}; s.left = {0, Len::Auto};
    s.zIndex = 0; s.hasZIndex = false;
    s.borderRadius = 0;
    s.textIndentOffscreen = false;
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
    if (!ParseNumber(t, f, u)) return false;
    out = (u == L"%") ? Len{f, Len::Pct} : Len{f, Len::Px};
    return true;
}

// Tarayici varsayilan stilleri (User-Agent stylesheet) + eski HTML sunum oznitelikleri
void ApplyUA(Style& s, const Node* n, bool scripting) {
    const std::wstring& t = n->name;
    if (scripting && t == L"noscript") { s.display = Style::None; return; }
    if (InList(t, {L"head", L"script", L"style", L"title", L"meta", L"link", L"template", L"iframe", L"svg", L"math",
                   L"object", L"embed", L"canvas", L"audio", L"video", L"map", L"area", L"datalist",
                   L"param", L"source", L"track", L"base", L"dialog", L"noembed", L"noframes", L"frameset", L"picture"})) {
        if (t != L"picture") { s.display = Style::None; return; }
    }
    if (n->Attr(L"hidden")) { s.display = Style::None; return; }
    if (auto* ah = n->Attr(L"aria-hidden")) { (void)ah; }

    if (InList(t, {L"html", L"body", L"div", L"p", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"ul", L"ol", L"dl",
                   L"dt", L"dd", L"blockquote", L"pre", L"form", L"fieldset", L"header", L"footer", L"nav", L"main",
                   L"section", L"article", L"aside", L"figure", L"figcaption", L"address", L"hr", L"center",
                   L"details", L"summary", L"noscript", L"legend", L"menu", L"dir", L"hgroup", L"caption",
                   L"thead", L"tbody", L"tfoot", L"listing", L"plaintext", L"xmp", L"search"}))
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
    else if (t == L"h1") heading(2.f, 0.67f);
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
    else if (t == L"sub" || t == L"sup") { s.fontSize = fs * 0.83f; }
    else if (t == L"mark") { s.bg = 0xFFFFF176; s.color = 0xFF000000; }
    else if (t == L"hr") {
        s.m[0] = s.m[2] = Len{8, Len::Px};
        s.bw[0] = 1; s.bc[0] = 0xFFC8CCD4;
    } else if (t == L"center") { s.align = 1; }
    else if (t == L"a" && n->Attr(L"href")) { s.color = 0xFF1F57C9; s.underline = true; }
    else if (t == L"table") {
        s.display = Style::Table;
        s.cellSpacing = 2;
        if (auto* cs = n->Attr(L"cellspacing")) s.cellSpacing = float(_wtof(cs->c_str()));
        if (auto* b = n->Attr(L"border")) { float bw = float(_wtof(b->c_str())); if (b->empty()) bw = 1; for (int k = 0; k < 4; k++) { s.bw[k] = std::min(bw, 10.f); s.bc[k] = 0xFF999999; } }
        if (auto* al = n->Attr(L"align")) { if (ToLower(*al) == L"center") { s.m[1] = s.m[3] = Len{0, Len::Auto}; } }
    } else if (t == L"tr") { s.display = Style::Row; }
    else if (t == L"td" || t == L"th") {
        s.display = Style::Cell;
        for (int k = 0; k < 4; k++) s.p[k] = Len{1, Len::Px};
        if (t == L"th") { s.weight = 700; s.align = 1; }
        if (const Node* tbl = FindAncestor(n, L"table", 4)) {
            if (auto* cp = tbl->Attr(L"cellpadding")) { float v = float(_wtof(cp->c_str())); for (int k = 0; k < 4; k++) s.p[k] = Len{v, Len::Px}; }
            if (auto* b = tbl->Attr(L"border")) { float v = float(_wtof(b->c_str())); if (b->empty() || v > 0) for (int k = 0; k < 4; k++) { s.bw[k] = 1; s.bc[k] = 0xFF999999; } }
        }
        if (auto* cs = n->Attr(L"colspan")) { int c = _wtoi(cs->c_str()); s.colspan = std::max(1, std::min(c, 64)); }
        if (auto* va = n->Attr(L"valign")) { std::wstring l = ToLower(*va); s.valign = l == L"top" ? 0 : l == L"bottom" ? 2 : 1; }
        if (n->Attr(L"nowrap")) s.nowrap = true;
    } else if (t == L"caption") { s.align = 1; }
    else if (t == L"button") { s.bg = 0xFFE9EBF0; }
    else if (t == L"summary") { s.weight = 700; }
    else if (t == L"fieldset") { for (int k = 0; k < 4; k++) { s.bw[k] = 1; s.bc[k] = 0xFFC0C4CC; s.p[k] = Len{8, Len::Px}; } s.m[0] = s.m[2] = Len{8, Len::Px}; }
    else if (t == L"select" || t == L"option" || t == L"optgroup") {
        if (t != L"select") s.display = Style::None;
    }

    // Sunum oznitelikleri (eski siteler icin)
    if (auto* al = n->Attr(L"align")) {
        std::wstring l = ToLower(*al);
        if (t != L"table" && t != L"img") s.align = l == L"center" || l == L"middle" ? 1 : l == L"right" ? 2 : 0;
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
    if (InList(t, {L"table", L"td", L"th", L"img", L"hr", L"div", L"col"})) {
        Len l;
        if (AttrLen(n, L"width", l)) s.width = l;
        if (AttrLen(n, L"height", l) && l.u == Len::Px) s.height = l;
    }
}

class Computer {
public:
    Computer(const Document& d, const StyleSheet& sh, std::vector<Style>& out, bool scripting) : doc_(d), sh_(sh), out_(out), scripting_(scripting) {}

    void Run() {
        Style root;
        root.display = Style::Block;
        out_[doc_.root->id] = root;
        Walk(doc_.root.get(), 0);
    }

private:
    const Document& doc_;
    const StyleSheet& sh_;
    std::vector<Style>& out_;
    bool scripting_;

    struct Match { bool imp; int spec; int order; const Decl* d; };

    void Walk(const Node* n, size_t depth) {
        if (depth > kMaxDepth + 4) return;
        const Style& ps = out_[n->id];
        for (auto& kp : n->kids) {
            const Node* k = kp.get();
            if (k->kind != Node::Elem) continue;
            Style& s = out_[k->id];
            s = Inherit(ps);
            Compute(k, s, ps);
            if (s.display != Style::None) Walk(k, depth + 1);
        }
    }

    void Collect(const std::vector<int>* v, const Node* n, std::vector<Match>& ms) {
        if (!v) return;
        for (int ri : *v) {
            const Rule& r = sh_.rules[ri];
            if (Matches(r.sel, n))
                for (auto& d : *r.decls) ms.push_back({d.important, r.sel.spec, r.order, &d});
        }
    }

    template <class M>
    static const std::vector<int>* Find(const M& m, const std::wstring& k) {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }

    void Compute(const Node* n, Style& s, const Style& ps) {
        ApplyUA(s, n, scripting_);
        if (s.display == Style::None) return;

        std::vector<Match> ms;
        if (!n->idAttr.empty()) Collect(Find(sh_.byId, n->idAttr), n, ms);
        for (auto& c : n->classes) Collect(Find(sh_.byClass, c), n, ms);
        Collect(Find(sh_.byTag, n->name), n, ms);
        Collect(&sh_.universal, n, ms);

        std::vector<Decl> inl;
        if (auto* st = n->Attr(L"style")) {
            inl = ParseDeclarations(*st);
            for (auto& d : inl) ms.push_back({d.important, 10000000, INT_MAX, &d});
        }
        if (ms.empty()) { PostFix(s); return; }
        std::stable_sort(ms.begin(), ms.end(), [](const Match& a, const Match& b) {
            if (a.imp != b.imp) return !a.imp;
            if (a.spec != b.spec) return a.spec < b.spec;
            return a.order < b.order;
        });

        // 1) CSS degiskenleri, 2) font boyutu (em icin), 3) geri kalan
        std::shared_ptr<VarMap> nv;
        for (auto& m : ms) {
            if (!StartsWith(m.d->prop, L"--")) continue;
            if (!nv) nv = std::make_shared<VarMap>(s.vars ? *s.vars : VarMap());
            if (nv->size() < 4096) (*nv)[m.d->prop] = m.d->value;
        }
        if (nv) s.vars = nv;
        for (auto& m : ms)
            if (m.d->prop == L"font-size" || m.d->prop == L"font") ApplyDecl(s, ps, m.d->prop, m.d->value);
        for (auto& m : ms) {
            const std::wstring& p = m.d->prop;
            if (StartsWith(p, L"--") || p == L"font-size" || p == L"font") continue;
            ApplyDecl(s, ps, p, m.d->value);
        }
        PostFix(s);
    }

    // Erisilebilirlik icin gizlenmis ("sr-only") ve ekran disi ogeleri gizle
    static void PostFix(Style& s) {
        if (s.invisible) { s.display = Style::None; return; }
        if (s.position >= 2) {
            bool tiny = (s.width.u == Len::Px && s.width.v <= 1.5f) || (s.height.u == Len::Px && s.height.v <= 1.5f);
            if (tiny || s.hiddenClip || s.offscreen) { s.display = Style::None; return; }
        }
        if (s.offscreen && s.overflowHidden) { s.display = Style::None; return; }
        if (s.overflowHidden && s.height.u == Len::Px && s.height.v <= 0.5f) s.display = Style::None;
    }
};

} // namespace

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
            r = hv(h[0]) * 17; g = hv(h[1]) * 17; b = hv(h[2]) * 17;
            if (h.size() == 4) a = hv(h[3]) * 17;
        } else if (h.size() == 6 || h.size() == 8) {
            r = hv(h[0]) * 16 + hv(h[1]); g = hv(h[2]) * 16 + hv(h[3]); b = hv(h[4]) * 16 + hv(h[5]);
            if (h.size() == 8) a = hv(h[6]) * 16 + hv(h[7]);
        } else return false;
        out = (a << 24) | (r << 16) | (g << 8) | b;
        return true;
    }
    if (StartsWith(v, L"rgb") || StartsWith(v, L"hsl")) {
        size_t a = v.find(L'('), b = v.rfind(L')');
        if (a == std::wstring::npos) return false;
        if (b == std::wstring::npos || b < a) b = v.size();
        std::wstring in = v.substr(a + 1, b - a - 1);
        for (auto& c : in) if (c == L',' || c == L'/') c = L' ';
        auto toks = SplitWS(in);
        if (toks.size() < 3) return false;
        float vals[4] = {0, 0, 0, 1};
        bool pct[4] = {false, false, false, false};
        for (size_t i = 0; i < toks.size() && i < 4; i++) {
            float f; std::wstring u;
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
    for (auto& c : kColors) if (v == c.n) { out = 0xFF000000 | c.c; return true; }
    return false;
}

std::vector<Style> ComputeStyles(const Document& doc, const StyleSheet& sheet, bool scripting) {
    std::vector<Style> out(size_t(doc.count) + 1);
    Computer c(doc, sheet, out, scripting);
    c.Run();
    return out;
}

} // namespace mer
