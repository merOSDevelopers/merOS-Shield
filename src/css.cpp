// merOS Shield - MerEngine : CSS ayrıştırıcı ve seçici eşleştirme
#include "css.h"
#include "url.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>

namespace mer {

namespace {

constexpr size_t kMaxRules = 80000;

std::wstring StripComments(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'/' && i + 1 < s.size() && s[i + 1] == L'*') {
            size_t e = s.find(L"*/", i + 2);
            i = (e == std::wstring::npos) ? s.size() : e + 2;
            continue;
        }
        if (s[i] == L'"' || s[i] == L'\'') {
            wchar_t q = s[i];
            size_t e = i + 1;
            while (e < s.size() && s[e] != q && s[e] != L'\n') { if (s[e] == L'\\') e++; e++; }
            if (e >= s.size()) e = s.size() - 1;
            o.append(s, i, e - i + 1);
            i = e + 1;
            continue;
        }
        o += s[i++];
    }
    return o;
}

size_t MatchBrace(const std::wstring& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'"' || c == L'\'') {
            size_t e = i + 1;
            while (e < s.size() && s[e] != c) { if (s[e] == L'\\') e++; e++; }
            i = e;
            continue;
        }
        if (c == L'{') depth++;
        else if (c == L'}') { if (--depth == 0) return i; }
    }
    return std::wstring::npos;
}

// s[i] == '(' iken dengeli parantez icini dondurur, i ')' sonrasina gecer. Hata: false
bool ReadParen(const std::wstring& s, size_t& i, std::wstring& inner) {
    if (i >= s.size() || s[i] != L'(') return false;
    int depth = 0;
    size_t st = i + 1;
    for (size_t k = i; k < s.size(); k++) {
        wchar_t c = s[k];
        if (c == L'"' || c == L'\'') {
            size_t e = k + 1;
            while (e < s.size() && s[e] != c) { if (s[e] == L'\\') e++; e++; }
            k = e;
            continue;
        }
        if (c == L'(') depth++;
        else if (c == L')') {
            if (--depth == 0) { inner = s.substr(st, k - st); i = k + 1; return true; }
        }
    }
    return false;
}

// Ust duzey virgullere gore bol (parantez/koseli parantez/tirnak icindekiler haric)
std::vector<std::wstring> SplitTopLevel(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    int paren = 0, br = 0;
    size_t st = 0;
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'"' || c == L'\'') {
            size_t e = i + 1;
            while (e < s.size() && s[e] != c) { if (s[e] == L'\\') e++; e++; }
            i = std::min(e, s.size());
            continue;
        }
        if (c == L'(') paren++;
        else if (c == L')' && paren > 0) paren--;
        else if (c == L'[') br++;
        else if (c == L']' && br > 0) br--;
        else if (c == sep && paren == 0 && br == 0) { out.push_back(s.substr(st, i - st)); st = i + 1; }
    }
    out.push_back(s.substr(st));
    return out;
}

// ---------------- Medya sorgulari ----------------

bool MediaLen(const std::wstring& t, float& px) {
    std::wstring v = Trim(t);
    if (v.empty()) return false;
    const wchar_t* b = v.c_str();
    wchar_t* e = nullptr;
    float f = wcstof(b, &e);
    if (e == b || !std::isfinite(f)) return false;
    std::wstring u = ToLower(Trim(std::wstring(e)));
    if (u.empty() || u == L"px") px = f;
    else if (u == L"em" || u == L"rem") px = f * 16;
    else if (u == L"pt") px = f * 4.f / 3.f;
    else if (u == L"cm") px = f * 37.8f;
    else if (u == L"mm") px = f * 3.78f;
    else if (u == L"in") px = f * 96;
    else if (u == L"ch" || u == L"ex") px = f * 8;
    else return false;
    return true;
}

bool MediaNum(const std::wstring& t, float& out) {
    std::wstring v = ToLower(Trim(t));
    if (v.empty()) return false;
    size_t slash = v.find(L'/');
    if (slash != std::wstring::npos) {
        float a = float(_wtof(v.substr(0, slash).c_str())), b = float(_wtof(v.substr(slash + 1).c_str()));
        if (b <= 0) return false;
        out = a / b;
        return true;
    }
    const wchar_t* b = v.c_str();
    wchar_t* e = nullptr;
    float f = wcstof(b, &e);
    if (e == b) return false;
    std::wstring u = Trim(std::wstring(e));
    if (u == L"dpi") f /= 96.f;
    else if (u == L"dpcm") f = f * 2.54f / 96.f;
    out = f;
    return true;
}

bool Compare(float a, const std::wstring& op, float b) {
    if (op == L"<") return a < b;
    if (op == L"<=") return a <= b + 0.001f;
    if (op == L">") return a > b;
    if (op == L">=") return a >= b - 0.001f;
    return std::fabs(a - b) < 0.01f;
}

bool MediaFeature(const std::wstring& inRaw, float vw, float vh);

// "(a) and (b)", "not (a)", "(a) or (b)" kosullari
bool MediaCondition(const std::wstring& s, float vw, float vh) {
    size_t i = 0;
    bool result = true, first = true;
    std::wstring joiner = L"and";
    while (i < s.size()) {
        while (i < s.size() && IsWS(s[i])) i++;
        if (i >= s.size()) break;
        bool neg = false;
        if (s.compare(i, 3, L"not") == 0 && (i + 3 >= s.size() || IsWS(s[i + 3]) || s[i + 3] == L'(')) {
            neg = true;
            i += 3;
            while (i < s.size() && IsWS(s[i])) i++;
        }
        bool v;
        if (i < s.size() && s[i] == L'(') {
            std::wstring inner;
            if (!ReadParen(s, i, inner)) return false;
            std::wstring t = Trim(inner);
            if (!t.empty() && (t[0] == L'(' || StartsWith(t, L"not "))) v = MediaCondition(t, vw, vh);
            else v = MediaFeature(t, vw, vh);
        } else {
            // medya turu
            size_t st = i;
            while (i < s.size() && !IsWS(s[i]) && s[i] != L'(') i++;
            std::wstring w = s.substr(st, i - st);
            if (w == L"only") continue;
            if (w == L"and" || w == L"or") { joiner = w; continue; }
            v = (w == L"all" || w == L"screen");
        }
        if (neg) v = !v;
        if (first) result = v;
        else if (joiner == L"or") result = result || v;
        else result = result && v;
        first = false;
        joiner = L"and";
    }
    return result;
}

