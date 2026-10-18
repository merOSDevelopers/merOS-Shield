// merOS Shield - MerEngine : CSS ayrıştırıcı ve seçici eşleştirme
#include "css.h"
#include "util.h"
#include <cstdlib>

namespace mer {

namespace {

constexpr size_t kMaxRules = 60000;

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

float FirstNumberAfter(const std::wstring& s, const wchar_t* key, bool& found) {
    size_t p = s.find(key);
    found = false;
    if (p == std::wstring::npos) return 0;
    p += wcslen(key);
    while (p < s.size() && (IsWS(s[p]) || s[p] == L':')) p++;
    const wchar_t* b = s.c_str() + p;
    wchar_t* e = nullptr;
    float v = wcstof(b, &e);
    if (e == b) return 0;
    found = true;
    std::wstring unit;
    while (*e && ((*e >= L'a' && *e <= L'z') || *e == L'%')) unit += *e++;
    if (unit == L"em" || unit == L"rem") v *= 16;
    return v;
}

// Masaustu varsayimi: ~1280px genislik, ekran ortami, acik tema.
bool MediaOk(const std::wstring& q) {
    std::wstring m = ToLower(q);
    if (m.find(L"print") != std::wstring::npos && m.find(L"screen") == std::wstring::npos && m.find(L"all") == std::wstring::npos) return false;
    if (m.find(L"speech") != std::wstring::npos) return false;
    if (m.find(L"prefers-color-scheme") != std::wstring::npos && m.find(L"dark") != std::wstring::npos) return false;
    if (m.find(L"orientation") != std::wstring::npos && m.find(L"portrait") != std::wstring::npos) return false;
    if (m.find(L"hover: none") != std::wstring::npos || m.find(L"hover:none") != std::wstring::npos) return false;
    if (m.find(L"pointer: coarse") != std::wstring::npos || m.find(L"pointer:coarse") != std::wstring::npos) return false;
    bool f = false;
    float mx = FirstNumberAfter(m, L"max-width", f);
    if (f && mx < 1000) return false;
    float mn = FirstNumberAfter(m, L"min-width", f);
    if (f && mn > 1400) return false;
    return true;
}

bool IdentChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c > 127;
}

std::wstring ReadIdent(const std::wstring& s, size_t& i) {
    std::wstring o;
    while (i < s.size()) {
        if (s[i] == L'\\' && i + 1 < s.size()) { o += s[i + 1]; i += 2; continue; }
        if (!IdentChar(s[i])) break;
        o += s[i++];
    }
    return o;
}

// Tek bir bilesik secici parcasi (etiket, .sinif, #id, [oznitelik], :sozde-sinif)
bool ParseCompoundPart(const std::wstring& s, size_t& i, Compound& cur, int depth) {
    wchar_t c = s[i];
    if (c == L'*') { i++; return true; }
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
            if (i < s.size() && (s[i] == L'i' || s[i] == L'I')) { a.icase = true; i++; while (i < s.size() && IsWS(s[i])) i++; }
        }
        if (i >= s.size() || s[i] != L']') return false;
        i++;
        cur.attrs.push_back(a);
        return true;
    }
    if (c == L':') {
        i++;
        if (i < s.size() && s[i] == L':') return false; // ::before vb.
        std::wstring ps = ToLower(ReadIdent(s, i));
        if (i < s.size() && s[i] == L'(') {
            size_t close = s.find(L')', i);
            if (close == std::wstring::npos) return false;
            std::wstring arg = Trim(s.substr(i + 1, close - i - 1));
            i = close + 1;
            if (ps == L"not" && depth == 0) {
                size_t j = 0;
                Compound nc;
                while (j < arg.size()) if (!ParseCompoundPart(arg, j, nc, depth + 1)) return false;
                cur.nots.push_back(nc);
                return true;
            }
            if (ps == L"nth-child" && (arg == L"1")) { cur.firstChild = true; return true; }
            return false; // :nth-child(2n), :is() ...
        }
        if (ps == L"link" || ps == L"visited" || ps == L"any-link") return true;
        if (ps == L"root") { cur.root = true; return true; }
        if (ps == L"first-child") { cur.firstChild = true; return true; }
        if (ps == L"last-child") { cur.lastChild = true; return true; }
        if (ps == L"only-child") { cur.onlyChild = true; return true; }
        if (ps == L"empty") { cur.empty = true; return true; }
        return false; // :hover, :focus ... statik sayfada uygulanmaz
    }
    if (IdentChar(c)) { cur.tag = ToLower(ReadIdent(s, i)); return true; }
    return false;
}

bool ParseSelector(const std::wstring& text, Selector& out) {
    std::wstring s = Trim(text);
    if (s.empty()) return false;
    Compound cur;
    bool have = false;
    wchar_t nextComb = L' ';
    auto finalize = [&]() { if (have) { out.parts.push_back(cur); cur = Compound(); have = false; } };
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (IsWS(c)) { finalize(); i++; continue; }
        if (c == L'>' || c == L'+' || c == L'~') { finalize(); nextComb = c; i++; continue; }
        if (!have) { cur.comb = nextComb; nextComb = L' '; have = true; }
        if (!ParseCompoundPart(s, i, cur, 0)) return false;
    }
    finalize();
    if (out.parts.empty() || out.parts.size() > 32) return false;
    int ids = 0, cls = 0, tags = 0;
    for (auto& p : out.parts) {
        if (!p.id.empty()) ids++;
        cls += int(p.classes.size() + p.attrs.size() + p.nots.size()) + (p.root ? 1 : 0) +
               (p.firstChild ? 1 : 0) + (p.lastChild ? 1 : 0) + (p.onlyChild ? 1 : 0) + (p.empty ? 1 : 0);
        if (!p.tag.empty()) tags++;
    }
    out.spec = ids * 10000 + cls * 100 + tags;
    return true;
}

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

