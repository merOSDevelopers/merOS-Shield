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
        if (c->shadow) st.push_back(c->shadow.get());
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
    if (kind == Node::Text || kind == Node::Comment) n->text = s;
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
    for (const Node* p = n; p; p = p->parent ? p->parent : (p->kind == Node::Shadow ? p->host : nullptr)) if (p == a) return true;
    return false;
}

bool ContainsLight(const Node* a, const Node* n) {
    for (const Node* p = n; p; p = p->parent) if (p == a) return true;
    return false;
}

// ---------------- Shadow DOM ----------------

Node* AttachShadowRoot(Document& doc, Node* host, uint8_t mode) {
    if (!host || host->kind != Node::Elem || host->shadow) return nullptr;
    std::unique_ptr<Node> s(new Node());
    s->kind = Node::Shadow;
    s->name = L"#shadow-root";
    s->id = doc.count++;
    s->host = host;
    s->shadowMode = mode ? mode : 1;
    host->shadow = std::move(s);
    return host->shadow.get();
}

const Node* ComposedParent(const Node* n) {
    if (!n) return nullptr;
    if (n->parent) return n->parent;
    return n->kind == Node::Shadow ? n->host : nullptr;
}

const Node* ShadowRootOf(const Node* n) {
    for (const Node* p = n; p; p = p->parent) if (p->kind == Node::Shadow) return p;
    return nullptr;
}

std::wstring SlotNameOf(const Node* n) {
    if (n->kind != Node::Elem) return L"";
    const std::wstring* s = n->Attr(L"slot");
    return s ? *s : L"";
}

static const Node* FindSlot(const Node* root, const std::wstring& name) {
    std::vector<const Node*> st;
    for (size_t i = root->kids.size(); i-- > 0;) st.push_back(root->kids[i].get());
    int guard = 0;
    while (!st.empty() && guard++ < 200000) {
        const Node* c = st.back();
        st.pop_back();
        if (c->kind != Node::Elem) continue;
        if (c->name == L"slot") {
            const std::wstring* nm = c->Attr(L"name");
            if ((nm ? *nm : std::wstring()) == name) return c;
        }
        for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
    }
    return nullptr;
}

const Node* AssignedSlot(const Node* n) {
    const Node* h = n ? n->parent : nullptr;
    if (!h || !h->shadow) return nullptr;
    if (n->kind != Node::Elem && n->kind != Node::Text) return nullptr;
    return FindSlot(h->shadow.get(), SlotNameOf(n));
}

void AssignedNodes(const Node* slot, std::vector<const Node*>& out) {
    const Node* root = ShadowRootOf(slot);
    if (!root || !root->host) return;
    const std::wstring* nm = slot->Attr(L"name");
    std::wstring name = nm ? *nm : L"";
    if (FindSlot(root, name) != slot) return;   // ayni adli ilk slot alir
    for (auto& k : root->host->kids) {
        if (k->kind != Node::Elem && k->kind != Node::Text) continue;
        if (SlotNameOf(k.get()) == name) out.push_back(k.get());
    }
}

void FlatKids(const Node* n, std::vector<const Node*>& out) {
    if (n->shadow) {
        for (auto& k : n->shadow->kids) out.push_back(k.get());
        return;
    }
    if (n->kind == Node::Elem && n->name == L"slot" && ShadowRootOf(n)) {
        size_t before = out.size();
        AssignedNodes(n, out);
        if (out.size() != before) return;
    }
    for (auto& k : n->kids) out.push_back(k.get());
}

std::vector<std::unique_ptr<Node>> TemplateNodes(Node* t, Document& doc) {
    std::vector<std::unique_ptr<Node>> out;
    bool raw = !t->kids.empty();
    for (auto& k : t->kids) if (k->kind != Node::Text) { raw = false; break; }
    if (raw) {
        std::wstring html;
        for (auto& k : t->kids) html += k->text;
        // Dugumler asla silinmez (JS sarmalayicilari onlara isaret edebilir): havuza tasi
        for (auto& k : t->kids) { k->parent = nullptr; doc.detached.push_back(std::move(k)); }
        t->kids.clear();
        out = ParseFragment(html, doc);
    } else {
        for (auto& k : t->kids) out.push_back(std::move(k));
        t->kids.clear();
    }
    return out;
}

void ApplyDeclarativeShadow(Document& doc, Node* root) {
    std::vector<Node*> st{root};
    std::vector<Node*> templates;
    int guard = 0;
    while (!st.empty() && guard++ < kMaxNodes) {
        Node* c = st.back();
        st.pop_back();
        if (c->kind == Node::Elem && c->name == L"template" && c->parent && c->parent->kind == Node::Elem && !c->parent->shadow) {
            const std::wstring* mode = c->Attr(L"shadowrootmode");
            if (!mode) mode = c->Attr(L"shadowroot");
            if (mode) {
                Node* s = AttachShadowRoot(doc, c->parent, ToLower(*mode) == L"closed" ? 2 : 1);
                if (s) {
                    for (auto& k : TemplateNodes(c, doc)) { k->parent = s; s->kids.push_back(std::move(k)); }
                    templates.push_back(c);
                    st.push_back(s);
                    continue;
                }
            }
        }
        for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
        if (c->shadow) st.push_back(c->shadow.get());
    }
    for (Node* t : templates) {
        std::unique_ptr<Node> own = Detach(doc, t);
        if (own) doc.detached.push_back(std::move(own));
    }
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
    if (child->name == L"#fragment") {
        std::vector<std::unique_ptr<Node>> kids;
        kids.swap(child->kids);
        for (auto& kd : kids) {
            kd->parent = parent;
            if (!ref) {
                parent->kids.push_back(std::move(kd));
            } else {
                auto it = parent->kids.begin();
                while (it != parent->kids.end() && it->get() != ref) ++it;
                parent->kids.insert(it, std::move(kd));
            }
        }
        return true;
    }
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
        bool raw = n->parent && (n->parent->name == L"script" || n->parent->name == L"style" || n->parent->name == L"svg" || n->parent->name == L"math");
        o += raw ? n->text : HtmlEscape(n->text);
        return;
    }
    if (n->kind == Node::Comment) { o += L"<!--"; o += n->text; o += L"-->"; return; }
    if (n->kind == Node::Doc || n->kind == Node::Shadow) { for (auto& k : n->kids) Ser(k.get(), o, depth + 1); return; }
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