bool MediaFeature(const std::wstring& inRaw, float vw, float vh) {
    std::wstring in = ToLower(Trim(inRaw));
    // Aralik sozdizimi: "width >= 600px", "400px <= width < 800px"
    for (const wchar_t* dim : {L"width", L"height", L"aspect-ratio", L"resolution"}) {
        size_t pos = in.find(dim);
        if (pos == std::wstring::npos || in.find(L':') != std::wstring::npos) continue;
        if (pos > 0 && (iswalpha(in[pos - 1]) || in[pos - 1] == L'-')) continue;
        float val = std::wcscmp(dim, L"width") == 0 ? vw : std::wcscmp(dim, L"height") == 0 ? vh :
                    std::wcscmp(dim, L"aspect-ratio") == 0 ? vw / std::max(1.f, vh) : 1.f;
        bool isLen = std::wcscmp(dim, L"width") == 0 || std::wcscmp(dim, L"height") == 0;
        auto readOp = [](const std::wstring& t, size_t& k) {
            std::wstring op;
            while (k < t.size() && IsWS(t[k])) k++;
            while (k < t.size() && (t[k] == L'<' || t[k] == L'>' || t[k] == L'=')) op += t[k++];
            return op;
        };
        std::wstring left = Trim(in.substr(0, pos)), right = Trim(in.substr(pos + wcslen(dim)));
        bool ok = true;
        if (!right.empty()) {
            size_t k = 0;
            std::wstring op = readOp(right, k);
            float b;
            if (op.empty() || !(isLen ? MediaLen(right.substr(k), b) : MediaNum(right.substr(k), b))) return false;
            ok = ok && Compare(val, op, b);
        }
        if (!left.empty()) {
            // "400px <=" : sondaki operatoru ayir
            size_t k = left.size();
            while (k > 0 && (left[k - 1] == L'<' || left[k - 1] == L'>' || left[k - 1] == L'=' || IsWS(left[k - 1]))) k--;
            std::wstring op = Trim(left.substr(k));
            float a;
            if (op.empty() || !(isLen ? MediaLen(left.substr(0, k), a) : MediaNum(left.substr(0, k), a))) return false;
            // a op val
            if (op == L"<") ok = ok && a < val;
            else if (op == L"<=") ok = ok && a <= val + 0.001f;
            else if (op == L">") ok = ok && a > val;
            else if (op == L">=") ok = ok && a >= val - 0.001f;
            else ok = ok && std::fabs(a - val) < 0.01f;
        }
        return ok;
    }
    size_t c = in.find(L':');
    std::wstring name = Trim(c == std::wstring::npos ? in : in.substr(0, c));
    std::wstring value = c == std::wstring::npos ? L"" : Trim(in.substr(c + 1));
    float n;
    if (name == L"min-width") return MediaLen(value, n) && vw >= n - 0.001f;
    if (name == L"max-width") return MediaLen(value, n) && vw <= n + 0.001f;
    if (name == L"width") return MediaLen(value, n) && std::fabs(vw - n) < 0.5f;
    if (name == L"min-height") return MediaLen(value, n) && vh >= n - 0.001f;
    if (name == L"max-height") return MediaLen(value, n) && vh <= n + 0.001f;
    if (name == L"min-device-width" || name == L"min-device-height") return MediaLen(value, n) && 1920 >= n;
    if (name == L"max-device-width" || name == L"max-device-height") return MediaLen(value, n) && 1080 <= n;
    if (name == L"orientation") return value == (vw >= vh ? L"landscape" : L"portrait");
    if (name == L"min-aspect-ratio") return MediaNum(value, n) && vw / std::max(1.f, vh) >= n;
    if (name == L"max-aspect-ratio") return MediaNum(value, n) && vw / std::max(1.f, vh) <= n;
    if (name == L"prefers-color-scheme") return value == L"light";
    if (name == L"prefers-reduced-motion" || name == L"prefers-reduced-transparency" || name == L"prefers-contrast" || name == L"prefers-reduced-data")
        return value == L"no-preference";
    if (name == L"forced-colors" || name == L"inverted-colors") return value == L"none";
    if (name == L"hover" || name == L"any-hover") return value.empty() || value == L"hover";
    if (name == L"pointer" || name == L"any-pointer") return value.empty() || value == L"fine";
    if (name == L"display-mode") return value == L"browser";
    if (name == L"scripting") return value == L"enabled" || value == L"initial-only";
    if (name == L"update") return value.empty() || value == L"fast";
    if (name == L"color" || name == L"min-color") return true;
    if (name == L"monochrome") return value == L"0";
    if (name == L"grid") return value == L"0";
    if (name == L"-webkit-min-device-pixel-ratio" || name == L"min-resolution" || name == L"min--moz-device-pixel-ratio")
        return MediaNum(value, n) && n <= 1.01f;
    if (name == L"-webkit-max-device-pixel-ratio" || name == L"max-resolution") return MediaNum(value, n) && n >= 1.f;
    if (name == L"resolution" || name == L"-webkit-device-pixel-ratio") return MediaNum(value, n) && std::fabs(n - 1) < 0.01f;
    return false; // bilinmeyen ozellik: eslesmez (tarayicilar da boyle yapar)
}

// ---------------- @supports ----------------

