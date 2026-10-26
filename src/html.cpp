// merOS Shield - MerEngine : HTML ayrıştırıcı (tokenizer + ağaç kurucu)
// Hedef: hizli, hataya dayanikli, sinirli kaynak kullanan bir ayristirici.
#include "dom.h"
#include "util.h"
#include <initializer_list>
#include <unordered_map>

namespace mer {

namespace {

bool In(const std::wstring& n, std::initializer_list<const wchar_t*> l) {
    for (auto x : l) if (n == x) return true;
    return false;
}

bool IsVoid(const std::wstring& n) {
    return In(n, {L"area", L"base", L"br", L"col", L"embed", L"hr", L"img", L"input", L"link", L"meta",
                  L"param", L"source", L"track", L"wbr", L"keygen", L"basefont", L"frame"});
}

bool IsRaw(const std::wstring& n) {
    return In(n, {L"script", L"style", L"title", L"textarea", L"xmp", L"iframe", L"noembed", L"noframes", L"template", L"svg", L"math"});
}

bool ClosesP(const std::wstring& n) {
    return In(n, {L"address", L"article", L"aside", L"blockquote", L"center", L"details", L"dialog", L"dir", L"div",
                  L"dl", L"fieldset", L"figcaption", L"figure", L"footer", L"form", L"h1", L"h2", L"h3", L"h4",
                  L"h5", L"h6", L"header", L"hgroup", L"hr", L"main", L"menu", L"nav", L"ol", L"p", L"pre",
                  L"section", L"table", L"ul", L"li", L"dd", L"dt", L"summary"});
}

const std::unordered_map<std::wstring, uint32_t>& EntityTable() {
    static const std::unordered_map<std::wstring, uint32_t> t = {
        {L"amp", '&'}, {L"lt", '<'}, {L"gt", '>'}, {L"quot", '"'}, {L"apos", '\''}, {L"nbsp", 0xA0},
        {L"copy", 0xA9}, {L"reg", 0xAE}, {L"trade", 0x2122}, {L"hellip", 0x2026}, {L"mdash", 0x2014},
        {L"ndash", 0x2013}, {L"laquo", 0xAB}, {L"raquo", 0xBB}, {L"lsquo", 0x2018}, {L"rsquo", 0x2019},
        {L"sbquo", 0x201A}, {L"ldquo", 0x201C}, {L"rdquo", 0x201D}, {L"bdquo", 0x201E}, {L"bull", 0x2022},
        {L"middot", 0xB7}, {L"euro", 0x20AC}, {L"pound", 0xA3}, {L"yen", 0xA5}, {L"cent", 0xA2},
        {L"sect", 0xA7}, {L"para", 0xB6}, {L"deg", 0xB0}, {L"plusmn", 0xB1}, {L"times", 0xD7},
        {L"divide", 0xF7}, {L"frac12", 0xBD}, {L"frac14", 0xBC}, {L"frac34", 0xBE}, {L"sup2", 0xB2},
        {L"sup3", 0xB3}, {L"iexcl", 0xA1}, {L"iquest", 0xBF}, {L"shy", 0xAD}, {L"larr", 0x2190},
        {L"rarr", 0x2192}, {L"uarr", 0x2191}, {L"darr", 0x2193}, {L"harr", 0x2194}, {L"hearts", 0x2665},
        {L"check", 0x2713}, {L"ccedil", 0xE7}, {L"Ccedil", 0xC7}, {L"ouml", 0xF6}, {L"Ouml", 0xD6},
        {L"uuml", 0xFC}, {L"Uuml", 0xDC}, {L"eacute", 0xE9}, {L"Eacute", 0xC9}, {L"aacute", 0xE1},
        {L"iacute", 0xED}, {L"oacute", 0xF3}, {L"uacute", 0xFA}, {L"egrave", 0xE8}, {L"agrave", 0xE0},
        {L"auml", 0xE4}, {L"Auml", 0xC4}, {L"euml", 0xEB}, {L"iuml", 0xEF}, {L"szlig", 0xDF},
        {L"ntilde", 0xF1}, {L"acirc", 0xE2}, {L"icirc", 0xEE}, {L"ucirc", 0xFB}, {L"ocirc", 0xF4},
        {L"zwnj", 0x200C}, {L"zwj", 0x200D}, {L"thinsp", 0x2009}, {L"ensp", 0x2002}, {L"emsp", 0x2003},
        {L"prime", 0x2032}, {L"Prime", 0x2033}, {L"alpha", 0x3B1}, {L"beta", 0x3B2}, {L"pi", 0x3C0},
        {L"micro", 0xB5}, {L"not", 0xAC}, {L"ordf", 0xAA}, {L"ordm", 0xBA}, {L"dagger", 0x2020},
        {L"lsaquo", 0x2039}, {L"rsaquo", 0x203A}, {L"star", 0x2606}, {L"starf", 0x2605}, {L"le", 0x2264},
        {L"ge", 0x2265}, {L"ne", 0x2260}, {L"infin", 0x221E}, {L"minus", 0x2212}, {L"crarr", 0x21B5},
        {L"asymp", 0x2248}, {L"equiv", 0x2261}, {L"part", 0x2202}, {L"empty", 0x2205}, {L"nabla", 0x2207},
        {L"isin", 0x2208}, {L"notin", 0x2209}, {L"radic", 0x221A}, {L"prop", 0x221D}, {L"ang", 0x2220},
        {L"and", 0x2227}, {L"or", 0x2228}, {L"cap", 0x2229}, {L"cup", 0x222A}, {L"int", 0x222B},
        {L"there4", 0x2234}, {L"sim", 0x223C}, {L"cong", 0x2245}, {L"sub", 0x2282}, {L"sup", 0x2283},
        {L"nsub", 0x2284}, {L"sube", 0x2286}, {L"supe", 0x2287}, {L"oplus", 0x2295}, {L"otimes", 0x2297},
        {L"perp", 0x22A5}, {L"sdot", 0x22C5}, {L"spades", 0x2660}, {L"clubs", 0x2663}, {L"diams", 0x2666},
        {L"loz", 0x25CA},
    };
    return t;
}

// Eski Windows-1252 kod noktalari (&#150; gibi) -> gercek Unicode
uint32_t Fix1252(uint32_t v) {
    static const uint16_t map[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160,
                                     0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                     0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    if (v >= 0x80 && v <= 0x9F) return map[v - 0x80];
    return v;
}

int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool IsAlnum(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
}

class Parser {
public:
    Parser(const std::wstring& s, Document& d) : s_(s), doc_(d) {}

    void Run() {
        doc_.root.reset(new Node());
        doc_.root->kind = Node::Doc;
        doc_.root->id = doc_.count++;
        stack_.push_back(doc_.root.get());
        const size_t n = s_.size();
        while (i_ < n) {
            if (s_[i_] == L'<' && i_ + 1 < n) {
                wchar_t c = s_[i_ + 1];
                if (c == L'!') {
                    if (s_.compare(i_, 4, L"<!--") == 0) {
                        size_t e = s_.find(L"-->", i_ + 4);
                        AddComment(s_.substr(i_ + 4, (e == std::wstring::npos ? n : e) - (i_ + 4)));
                        i_ = (e == std::wstring::npos) ? n : e + 3;
                    } else {
                        size_t e = s_.find(L'>', i_);
                        // <!DOCTYPE> atlanir; diger <!...> sahte yorumdur
                        std::wstring body = s_.substr(i_ + 2, (e == std::wstring::npos ? n : e) - (i_ + 2));
                        if (ToLower(body.substr(0, 7)) != L"doctype") AddComment(body);
                        i_ = (e == std::wstring::npos) ? n : e + 1;
                    }
                    continue;
                }
                if (c == L'?') {
                    // <?...> HTML'de sahte yorum olur (lit-html isaretleri bunu kullanir)
                    size_t e = s_.find(L'>', i_);
                    AddComment(s_.substr(i_ + 1, (e == std::wstring::npos ? n : e) - (i_ + 1)));
                    i_ = (e == std::wstring::npos) ? n : e + 1;
                    continue;
                }
                if (c == L'/') {
                    if (i_ + 2 < n && iswalpha(s_[i_ + 2])) { EndTag(); continue; }
                    size_t e = s_.find(L'>', i_); i_ = (e == std::wstring::npos) ? n : e + 1;
                    continue;
                }
                if (iswalpha(c)) { StartTag(); continue; }
            }
            size_t e = s_.find(L'<', i_ + 1);
            if (e == std::wstring::npos) e = n;
            AddText(DecodeEntities(s_.substr(i_, e - i_)));
            i_ = e;
        }
    }

private:
    const std::wstring& s_;
    Document& doc_;
    size_t i_ = 0;
    std::vector<Node*> stack_;

    Node* Cur() { return stack_.back(); }

    Node* Append(std::unique_ptr<Node> n) {
        if (doc_.count >= kMaxNodes) return nullptr;
        n->parent = Cur();
        n->id = doc_.count++;
        Node* p = n.get();
        Cur()->kids.push_back(std::move(n));
        return p;
    }

    void AddTextTo(Node* parent, const std::wstring& t) {
        if (t.empty() || !parent) return;
        auto& k = parent->kids;
        if (!k.empty() && k.back()->kind == Node::Text) {
            if (k.back()->text.size() < 4000000) k.back()->text += t;
            return;
        }
        if (doc_.count >= kMaxNodes) return;
        std::unique_ptr<Node> n(new Node());
        n->kind = Node::Text;
        n->text = t;
        n->parent = parent;
        n->id = doc_.count++;
        k.push_back(std::move(n));
    }
    void AddText(const std::wstring& t) { AddTextTo(Cur(), t); }
    void AddComment(const std::wstring& t) {
        Node* parent = Cur();
        if (!parent || doc_.count >= kMaxNodes || t.size() > 1000000) return;
        std::unique_ptr<Node> n(new Node());
        n->kind = Node::Comment;
        n->text = t;
        n->parent = parent;
        n->id = doc_.count++;
        parent->kids.push_back(std::move(n));
    }

    std::wstring ReadName() {
        std::wstring nm;
        while (i_ < s_.size()) {
            wchar_t c = s_[i_];
            if (IsAlnum(c) || c == L'-' || c == L':' || c == L'_') { nm += LowerC(c); i_++; }
            else break;
        }
        return nm;
    }

    void EndTag() {
        i_ += 2;
        std::wstring nm = ReadName();
        size_t e = s_.find(L'>', i_);
        i_ = (e == std::wstring::npos) ? s_.size() : e + 1;
        if (nm == L"br") { OpenElement(L"br", {}, true); return; }
        if (nm == L"p" && !HasOpen(L"p", true)) return;
        CloseTag(nm);
    }

    bool HasOpen(const wchar_t* nm, bool stopAtBoundary) {
        for (size_t k = stack_.size(); k-- > 1;) {
            if (stack_[k]->name == nm) return true;
            if (stopAtBoundary && In(stack_[k]->name, {L"table", L"td", L"th", L"button", L"html"})) return false;
        }
        return false;
    }

    void PopTo(size_t k) { while (stack_.size() > k && stack_.size() > 1) stack_.pop_back(); }

    void CloseTag(const std::wstring& nm) {
        for (size_t k = stack_.size(); k-- > 1;) {
            if (stack_[k]->name == nm) { PopTo(k); return; }
            // tablo sinirini asma (bozuk HTML'in tum agaci kapatmasini onler)
            if ((stack_[k]->name == L"table") && nm != L"table") return;
            if ((stack_[k]->name == L"td" || stack_[k]->name == L"th") && nm != L"td" && nm != L"th" && nm != L"tr" && nm != L"tbody" && nm != L"thead" && nm != L"tfoot" && nm != L"table") {
                if (In(nm, {L"span", L"a", L"b", L"i", L"em", L"strong", L"u", L"s", L"small", L"font", L"code", L"sub", L"sup", L"label", L"button"})) return;
            }
            if (In(nm, {L"span", L"b", L"i", L"em", L"strong", L"u", L"s", L"small", L"font", L"code", L"sub", L"sup", L"label"}) &&
                In(stack_[k]->name, {L"div", L"p", L"ul", L"ol", L"li", L"dl", L"dt", L"dd", L"section", L"article", L"main", L"aside", L"header", L"footer", L"nav", L"form", L"fieldset", L"blockquote", L"details", L"dialog", L"figure", L"body", L"html"})) return;
            if ((stack_[k]->name == L"body" || stack_[k]->name == L"html") && nm != L"body" && nm != L"html") return;
        }
    }

    // nm etiketini, 'bounds' etiketlerinden birine ulasmadan aciksa kapat
    void CloseInScope(std::initializer_list<const wchar_t*> names, std::initializer_list<const wchar_t*> bounds) {
        for (size_t k = stack_.size(); k-- > 1;) {
            const std::wstring& t = stack_[k]->name;
            if (In(t, names)) { PopTo(k); return; }
            if (In(t, bounds)) return;
        }
    }

    void StartTag() {
        i_ += 1;
        std::wstring nm = ReadName();
        std::vector<std::pair<std::wstring, std::wstring>> attrs;
        bool selfClose = false;
        const size_t n = s_.size();
        while (i_ < n) {
            while (i_ < n && IsWS(s_[i_])) i_++;
            if (i_ >= n) break;
            if (s_[i_] == L'>') { i_++; break; }
            if (s_[i_] == L'/') {
                if (i_ + 1 < n && s_[i_ + 1] == L'>') { selfClose = true; i_ += 2; break; }
                i_++; continue;
            }
            std::wstring an;
            while (i_ < n && !IsWS(s_[i_]) && s_[i_] != L'=' && s_[i_] != L'>' && !(s_[i_] == L'/' && i_ + 1 < n && s_[i_ + 1] == L'>')) {
                an += LowerC(s_[i_]); i_++;
            }
            while (i_ < n && IsWS(s_[i_])) i_++;
            std::wstring av;
            if (i_ < n && s_[i_] == L'=') {
                i_++;
                while (i_ < n && IsWS(s_[i_])) i_++;
                if (i_ < n && (s_[i_] == L'"' || s_[i_] == L'\'')) {
                    wchar_t q = s_[i_++];
                    size_t e = s_.find(q, i_);
                    if (e == std::wstring::npos) e = n;
                    av = s_.substr(i_, e - i_);
                    i_ = (e < n) ? e + 1 : n;
                } else {
                    size_t st = i_;
                    while (i_ < n && !IsWS(s_[i_]) && s_[i_] != L'>') i_++;
                    av = s_.substr(st, i_ - st);
                }
                av = DecodeEntities(av);
            }
            if (!an.empty() && attrs.size() < 64) {
                bool dup = false;
                for (auto& a : attrs) if (a.first == an) { dup = true; break; }
                if (!dup) attrs.emplace_back(an, av);
            }
        }
        if (nm.empty()) return;
        OpenElement(nm, std::move(attrs), selfClose);
    }

    void OpenElement(const std::wstring& nm, std::vector<std::pair<std::wstring, std::wstring>> attrs, bool selfClose) {
        // Ortuk kapanislar (HTML'de kapanis etiketi yazilmayan durumlar)
        if (ClosesP(nm) && HasOpen(L"p", true)) CloseInScope({L"p"}, {L"table", L"td", L"th", L"button"});
        if (nm == L"li") CloseInScope({L"li"}, {L"ul", L"ol", L"menu", L"table", L"td"});
        if (nm == L"dt" || nm == L"dd") CloseInScope({L"dt", L"dd"}, {L"dl", L"table"});
        if (nm == L"tr") {
            CloseInScope({L"tr"}, {L"table", L"tbody", L"thead", L"tfoot"});
            if (Cur()->name == L"table") {
                std::unique_ptr<Node> tb(new Node());
                tb->kind = Node::Elem;
                tb->name = L"tbody";
                Node* ptb = Append(std::move(tb));
                if (ptb && stack_.size() < kMaxDepth) stack_.push_back(ptb);
            }
        }
        if (nm == L"td" || nm == L"th") CloseInScope({L"td", L"th"}, {L"tr", L"table"});
        if (nm == L"tbody" || nm == L"thead" || nm == L"tfoot") CloseInScope({L"tbody", L"thead", L"tfoot"}, {L"table"});
        if (nm == L"option") CloseInScope({L"option"}, {L"select", L"datalist"});
        if (nm == L"a") CloseInScope({L"a"}, {L"table", L"td"});
        if (nm == L"button") CloseInScope({L"button"}, {L"table", L"td", L"form", L"div"});
        if (In(nm, {L"h1", L"h2", L"h3", L"h4", L"h5", L"h6"})) CloseInScope({L"h1", L"h2", L"h3", L"h4", L"h5", L"h6"}, {L"table", L"td", L"th", L"div", L"section", L"article", L"aside", L"header", L"footer", L"main", L"nav", L"body"});
        if ((nm == L"html" || nm == L"body" || nm == L"head") && HasOpen(nm.c_str(), false)) return;

        std::unique_ptr<Node> node(new Node());
        node->kind = Node::Elem;
        node->name = nm;
        node->attrs = std::move(attrs);
        Node* p = Append(std::move(node));
        if (!p) return;

        if (IsRaw(nm) && !selfClose) {
            std::wstring raw = ReadRawText(nm);
            if (nm == L"title" || nm == L"textarea") raw = DecodeEntities(raw);
            AddTextTo(p, raw);
            return;
        }
        if (!IsVoid(nm) && !selfClose && stack_.size() < kMaxDepth) stack_.push_back(p);
    }

    std::wstring ReadRawText(const std::wstring& nm) {
        const size_t n = s_.size();
        size_t pos = i_;
        while (true) {
            pos = s_.find(L"</", pos);
            if (pos == std::wstring::npos) {
                if (In(nm, {L"iframe", L"noembed", L"noframes", L"svg", L"math"})) return L"";
                std::wstring r = s_.substr(i_); i_ = n; return r;
            }
            size_t k = 0;
            while (k < nm.size() && pos + 2 + k < n && LowerC(s_[pos + 2 + k]) == nm[k]) k++;
            if (k == nm.size()) {
                size_t after = pos + 2 + k;
                if (after >= n || IsWS(s_[after]) || s_[after] == L'>' || s_[after] == L'/') break;
            }
            pos += 2;
        }
        std::wstring r = s_.substr(i_, pos - i_);
        size_t e = s_.find(L'>', pos);
        i_ = (e == std::wstring::npos) ? n : e + 1;
        return r;
    }
};

// <template> icindeki (or. bildirimsel shadow DOM) <style> belge stiline karismaz
static bool InTemplate(const Node* n) {
    for (const Node* p = n->parent; p; p = p->parent) if (p->kind == Node::Elem && p->name == L"template") return true;
    return false;
}

void PostProcess(Node* n, Document& doc) {
    // Iteratif gezinme (derin agaclarda yigin tasmasi olmasin)
    std::vector<Node*> st{n};
    while (!st.empty()) {
        Node* c = st.back();
        st.pop_back();
        if (c->kind == Node::Elem) {
            if (auto* cls = c->Attr(L"class")) {
                for (auto& w : SplitWS(*cls)) {
                    bool dup = false;
                    for (auto& e : c->classes) if (e == w) { dup = true; break; }
                    if (!dup) c->classes.push_back(w);
                }
            }
            if (auto* id = c->Attr(L"id")) c->idAttr = *id;
            if (c->name == L"title" && doc.title.empty() && !c->kids.empty()) {
                std::wstring t;
                bool sp = false;
                for (wchar_t ch : c->kids[0]->text) {
                    if (IsWS(ch)) sp = true;
                    else { if (sp && !t.empty()) t += L' '; sp = false; t += ch; }
                }
                doc.title = t;
            } else if (c->name == L"base" && doc.baseHref.empty()) {
                if (auto* h = c->Attr(L"href")) doc.baseHref = *h;
            } else if (c->name == L"style" && !InTemplate(c)) {
                bool screen = true;
                if (auto* m = c->Attr(L"media")) { std::wstring ml = ToLower(*m); if (ml.find(L"print") != std::wstring::npos && ml.find(L"screen") == std::wstring::npos) screen = false; }
                if (screen && !c->kids.empty()) {
                    doc.styleOrder.push_back(int(doc.inlineStyles.size()));
                    doc.inlineStyles.push_back(c->kids[0]->text);
                }
            } else if (c->name == L"link") {
                auto* rel = c->Attr(L"rel");
                auto* href = c->Attr(L"href");
                if (rel && href) {
                    std::wstring r = ToLower(*rel);
                    bool screen = true;
                    if (auto* m = c->Attr(L"media")) { std::wstring ml = ToLower(*m); if (ml.find(L"print") != std::wstring::npos && ml.find(L"screen") == std::wstring::npos) screen = false; }
                    if (r.find(L"stylesheet") != std::wstring::npos && r.find(L"alternate") == std::wstring::npos && screen) {
                        doc.styleOrder.push_back(-int(doc.styleLinks.size()) - 1);
                        doc.styleLinks.push_back(*href);
                    }
                }
            }
        }
        for (size_t k = c->kids.size(); k-- > 0;) st.push_back(c->kids[k].get());
    }
}

} // namespace

std::wstring DecodeEntities(const std::wstring& s) {
    if (s.find(L'&') == std::wstring::npos) return s;
    std::wstring o;
    o.reserve(s.size());
    const auto& tbl = EntityTable();
    for (size_t i = 0; i < s.size();) {
        if (s[i] != L'&') { o += s[i++]; continue; }
        size_t j = i + 1;
        if (j < s.size() && s[j] == L'#') {
            j++;
            bool hex = false;
            if (j < s.size() && (s[j] == L'x' || s[j] == L'X')) { hex = true; j++; }
            uint32_t v = 0;
            size_t st = j;
            while (j < s.size() && j - st < 8) {
                int d = HexVal(s[j]);
                if (d < 0 || (!hex && d > 9)) break;
                v = v * (hex ? 16 : 10) + uint32_t(d);
                j++;
            }
            if (j == st) { o += L'&'; i++; continue; }
            if (j < s.size() && s[j] == L';') j++;
            AppendCodepoint(o, Fix1252(v));
            i = j;
            continue;
        }
        size_t st = j;
        while (j < s.size() && IsAlnum(s[j]) && j - st < 32) j++;
        std::wstring name = s.substr(st, j - st);
        bool semi = j < s.size() && s[j] == L';';
        auto it = tbl.find(name);
        bool legacy = (name == L"amp" || name == L"lt" || name == L"gt" || name == L"quot" || name == L"nbsp" || name == L"copy" || name == L"reg");
        if (it != tbl.end() && (semi || legacy)) {
            AppendCodepoint(o, it->second);
            i = j + (semi ? 1 : 0);
        } else {
            o += L'&';
            i++;
        }
    }
    return o;
}

std::unique_ptr<Document> ParseHTML(const std::wstring& src) {
    std::unique_ptr<Document> doc(new Document());
    Parser p(src, *doc);
    p.Run();
    PostProcess(doc->root.get(), *doc);
    ApplyDeclarativeShadow(*doc, doc->root.get());
    return doc;
}

} // namespace mer
