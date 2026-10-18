// merOS Shield - MerEngine : DOM değiştirme yardımcıları (JS motoru için)
#include "dom.h"
#include "util.h"

namespace mer {

namespace {

bool IsVoidTag(const std::wstring& n) {
    static const wchar_t* v[] = {L"area", L"base", L"br", L"col", L"embed", L"hr", L"img", L"input", L"link",
                                 L"meta", L"param", L"source", L"track", L"wbr"};
    for (auto x : v) if (n == x) return true;
    return false;
}

void Renumber(Node* n, Document& doc) {
    std::vector<Node*> st{n};
    while (!st.empty()) {
        Node* c = st.back();
        st.pop_back();
        c->id = doc.count++;
        for (auto& k : c->kids) st.push_back(k.get());
    }
}

} // namespace

void SyncAttrCache(Node* n) {
    n->classes.clear();
    n->idAttr.clear();
    if (auto* cls = n->Attr(L"class"))
        for (auto& w : SplitWS(*cls)) {
            bool dup = false;
            for (auto& e : n->classes) if (e == w) { dup = true; break; }
            if (!dup) n->classes.push_back(w);
        }
    if (auto* id = n->Attr(L"id")) n->idAttr = *id;
}

std::vector<std::unique_ptr<Node>> ParseFragment(const std::wstring& html, Document& doc) {
    std::vector<std::unique_ptr<Node>> out;
    if (doc.count >= kMaxNodes) return out;
    auto tmp = ParseHTML(html);
    for (auto& k : tmp->root->kids) {
        if (doc.count + tmp->count >= kMaxNodes) break;
        Renumber(k.get(), doc);
        k->parent = nullptr;
        out.push_back(std::move(k));
    }
    return out;
}

Node* NewNode(Document& doc, Node::Kind kind, const std::wstring& s) {
    std::unique_ptr<Node> n(new Node());
    n->kind = kind;
    if (kind == Node::Text) n->text = s;
    else n->name = ToLower(s);
    n->id = doc.count++;
    Node* p = n.get();
    doc.detached.push_back(std::move(n));
    return p;
}

std::unique_ptr<Node> Detach(Document& doc, Node* n) {
    if (n->parent) {
        auto& k = n->parent->kids;
        for (size_t i = 0; i < k.size(); i++) {
            if (k[i].get() == n) {
                std::unique_ptr<Node> r = std::move(k[i]);
                k.erase(k.begin() + long(i));
                r->parent = nullptr;
                return r;
            }
        }
    }
    auto& d = doc.detached;
    for (size_t i = 0; i < d.size(); i++) {
        if (d[i].get() == n) {
            std::unique_ptr<Node> r = std::move(d[i]);
            d[i] = std::move(d.back());
            d.pop_back();
            return r;
        }
    }
    return nullptr;
}

bool Contains(const Node* a, const Node* n) {
    for (const Node* p = n; p; p = p->parent) if (p == a) return true;
    return false;
}

static int Depth(const Node* n) {
    int d = 0;
    for (const Node* p = n; p; p = p->parent) d++;
    return d;
}

bool InsertBefore(Document& doc, Node* parent, Node* child, Node* ref) {
    if (!parent || !child || parent == child || Contains(child, parent)) return false;
    if (parent->kind == Node::Text) return false;
    if (Depth(parent) >= int(kMaxDepth)) return false;
    if (ref && ref->parent != parent) return false;
    if (ref == child) return true;
    std::unique_ptr<Node> own = Detach(doc, child);
    if (!own) return false;
    own->parent = parent;
    auto& k = parent->kids;
    if (!ref) { k.push_back(std::move(own)); return true; }
    for (size_t i = 0; i < k.size(); i++) {
        if (k[i].get() == ref) { k.insert(k.begin() + long(i), std::move(own)); return true; }
    }
    k.push_back(std::move(own));
    return true;
}

bool AppendChild(Document& doc, Node* parent, Node* child) { return InsertBefore(doc, parent, child, nullptr); }

void RemoveNode(Document& doc, Node* n) {
    if (!n || !n->parent) return;
    std::unique_ptr<Node> own = Detach(doc, n);
    if (own) doc.detached.push_back(std::move(own));
}

std::wstring TextContent(const Node* n) {
    if (n->kind == Node::Text) return n->text;
    std::wstring o;
    std::vector<const Node*> st{n};
    while (!st.empty()) {
        const Node* c = st.back();
        st.pop_back();
        if (c->kind == Node::Text) { o += c->text; if (o.size() > 8000000) break; }
        for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
    }
    return o;
}

static void Ser(const Node* n, std::wstring& o, int depth) {
    if (depth > 400 || o.size() > 16000000) return;
    if (n->kind == Node::Text) {
        bool raw = n->parent && (n->parent->name == L"script" || n->parent->name == L"style");
        o += raw ? n->text : HtmlEscape(n->text);
        return;
    }
    if (n->kind == Node::Doc) { for (auto& k : n->kids) Ser(k.get(), o, depth + 1); return; }
    o += L'<';
    o += n->name;
    for (auto& a : n->attrs) { o += L' '; o += a.first; o += L"=\""; o += HtmlEscape(a.second); o += L'"'; }
    o += L'>';
    if (IsVoidTag(n->name)) return;
    for (auto& k : n->kids) Ser(k.get(), o, depth + 1);
    o += L"</";
    o += n->name;
    o += L'>';
}

std::wstring SerializeHTML(const Node* n, bool outer) {
    std::wstring o;
    if (outer) Ser(n, o, 0);
    else for (auto& k : n->kids) Ser(k.get(), o, 0);
    return o;
}

} // namespace mer