bool PropertyKnown(const std::wstring& p) {
    static const wchar_t* known[] = {
        L"display", L"position", L"color", L"background", L"background-color", L"background-image", L"margin", L"padding",
        L"width", L"height", L"min-width", L"max-width", L"min-height", L"max-height", L"border", L"border-radius",
        L"flex", L"flex-direction", L"flex-wrap", L"gap", L"row-gap", L"column-gap", L"grid-template-columns",
        L"grid-template-areas", L"grid-template-rows", L"grid-area", L"grid-column", L"grid-row", L"justify-content",
        L"align-items", L"float", L"clear", L"opacity", L"visibility", L"overflow", L"font-size", L"font-weight",
        L"text-align", L"line-height", L"white-space", L"top", L"left", L"right", L"bottom", L"inset", L"transform",
        L"mask-image", L"-webkit-mask-image", L"mask", L"-webkit-mask", L"aspect-ratio", L"box-sizing", L"order",
        L"text-transform", L"text-decoration", L"vertical-align", L"content", L"list-style", L"place-items",
        L"inset-inline-start", L"margin-inline", L"padding-inline", L"margin-block", L"padding-block"};
    for (auto k : known) if (p == k) return true;
    return false;
}

bool SupportsCond(const std::wstring& sIn, int depth) {
    std::wstring s = ToLower(Trim(sIn));
    if (depth > 8 || s.empty()) return false;
    size_t i = 0;
    bool result = true, first = true;
    std::wstring joiner = L"and";
    while (i < s.size()) {
        while (i < s.size() && IsWS(s[i])) i++;
        if (i >= s.size()) break;
        bool neg = false;
        if (s.compare(i, 3, L"not") == 0 && (i + 3 < s.size()) && (IsWS(s[i + 3]) || s[i + 3] == L'(')) {
            neg = true;
            i += 3;
            while (i < s.size() && IsWS(s[i])) i++;
        }
        if (s.compare(i, 3, L"and") == 0 && i + 3 < s.size() && (IsWS(s[i + 3]) || s[i + 3] == L'(')) { joiner = L"and"; i += 3; continue; }
        if (s.compare(i, 2, L"or") == 0 && i + 2 < s.size() && (IsWS(s[i + 2]) || s[i + 2] == L'(')) { joiner = L"or"; i += 2; continue; }
        bool v = false;
        if (s.compare(i, 9, L"selector(") == 0) {
            i += 8;
            std::wstring inner;
            if (!ReadParen(s, i, inner)) return false;
            std::vector<Selector> sl;
            v = ParseSelectorList(inner, sl);
        } else if (s[i] == L'(') {
            std::wstring inner;
            if (!ReadParen(s, i, inner)) return false;
            std::wstring t = Trim(inner);
            size_t c = t.find(L':');
            if (!t.empty() && t[0] == L'(') v = SupportsCond(t, depth + 1);
            else if (StartsWith(t, L"not ") || StartsWith(t, L"not(")) v = SupportsCond(t, depth + 1);
            else if (c != std::wstring::npos) {
                std::wstring prop = Trim(t.substr(0, c)), val = Trim(t.substr(c + 1));
                v = PropertyKnown(prop);
                // Desteklemedigimiz degerler
                if (v && (val.find(L"subgrid") != std::wstring::npos || val.find(L"masonry") != std::wstring::npos ||
                          val.find(L"anchor") != std::wstring::npos || val.find(L"color-mix") != std::wstring::npos ||
                          val.find(L"contents") != std::wstring::npos || val.find(L"oklch") != std::wstring::npos ||
                          val.find(L"lab(") != std::wstring::npos || val.find(L"backdrop") != std::wstring::npos ||
                          val.find(L"-webkit-box") != std::wstring::npos || val.find(L"sticky") != std::wstring::npos))
                    v = false;
            }
        } else {
            // tanimadigimiz sozdizimi (font-tech() vb.)
            size_t st = i;
            while (i < s.size() && s[i] != L'(' && !IsWS(s[i])) i++;
            if (i < s.size() && s[i] == L'(') { std::wstring inner; ReadParen(s, i, inner); }
            if (i == st) i++;
            v = false;
        }
        if (neg) v = !v;
        if (first) result = v;
        else if (joiner == L"or") result = result || v;
        else result = result && v;
        first = false;
    }
    return result;
}

// ---------------- Secici ayristirma ----------------

bool IdentChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c > 127;
}

std::wstring ReadIdent(const std::wstring& s, size_t& i) {
    std::wstring o;
    while (i < s.size()) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            // CSS kacisi: \31 0 (hex) ya da \: gibi
            size_t k = i + 1;
            uint32_t cp = 0;
            int nd = 0;
            while (k < s.size() && nd < 6 && iswxdigit(s[k])) {
                wchar_t c = s[k];
                cp = cp * 16 + uint32_t(c <= L'9' ? c - L'0' : (towlower(c) - L'a' + 10));
                k++; nd++;
            }
            if (nd > 0) {
                if (k < s.size() && IsWS(s[k])) k++;
                AppendCodepoint(o, cp ? cp : 0xFFFD);
                i = k;
            } else {
                o += s[i + 1];
                i += 2;
            }
            continue;
        }
        if (!IdentChar(s[i])) break;
        o += s[i++];
    }
    return o;
}

bool ParseSelector(const std::wstring& text, Selector& out, bool relative = false);
int SpecOf(const Selector& s);

bool ParseNth(const std::wstring& argIn, NthSel& n) {
    std::wstring a = ToLower(Trim(argIn));
    std::wstring t;
    for (wchar_t c : a) if (!IsWS(c)) t += c;
    if (t == L"odd") { n.a = 2; n.b = 1; return true; }
    if (t == L"even") { n.a = 2; n.b = 0; return true; }
    size_t np = t.find(L'n');
    if (np == std::wstring::npos) {
        if (t.empty()) return false;
        wchar_t* e = nullptr;
        long v = wcstol(t.c_str(), &e, 10);
        if (*e) return false;
        n.a = 0; n.b = int(v);
        return true;
    }
    std::wstring as = t.substr(0, np), bs = t.substr(np + 1);
    if (as.empty() || as == L"+") n.a = 1;
    else if (as == L"-") n.a = -1;
    else { wchar_t* e = nullptr; long v = wcstol(as.c_str(), &e, 10); if (*e) return false; n.a = int(v); }
    if (bs.empty()) n.b = 0;
    else { wchar_t* e = nullptr; long v = wcstol(bs.c_str(), &e, 10); if (*e) return false; n.b = int(v); }
    return true;
}