bool MatchCompound(const Compound& c, const Node* n) {
    if (n->kind != Node::Elem) return false;
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
    if (c.empty && !n->kids.empty()) return false;
    for (auto& nc : c.nots) if (MatchCompound(nc, n)) return false;
    return true;
}

bool MatchFrom(const Selector& s, int idx, const Node* n, int budget) {
    if (budget <= 0 || !MatchCompound(s.parts[idx], n)) return false;
    if (idx == 0) return true;
    wchar_t comb = s.parts[idx].comb;
    if (comb == L'>') {
        const Node* p = n->parent;
        return p && p->kind == Node::Elem && MatchFrom(s, idx - 1, p, budget - 1);
    }
    if (comb == L'+') {
        const Node* p = PrevElem(n);
        return p && MatchFrom(s, idx - 1, p, budget - 1);
    }
    if (comb == L'~') {
        const Node* par = n->parent;
        if (!par) return false;
        for (auto& k : par->kids) {
            if (k.get() == n) break;
            if (k->kind == Node::Elem && MatchFrom(s, idx - 1, k.get(), budget - 1)) return true;
        }
        return false;
    }
    for (const Node* p = n->parent; p; p = p->parent)
        if (p->kind == Node::Elem && MatchFrom(s, idx - 1, p, budget - 1)) return true;
    return false;
}

} // namespace

bool Matches(const Selector& sel, const Node* n) {
    if (sel.parts.empty()) return false;
    return MatchFrom(sel, int(sel.parts.size()) - 1, n, 64);
}

bool ParseSelectorList(const std::wstring& text, std::vector<Selector>& out) {
    size_t st = 0;
    int paren = 0, br = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        wchar_t c = i < text.size() ? text[i] : L',';
        if (c == L'(') paren++;
        else if (c == L')' && paren > 0) paren--;
        else if (c == L'[') br++;
        else if (c == L']' && br > 0) br--;
        else if (c == L',' && paren == 0 && br == 0) {
            Selector sel;
            if (!ParseSelector(text.substr(st, i - st), sel)) return false;
            out.push_back(std::move(sel));
            st = i + 1;
        }
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
    // Virgulle ayrilmis secici listesi (parantez icindeki virguller haric)
    size_t st = 0;
    int paren = 0;
    for (size_t i = 0; i <= selectorText.size(); i++) {
        wchar_t c = i < selectorText.size() ? selectorText[i] : L',';
        if (c == L'(' || c == L'[') paren++;
        else if ((c == L')' || c == L']') && paren > 0) paren--;
        else if (c == L',' && paren == 0) {
            Selector sel;
            if (ParseSelector(selectorText.substr(st, i - st), sel)) {
                Rule r;
                r.sel = std::move(sel);
                r.decls = dp;
                r.order = order;
                rules.push_back(std::move(r));
            }
            st = i + 1;
        }
    }
}

void StyleSheet::ParseList(const std::wstring& s, int depth) {
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && (IsWS(s[i]) || s[i] == L';')) i++;
        if (i >= n) break;
        if (s[i] == L'}') { i++; continue; }
        if (s[i] == L'@') {
            size_t j = i;
            while (j < n && s[j] != L'{' && s[j] != L';') j++;
            std::wstring prelude = ToLower(Trim(s.substr(i, j - i)));
            if (j >= n) break;
            if (s[j] == L';') { i = j + 1; continue; }
            size_t e = MatchBrace(s, j);
            if (e == std::wstring::npos) e = n;
            if (depth < 4) {
                std::wstring inner = s.substr(j + 1, e - j - 1);
                if (StartsWith(prelude, L"@media")) { if (MediaOk(prelude.substr(6))) ParseList(inner, depth + 1); }
                else if (StartsWith(prelude, L"@supports")) { if (prelude.find(L"not") == std::wstring::npos) ParseList(inner, depth + 1); }
                else if (StartsWith(prelude, L"@layer") || StartsWith(prelude, L"@container")) ParseList(inner, depth + 1);
                // @font-face, @keyframes, @page ... atlanir
            }
            i = e + 1;
            continue;
        }
        size_t j = s.find(L'{', i);
        if (j == std::wstring::npos) break;
        std::wstring sel = s.substr(i, j - i);
        size_t e = MatchBrace(s, j);
        if (e == std::wstring::npos) e = n;
        auto decls = ParseDeclarations(s.substr(j + 1, e - j - 1));
        if (!decls.empty()) AddRule(sel, std::move(decls));
        i = e + 1;
    }
}

void StyleSheet::Add(const std::wstring& css) {
    if (css.size() > 8 * 1024 * 1024) return;
    ParseList(StripComments(css), 0);
}

void StyleSheet::BuildIndex() {
    byId.clear(); byClass.clear(); byTag.clear(); universal.clear();
    for (int i = 0; i < int(rules.size()); i++) {
        const Compound& c = rules[i].sel.parts.back();
        if (!c.id.empty()) byId[c.id].push_back(i);
        else if (!c.classes.empty()) byClass[c.classes[0]].push_back(i);
        else if (!c.tag.empty()) byTag[c.tag].push_back(i);
        else universal.push_back(i);
    }
}

} // namespace mer