// Bagislayici (forgiving) liste: hatali ogeler atlanir
std::vector<Selector> ParseForgiving(const std::wstring& arg, bool relative) {
    std::vector<Selector> out;
    for (auto& part : SplitTopLevel(arg, L',')) {
        Selector s;
        if (ParseSelector(part, s, relative) && s.pseudo == 0) out.push_back(std::move(s));
    }
    return out;
}

// Tek bir bilesik secici parcasi (etiket, .sinif, #id, [oznitelik], :sozde-sinif, ::sozde-oge)
bool ParseCompoundPart(const std::wstring& s, size_t& i, Compound& cur, uint8_t& pseudo, int depth) {
    wchar_t c = s[i];
    if (pseudo || cur.slotted) return false; // sozde ogeden sonra baska bir sey gelemez
    if (c == L'*') { i++; if (i < s.size() && s[i] == L'|') { i++; } return true; }
    if (c == L'.') { i++; std::wstring id = ReadIdent(s, i); if (id.empty()) return false; cur.classes.push_back(id); return true; }
    if (c == L'#') { i++; std::wstring id = ReadIdent(s, i); if (id.empty()) return false; cur.id = id; return true; }
    if (c == L'[') {
        i++;
        while (i < s.size() && IsWS(s[i])) i++;
        AttrSel a;
        a.name = ToLower(ReadIdent(s, i));
        if (a.name.empty()) return false;
        while (i < s.size() && IsWS(s[i])) i++;
        if (i < s.size() && s[i] != L']') {
            wchar_t op = s[i];
            if (op == L'=') { a.op = L'='; i++; }
            else if ((op == L'^' || op == L'$' || op == L'*' || op == L'~' || op == L'|') && i + 1 < s.size() && s[i + 1] == L'=') { a.op = op; i += 2; }
            else return false;
            while (i < s.size() && IsWS(s[i])) i++;
            if (i < s.size() && (s[i] == L'"' || s[i] == L'\'')) {
                wchar_t q = s[i++];
                while (i < s.size() && s[i] != q) { if (s[i] == L'\\' && i + 1 < s.size()) i++; a.value += s[i++]; }
                if (i >= s.size()) return false;
                i++;
            } else {
                a.value = ReadIdent(s, i);
            }
            while (i < s.size() && IsWS(s[i])) i++;
            if (i < s.size() && (s[i] == L'i' || s[i] == L'I' || s[i] == L's' || s[i] == L'S')) {
                a.icase = (s[i] == L'i' || s[i] == L'I');
                i++;
                while (i < s.size() && IsWS(s[i])) i++;
            }
        }
        if (i >= s.size() || s[i] != L']') return false;
        i++;
        cur.attrs.push_back(a);
        return true;
    }
    if (c == L':') {
        i++;
        bool element = false;
        if (i < s.size() && s[i] == L':') { element = true; i++; }
        std::wstring ps = ToLower(ReadIdent(s, i));
        if (ps.empty()) return false;
        if (element && ps == L"slotted") {
            if (i >= s.size() || s[i] != L'(' || depth > 6) return false;
            std::wstring arg;
            if (!ReadParen(s, i, arg)) return false;
            for (auto& part : SplitTopLevel(arg, L',')) {
                Selector sub;
                if (!ParseSelector(part, sub) || sub.pseudo) return false;
                cur.slottedArgs.push_back(std::move(sub));
            }
            if (cur.slottedArgs.empty()) return false;
            cur.slotted = true;
            return true;
        }
        if (element || ps == L"before" || ps == L"after" || ps == L"first-line" || ps == L"first-letter") {
            if (ps == L"before") { pseudo = 1; return true; }
            if (ps == L"after") { pseudo = 2; return true; }
            return false; // ::placeholder, ::selection, ::marker ... uygulanmaz
        }
        if (i < s.size() && s[i] == L'(') {
            std::wstring arg;
            if (!ReadParen(s, i, arg)) return false;
            if (depth > 6) return false;
            if (ps == L"not") {
                for (auto& part : SplitTopLevel(arg, L',')) {
                    Selector sub;
                    if (!ParseSelector(part, sub) || sub.pseudo) return false;
                    cur.nots.push_back(std::move(sub));
                }
                return true;
            }
            if (ps == L"is" || ps == L"where" || ps == L"matches" || ps == L"-webkit-any" || ps == L"-moz-any" || ps == L"any") {
                auto list = ParseForgiving(arg, false);
                if (list.empty()) { cur.never = true; return true; }
                if (ps == L"where") for (auto& x : list) x.spec = 0;
                cur.iss.push_back(std::move(list));
                return true;
            }
            if (ps == L"has") {
                auto list = ParseForgiving(arg, true);
                if (list.empty()) { cur.never = true; return true; }
                cur.has.push_back(std::move(list));
                return true;
            }
            if (ps == L"nth-child" || ps == L"nth-last-child" || ps == L"nth-of-type" || ps == L"nth-last-of-type") {
                NthSel n;
                std::wstring a = arg;
                if (ToLower(a).find(L" of ") != std::wstring::npos) return false;
                if (!ParseNth(a, n)) return false;
                n.kind = ps == L"nth-child" ? 0 : ps == L"nth-last-child" ? 1 : ps == L"nth-of-type" ? 2 : 3;
                cur.nths.push_back(n);
                return true;
            }
            if (ps == L"dir") { if (ToLower(Trim(arg)) != L"ltr") cur.never = true; return true; }
            if (ps == L"lang") {
                AttrSel a;
                a.name = L"lang"; a.op = L'|'; a.value = ToLower(Trim(arg)); a.icase = true;
                cur.attrs.push_back(a);
                return true;
            }
            if (ps == L"host" || ps == L"host-context") {
                for (auto& part : SplitTopLevel(arg, L',')) {
                    Selector sub;
                    if (!ParseSelector(part, sub) || sub.pseudo) return false;
                    (ps == L"host" ? cur.hostArgs : cur.hostCtx).push_back(std::move(sub));
                }
                cur.host = true;
                return true;
            }
            return false; // :state() ...
        }
        if (ps == L"link" || ps == L"any-link" || ps == L"-webkit-any-link") { AttrSel a; a.name = L"href"; cur.attrs.push_back(a); return true; }
        if (ps == L"root" || ps == L"scope") { cur.root = true; return true; }
        if (ps == L"first-child") { cur.firstChild = true; return true; }
        if (ps == L"last-child") { cur.lastChild = true; return true; }
        if (ps == L"only-child") { cur.onlyChild = true; return true; }
        if (ps == L"first-of-type") { cur.firstType = true; return true; }
        if (ps == L"last-of-type") { cur.lastType = true; return true; }
        if (ps == L"only-of-type") { cur.onlyType = true; return true; }
        if (ps == L"empty") { cur.empty = true; return true; }
        if (ps == L"checked") { cur.checked = true; return true; }
        if (ps == L"disabled") { cur.disabled = true; return true; }
        if (ps == L"enabled") { cur.enabled = true; return true; }
        if (ps == L"placeholder-shown") { cur.placeholderShown = true; return true; }
        if (ps == L"required") { AttrSel a; a.name = L"required"; cur.attrs.push_back(a); return true; }
        if (ps == L"defined") { cur.defined = true; return true; }
        if (ps == L"host") { cur.host = true; return true; }
        // Etkilesim durumlari: statik cizimde hicbir zaman eslesmez
        if (ps == L"hover" || ps == L"active" || ps == L"focus" || ps == L"focus-visible" || ps == L"focus-within" ||
            ps == L"visited" || ps == L"target" || ps == L"indeterminate" || ps == L"invalid" || ps == L"user-invalid" ||
            ps == L"fullscreen" || ps == L"modal" || ps == L"popover-open" || ps == L"autofill" || ps == L"-webkit-autofill" ||
            ps == L"open" || ps == L"playing" || ps == L"paused" || ps == L"read-only" || ps == L"out-of-range") {
            cur.never = true;
            return true;
        }
        if (ps == L"valid" || ps == L"optional" || ps == L"read-write" || ps == L"in-range" || ps == L"closed") return true;
        return false;
    }
    if (IdentChar(c) || c == L'\\') { cur.tag = ToLower(ReadIdent(s, i)); if (i < s.size() && s[i] == L'|') { i++; cur.tag = ToLower(ReadIdent(s, i)); } return true; }
    return false;
}

int SpecOf(const Selector& s) {
    int spec = 0;
    for (auto& p : s.parts) {
        if (!p.id.empty()) spec += 10000;
        spec += 100 * int(p.classes.size() + p.attrs.size() + p.nths.size()) +
                100 * ((p.root ? 1 : 0) + (p.firstChild ? 1 : 0) + (p.lastChild ? 1 : 0) + (p.onlyChild ? 1 : 0) + (p.empty ? 1 : 0) +
                       (p.firstType ? 1 : 0) + (p.lastType ? 1 : 0) + (p.onlyType ? 1 : 0) + (p.checked ? 1 : 0) +
                       (p.disabled ? 1 : 0) + (p.enabled ? 1 : 0) + (p.placeholderShown ? 1 : 0));
        if (!p.tag.empty()) spec += 1;
        int notMax = 0;
        for (auto& n : p.nots) notMax = std::max(notMax, n.spec);
        spec += notMax;
        for (auto& l : p.iss) { int mx = 0; for (auto& x : l) mx = std::max(mx, x.spec); spec += mx; }
        for (auto& l : p.has) { int mx = 0; for (auto& x : l) mx = std::max(mx, x.spec); spec += mx; }
        if (p.host) {
            spec += 100;
            int mx = 0;
            for (auto& x : p.hostArgs) mx = std::max(mx, x.spec);
            for (auto& x : p.hostCtx) mx = std::max(mx, x.spec);
            spec += mx;
        }
        if (p.slotted) {
            int mx = 0;
            for (auto& x : p.slottedArgs) mx = std::max(mx, x.spec);
            spec += 1 + mx;
        }
    }
    return spec;
}

bool ParseSelector(const std::wstring& text, Selector& out, bool relative) {
    std::wstring s = Trim(text);
    if (s.empty()) return false;
    Compound cur;
    bool have = false;
    wchar_t nextComb = L' ';
    uint8_t pseudo = 0;
    auto finalize = [&]() { if (have) { out.parts.push_back(std::move(cur)); cur = Compound(); have = false; } };
    size_t i = 0;
    // :has() icindeki goreli secici: "> .a" ya da "+ .b"
    if (relative && (s[0] == L'>' || s[0] == L'+' || s[0] == L'~')) { nextComb = s[0]; i = 1; }
    while (i < s.size()) {
        wchar_t c = s[i];
        if (IsWS(c)) { finalize(); i++; continue; }
        if (c == L'>' || c == L'+' || c == L'~') {
            finalize();
            if (out.parts.empty() && !relative) return false;
            nextComb = c;
            i++;
            continue;
        }
        if (!have) { cur.comb = nextComb; nextComb = L' '; have = true; }
        if (!ParseCompoundPart(s, i, cur, pseudo, 0)) return false;
    }
    finalize();
    if (out.parts.empty() || out.parts.size() > 32) return false;
    out.pseudo = pseudo;
    out.spec = SpecOf(out);
    if (pseudo) out.spec += 1;
    return true;
}

// ---------------- Eslestirme ----------------

const Node* PrevElem(const Node* n) {
    const Node* p = n->parent;
    if (!p) return nullptr;
    const Node* prev = nullptr;
    for (auto& k : p->kids) {
        if (k.get() == n) return prev;
        if (k->kind == Node::Elem) prev = k.get();
    }
    return nullptr;
}
const Node* NextElem(const Node* n) {
    const Node* p = n->parent;
    if (!p) return nullptr;
    bool seen = false;
    for (auto& k : p->kids) {
        if (seen && k->kind == Node::Elem) return k.get();
        if (k.get() == n) seen = true;
    }
    return nullptr;
}

bool AttrMatch(const AttrSel& a, const Node* n) {
    const std::wstring* v = nullptr;
    for (auto& at : n->attrs) if (at.first == a.name) { v = &at.second; break; }
    if (!v) return false;
    if (!a.op) return true;
    std::wstring val = a.icase ? ToLower(*v) : *v;
    std::wstring want = a.icase ? ToLower(a.value) : a.value;
    switch (a.op) {
    case L'=': return val == want;
    case L'^': return !want.empty() && StartsWith(val, want);
    case L'$': return !want.empty() && EndsWith(val, want);
    case L'*': return !want.empty() && val.find(want) != std::wstring::npos;
    case L'~': for (auto& w : SplitWS(val)) if (w == want) return true; return false;
    case L'|': return val == want || StartsWith(val, want + L"-");
    }
    return false;
}

// Kardesler arasinda 1 tabanli sira (ofType: ayni etiketliler)
int SiblingIndex(const Node* n, bool fromEnd, bool ofType) {
    const Node* p = n->parent;
    if (!p) return 1;
    int idx = 0, found = 0;
    if (!fromEnd) {
        for (auto& k : p->kids) {
            if (k->kind != Node::Elem || (ofType && k->name != n->name)) continue;
            idx++;
            if (k.get() == n) { found = idx; break; }
        }
    } else {
        for (size_t i = p->kids.size(); i-- > 0;) {
            const Node* k = p->kids[i].get();
            if (k->kind != Node::Elem || (ofType && k->name != n->name)) continue;
            idx++;
            if (k == n) { found = idx; break; }
        }
    }
    return found;
}

bool IsFormElem(const Node* n) {
    const std::wstring& t = n->name;
    return t == L"input" || t == L"button" || t == L"select" || t == L"textarea" || t == L"option" || t == L"fieldset" || t == L"optgroup";
}

bool MatchFrom(const Selector& s, int idx, const Node* n, int& budget, const Node* anchor);
bool Matches2(const Selector& sel, const Node* n, int& budget, const Node* anchor);

// :has() goreli secicisi: anchor'a gore
bool HasMatch(const Selector& sel, const Node* anchor, int& budget) {
    if (sel.parts.empty()) return false;
    wchar_t rel = sel.parts[0].comb;
    std::vector<const Node*> st;
    if (rel == L'+' || rel == L'~') {
        const Node* p = anchor->parent;
        if (!p) return false;
        bool seen = false;
        for (auto& k : p->kids) {
            if (seen) st.push_back(k.get());
            if (k.get() == anchor) seen = true;
        }
    } else {
        for (auto& k : anchor->kids) st.push_back(k.get());
    }
    while (!st.empty() && budget > 0) {
        const Node* c = st.back();
        st.pop_back();
        budget--;
        if (c->kind != Node::Elem) continue;
        if (Matches2(sel, c, budget, anchor)) return true;
        if (rel == L' ' || rel == L'>' || sel.parts.size() > 1)
            for (auto& k : c->kids) st.push_back(k.get());
    }
    return false;
}

bool MatchCompound(const Compound& c, const Node* n, int& budget) {
    if (n->kind != Node::Elem || c.never) return false;
    if (c.host) {
        if (!n->shadow) return false;
        if (!c.hostArgs.empty()) {
            bool any = false;
            for (auto& x : c.hostArgs) if (Matches2(x, n, budget, nullptr)) { any = true; break; }
            if (!any) return false;
        }
        if (!c.hostCtx.empty()) {
            bool any = false;
            for (const Node* p = n; p && !any; p = ComposedParent(p)) {
                if (p->kind != Node::Elem) continue;
                for (auto& x : c.hostCtx) if (Matches2(x, p, budget, nullptr)) { any = true; break; }
            }
            if (!any) return false;
        }
    }
    if (c.defined && n->name.find(L'-') != std::wstring::npos && n->ceState != 1) return false;
    if (!c.tag.empty() && c.tag != n->name) return false;
    if (!c.id.empty() && c.id != n->idAttr) return false;
    if (c.root && n->name != L"html") return false;
    for (auto& cl : c.classes) {
        bool f = false;
        for (auto& x : n->classes) if (x == cl) { f = true; break; }
        if (!f) return false;
    }
    for (auto& a : c.attrs) if (!AttrMatch(a, n)) return false;
    if ((c.firstChild || c.onlyChild) && PrevElem(n)) return false;
    if ((c.lastChild || c.onlyChild) && NextElem(n)) return false;
    if ((c.firstType || c.onlyType) && SiblingIndex(n, false, true) != 1) return false;
    if ((c.lastType || c.onlyType) && SiblingIndex(n, true, true) != 1) return false;
    if (c.empty) {
        for (auto& k : n->kids) if (k->kind == Node::Elem || (k->kind == Node::Text && !k->text.empty())) return false;
    }
    for (auto& nth : c.nths) {
        int idx = SiblingIndex(n, nth.kind == 1 || nth.kind == 3, nth.kind >= 2);
        if (nth.a == 0) { if (idx != nth.b) return false; }
        else {
            int d = idx - nth.b;
            if (d % nth.a != 0 || d / nth.a < 0) return false;
        }
    }
    if (c.checked) {
        bool on = false;
        if (n->name == L"input") on = n->dirtyChecked ? n->checked : n->Attr(L"checked") != nullptr;
        else if (n->name == L"option") on = n->Attr(L"selected") != nullptr;
        if (!on) return false;
    }
    if (c.disabled && !(IsFormElem(n) && n->Attr(L"disabled"))) return false;
    if (c.enabled && !(IsFormElem(n) && !n->Attr(L"disabled"))) return false;
    if (c.placeholderShown) {
        if ((n->name != L"input" && n->name != L"textarea") || !n->Attr(L"placeholder")) return false;
        std::wstring v = n->dirtyValue ? n->value : (n->Attr(L"value") ? *n->Attr(L"value") : L"");
        if (!v.empty()) return false;
    }
    for (auto& ns : c.nots) if (Matches2(ns, n, budget, nullptr)) return false;
    for (auto& l : c.iss) {
        bool any = false;
        for (auto& x : l) if (Matches2(x, n, budget, nullptr)) { any = true; break; }
        if (!any) return false;
    }
    for (auto& l : c.has) {
        bool any = false;
        for (auto& x : l) if (HasMatch(x, n, budget)) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

bool MatchFrom(const Selector& s, int idx, const Node* n, int& budget, const Node* anchor) {
    if (--budget <= 0 || !MatchCompound(s.parts[size_t(idx)], n, budget)) return false;
    wchar_t comb = s.parts[size_t(idx)].comb;
    if (idx == 0) {
        if (!anchor) return true;
        // :has() goreli secici: ilk parca anchor'a gore konumlanir
        if (comb == L'>') return n->parent == anchor;
        if (comb == L'+') return PrevElem(n) == anchor;
        if (comb == L'~') { for (const Node* p = PrevElem(n); p; p = PrevElem(p)) if (p == anchor) return true; return false; }
        for (const Node* p = n->parent; p; p = p->parent) if (p == anchor) return true;
        return false;
    }
    if (comb == L'>') {
        const Node* p = n->parent;
        // shadow sinirinda yalnizca :host parcasi host'a eslesebilir
        if (p && p->kind == Node::Shadow) return s.parts[size_t(idx - 1)].host && p->host && MatchFrom(s, idx - 1, p->host, budget, anchor);
        return p && p->kind == Node::Elem && MatchFrom(s, idx - 1, p, budget, anchor);
    }
    if (comb == L'+') {
        const Node* p = PrevElem(n);
        return p && MatchFrom(s, idx - 1, p, budget, anchor);
    }
    if (comb == L'~') {
        const Node* par = n->parent;
        if (!par) return false;
        for (auto& k : par->kids) {
            if (k.get() == n) break;
            if (k->kind == Node::Elem && MatchFrom(s, idx - 1, k.get(), budget, anchor)) return true;
        }
        return false;
    }
    for (const Node* p = n->parent; p; p = p->parent) {
        if (anchor && p == anchor) return false;
        if (p->kind == Node::Shadow) return s.parts[size_t(idx - 1)].host && p->host && MatchFrom(s, idx - 1, p->host, budget, anchor);
        if (p->kind == Node::Elem && MatchFrom(s, idx - 1, p, budget, anchor)) return true;
        if (budget <= 0) return false;
    }
    return false;
}

bool Matches2(const Selector& sel, const Node* n, int& budget, const Node* anchor) {
    if (sel.parts.empty()) return false;
    return MatchFrom(sel, int(sel.parts.size()) - 1, n, budget, anchor);
}

bool TopLevelNever(const Selector& s) {
    for (auto& p : s.parts) if (p.never) return true;
    return false;
}

const std::wstring* g_emptyStr = nullptr;

} // namespace

bool MediaMatches(const std::wstring& query, float vw, float vh) {
    std::wstring q = ToLower(Trim(query));
    if (q.empty()) return true;
    for (auto& part : SplitTopLevel(q, L',')) {
        std::wstring p = Trim(part);
        if (p.empty()) continue;
        bool neg = false;
        if (StartsWith(p, L"not ")) { neg = true; p = Trim(p.substr(4)); }
        bool v = MediaCondition(p, vw, vh);
        if (neg) v = !v;
        if (v) return true;
    }
    return false;
}

std::wstring ResolveCssUrls(const std::wstring& v, const std::wstring& base) {
    if (base.empty() || v.find(L"url(") == std::wstring::npos) return v;
    std::wstring out;
    size_t i = 0;
    while (i < v.size()) {
        size_t p = v.find(L"url(", i);
        if (p == std::wstring::npos) { out += v.substr(i); break; }
        out += v.substr(i, p - i);
        size_t j = p + 3;
        std::wstring inner;
        if (!ReadParen(v, j, inner)) { out += v.substr(p); break; }
        std::wstring u = Trim(inner);
        if (!u.empty() && (u[0] == L'"' || u[0] == L'\'')) {
            wchar_t q = u[0];
            size_t e = u.find(q, 1);
            u = u.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
        }
        std::wstring lu = ToLower(u.substr(0, std::min<size_t>(u.size(), 8)));
        if (!u.empty() && !StartsWith(lu, L"data:") && u[0] != L'#') u = ResolveUrl(base, u);
        std::wstring esc;
        for (wchar_t c : u) { if (c == L'"' || c == L'\\') esc += L'\\'; esc += c; }
        out += L"url(\"" + esc + L"\")";
        i = j;
    }
    return out;
}

bool Matches(const Selector& sel, const Node* n) {
    if (sel.parts.empty()) return false;
    int budget = 4000;
    return MatchFrom(sel, int(sel.parts.size()) - 1, n, budget, nullptr);
}

bool ParseSelectorList(const std::wstring& text, std::vector<Selector>& out) {
    for (auto& part : SplitTopLevel(text, L',')) {
        Selector sel;
        if (!ParseSelector(part, sel) || sel.pseudo) return false;
        out.push_back(std::move(sel));
    }
    return !out.empty();
}

std::vector<Decl> ParseDeclarations(const std::wstring& sIn) {
    std::vector<Decl> out;
    const std::wstring& s = sIn;
    size_t i = 0;
    while (i < s.size()) {
        size_t st = i;
        int paren = 0;
        while (i < s.size()) {
            wchar_t c = s[i];
            if (c == L'"' || c == L'\'') {
                size_t e = i + 1;
                while (e < s.size() && s[e] != c) { if (s[e] == L'\\') e++; e++; }
                i = e + 1;
                continue;
            }
            if (c == L'(') paren++;
            else if (c == L')' && paren > 0) paren--;
            else if (c == L';' && paren == 0) break;
            i++;
        }
        std::wstring d = s.substr(st, (i < s.size() ? i : s.size()) - st);
        i++;
        size_t colon = d.find(L':');
        if (colon == std::wstring::npos) continue;
        Decl dc;
        dc.prop = Trim(d.substr(0, colon));
        if (dc.prop.empty()) continue;
        if (!StartsWith(dc.prop, L"--")) dc.prop = ToLower(dc.prop);
        dc.value = Trim(d.substr(colon + 1));
        std::wstring low = ToLower(dc.value);
        size_t imp = low.rfind(L"!important");
        if (imp != std::wstring::npos && Trim(low.substr(imp + 10)).empty()) {
            dc.important = true;
            dc.value = Trim(dc.value.substr(0, imp));
        }
        if (out.size() < 512) out.push_back(std::move(dc));
    }
    return out;
}

void StyleSheet::AddRule(const std::wstring& selectorText, std::vector<Decl> decls) {
    if (rules.size() >= kMaxRules) return;
    store_.emplace_back(new std::vector<Decl>(std::move(decls)));
    const std::vector<Decl>* dp = store_.back().get();
    int order = order_++;
    for (auto& part : SplitTopLevel(selectorText, L',')) {
        Selector sel;
        if (!ParseSelector(part, sel) || TopLevelNever(sel)) continue;
        Rule r;
        r.sel = std::move(sel);
        r.decls = dp;
        r.order = order;
        rules.push_back(std::move(r));
    }
}

void StyleSheet::ParseList(const std::wstring& s, int depth, const std::wstring& base) {
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && (IsWS(s[i]) || s[i] == L';')) i++;
        if (i >= n) break;
        if (s[i] == L'}') { i++; continue; }
        if (s[i] == L'@') {
            size_t j = i;
            while (j < n && s[j] != L'{' && s[j] != L';') {
                if (s[j] == L'"' || s[j] == L'\'') { wchar_t q = s[j]; j++; while (j < n && s[j] != q) j++; }
                if (j < n) j++;
            }
            std::wstring preludeRaw = Trim(s.substr(i, std::min(j, n) - i));
            std::wstring prelude = ToLower(preludeRaw);
            if (j >= n) break;
            if (s[j] == L';') {
                if (StartsWith(prelude, L"@import") && importer && depth < 4) {
                    std::wstring rest = Trim(preludeRaw.substr(7));
                    std::wstring url;
                    size_t k = 0;
                    if (StartsWith(ToLower(rest), L"url(")) {
                        k = 3;
                        std::wstring inner;
                        if (ReadParen(rest, k, inner)) url = Trim(inner);
                    } else if (!rest.empty() && (rest[0] == L'"' || rest[0] == L'\'')) {
                        size_t e = rest.find(rest[0], 1);
                        url = rest.substr(0, e == std::wstring::npos ? std::wstring::npos : e + 1);
                        k = e == std::wstring::npos ? rest.size() : e + 1;
                    }
                    if (!url.empty() && (url[0] == L'"' || url[0] == L'\'')) url = url.substr(1, url.size() >= 2 ? url.size() - 2 : 0);
                    std::wstring media = ToLower(Trim(rest.substr(std::min(k, rest.size()))));
                    if (StartsWith(media, L"layer")) media.clear();
                    if (!url.empty() && ToLower(url).find(L"fonts.googleapis") == std::wstring::npos && (media.empty() || MediaMatches(media, vw, vh))) {
                        std::wstring abs = base.empty() ? url : ResolveUrl(base, url);
                        if (const std::wstring* css = importer(abs)) ParseList(StripComments(*css), depth + 1, abs);
                    }
                }
                i = j + 1;
                continue;
            }
            size_t e = MatchBrace(s, j);
            if (e == std::wstring::npos) e = n;
            if (depth < 6) {
                std::wstring inner = s.substr(j + 1, e - j - 1);
                if (StartsWith(prelude, L"@media")) { if (MediaMatches(prelude.substr(6), vw, vh)) ParseList(inner, depth + 1, base); }
                else if (StartsWith(prelude, L"@supports")) { if (SupportsCond(prelude.substr(9), 0)) ParseList(inner, depth + 1, base); }
                else if (StartsWith(prelude, L"@container")) {
                    // kapsayici sorgusu: kapsayici <= gorunur alan varsayimi ile degerlendir
                    std::wstring cond = Trim(prelude.substr(10));
                    size_t par = cond.find(L'(');
                    if (par != std::wstring::npos) cond = cond.substr(par);
                    if (cond.empty() || MediaMatches(cond, vw, vh)) ParseList(inner, depth + 1, base);
                } else if (StartsWith(prelude, L"@layer") || StartsWith(prelude, L"@document") || StartsWith(prelude, L"@-moz-document") || StartsWith(prelude, L"@scope"))
                    ParseList(inner, depth + 1, base);
                // @font-face, @keyframes, @page, @counter-style ... atlanir
            }
            i = e + 1;
            continue;
        }
        size_t j = i;
        while (j < n && s[j] != L'{') {
            if (s[j] == L'"' || s[j] == L'\'') { wchar_t q = s[j]; j++; while (j < n && s[j] != q) { if (s[j] == L'\\') j++; j++; } }
            if (j < n) j++;
        }
        if (j >= n) break;
        std::wstring sel = s.substr(i, j - i);
        size_t e = MatchBrace(s, j);
        if (e == std::wstring::npos) e = n;
        std::wstring body = s.substr(j + 1, e - j - 1);
        auto decls = ParseDeclarations(body);
        if (!base.empty())
            for (auto& d : decls) if (d.value.find(L"url(") != std::wstring::npos) d.value = ResolveCssUrls(d.value, base);
        if (!decls.empty()) AddRule(sel, std::move(decls));
        i = e + 1;
    }
}

void StyleSheet::Add(const std::wstring& css, const std::wstring& base) {
    if (css.size() > 8 * 1024 * 1024) return;
    ParseList(StripComments(css), 0, base);
}

void StyleSheet::BuildIndex() {
    byId.clear(); byClass.clear(); byTag.clear(); universal.clear();
    hostRules.clear(); slottedRules.clear();
    hasPseudo = false;
    for (int i = 0; i < int(rules.size()); i++) {
        const Compound& c = rules[size_t(i)].sel.parts.back();
        if (rules[size_t(i)].sel.pseudo) hasPseudo = true;
        if (c.slotted) { slottedRules.push_back(i); continue; }
        if (c.host) { hostRules.push_back(i); continue; }
        if (!c.id.empty()) byId[c.id].push_back(i);
        else if (!c.classes.empty()) byClass[c.classes[0]].push_back(i);
        else if (!c.tag.empty()) byTag[c.tag].push_back(i);
        else universal.push_back(i);
    }
    (void)g_emptyStr;
}

} // namespace mer
