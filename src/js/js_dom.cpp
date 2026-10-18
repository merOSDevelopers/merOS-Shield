#ifndef NOMINMAX
#define NOMINMAX
#endif
// merOS Shield - merJS : DOM bağlantıları (document, element, olaylar, window, location, storage...)
#include <algorithm>
#include <cmath>
#include <list>
#include "../css.h"
#include "../dom.h"
#include "../net.h"
#include "../url.h"
#include "../util.h"
#include "../video_decoder.h"
#include "../i18n.h"
#include "js_ast.h"

namespace mer {
namespace js {

namespace {

#define A(i) (args.size() > (i) ? args[i] : Value())
#define NATIVE(name) Value name(Runtime& rt, Obj* self, const Value& thisV, std::vector<Value>& args)
#define UNUSED (void)self; (void)thisV; (void)args; (void)rt

bool IsCallable(const Value& v) { return v.IsObj() && v.o->IsCallable(); }

Node* N(Runtime& rt, const Value& v) {
    if (v.IsObj() && v.o->cls == Cls::Node && v.o->x && v.o->x->node) return v.o->x->node;
    rt.Throw(L"TypeError", L"Illegal invocation");
}
Node* NOpt(const Value& v) {
    if (v.IsObj() && v.o->cls == Cls::Node && v.o->x) return v.o->x->node;
    return nullptr;
}
Value W(Runtime& rt, Node* n) { return n ? Value::O(rt.WrapNode(n)) : Value::Null_(); }

Document& Doc(Runtime& rt) {
    if (!rt.doc) rt.Throw(L"Error", L"No document");
    return *rt.doc;
}

void Dirty(Runtime& rt) { rt.dirty = true; }

template <class F>
void ForEachElem(Node* root, bool includeRoot, F f) {
    std::vector<Node*> st;
    if (includeRoot) st.push_back(root);
    else for (size_t i = root->kids.size(); i-- > 0;) st.push_back(root->kids[i].get());
    while (!st.empty()) {
        Node* c = st.back();
        st.pop_back();
        if (c->kind == Node::Elem && !f(c)) return;
        for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
    }
}

const std::vector<Selector>& Selectors(Runtime& rt, const std::wstring& text) {
    static thread_local std::list<std::pair<std::wstring, std::vector<Selector>>> cache;
    for (auto it = cache.begin(); it != cache.end(); ++it)
        if (it->first == text) { cache.splice(cache.begin(), cache, it); return cache.front().second; }
    std::vector<Selector> sel;
    if (!ParseSelectorList(text, sel)) rt.Throw(L"SyntaxError", L"'" + text + L"' is not a valid selector (merJS)");
    cache.emplace_front(text, std::move(sel));
    if (cache.size() > 200) cache.pop_back();
    return cache.front().second;
}

bool MatchesAny(const std::vector<Selector>& sel, const Node* n) {
    for (auto& s : sel) if (Matches(s, n)) return true;
    return false;
}

void SetAttr(Node* n, const std::wstring& name, const std::wstring& value) {
    std::wstring k = ToLower(name);
    for (auto& a : n->attrs) if (a.first == k) { a.second = value; SyncAttrCache(n); return; }
    if (n->attrs.size() < 256) n->attrs.emplace_back(k, value);
    SyncAttrCache(n);
}
void RemoveAttr(Node* n, const std::wstring& name) {
    std::wstring k = ToLower(name);
    for (size_t i = 0; i < n->attrs.size(); i++) if (n->attrs[i].first == k) { n->attrs.erase(n->attrs.begin() + long(i)); break; }
    SyncAttrCache(n);
}

Node* ToNodeArg(Runtime& rt, const Value& v) {
    if (Node* n = NOpt(v)) return n;
    // dize -> metin dugumu (append/prepend icin)
    return NewNode(Doc(rt), Node::Text, rt.ToStr(v));
}

Node* FindChildElem(Node* n, const wchar_t* name) {
    if (!n) return nullptr;
    for (auto& k : n->kids) if (k->kind == Node::Elem && k->name == name) return k.get();
    return nullptr;
}
Node* HtmlElem(Runtime& rt, Node* root = nullptr) { return FindChildElem(root ? root : Doc(rt).root.get(), L"html"); }
Node* BodyElem(Runtime& rt, Node* root = nullptr) {
    Node* r = root ? root : Doc(rt).root.get();
    Node* b = FindChildElem(HtmlElem(rt, r), L"body");
    if (!b) ForEachElem(r, false, [&](Node* e) { if (e->name == L"body") { b = e; return false; } return true; });
    return b;
}
Node* HeadElem(Runtime& rt, Node* root = nullptr) { return FindChildElem(HtmlElem(rt, root), L"head"); }

void AfterInsert(Runtime& rt, Node* n);
Obj* MakeEvent(Runtime& rt, const std::wstring& type, bool bubbles, bool cancelable);
void DispatchSimpleToObj(Runtime& rt, Obj* target, const std::wstring& type);
void OnMediaSrcChanged(Runtime& rt, Node* n, const std::wstring& srcVal, const Value& videoElemVal);

// Eklenen dugum ya da torunlarindaki <script>'leri calistir
void RunInsertedScripts(Runtime& rt, Node* n) {
    std::vector<Node*> scripts;
    if (n->kind == Node::Elem && n->name == L"script") scripts.push_back(n);
    else ForEachElem(n, false, [&](Node* e) { if (e->name == L"script") scripts.push_back(e); return true; });
    for (Node* s : scripts) {
        if (s->Attr(L"data-merjs-ran")) continue;
        SetAttr(s, L"data-merjs-ran", L"1");
        std::wstring type = s->Attr(L"type") ? ToLower(Trim(*s->Attr(L"type"))) : L"";
        if (!type.empty() && type != L"text/javascript" && type != L"application/javascript" && type != L"module" && type != L"text/ecmascript") continue;
        if (type == L"module") {
            if (auto* src = s->Attr(L"src")) {
                std::wstring u = rt.host ? rt.host->Resolve(*src) : *src;
                rt.LoadModule(u, s);
            } else {
                std::wstring base = rt.host ? rt.host->Resolve(L"") : L"";
                rt.RunModule(TextContent(s), base, s);
            }
            continue;
        }
        if (auto* src = s->Attr(L"src")) {
            if (rt.host) rt.host->RequestScript(s, rt.host->Resolve(*src));
        } else {
            rt.RunScript(TextContent(s), L"inline-dinamik", s);
        }
    }
}

void AfterInsert(Runtime& rt, Node* n) {
    Dirty(rt);
    // yalnizca belgeye bagli dugumlerde betik calistir
    if (Contains(Doc(rt).root.get(), n)) RunInsertedScripts(rt, n);
}

std::vector<Node*> FragmentKids(Node* n) {
    std::vector<Node*> out;
    if (n->kind == Node::Elem && n->name == L"#fragment") for (auto& k : n->kids) out.push_back(k.get());
    else out.push_back(n);
    return out;
}

bool InsertNodes(Runtime& rt, Node* parent, std::vector<Node*> nodes, Node* ref) {
    bool ok = true;
    for (Node* c : nodes) {
        if (Contains(c, parent)) rt.Throw(L"Error", L"HierarchyRequestError: The new child element contains the parent.");
        if (!InsertBefore(Doc(rt), parent, c, ref)) ok = false;
        else AfterInsert(rt, c);
    }
    return ok;
}

// ---------------- Node ----------------

NATIVE(Node_nodeType) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Text) return Value::N(3);
    if (n->kind == Node::Doc) return Value::N(9);
    if (n->name == L"#fragment") return Value::N(11);
    return Value::N(1);
}
NATIVE(Node_nodeName) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Text) return Value::S(L"#text");
    if (n->kind == Node::Doc) return Value::S(L"#document");
    if (n->name == L"#fragment") return Value::S(L"#document-fragment");
    std::wstring u = n->name;
    for (auto& c : u) if (c >= L'a' && c <= L'z') c = wchar_t(c - 32);
    return Value::S(u);
}
NATIVE(Node_nodeValue) { UNUSED; Node* n = N(rt, thisV); return n->kind == Node::Text ? Value::S(n->text) : Value::Null_(); }
NATIVE(Node_setNodeValue) { UNUSED; Node* n = N(rt, thisV); if (n->kind == Node::Text) { n->text = rt.ToStr(A(0)); Dirty(rt); } return Value(); }
NATIVE(Node_textContent) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Doc) return Value::Null_();
    return Value::S(TextContent(n));
}
NATIVE(Node_setTextContent) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring t = A(0).IsNullish() ? L"" : rt.ToStr(A(0));
    if (n->kind == Node::Text) { n->text = t; Dirty(rt); return Value(); }
    while (!n->kids.empty()) RemoveNode(Doc(rt), n->kids.back().get());
    if (!t.empty()) {
        Node* tn = NewNode(Doc(rt), Node::Text, t);
        AppendChild(Doc(rt), n, tn);
    }
    Dirty(rt);
    return Value();
}
NATIVE(Node_parentNode) { UNUSED; Node* n = N(rt, thisV); return W(rt, n->parent); }
NATIVE(Node_parentElement) { UNUSED; Node* n = N(rt, thisV); return (n->parent && n->parent->kind == Node::Elem) ? W(rt, n->parent) : Value::Null_(); }
NATIVE(Node_childNodes) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::vector<Value> v;
    for (auto& k : n->kids) v.push_back(W(rt, k.get()));
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(Node_firstChild) { UNUSED; Node* n = N(rt, thisV); return n->kids.empty() ? Value::Null_() : W(rt, n->kids.front().get()); }
NATIVE(Node_lastChild) { UNUSED; Node* n = N(rt, thisV); return n->kids.empty() ? Value::Null_() : W(rt, n->kids.back().get()); }
Node* Sibling(Node* n, int dir, bool elemOnly) {
    if (!n->parent) return nullptr;
    auto& k = n->parent->kids;
    for (size_t i = 0; i < k.size(); i++) {
        if (k[i].get() != n) continue;
        long j = long(i) + dir;
        while (j >= 0 && j < long(k.size())) {
            if (!elemOnly || k[size_t(j)]->kind == Node::Elem) return k[size_t(j)].get();
            j += dir;
        }
        return nullptr;
    }
    return nullptr;
}
NATIVE(Node_nextSibling) { UNUSED; return W(rt, Sibling(N(rt, thisV), 1, false)); }
NATIVE(Node_previousSibling) { UNUSED; return W(rt, Sibling(N(rt, thisV), -1, false)); }
NATIVE(Node_nextElementSibling) { UNUSED; return W(rt, Sibling(N(rt, thisV), 1, true)); }
NATIVE(Node_previousElementSibling) { UNUSED; return W(rt, Sibling(N(rt, thisV), -1, true)); }
NATIVE(Node_ownerDocument) { UNUSED; return W(rt, Doc(rt).root.get()); }
NATIVE(Node_isConnected) { UNUSED; return Value::B(Contains(Doc(rt).root.get(), N(rt, thisV))); }
NATIVE(Node_hasChildNodes) { UNUSED; return Value::B(!N(rt, thisV)->kids.empty()); }
NATIVE(Node_contains) { UNUSED; Node* o = NOpt(A(0)); return Value::B(o && Contains(N(rt, thisV), o)); }
NATIVE(Node_getRootNode) { UNUSED; Node* n = N(rt, thisV); while (n->parent) n = n->parent; return W(rt, n); }
NATIVE(Node_appendChild) {
    UNUSED;
    Node* p = N(rt, thisV);
    Node* c = NOpt(A(0));
    if (!c) rt.Throw(L"TypeError", L"Failed to execute 'appendChild' on 'Node': parameter 1 is not of type 'Node'.");
    InsertNodes(rt, p, FragmentKids(c), nullptr);
    return A(0);
}
NATIVE(Node_insertBefore) {
    UNUSED;
    Node* p = N(rt, thisV);
    Node* c = NOpt(A(0));
    if (!c) rt.Throw(L"TypeError", L"Failed to execute 'insertBefore' on 'Node': parameter 1 is not of type 'Node'.");
    Node* ref = NOpt(A(1));
    if (ref && ref->parent != p) rt.Throw(L"Error", L"NotFoundError: The node before which the new node is to be inserted is not a child of this node.");
    InsertNodes(rt, p, FragmentKids(c), ref);
    return A(0);
}
NATIVE(Node_removeChild) {
    UNUSED;
    Node* p = N(rt, thisV);
    Node* c = NOpt(A(0));
    if (!c || c->parent != p) rt.Throw(L"Error", L"NotFoundError: The node to be removed is not a child of this node.");
    RemoveNode(Doc(rt), c);
    Dirty(rt);
    return A(0);
}
NATIVE(Node_replaceChild) {
    UNUSED;
    Node* p = N(rt, thisV);
    Node* nw = NOpt(A(0));
    Node* old = NOpt(A(1));
    if (!nw || !old || old->parent != p) rt.Throw(L"Error", L"NotFoundError: The node to be replaced is not a child of this node.");
    InsertNodes(rt, p, FragmentKids(nw), old);
    RemoveNode(Doc(rt), old);
    Dirty(rt);
    return A(1);
}
Node* Clone(Runtime& rt, const Node* n, bool deep) {
    Node* c = NewNode(Doc(rt), n->kind == Node::Text ? Node::Text : Node::Elem, n->kind == Node::Text ? n->text : n->name);
    if (n->kind == Node::Elem) {
        c->name = n->name;
        c->attrs = n->attrs;
        SyncAttrCache(c);
        if (deep) for (auto& k : n->kids) AppendChild(Doc(rt), c, Clone(rt, k.get(), true));
    }
    return c;
}
NATIVE(Node_cloneNode) { UNUSED; return W(rt, Clone(rt, N(rt, thisV), rt.Truthy(A(0)))); }

// ---------------- Element ----------------

NATIVE(El_tagName) { UNUSED; return Node_nodeName(rt, self, thisV, args); }
NATIVE(El_localName) { UNUSED; return Value::S(N(rt, thisV)->name); }
NATIVE(El_id) { UNUSED; return Value::S(N(rt, thisV)->idAttr); }
NATIVE(El_setId) { UNUSED; SetAttr(N(rt, thisV), L"id", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_className) { UNUSED; auto* c = N(rt, thisV)->Attr(L"class"); return Value::S(c ? *c : L""); }
NATIVE(El_setClassName) { UNUSED; SetAttr(N(rt, thisV), L"class", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_getAttribute) { UNUSED; auto* v = N(rt, thisV)->Attr(ToLower(rt.ToStr(A(0))).c_str()); return v ? Value::S(*v) : Value::Null_(); }
NATIVE(El_setAttribute) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring name = ToLower(rt.ToStr(A(0)));
    if (name.empty() || name.find_first_of(L" \"'>/=") != std::wstring::npos) rt.Throw(L"Error", L"InvalidCharacterError: invalid attribute name");
    SetAttr(n, name, rt.ToStr(A(1)));
    if (name == L"value") n->dirtyValue = false;
    Dirty(rt);
    if (name == L"src" && n->name == L"script" && Contains(Doc(rt).root.get(), n) && !n->Attr(L"data-merjs-ran")) RunInsertedScripts(rt, n);
    if (name == L"src" && (n->name == L"video" || n->name == L"audio")) OnMediaSrcChanged(rt, n, rt.ToStr(A(1)), thisV);
    return Value();
}
NATIVE(El_removeAttribute) { UNUSED; RemoveAttr(N(rt, thisV), rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_hasAttribute) { UNUSED; return Value::B(N(rt, thisV)->Attr(ToLower(rt.ToStr(A(0))).c_str()) != nullptr); }
NATIVE(El_toggleAttribute) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring k = ToLower(rt.ToStr(A(0)));
    bool has = n->Attr(k.c_str()) != nullptr;
    bool want = A(1).IsUndef() ? !has : rt.Truthy(A(1));
    if (want && !has) SetAttr(n, k, L"");
    if (!want && has) RemoveAttr(n, k);
    Dirty(rt);
    return Value::B(want);
}
NATIVE(El_getAttributeNames) {
    UNUSED;
    std::vector<Value> v;
    for (auto& a : N(rt, thisV)->attrs) v.push_back(Value::S(a.first));
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_attributes) {
    UNUSED;
    std::vector<Value> v;
    for (auto& a : N(rt, thisV)->attrs) {
        Obj* o = rt.NewObject();
        rt.DefineData(o, L"name", Value::S(a.first));
        rt.DefineData(o, L"value", Value::S(a.second));
        v.push_back(Value::O(o));
    }
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_innerHTML) { UNUSED; return Value::S(SerializeHTML(N(rt, thisV), false)); }
NATIVE(El_outerHTML) { UNUSED; return Value::S(SerializeHTML(N(rt, thisV), true)); }
NATIVE(El_setInnerHTML) {
    UNUSED;
    Node* n = N(rt, thisV);
    Document& d = Doc(rt);
    while (!n->kids.empty()) RemoveNode(d, n->kids.back().get());
    auto frag = ParseFragment(rt.ToStr(A(0)), d);
    for (auto& f : frag) {
        f->parent = n;
        // innerHTML ile eklenen <script> calistirilmaz (tarayici davranisi)
        ForEachElem(f.get(), true, [](Node* e) { if (e->name == L"script") { e->attrs.emplace_back(L"data-merjs-ran", L"1"); } return true; });
        n->kids.push_back(std::move(f));
    }
    Dirty(rt);
    return Value();
}
NATIVE(El_setOuterHTML) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (!n->parent) return Value();
    Document& d = Doc(rt);
    auto frag = ParseFragment(rt.ToStr(A(0)), d);
    std::vector<Node*> raw;
    for (auto& f : frag) { raw.push_back(f.get()); d.detached.push_back(std::move(f)); }
    for (Node* r : raw) InsertBefore(d, n->parent, r, n);
    RemoveNode(d, n);
    Dirty(rt);
    return Value();
}
NATIVE(El_insertAdjacentHTML) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring pos = ToLower(rt.ToStr(A(0)));
    Document& d = Doc(rt);
    auto frag = ParseFragment(rt.ToStr(A(1)), d);
    std::vector<Node*> raw;
    for (auto& f : frag) { raw.push_back(f.get()); d.detached.push_back(std::move(f)); }
    if (pos == L"beforebegin") { if (n->parent) for (Node* r : raw) InsertBefore(d, n->parent, r, n); }
    else if (pos == L"afterbegin") { Node* first = n->kids.empty() ? nullptr : n->kids.front().get(); for (Node* r : raw) InsertBefore(d, n, r, first); }
    else if (pos == L"beforeend") { for (Node* r : raw) AppendChild(d, n, r); }
    else if (pos == L"afterend") { if (n->parent) { Node* nx = Sibling(n, 1, false); for (Node* r : raw) InsertBefore(d, n->parent, r, nx); } }
    else rt.Throw(L"SyntaxError", L"Invalid position for insertAdjacentHTML");
    Dirty(rt);
    return Value();
}
NATIVE(El_insertAdjacentElement) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring pos = ToLower(rt.ToStr(A(0)));
    Node* e = NOpt(A(1));
    if (!e) return Value::Null_();
    if (pos == L"beforebegin" && n->parent) InsertNodes(rt, n->parent, {e}, n);
    else if (pos == L"afterbegin") InsertNodes(rt, n, {e}, n->kids.empty() ? nullptr : n->kids.front().get());
    else if (pos == L"beforeend") InsertNodes(rt, n, {e}, nullptr);
    else if (pos == L"afterend" && n->parent) InsertNodes(rt, n->parent, {e}, Sibling(n, 1, false));
    return A(1);
}
NATIVE(El_children) {
    UNUSED;
    std::vector<Value> v;
    for (auto& k : N(rt, thisV)->kids) if (k->kind == Node::Elem) v.push_back(W(rt, k.get()));
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_childElementCount) {
    UNUSED;
    double c = 0;
    for (auto& k : N(rt, thisV)->kids) if (k->kind == Node::Elem) c++;
    return Value::N(c);
}
NATIVE(El_firstElementChild) { UNUSED; for (auto& k : N(rt, thisV)->kids) if (k->kind == Node::Elem) return W(rt, k.get()); return Value::Null_(); }
NATIVE(El_lastElementChild) {
    UNUSED;
    auto& k = N(rt, thisV)->kids;
    for (size_t i = k.size(); i-- > 0;) if (k[i]->kind == Node::Elem) return W(rt, k[i].get());
    return Value::Null_();
}
NATIVE(El_querySelector) {
    UNUSED;
    Node* n = N(rt, thisV);
    const std::vector<Selector> sel = Selectors(rt, rt.ToStr(A(0)));
    Node* found = nullptr;
    ForEachElem(n, false, [&](Node* e) { if (MatchesAny(sel, e)) { found = e; return false; } return true; });
    return W(rt, found);
}
NATIVE(El_querySelectorAll) {
    UNUSED;
    Node* n = N(rt, thisV);
    const std::vector<Selector> sel = Selectors(rt, rt.ToStr(A(0)));
    std::vector<Value> v;
    ForEachElem(n, false, [&](Node* e) { if (MatchesAny(sel, e)) v.push_back(W(rt, e)); return v.size() < 100000; });
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_getElementsByTagName) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring t = ToLower(rt.ToStr(A(0)));
    std::vector<Value> v;
    ForEachElem(n, false, [&](Node* e) { if (t == L"*" || e->name == t) v.push_back(W(rt, e)); return true; });
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_getElementsByClassName) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::vector<std::wstring> want = SplitWS(rt.ToStr(A(0)));
    std::vector<Value> v;
    ForEachElem(n, false, [&](Node* e) {
        for (auto& w : want) { bool f = false; for (auto& c : e->classes) if (c == w) f = true; if (!f) return true; }
        if (!want.empty()) v.push_back(W(rt, e));
        return true;
    });
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_matches) { UNUSED; return Value::B(MatchesAny(Selectors(rt, rt.ToStr(A(0))), N(rt, thisV))); }
NATIVE(El_closest) {
    UNUSED;
    const std::vector<Selector> sel = Selectors(rt, rt.ToStr(A(0)));
    for (Node* p = N(rt, thisV); p; p = p->parent) if (p->kind == Node::Elem && MatchesAny(sel, p)) return W(rt, p);
    return Value::Null_();
}
NATIVE(El_append) {
    UNUSED;
    Node* n = N(rt, thisV);
    for (auto& a : args) InsertNodes(rt, n, FragmentKids(ToNodeArg(rt, a)), nullptr);
    return Value();
}
NATIVE(El_prepend) {
    UNUSED;
    Node* n = N(rt, thisV);
    Node* first = n->kids.empty() ? nullptr : n->kids.front().get();
    for (auto& a : args) InsertNodes(rt, n, FragmentKids(ToNodeArg(rt, a)), first);
    return Value();
}
NATIVE(El_before) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (!n->parent) return Value();
    for (auto& a : args) InsertNodes(rt, n->parent, FragmentKids(ToNodeArg(rt, a)), n);
    return Value();
}
NATIVE(El_after) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (!n->parent) return Value();
    Node* nx = Sibling(n, 1, false);
    for (auto& a : args) InsertNodes(rt, n->parent, FragmentKids(ToNodeArg(rt, a)), nx);
    return Value();
}
NATIVE(El_remove) { UNUSED; Node* n = N(rt, thisV); if (n->parent) { RemoveNode(Doc(rt), n); Dirty(rt); } return Value(); }
NATIVE(El_replaceWith) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (!n->parent) return Value();
    Node* p = n->parent;
    for (auto& a : args) InsertNodes(rt, p, FragmentKids(ToNodeArg(rt, a)), n);
    RemoveNode(Doc(rt), n);
    Dirty(rt);
    return Value();
}

std::wstring AttrOr(Node* n, const wchar_t* a, const wchar_t* def = L"") { auto* v = n->Attr(a); return v ? *v : def; }

NATIVE(El_value) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->dirtyValue) return Value::S(n->value);
    if (n->name == L"textarea") return Value::S(TextContent(n));
    if (n->name == L"select") {
        std::wstring v;
        bool found = false;
        ForEachElem(n, false, [&](Node* e) {
            if (e->name != L"option") return true;
            std::wstring ov = e->Attr(L"value") ? *e->Attr(L"value") : Trim(TextContent(e));
            if (!found) { v = ov; found = true; }
            if (e->Attr(L"selected")) { v = ov; return false; }
            return true;
        });
        return Value::S(v);
    }
    if (n->name == L"input" && ToLower(AttrOr(n, L"type")) == L"checkbox" && !n->Attr(L"value")) return Value::S(L"on");
    return Value::S(AttrOr(n, L"value"));
}
NATIVE(El_setValue) {
    UNUSED;
    Node* n = N(rt, thisV);
    n->value = A(0).IsNullish() ? L"" : rt.ToStr(A(0));
    n->dirtyValue = true;
    if (n->name == L"select") {
        ForEachElem(n, false, [&](Node* e) {
            if (e->name != L"option") return true;
            std::wstring ov = e->Attr(L"value") ? *e->Attr(L"value") : Trim(TextContent(e));
            if (ov == n->value) SetAttr(e, L"selected", L""); else RemoveAttr(e, L"selected");
            return true;
        });
    }
    Dirty(rt);
    return Value();
}
NATIVE(El_checked) { UNUSED; Node* n = N(rt, thisV); return Value::B(n->dirtyChecked ? n->checked : n->Attr(L"checked") != nullptr); }
NATIVE(El_setChecked) { UNUSED; Node* n = N(rt, thisV); n->checked = rt.Truthy(A(0)); n->dirtyChecked = true; Dirty(rt); return Value(); }
Value BoolAttrGet(Runtime& rt, const Value& thisV, const wchar_t* a) { return Value::B(N(rt, thisV)->Attr(a) != nullptr); }
void BoolAttrSet(Runtime& rt, const Value& thisV, const wchar_t* a, const Value& v) {
    Node* n = N(rt, thisV);
    if (rt.Truthy(v)) SetAttr(n, a, L""); else RemoveAttr(n, a);
    Dirty(rt);
}
NATIVE(El_disabled) { UNUSED; return BoolAttrGet(rt, thisV, L"disabled"); }
NATIVE(El_setDisabled) { UNUSED; BoolAttrSet(rt, thisV, L"disabled", A(0)); return Value(); }
NATIVE(El_hidden) { UNUSED; return BoolAttrGet(rt, thisV, L"hidden"); }
NATIVE(El_setHidden) { UNUSED; BoolAttrSet(rt, thisV, L"hidden", A(0)); return Value(); }
NATIVE(El_href) {
    UNUSED;
    Node* n = N(rt, thisV);
    auto* h = n->Attr(L"href");
    if (!h) return Value::S(L"");
    return Value::S(rt.host ? rt.host->Resolve(*h) : *h);
}
NATIVE(El_setHref) { UNUSED; SetAttr(N(rt, thisV), L"href", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_src) {
    UNUSED;
    Node* n = N(rt, thisV);
    auto* h = n->Attr(L"src");
    if (!h) return Value::S(L"");
    return Value::S(rt.host ? rt.host->Resolve(*h) : *h);
}
NATIVE(El_setSrc) {
    UNUSED;
    Node* n = N(rt, thisV);
    SetAttr(n, L"src", rt.ToStr(A(0)));
    Dirty(rt);
    if (n->name == L"script" && Contains(Doc(rt).root.get(), n) && !n->Attr(L"data-merjs-ran")) RunInsertedScripts(rt, n);
    if (n->name == L"video" || n->name == L"audio") OnMediaSrcChanged(rt, n, rt.ToStr(A(0)), thisV);
    return Value();
}
NATIVE(El_title) { UNUSED; return Value::S(AttrOr(N(rt, thisV), L"title")); }
NATIVE(El_setTitle) { UNUSED; SetAttr(N(rt, thisV), L"title", rt.ToStr(A(0))); return Value(); }
NATIVE(El_type) { UNUSED; Node* n = N(rt, thisV); std::wstring t = ToLower(AttrOr(n, L"type", n->name == L"input" ? L"text" : n->name == L"button" ? L"submit" : L"")); return Value::S(t); }
NATIVE(El_setType) { UNUSED; SetAttr(N(rt, thisV), L"type", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_name) { UNUSED; return Value::S(AttrOr(N(rt, thisV), L"name")); }
NATIVE(El_setName) { UNUSED; SetAttr(N(rt, thisV), L"name", rt.ToStr(A(0))); return Value(); }
NATIVE(El_text) { UNUSED; return Value::S(TextContent(N(rt, thisV))); }
NATIVE(El_form) {
    UNUSED;
    for (Node* p = N(rt, thisV)->parent; p; p = p->parent) if (p->kind == Node::Elem && p->name == L"form") return W(rt, p);
    return Value::Null_();
}
NATIVE(El_namedItem) {
    UNUSED;
    if (args.empty() || args[0].IsUndef()) return Value::Null_();
    std::wstring key = rt.ToStr(args[0]);
    if (key.empty()) return Value::Null_();

    // 1) Eğer thisV bir Array ise (örn. form.elements.namedItem(name))
    if (thisV.IsObj() && thisV.o->cls == Cls::Array) {
        for (const auto& item : thisV.o->elems) {
            if (Node* e = NOpt(item)) {
                const std::wstring* nm = e->Attr(L"name");
                if ((nm && *nm == key) || e->idAttr == key) return item;
            }
        }
        return Value::Null_();
    }

    // 2) Eğer thisV bir DOM Node ise (örn. form.namedItem(name))
    Node* root = NOpt(thisV);
    if (!root) return Value::Null_();

    Node* found = nullptr;
    ForEachElem(root, false, [&](Node* e) {
        const std::wstring* nm = e->Attr(L"name");
        if ((nm && *nm == key) || e->idAttr == key) {
            found = e;
            return false;
        }
        return true;
    });

    return W(rt, found);
}
NATIVE(El_elements) {
    UNUSED;
    std::vector<Value> v;
    ForEachElem(N(rt, thisV), false, [&](Node* e) { if (e->name == L"input" || e->name == L"select" || e->name == L"textarea" || e->name == L"button") v.push_back(W(rt, e)); return true; });
    Obj* arr = rt.NewArrayFrom(v);
    rt.DefineMethod(arr, L"namedItem", El_namedItem, 1);
    for (auto& item : v) {
        if (Node* e = NOpt(item)) {
            if (auto* nm = e->Attr(L"name"); nm && !nm->empty() && !rt.HasProperty(arr, *nm)) {
                rt.DefineData(arr, *nm, item, false);
            }
            if (!e->idAttr.empty() && !rt.HasProperty(arr, e->idAttr)) {
                rt.DefineData(arr, e->idAttr, item, false);
            }
        }
    }
    return Value::O(arr);
}
NATIVE(El_submit) { UNUSED; if (rt.host) rt.host->SubmitForm(N(rt, thisV), nullptr); return Value(); }
NATIVE(El_reset) { UNUSED; ForEachElem(N(rt, thisV), false, [](Node* e) { e->dirtyValue = false; e->dirtyChecked = false; return true; }); Dirty(rt); return Value(); }
NATIVE(El_options) {
    UNUSED;
    std::vector<Value> v;
    ForEachElem(N(rt, thisV), false, [&](Node* e) { if (e->name == L"option") v.push_back(W(rt, e)); return true; });
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(El_selectedIndex) {
    UNUSED;
    double idx = -1, i = 0;
    ForEachElem(N(rt, thisV), false, [&](Node* e) { if (e->name != L"option") return true; if (idx < 0) idx = 0; if (e->Attr(L"selected")) { idx = i; return false; } i++; return true; });
    return Value::N(idx);
}
NATIVE(El_noop) { UNUSED; return Value(); }
NATIVE(El_zero) { UNUSED; return Value::N(0); }
NATIVE(El_contentDocument) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n && n->name == L"iframe") return W(rt, Doc(rt).root.get());
    return Value::Null_();
}
NATIVE(El_contentWindow) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n && n->name == L"iframe") return Value::O(rt.global);
    return Value::Null_();
}

// Media (HTMLMediaElement, HTMLVideoElement, HTMLAudioElement)
NATIVE(El_canPlayType) {
    UNUSED;
    std::wstring type = ToLower(args.empty() || args[0].IsUndef() ? L"" : rt.ToStr(A(0)));
    while (!type.empty() && iswspace(type.front())) type.erase(type.begin());
    while (!type.empty() && iswspace(type.back())) type.pop_back();
    if (type.empty()) return Value::S(L"");

    if (type.find(L"video/mp4") != std::wstring::npos ||
        type.find(L"audio/mp4") != std::wstring::npos ||
        type.find(L"audio/x-m4a") != std::wstring::npos ||
        type.find(L"video/webm") != std::wstring::npos ||
        type.find(L"audio/webm") != std::wstring::npos ||
        type.find(L"audio/mpeg") != std::wstring::npos ||
        type.find(L"audio/mp3") != std::wstring::npos) {
        return Value::S(L"probably");
    }
    if (type.find(L"audio/wav") != std::wstring::npos ||
        type.find(L"audio/wave") != std::wstring::npos ||
        type.find(L"video/ogg") != std::wstring::npos ||
        type.find(L"audio/ogg") != std::wstring::npos ||
        type.find(L"application/x-mpegurl") != std::wstring::npos ||
        type.find(L"application/vnd.apple.mpegurl") != std::wstring::npos) {
        return Value::S(L"maybe");
    }
    return Value::S(L"");
}

NATIVE(El_play) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        SetAttr(n, L"data-mer-paused", L"0");
        rt.DispatchSimple(n, L"play", false);
        rt.DispatchSimple(n, L"playing", false);
        if (auto* srcAttr = n->Attr(L"src")) {
            double cur = _wtof(AttrOr(n, L"data-mer-currenttime", L"0").c_str());
            double dur = _wtof(AttrOr(n, L"data-mer-duration", L"100").c_str());
            uint32_t vw = 640, vh = 360;
            if (auto* wAttr = n->Attr(L"width")) { int v = _wtoi(wAttr->c_str()); if (v > 0) vw = v; }
            if (auto* hAttr = n->Attr(L"height")) { int v = _wtoi(hAttr->c_str()); if (v > 0) vh = v; }
            VideoFrame preview = VideoDecoder::CreatePlayerPreview(vw, vh, cur, dur);
            if (rt.host) rt.host->UpdateVideoFrame(*srcAttr, preview.width, preview.height, preview.bgra);
        }
    }
    Obj* p = rt.NewPromise();
    rt.ResolvePromise(p, Value());
    return Value::O(p);
}

NATIVE(El_pause) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        SetAttr(n, L"data-mer-paused", L"1");
        rt.DispatchSimple(n, L"pause", false);
    }
    return Value();
}

NATIVE(El_load) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        SetAttr(n, L"data-mer-paused", L"1");
        rt.DispatchSimple(n, L"emptied", false);
    }
    return Value();
}

NATIVE(El_currentTime) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(0);
    auto* s = n->Attr(L"data-mer-currenttime");
    if (!s) return Value::N(0);
    return Value::N(_wtof(s->c_str()));
}
NATIVE(El_setCurrentTime) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        double t = rt.ToNum(A(0));
        if (std::isnan(t) || t < 0) t = 0;
        SetAttr(n, L"data-mer-currenttime", Runtime::NumToStr(t));
        rt.DispatchSimple(n, L"timeupdate", false);
    }
    return Value();
}

NATIVE(El_duration) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(0);
    auto* s = n->Attr(L"data-mer-duration");
    if (s) return Value::N(_wtof(s->c_str()));
    return Value::N(100.0);
}
NATIVE(El_setDuration) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        double d = rt.ToNum(A(0));
        SetAttr(n, L"data-mer-duration", Runtime::NumToStr(d));
        rt.DispatchSimple(n, L"durationchange", false);
    }
    return Value();
}

NATIVE(El_paused) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::B(true);
    auto* s = n->Attr(L"data-mer-paused");
    if (s && *s == L"0") return Value::B(false);
    return Value::B(true);
}

NATIVE(El_ended) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::B(false);
    auto* s = n->Attr(L"data-mer-ended");
    return Value::B(s && *s == L"1");
}

NATIVE(El_muted) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::B(false);
    return Value::B(n->Attr(L"muted") != nullptr);
}
NATIVE(El_setMuted) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        if (rt.Truthy(A(0))) SetAttr(n, L"muted", L"");
        else RemoveAttr(n, L"muted");
        rt.DispatchSimple(n, L"volumechange", false);
    }
    return Value();
}

NATIVE(El_volume) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(1.0);
    auto* s = n->Attr(L"data-mer-volume");
    if (s) return Value::N(_wtof(s->c_str()));
    return Value::N(1.0);
}
NATIVE(El_setVolume) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        double v = rt.ToNum(A(0));
        if (std::isnan(v) || v < 0 || v > 1.0) rt.Throw(L"IndexSizeError", L"The volume provided is outside the range [0.0, 1.0]");
        SetAttr(n, L"data-mer-volume", Runtime::NumToStr(v));
        rt.DispatchSimple(n, L"volumechange", false);
    }
    return Value();
}

NATIVE(El_mediaReadyState) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(0);
    auto* s = n->Attr(L"data-mer-readystate");
    if (s) return Value::N(_wtof(s->c_str()));
    return Value::N(4); // HAVE_ENOUGH_DATA
}

NATIVE(El_networkState) {
    UNUSED;
    return Value::N(1); // NETWORK_IDLE
}

NATIVE(El_videoWidth) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(0);
    auto* s = n->Attr(L"width");
    if (s) return Value::N(_wtof(s->c_str()));
    return Value::N(640);
}

NATIVE(El_videoHeight) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(0);
    auto* s = n->Attr(L"height");
    if (s) return Value::N(_wtof(s->c_str()));
    return Value::N(360);
}

NATIVE(El_currentSrc) {
    UNUSED;
    return El_src(rt, self, thisV, args);
}

NATIVE(El_autoplay) { UNUSED; return BoolAttrGet(rt, thisV, L"autoplay"); }
NATIVE(El_setAutoplay) { UNUSED; BoolAttrSet(rt, thisV, L"autoplay", A(0)); return Value(); }
NATIVE(El_loop) { UNUSED; return BoolAttrGet(rt, thisV, L"loop"); }
NATIVE(El_setLoop) { UNUSED; BoolAttrSet(rt, thisV, L"loop", A(0)); return Value(); }
NATIVE(El_controls) { UNUSED; return BoolAttrGet(rt, thisV, L"controls"); }
NATIVE(El_setControls) { UNUSED; BoolAttrSet(rt, thisV, L"controls", A(0)); return Value(); }
NATIVE(El_seeking) { UNUSED; return Value::B(false); }
NATIVE(El_playbackRate) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (!n) return Value::N(1.0);
    auto* s = n->Attr(L"data-mer-playbackrate");
    if (s) return Value::N(_wtof(s->c_str()));
    return Value::N(1.0);
}
NATIVE(El_setPlaybackRate) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        double r = rt.ToNum(A(0));
        if (std::isnan(r)) r = 1.0;
        SetAttr(n, L"data-mer-playbackrate", Runtime::NumToStr(r));
        rt.DispatchSimple(n, L"ratechange", false);
    }
    return Value();
}

NATIVE(TimeRanges_start) {
    UNUSED;
    return Value::N(0);
}
NATIVE(TimeRanges_end) {
    UNUSED;
    Node* n = NOpt(thisV);
    if (n) {
        auto* s = n->Attr(L"data-mer-duration");
        if (s) return Value::N(_wtof(s->c_str()));
    }
    return Value::N(100.0);
}
NATIVE(El_mediaTimeRanges) {
    UNUSED;
    Obj* tr = rt.NewObject();
    rt.DefineData(tr, L"length", Value::N(1));
    rt.DefineMethod(tr, L"start", TimeRanges_start, 1);
    rt.DefineMethod(tr, L"end", TimeRanges_end, 1);
    return Value::O(tr);
}

NATIVE(Audio_ctor) {
    UNUSED;
    Node* n = NewNode(Doc(rt), Node::Elem, L"audio");
    if (!args.empty() && !A(0).IsUndef()) {
        SetAttr(n, L"src", rt.ToStr(A(0)));
    }
    return W(rt, n);
}

// MediaSource & SourceBuffer (MSE)
NATIVE(MediaSource_isTypeSupported) {
    UNUSED;
    std::wstring type = ToLower(args.empty() || args[0].IsUndef() ? L"" : rt.ToStr(A(0)));
    while (!type.empty() && iswspace(type.front())) type.erase(type.begin());
    while (!type.empty() && iswspace(type.back())) type.pop_back();
    if (type.empty()) return Value::B(false);

    if (type.find(L"video/mp4") != std::wstring::npos ||
        type.find(L"audio/mp4") != std::wstring::npos ||
        type.find(L"audio/x-m4a") != std::wstring::npos ||
        type.find(L"video/webm") != std::wstring::npos ||
        type.find(L"audio/webm") != std::wstring::npos ||
        type.find(L"audio/mpeg") != std::wstring::npos ||
        type.find(L"audio/mp3") != std::wstring::npos) {
        return Value::B(true);
    }
    return Value::B(false);
}

NATIVE(MediaSource_ctor) {
    UNUSED;
    Obj* proto = rt.mediaSourceProto;
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* ms = rt.NewObject(proto ? proto : rt.eventTargetProto);
    ms->X();

    Obj* sbList = rt.NewArray(0);
    Obj* asbList = rt.NewArray(0);

    rt.DefineData(ms, L"readyState", Value::S(L"closed"));
    rt.DefineData(ms, L"duration", Value::N(std::numeric_limits<double>::quiet_NaN()));
    rt.DefineData(ms, L"sourceBuffers", Value::O(sbList));
    rt.DefineData(ms, L"activeSourceBuffers", Value::O(asbList));
    rt.DefineData(ms, L"onsourceopen", Value::Null_());
    rt.DefineData(ms, L"onsourceended", Value::Null_());
    rt.DefineData(ms, L"onsourceclose", Value::Null_());
    rt.DefineData(ms, L"\x02isMediaSource", Value::B(true), false);
    return Value::O(ms);
}

NATIVE(MediaSource_addSourceBuffer) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    Obj* ms = thisV.o;
    std::wstring state = rt.ToStr(rt.Get(thisV, L"readyState"));
    if (state != L"open") {
        rt.Throw(L"InvalidStateError", L"The MediaSource's readyState is not 'open'");
    }
    std::wstring type = args.empty() || args[0].IsUndef() ? L"" : rt.ToStr(A(0));
    std::vector<Value> chkArgs{Value::S(type)};
    if (!MediaSource_isTypeSupported(rt, self, thisV, chkArgs).b) {
        rt.Throw(L"NotSupportedError", L"The type provided is not supported");
    }

    Obj* sb = rt.NewObject(rt.sourceBufferProto ? rt.sourceBufferProto : rt.eventTargetProto);
    sb->X();
    rt.DefineData(sb, L"\x02isSourceBuffer", Value::B(true), false);
    rt.DefineData(sb, L"\x02mediaSource", Value::O(ms), false);
    rt.DefineData(sb, L"updating", Value::B(false));
    rt.DefineData(sb, L"mode", Value::S(L"segments"));
    rt.DefineData(sb, L"timestampOffset", Value::N(0));
    rt.DefineData(sb, L"appendWindowStart", Value::N(0));
    rt.DefineData(sb, L"appendWindowEnd", Value::N(std::numeric_limits<double>::infinity()));
    rt.DefineData(sb, L"\x02bufferedEnd", Value::N(0), false);
    rt.DefineData(sb, L"\x02bytesAppended", Value::N(0), false);
    rt.DefineData(sb, L"onupdatestart", Value::Null_());
    rt.DefineData(sb, L"onupdate", Value::Null_());
    rt.DefineData(sb, L"onupdateend", Value::Null_());
    rt.DefineData(sb, L"onerror", Value::Null_());
    rt.DefineData(sb, L"onabort", Value::Null_());

    Obj* tr = rt.NewObject();
    rt.DefineData(tr, L"length", Value::N(0));
    rt.DefineMethod(tr, L"start", TimeRanges_start, 1);
    rt.DefineMethod(tr, L"end", TimeRanges_end, 1);
    rt.DefineData(sb, L"buffered", Value::O(tr));

    Value sbListV = rt.Get(thisV, L"sourceBuffers");
    if (sbListV.IsObj()) {
        sbListV.o->elems.push_back(Value::O(sb));
        rt.DefineData(sbListV.o, L"length", Value::N(double(sbListV.o->elems.size())));
        rt.DefineData(sbListV.o, std::to_wstring(sbListV.o->elems.size() - 1), Value::O(sb));
    }
    Value asbListV = rt.Get(thisV, L"activeSourceBuffers");
    if (asbListV.IsObj()) {
        asbListV.o->elems.push_back(Value::O(sb));
        rt.DefineData(asbListV.o, L"length", Value::N(double(asbListV.o->elems.size())));
        rt.DefineData(asbListV.o, std::to_wstring(asbListV.o->elems.size() - 1), Value::O(sb));
    }
    return Value::O(sb);
}

NATIVE(MediaSource_removeSourceBuffer) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    Value sbV = A(0);
    if (!sbV.IsObj()) rt.Throw(L"TypeError", L"Argument 1 of MediaSource.removeSourceBuffer is not an object");
    Value sbListV = rt.Get(thisV, L"sourceBuffers");
    if (sbListV.IsObj()) {
        auto& el = sbListV.o->elems;
        for (auto it = el.begin(); it != el.end(); ++it) {
            if (it->IsObj() && it->o == sbV.o) {
                el.erase(it);
                break;
            }
        }
        rt.DefineData(sbListV.o, L"length", Value::N(double(el.size())));
    }
    return Value();
}

NATIVE(MediaSource_endOfStream) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    Obj* ms = thisV.o;
    std::wstring state = rt.ToStr(rt.Get(thisV, L"readyState"));
    if (state != L"open") {
        rt.Throw(L"InvalidStateError", L"The MediaSource's readyState is not 'open'");
    }
    rt.DefineData(ms, L"readyState", Value::S(L"ended"));
    DispatchSimpleToObj(rt, ms, L"sourceended");
    return Value();
}

// SourceBuffer
NATIVE(SourceBuffer_appendBuffer) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    Obj* sb = thisV.o;
    if (rt.Truthy(rt.Get(thisV, L"updating"))) {
        rt.Throw(L"InvalidStateError", L"The SourceBuffer is already updating");
    }
    const uint8_t* ptr = nullptr;
    size_t len = 0;
    Value a0 = A(0);
    if (a0.IsObj() && a0.o->cls == Cls::ArrayBuffer && a0.o->x) {
        ptr = a0.o->x->bufferData ? a0.o->x->bufferData->data() : nullptr;
        len = a0.o->x->byteLength;
    } else if (a0.IsObj() && (a0.o->cls == Cls::TypedArray || a0.o->cls == Cls::DataView) && a0.o->x) {
        ptr = (a0.o->x->bufferData ? a0.o->x->bufferData->data() : nullptr) + a0.o->x->byteOffset;
        len = a0.o->x->byteLength;
    } else {
        rt.Throw(L"TypeError", L"Failed to execute 'appendBuffer' on 'SourceBuffer': parameter 1 is not of type 'BufferSource'");
    }

    if (ptr && len > 0) {
        if (!sb->x) sb->X();
        if (!sb->x->bufferData) {
            sb->x->bufferData = std::make_shared<std::vector<uint8_t>>();
        }
        size_t oldSize = sb->x->bufferData->size();
        sb->x->bufferData->resize(oldSize + len);
        memcpy(sb->x->bufferData->data() + oldSize, ptr, len);

        double prevAppended = rt.ToNum(rt.Get(thisV, L"\x02bytesAppended"));
        rt.DefineData(sb, L"\x02bytesAppended", Value::N(prevAppended + double(len)), false);

        VideoDecoder decoder;
        VideoFrame decoded;
        if (decoder.DecodeBuffer(sb->x->bufferData->data(), sb->x->bufferData->size(), decoded)) {
            Value msV = rt.Get(thisV, L"\x02mediaSource");
            if (msV.IsObj()) {
                Node* vNode = NOpt(rt.Get(msV, L"\x02videoNode"));
                if (vNode) {
                    auto* srcAttr = vNode->Attr(L"src");
                    if (srcAttr && rt.host) {
                        rt.host->UpdateVideoFrame(*srcAttr, decoded.width, decoded.height, decoded.bgra);
                    }
                }
            }
        }
    }

    rt.DefineData(sb, L"updating", Value::B(true));
    DispatchSimpleToObj(rt, sb, L"updatestart");

    Obj* job = rt.NewFunction([](Runtime& r, Obj* s, const Value&, std::vector<Value>&) {
        Obj* target = s->x && !s->x->slots.empty() && s->x->slots[0].IsObj() ? s->x->slots[0].o : nullptr;
        if (!target) return Value();

        double curBuf = r.ToNum(r.Get(Value::O(target), L"\x02bufferedEnd"));
        double newBuf = curBuf + 5.0; // her parca 5 saniye
        r.DefineData(target, L"\x02bufferedEnd", Value::N(newBuf), false);

        Obj* tr = r.NewObject();
        r.DefineData(tr, L"length", Value::N(1));
        r.DefineMethod(tr, L"start", TimeRanges_start, 1);
        r.DefineMethod(tr, L"end", [](Runtime& r2, Obj*, const Value& thisV2, std::vector<Value>&) {
            Obj* trObj = thisV2.IsObj() ? thisV2.o : nullptr;
            Obj* origSb = trObj && trObj->x && !trObj->x->slots.empty() && trObj->x->slots[0].IsObj() ? trObj->x->slots[0].o : nullptr;
            if (origSb) return r2.Get(Value::O(origSb), L"\x02bufferedEnd");
            return Value::N(0.0);
        }, 1);
        tr->X().slots = {Value::O(target)};
        r.DefineData(target, L"buffered", Value::O(tr));

        Value msV = r.Get(Value::O(target), L"\x02mediaSource");
        if (msV.IsObj()) {
            double msDur = r.ToNum(r.Get(msV, L"duration"));
            if (std::isnan(msDur) || msDur < newBuf) {
                r.DefineData(msV.o, L"duration", Value::N(newBuf));
            }
            Node* vNode = NOpt(r.Get(msV, L"\x02videoNode"));
            if (vNode) {
                SetAttr(vNode, L"data-mer-duration", Runtime::NumToStr(newBuf));
                SetAttr(vNode, L"data-mer-readystate", L"4");
                r.DispatchSimple(vNode, L"durationchange", false);
                r.DispatchSimple(vNode, L"progress", false);
                if (auto* srcAttr = vNode->Attr(L"src")) {
                    double cur = _wtof(AttrOr(vNode, L"data-mer-currenttime", L"0").c_str());
                    uint32_t vw = 640, vh = 360;
                    if (auto* wAttr = vNode->Attr(L"width")) { int v = _wtoi(wAttr->c_str()); if (v > 0) vw = v; }
                    if (auto* hAttr = vNode->Attr(L"height")) { int v = _wtoi(hAttr->c_str()); if (v > 0) vh = v; }
                    VideoFrame preview = VideoDecoder::CreatePlayerPreview(vw, vh, cur, newBuf);
                    if (r.host) r.host->UpdateVideoFrame(*srcAttr, preview.width, preview.height, preview.bgra);
                }
            }
        }

        r.DefineData(target, L"updating", Value::B(false));
        DispatchSimpleToObj(r, target, L"update");
        DispatchSimpleToObj(r, target, L"updateend");
        return Value();
    }, L"sb_append_job", 0);
    job->x->slots = {Value::O(sb)};
    rt.Enqueue(Value::O(job), {});
    return Value();
}

NATIVE(SourceBuffer_abort) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    if (rt.Truthy(rt.Get(thisV, L"updating"))) {
        rt.DefineData(thisV.o, L"updating", Value::B(false));
        DispatchSimpleToObj(rt, thisV.o, L"abort");
        DispatchSimpleToObj(rt, thisV.o, L"updateend");
    }
    return Value();
}

NATIVE(SourceBuffer_remove) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    if (rt.Truthy(rt.Get(thisV, L"updating"))) {
        rt.Throw(L"InvalidStateError", L"The SourceBuffer is already updating");
    }
    rt.DefineData(thisV.o, L"updating", Value::B(true));
    DispatchSimpleToObj(rt, thisV.o, L"updatestart");

    Obj* sb = thisV.o;
    Obj* job = rt.NewFunction([](Runtime& r, Obj* s, const Value&, std::vector<Value>&) {
        Obj* target = s->x && !s->x->slots.empty() && s->x->slots[0].IsObj() ? s->x->slots[0].o : nullptr;
        if (!target) return Value();
        r.DefineData(target, L"updating", Value::B(false));
        DispatchSimpleToObj(r, target, L"update");
        DispatchSimpleToObj(r, target, L"updateend");
        return Value();
    }, L"sb_remove_job", 0);
    job->x->slots = {Value::O(sb)};
    rt.Enqueue(Value::O(job), {});
    return Value();
}

// Blob
NATIVE(Blob_ctor) {
    UNUSED;
    Obj* proto = rt.blobProto;
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* blob = rt.NewObject(proto ? proto : rt.objectProto);
    blob->X();
    blob->x->bufferData = std::make_shared<std::vector<uint8_t>>();

    Value parts = A(0);
    if (parts.IsObj()) {
        std::vector<Value> list;
        if (parts.o->cls == Cls::Array) list = parts.o->elems;
        else list = rt.IterateToList(parts);

        for (auto& item : list) {
            if (item.IsStr()) {
                std::string u8 = ToUtf8(*item.s);
                size_t oldSz = blob->x->bufferData->size();
                blob->x->bufferData->resize(oldSz + u8.size());
                if (!u8.empty()) memcpy(blob->x->bufferData->data() + oldSz, u8.data(), u8.size());
            } else if (item.IsObj() && item.o->cls == Cls::ArrayBuffer && item.o->x) {
                const uint8_t* p = item.o->x->bufferData ? item.o->x->bufferData->data() : nullptr;
                size_t sz = item.o->x->byteLength;
                if (p && sz > 0) {
                    size_t oldSz = blob->x->bufferData->size();
                    blob->x->bufferData->resize(oldSz + sz);
                    memcpy(blob->x->bufferData->data() + oldSz, p, sz);
                }
            } else if (item.IsObj() && (item.o->cls == Cls::TypedArray || item.o->cls == Cls::DataView) && item.o->x) {
                const uint8_t* p = (item.o->x->bufferData ? item.o->x->bufferData->data() : nullptr) + item.o->x->byteOffset;
                size_t sz = item.o->x->byteLength;
                if (p && sz > 0) {
                    size_t oldSz = blob->x->bufferData->size();
                    blob->x->bufferData->resize(oldSz + sz);
                    memcpy(blob->x->bufferData->data() + oldSz, p, sz);
                }
            } else if (item.IsObj() && rt.Get(item, L"\x02isBlob").b && item.o->x && item.o->x->bufferData) {
                auto& otherBuf = *item.o->x->bufferData;
                size_t oldSz = blob->x->bufferData->size();
                blob->x->bufferData->resize(oldSz + otherBuf.size());
                if (!otherBuf.empty()) memcpy(blob->x->bufferData->data() + oldSz, otherBuf.data(), otherBuf.size());
            } else {
                std::string u8 = ToUtf8(rt.ToStr(item));
                size_t oldSz = blob->x->bufferData->size();
                blob->x->bufferData->resize(oldSz + u8.size());
                if (!u8.empty()) memcpy(blob->x->bufferData->data() + oldSz, u8.data(), u8.size());
            }
        }
    }

    std::wstring type = L"";
    if (args.size() > 1 && A(1).IsObj()) {
        Value tVal = rt.Get(A(1), L"type");
        if (!tVal.IsUndef()) type = ToLower(rt.ToStr(tVal));
    }

    rt.DefineData(blob, L"size", Value::N(double(blob->x->bufferData->size())));
    rt.DefineData(blob, L"type", Value::S(type));
    rt.DefineData(blob, L"\x02isBlob", Value::B(true), false);
    return Value::O(blob);
}

NATIVE(BlobP_text) {
    UNUSED;
    if (!thisV.IsObj() || !thisV.o->x || !thisV.o->x->bufferData) rt.Throw(L"TypeError", L"Illegal invocation");
    Obj* p = rt.NewPromise();
    std::wstring s = FromUtf8(thisV.o->x->bufferData->data(), thisV.o->x->bufferData->size());
    rt.ResolvePromise(p, Value::S(s));
    return Value::O(p);
}

NATIVE(BlobP_arrayBuffer) {
    UNUSED;
    if (!thisV.IsObj() || !thisV.o->x || !thisV.o->x->bufferData) rt.Throw(L"TypeError", L"Illegal invocation");
    Obj* p = rt.NewPromise();
    Obj* ab = rt.NewArrayBufferFrom(thisV.o->x->bufferData->data(), thisV.o->x->bufferData->size());
    rt.ResolvePromise(p, Value::O(ab));
    return Value::O(p);
}

NATIVE(BlobP_slice) {
    UNUSED;
    if (!thisV.IsObj() || !thisV.o->x || !thisV.o->x->bufferData) rt.Throw(L"TypeError", L"Illegal invocation");
    auto& data = *thisV.o->x->bufferData;
    int64_t len = int64_t(data.size());
    int64_t b = A(0).IsUndef() ? 0 : int64_t(rt.ToNum(A(0)));
    if (b < 0) b = std::max<int64_t>(0, len + b); else b = std::min<int64_t>(len, b);
    int64_t e = A(1).IsUndef() ? len : int64_t(rt.ToNum(A(1)));
    if (e < 0) e = std::max<int64_t>(0, len + e); else e = std::min<int64_t>(len, e);
    size_t count = b < e ? size_t(e - b) : 0;

    std::wstring contentType = A(2).IsUndef() ? L"" : ToLower(rt.ToStr(A(2)));

    Obj* newBlob = rt.NewObject(rt.blobProto ? rt.blobProto : rt.objectProto);
    newBlob->X();
    newBlob->x->bufferData = std::make_shared<std::vector<uint8_t>>(count);
    if (count > 0) {
        memcpy(newBlob->x->bufferData->data(), data.data() + b, count);
    }
    rt.DefineData(newBlob, L"size", Value::N(double(count)));
    rt.DefineData(newBlob, L"type", Value::S(contentType));
    rt.DefineData(newBlob, L"\x02isBlob", Value::B(true), false);
    return Value::O(newBlob);
}

// URL.createObjectURL / URL.revokeObjectURL
NATIVE(URL_createObjectURL) {
    UNUSED;
    if (args.empty() || !A(0).IsObj()) {
        rt.Throw(L"TypeError", L"Failed to execute 'createObjectURL' on 'URL': Overload resolution failed.");
    }
    Obj* target = A(0).o;
    std::wstring origin = rt.host ? ParseUrl(rt.host->PageUrl()).Origin() : L"https://www.youtube.com";
    if (origin.empty() || origin == L"about:blank") origin = L"https://www.youtube.com";
    std::wstring url = L"blob:" + origin + L"/" + std::to_wstring(rt.nextBlobId++);
    rt.objectUrls[url] = target;
    return Value::S(url);
}

NATIVE(URL_revokeObjectURL) {
    UNUSED;
    if (!args.empty() && !A(0).IsUndef()) {
        std::wstring url = rt.ToStr(A(0));
        rt.objectUrls.erase(url);
    }
    return Value();
}

void OnMediaSrcChanged(Runtime& rt, Node* n, const std::wstring& srcVal, const Value& videoElemVal) {
    if (!n || (n->name != L"video" && n->name != L"audio")) return;
    if (n->name == L"video") {
        uint32_t vw = 640, vh = 360;
        if (auto* wAttr = n->Attr(L"width")) { int v = _wtoi(wAttr->c_str()); if (v > 0) vw = v; }
        if (auto* hAttr = n->Attr(L"height")) { int v = _wtoi(hAttr->c_str()); if (v > 0) vh = v; }
        VideoFrame preview = VideoDecoder::CreatePlayerPreview(vw, vh, 0.0, 100.0);
        if (rt.host) {
            rt.host->UpdateVideoFrame(srcVal, preview.width, preview.height, preview.bgra);
        }
    }
    if (StartsWith(srcVal, L"blob:")) {
        auto it = rt.objectUrls.find(srcVal);
        if (it != rt.objectUrls.end() && it->second) {
            Obj* targetObj = it->second;
            Value isMS = rt.Get(Value::O(targetObj), L"\x02isMediaSource");
            if (rt.Truthy(isMS)) {
                Obj* ms = targetObj;
                rt.DefineData(ms, L"\x02videoNode", videoElemVal, false);
                if (videoElemVal.IsObj()) {
                    rt.DefineData(videoElemVal.o, L"\x02mediaSource", Value::O(ms), false);
                }

                // 1. MediaSource readyState -> "open"
                rt.DefineData(ms, L"readyState", Value::S(L"open"));

                // 2. Queue 'sourceopen' event on MediaSource
                Obj* job = rt.NewFunction([](Runtime& r, Obj* s, const Value&, std::vector<Value>&) {
                    Obj* msTarget = s->x && !s->x->slots.empty() && s->x->slots[0].IsObj() ? s->x->slots[0].o : nullptr;
                    if (msTarget) {
                        DispatchSimpleToObj(r, msTarget, L"sourceopen");
                    }
                    return Value();
                }, L"ms_sourceopen_job", 0);
                job->x->slots = {Value::O(ms)};
                rt.Enqueue(Value::O(job), {});

                // 3. Update video readyState & fire load events
                SetAttr(n, L"data-mer-readystate", L"2"); // HAVE_CURRENT_DATA
                rt.DispatchSimple(n, L"loadstart", false);
                rt.DispatchSimple(n, L"loadedmetadata", false);
                rt.DispatchSimple(n, L"loadeddata", false);
                rt.DispatchSimple(n, L"canplay", false);
                rt.DispatchSimple(n, L"canplaythrough", false);
            }
        }
    }
}

NATIVE(Doc_write);

NATIVE(Win_writeEmbed) {
    UNUSED;
    std::wstring html;
    for (auto& a : args) html += rt.ToStr(a);
    if (!html.empty()) {
        std::vector<Value> docArgs{Value::S(html)};
        return Doc_write(rt, self, Value::O(rt.doc ? rt.WrapNode(rt.doc->root.get()) : nullptr), docArgs);
    }
    return Value();
}

NATIVE(Doc_open) {
    UNUSED;
    return Value::O(rt.doc ? rt.WrapNode(rt.doc->root.get()) : nullptr);
}

NATIVE(Doc_close) {
    UNUSED;
    Dirty(rt);
    return Value();
}

Obj* MakeRect(Runtime& rt, double x, double y, double w, double h) {
    Obj* r = rt.NewObject();
    rt.DefineData(r, L"x", Value::N(x)); rt.DefineData(r, L"y", Value::N(y));
    rt.DefineData(r, L"left", Value::N(x)); rt.DefineData(r, L"top", Value::N(y));
    rt.DefineData(r, L"width", Value::N(w)); rt.DefineData(r, L"height", Value::N(h));
    rt.DefineData(r, L"right", Value::N(x + w)); rt.DefineData(r, L"bottom", Value::N(y + h));
    return r;
}
bool RectOf(Runtime& rt, Node* n, double& x, double& y, double& w, double& h) {
    x = y = w = h = 0;
    return rt.host && rt.host->NodeRect(n, x, y, w, h);
}
NATIVE(El_getBoundingClientRect) { UNUSED; double x, y, w, h; RectOf(rt, N(rt, thisV), x, y, w, h); return Value::O(MakeRect(rt, x, y, w, h)); }
NATIVE(El_getClientRects) { UNUSED; double x, y, w, h; RectOf(rt, N(rt, thisV), x, y, w, h); return Value::O(rt.NewArrayFrom({Value::O(MakeRect(rt, x, y, w, h))})); }
NATIVE(El_offsetWidth) { UNUSED; double x, y, w, h; RectOf(rt, N(rt, thisV), x, y, w, h); return Value::N(std::round(w)); }
NATIVE(El_offsetHeight) { UNUSED; double x, y, w, h; RectOf(rt, N(rt, thisV), x, y, w, h); return Value::N(std::round(h)); }
NATIVE(El_offsetTop) { UNUSED; double x, y, w, h; RectOf(rt, N(rt, thisV), x, y, w, h); return Value::N(std::round(y)); }
NATIVE(El_offsetLeft) { UNUSED; double x, y, w, h; RectOf(rt, N(rt, thisV), x, y, w, h); return Value::N(std::round(x)); }
NATIVE(El_offsetParent) { UNUSED; return W(rt, BodyElem(rt)); }
NATIVE(El_click) {
    UNUSED;
    Node* n = N(rt, thisV);
    Obj* ev = nullptr;
    {
        std::vector<Value> a{Value::S(L"click")};
        Obj* opts = rt.NewObject();
        rt.DefineData(opts, L"bubbles", Value::B(true));
        rt.DefineData(opts, L"cancelable", Value::B(true));
        a.push_back(Value::O(opts));
        Value e = rt.Construct(Value::O(rt.ctors[L"Event"]), a);
        ev = e.o;
    }
    bool ok = rt.DispatchEvent(n, ev);
    if (ok) {
        // varsayilan eylemler: onay kutusu, gonder dugmesi
        if (n->name == L"input") {
            std::wstring t = ToLower(AttrOr(n, L"type"));
            if (t == L"checkbox") { n->checked = !(n->dirtyChecked ? n->checked : n->Attr(L"checked") != nullptr); n->dirtyChecked = true; Dirty(rt); }
            if (t == L"submit" && rt.host) for (Node* p = n->parent; p; p = p->parent) if (p->name == L"form") { rt.host->SubmitForm(p, n); break; }
        }
    }
    return Value();
}

// ---------------- style / dataset / classList (ozel nesneler) ----------------

std::wstring Kebab(const std::wstring& k) {
    std::wstring o;
    for (wchar_t c : k) {
        if (c >= L'A' && c <= L'Z') { o += L'-'; o += wchar_t(c + 32); }
        else o += c;
    }
    if (StartsWith(o, L"webkit-") || StartsWith(o, L"moz-") || StartsWith(o, L"ms-")) o = L"-" + o;
    if (o == L"css-float") o = L"float";
    return o;
}
std::wstring Camel(const std::wstring& k) {
    std::wstring o;
    bool up = false;
    for (wchar_t c : k) {
        if (c == L'-') { up = true; continue; }
        o += up ? wchar_t(towupper(c)) : c;
        up = false;
    }
    return o;
}

NATIVE(El_style) {
    UNUSED;
    Obj* s = rt.NewObject(rt.styleProto);
    s->cls = Cls::StyleDecl;
    s->X().node = N(rt, thisV);
    return Value::O(s);
}
NATIVE(El_dataset) {
    UNUSED;
    Obj* s = rt.NewObject();
    s->cls = Cls::Dataset;
    s->X().node = N(rt, thisV);
    return Value::O(s);
}
NATIVE(El_classList) {
    UNUSED;
    Obj* s = rt.NewObject(rt.classListProto);
    s->cls = Cls::ClassList;
    s->X().node = N(rt, thisV);
    return Value::O(s);
}

std::vector<Decl> InlineDecls(Node* n) {
    auto* st = n->Attr(L"style");
    return st ? ParseDeclarations(*st) : std::vector<Decl>();
}
void WriteDecls(Node* n, const std::vector<Decl>& d) {
    std::wstring o;
    for (auto& x : d) {
        if (!o.empty()) o += L" ";
        o += x.prop + L": " + x.value + (x.important ? L" !important" : L"") + L";";
    }
    if (o.empty()) RemoveAttr(n, L"style"); else SetAttr(n, L"style", o);
}
void StyleSet(Runtime& rt, Node* n, const std::wstring& prop, const std::wstring& value, bool important) {
    std::vector<Decl> d = InlineDecls(n);
    bool found = false;
    for (size_t i = 0; i < d.size(); i++) {
        if (d[i].prop == prop) {
            if (value.empty()) d.erase(d.begin() + long(i));
            else { d[i].value = value; d[i].important = important; }
            found = true;
            break;
        }
    }
    if (!found && !value.empty()) { Decl x; x.prop = prop; x.value = value; x.important = important; d.push_back(x); }
    WriteDecls(n, d);
    Dirty(rt);
}
NATIVE(Style_setProperty) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::StyleDecl) rt.Throw(L"TypeError", L"Illegal invocation");
    StyleSet(rt, thisV.o->x->node, ToLower(rt.ToStr(A(0))), A(1).IsNullish() ? L"" : rt.ToStr(A(1)), ToLower(rt.ToStr(A(2))) == L"important");
    return Value();
}
NATIVE(Style_getPropertyValue) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::StyleDecl) rt.Throw(L"TypeError", L"Illegal invocation");
    std::wstring p = ToLower(rt.ToStr(A(0)));
    for (auto& d : InlineDecls(thisV.o->x->node)) if (d.prop == p) return Value::S(d.value);
    // getComputedStyle nesnesi icin kayitli degerler
    Value v = rt.Get(thisV, L"\x02computed:" + p);
    return v.IsStr() ? v : Value::S(L"");
}
NATIVE(Style_removeProperty) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::StyleDecl) rt.Throw(L"TypeError", L"Illegal invocation");
    StyleSet(rt, thisV.o->x->node, ToLower(rt.ToStr(A(0))), L"", false);
    return Value::S(L"");
}

Node* CLNode(Runtime& rt, const Value& thisV) {
    if (thisV.IsObj() && thisV.o->cls == Cls::ClassList && thisV.o->x) return thisV.o->x->node;
    rt.Throw(L"TypeError", L"Illegal invocation");
}
void SetClasses(Runtime& rt, Node* n, const std::vector<std::wstring>& cls) {
    std::wstring s;
    for (auto& c : cls) { if (!s.empty()) s += L' '; s += c; }
    SetAttr(n, L"class", s);
    Dirty(rt);
}
NATIVE(CL_add) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::vector<std::wstring> c = n->classes;
    for (auto& a : args) { std::wstring t = rt.ToStr(a); if (t.empty()) continue; if (std::find(c.begin(), c.end(), t) == c.end()) c.push_back(t); }
    SetClasses(rt, n, c);
    return Value();
}
NATIVE(CL_remove) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::vector<std::wstring> c = n->classes;
    for (auto& a : args) { std::wstring t = rt.ToStr(a); c.erase(std::remove(c.begin(), c.end(), t), c.end()); }
    SetClasses(rt, n, c);
    return Value();
}
NATIVE(CL_contains) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::wstring t = rt.ToStr(A(0));
    return Value::B(std::find(n->classes.begin(), n->classes.end(), t) != n->classes.end());
}
NATIVE(CL_toggle) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::wstring t = rt.ToStr(A(0));
    std::vector<std::wstring> c = n->classes;
    bool has = std::find(c.begin(), c.end(), t) != c.end();
    bool want = A(1).IsUndef() ? !has : rt.Truthy(A(1));
    if (want && !has) c.push_back(t);
    if (!want && has) c.erase(std::remove(c.begin(), c.end(), t), c.end());
    SetClasses(rt, n, c);
    return Value::B(want);
}
NATIVE(CL_replace) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::wstring a = rt.ToStr(A(0)), b = rt.ToStr(A(1));
    std::vector<std::wstring> c = n->classes;
    bool f = false;
    for (auto& x : c) if (x == a) { x = b; f = true; }
    if (f) SetClasses(rt, n, c);
    return Value::B(f);
}
NATIVE(CL_item) { UNUSED; Node* n = CLNode(rt, thisV); double i = rt.ToNum(A(0)); return (i >= 0 && i < double(n->classes.size())) ? Value::S(n->classes[size_t(i)]) : Value::Null_(); }
NATIVE(CL_length) { UNUSED; return Value::N(double(CLNode(rt, thisV)->classes.size())); }
NATIVE(CL_value) { UNUSED; Node* n = CLNode(rt, thisV); auto* c = n->Attr(L"class"); return Value::S(c ? *c : L""); }
NATIVE(CL_iter) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::vector<Value> v;
    for (auto& c : n->classes) v.push_back(Value::S(c));
    return Value::O(rt.MakeIteratorFromList(v));
}
NATIVE(CL_forEach) {
    UNUSED;
    Node* n = CLNode(rt, thisV);
    std::vector<std::wstring> c = n->classes;
    for (size_t i = 0; i < c.size(); i++) { std::vector<Value> a{Value::S(c[i]), Value::N(double(i)), thisV}; rt.Call(A(0), A(1), a); }
    return Value();
}

// ---------------- Storage ----------------

std::map<std::wstring, std::wstring>& Store(Runtime& rt, Obj* o) { return o->x->num ? rt.sessionStore : rt.localStore; }
Obj* ThisStore(Runtime& rt, const Value& v) {
    if (v.IsObj() && v.o->cls == Cls::Storage) return v.o;
    rt.Throw(L"TypeError", L"Illegal invocation");
}
NATIVE(St_getItem) { UNUSED; auto& m = Store(rt, ThisStore(rt, thisV)); auto it = m.find(rt.ToStr(A(0))); return it == m.end() ? Value::Null_() : Value::S(it->second); }
NATIVE(St_setItem) {
    UNUSED;
    auto& m = Store(rt, ThisStore(rt, thisV));
    std::wstring k = rt.ToStr(A(0)), v = rt.ToStr(A(1));
    size_t total = 0;
    for (auto& e : m) total += e.first.size() + e.second.size();
    if (total + k.size() + v.size() > 5u * 1024 * 1024) rt.Throw(L"Error", L"QuotaExceededError: Storage quota exceeded");
    m[k] = v;
    return Value();
}
NATIVE(St_removeItem) { UNUSED; Store(rt, ThisStore(rt, thisV)).erase(rt.ToStr(A(0))); return Value(); }
NATIVE(St_clear) { UNUSED; Store(rt, ThisStore(rt, thisV)).clear(); return Value(); }
NATIVE(St_key) {
    UNUSED;
    auto& m = Store(rt, ThisStore(rt, thisV));
    double i = rt.ToNum(A(0));
    if (i < 0 || i >= double(m.size())) return Value::Null_();
    auto it = m.begin();
    std::advance(it, long(i));
    return Value::S(it->first);
}
NATIVE(St_length) { UNUSED; return Value::N(double(Store(rt, ThisStore(rt, thisV)).size())); }

// Ozel nesne get/set kancalari
bool ExoticGet(Runtime& rt, Obj* o, const std::wstring& key, Value& out) {
    if (!o->x) return false;
    if (o->cls == Cls::StyleDecl) {
        Node* n = o->x->node;
        if (key == L"\x02keys") {
            std::vector<Value> ks;
            if (n) for (auto& d : InlineDecls(n)) ks.push_back(Value::S(Camel(d.prop)));
            out = Value::O(rt.NewArrayFrom(ks));
            return true;
        }
        if (!n || rt.HasProperty(rt.styleProto, key) || (!key.empty() && key[0] < 32)) return false;
        if (key == L"cssText") { auto* s = n->Attr(L"style"); out = Value::S(s ? *s : L""); return true; }
        if (key == L"length") { out = Value::N(double(InlineDecls(n).size())); return true; }
        std::wstring prop = Kebab(key);
        for (auto& d : InlineDecls(n)) if (d.prop == prop) { out = Value::S(d.value); return true; }
        Prop* p = o->props.Find(L"\x02computed:" + prop);
        out = p ? p->v : Value::S(L"");
        return true;
    }
    if (o->cls == Cls::Dataset) {
        Node* n = o->x->node;
        if (!n) return false;
        if (key == L"\x02keys") {
            std::vector<Value> ks;
            for (auto& a : n->attrs) if (StartsWith(a.first, L"data-")) ks.push_back(Value::S(Camel(a.first.substr(5))));
            out = Value::O(rt.NewArrayFrom(ks));
            return true;
        }
        if (!key.empty() && key[0] < 32) return false;
        auto* v = n->Attr((L"data-" + Kebab(key)).c_str());
        if (!v) return false;
        out = Value::S(*v);
        return true;
    }
    if (o->cls == Cls::Storage) {
        auto& m = Store(rt, o);
        if (key == L"\x02keys") {
            std::vector<Value> ks;
            for (auto& e : m) ks.push_back(Value::S(e.first));
            out = Value::O(rt.NewArrayFrom(ks));
            return true;
        }
        if (rt.HasProperty(rt.storageProto, key)) return false;
        auto it = m.find(key);
        if (it == m.end()) return false;
        out = Value::S(it->second);
        return true;
    }
    return false;
}
bool ExoticSet(Runtime& rt, Obj* o, const std::wstring& key, const Value& v) {
    if (!o->x) return false;
    bool del = StartsWith(key, L"\x02del:");
    std::wstring k = del ? key.substr(5) : key;
    if (o->cls == Cls::StyleDecl) {
        Node* n = o->x->node;
        if (!n || StartsWith(k, L"\x02")) return false;
        if (k == L"cssText") { SetAttr(n, L"style", rt.ToStr(v)); Dirty(rt); return true; }
        StyleSet(rt, n, Kebab(k), del || v.IsNullish() ? L"" : rt.ToStr(v), false);
        return true;
    }
    if (o->cls == Cls::Dataset) {
        Node* n = o->x->node;
        if (!n) return false;
        if (del) RemoveAttr(n, L"data-" + Kebab(k));
        else SetAttr(n, L"data-" + Kebab(k), rt.ToStr(v));
        Dirty(rt);
        return true;
    }
    if (o->cls == Cls::Storage) {
        if (rt.HasProperty(rt.storageProto, k)) return false;
        if (del) Store(rt, o).erase(k);
        else Store(rt, o)[k] = rt.ToStr(v);
        return true;
    }
    return false;
}

// ---------------- Olaylar ----------------

NATIVE(Event_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Failed to construct 'Event': Please use the 'new' operator");
    Obj* proto = rt.eventProto;
    Value np = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
    if (np.IsObj()) proto = np.o;
    Obj* e = rt.NewObject(proto);
    e->cls = Cls::Event;
    e->X();
    rt.DefineData(e, L"type", Value::S(rt.ToStr(A(0))));
    bool bubbles = false, cancelable = false;
    Value detail = Value::Null_();
    if (A(1).IsObj()) {
        bubbles = rt.Truthy(rt.Get(A(1), L"bubbles"));
        cancelable = rt.Truthy(rt.Get(A(1), L"cancelable"));
        if (rt.HasProperty(A(1).o, L"detail")) detail = rt.Get(A(1), L"detail");
        for (const wchar_t* k : {L"key", L"code", L"keyCode", L"clientX", L"clientY", L"button", L"ctrlKey", L"shiftKey", L"altKey", L"metaKey"})
            if (rt.HasProperty(A(1).o, k)) rt.DefineData(e, k, rt.Get(A(1), k));
    }
    rt.DefineData(e, L"bubbles", Value::B(bubbles));
    rt.DefineData(e, L"cancelable", Value::B(cancelable));
    rt.DefineData(e, L"detail", detail);
    rt.DefineData(e, L"timeStamp", Value::N(rt.Now()));
    rt.DefineData(e, L"isTrusted", Value::B(false));
    rt.DefineData(e, L"target", Value::Null_());
    rt.DefineData(e, L"currentTarget", Value::Null_());
    rt.DefineData(e, L"eventPhase", Value::N(0));
    return Value::O(e);
}
Obj* ThisEvent(Runtime& rt, const Value& v) {
    if (v.IsObj() && v.o->cls == Cls::Event) return v.o;
    rt.Throw(L"TypeError", L"Illegal invocation");
}
NATIVE(Ev_preventDefault) { UNUSED; Obj* e = ThisEvent(rt, thisV); if (rt.Truthy(rt.Get(thisV, L"cancelable"))) e->x->defaultPrevented = true; return Value(); }
NATIVE(Ev_stopPropagation) { UNUSED; ThisEvent(rt, thisV)->x->stopProp = true; return Value(); }
NATIVE(Ev_defaultPrevented) { UNUSED; return Value::B(ThisEvent(rt, thisV)->x->defaultPrevented); }
NATIVE(Ev_composedPath) { UNUSED; return Value::O(rt.NewArray(0)); }
NATIVE(Ev_initEvent) {
    UNUSED;
    Obj* e = ThisEvent(rt, thisV);
    rt.DefineData(e, L"type", Value::S(rt.ToStr(A(0))));
    rt.DefineData(e, L"bubbles", Value::B(rt.Truthy(A(1))));
    rt.DefineData(e, L"cancelable", Value::B(rt.Truthy(A(2))));
    return Value();
}

Obj* ListenerHost(Runtime& rt, const Value& thisV) {
    if (thisV.IsObj()) return thisV.o;
    if (thisV.IsUndef()) return rt.global;
    rt.Throw(L"TypeError", L"Illegal invocation");
}
NATIVE(ET_addEventListener) {
    UNUSED;
    Obj* t = ListenerHost(rt, thisV);
    if (!IsCallable(A(1)) && !A(1).IsObj()) return Value();
    Listener l;
    l.type = rt.ToStr(A(0));
    l.fn = A(1);
    if (A(2).IsObj()) { l.capture = rt.Truthy(rt.Get(A(2), L"capture")); l.once = rt.Truthy(rt.Get(A(2), L"once")); }
    else l.capture = rt.Truthy(A(2));
    auto& ls = t->X().listeners;
    for (auto& e : ls) if (e.type == l.type && rt.StrictEq(e.fn, l.fn) && e.capture == l.capture) return Value();
    if (ls.size() < 10000) ls.push_back(l);
    return Value();
}
NATIVE(ET_removeEventListener) {
    UNUSED;
    Obj* t = ListenerHost(rt, thisV);
    if (!t->x) return Value();
    auto& ls = t->x->listeners;
    std::wstring type = rt.ToStr(A(0));
    for (size_t i = 0; i < ls.size(); i++) if (ls[i].type == type && rt.StrictEq(ls[i].fn, A(1))) { ls.erase(ls.begin() + long(i)); break; }
    return Value();
}
NATIVE(ET_dispatchEvent) {
    UNUSED;
    if (!A(0).IsObj() || A(0).o->cls != Cls::Event) rt.Throw(L"TypeError", L"Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
    Node* n = NOpt(thisV);
    if (n) return Value::B(rt.DispatchEvent(n, A(0).o));
    // window ya da duz nesne
    Obj* t = ListenerHost(rt, thisV);
    Value ev = A(0);
    rt.Put(ev, L"target", Value::O(t));
    rt.Put(ev, L"currentTarget", Value::O(t));
    std::wstring type = rt.ToStr(rt.Get(ev, L"type"));
    if (t->x) {
        auto ls = t->x->listeners;
        for (auto& l : ls) {
            if (l.type != type) continue;
            std::vector<Value> a{ev};
            if (IsCallable(l.fn)) rt.Call(l.fn, Value::O(t), a);
            else if (l.fn.IsObj()) rt.Call(rt.Get(l.fn, L"handleEvent"), l.fn, a);
        }
    }
    return Value::B(!A(0).o->x->defaultPrevented);
}

void DispatchSimpleToObj(Runtime& rt, Obj* target, const std::wstring& type) {
    if (!target) return;
    std::vector<Value> a{Value::S(type)};
    Value ev = rt.Construct(Value::O(rt.ctors[L"Event"]), a);
    if (!ev.IsObj()) return;
    rt.Put(ev, L"target", Value::O(target));
    rt.Put(ev, L"currentTarget", Value::O(target));

    // 1) inline handler: target["on" + type]
    std::wstring onName = L"on" + type;
    Value h = rt.Get(Value::O(target), onName);
    if (IsCallable(h)) {
        std::vector<Value> args{ev};
        rt.Call(h, Value::O(target), args);
    }

    // 2) EventTarget listeners: target->x->listeners
    if (target->x) {
        auto ls = target->x->listeners;
        for (auto& l : ls) {
            if (l.type != type) continue;
            std::vector<Value> args{ev};
            if (IsCallable(l.fn)) rt.Call(l.fn, Value::O(target), args);
            else if (l.fn.IsObj()) rt.Call(rt.Get(l.fn, L"handleEvent"), l.fn, args);
        }
    }
}

// ---------------- Document ----------------

NATIVE(Doc_documentElement) { UNUSED; return W(rt, HtmlElem(rt, NOpt(thisV))); }
NATIVE(Doc_body) { UNUSED; return W(rt, BodyElem(rt, NOpt(thisV))); }
NATIVE(Doc_head) { UNUSED; return W(rt, HeadElem(rt, NOpt(thisV))); }
NATIVE(Doc_createHTMLDocument) {
    UNUSED;
    Document& d = Doc(rt);
    Node* nd = NewNode(d, Node::Doc, L"");
    nd->kind = Node::Doc;
    nd->name.clear();
    auto frag = ParseFragment(L"<html><head><title>" + HtmlEscape(A(0).IsUndef() ? L"" : rt.ToStr(A(0))) + L"</title></head><body></body></html>", d);
    for (auto& f : frag) { f->parent = nd; nd->kids.push_back(std::move(f)); }
    return W(rt, nd);
}
NATIVE(Doc_title) { UNUSED; return Value::S(Doc(rt).title); }
NATIVE(Doc_setTitle) { UNUSED; Doc(rt).title = rt.ToStr(A(0)); Dirty(rt); return Value(); }
NATIVE(Doc_cookie) {
    UNUSED;
    double now = rt.Now();
    std::wstring result;
    for (auto it = rt.cookies.begin(); it != rt.cookies.end(); ) {
        if (it->second.expires >= 0 && it->second.expires <= now) {
            it = rt.cookies.erase(it);
        } else {
            if (!it->second.httpOnly) {
                if (!result.empty()) result += L"; ";
                result += it->first + L"=" + it->second.value;
            }
            ++it;
        }
    }
    return Value::S(result);
}

NATIVE(Doc_setCookie) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0));
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty()) return Value();

    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < s.size()) {
        size_t semi = s.find(L';', start);
        if (semi == std::wstring::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, semi - start));
        start = semi + 1;
    }
    if (parts.empty()) return Value();

    std::wstring pair = parts[0];
    while (!pair.empty() && iswspace(pair.front())) pair.erase(pair.begin());
    while (!pair.empty() && iswspace(pair.back())) pair.pop_back();

    size_t eq = pair.find(L'=');
    std::wstring name, val;
    if (eq == std::wstring::npos) {
        name = pair;
    } else {
        name = pair.substr(0, eq);
        val = pair.substr(eq + 1);
    }
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!val.empty() && iswspace(val.front())) val.erase(val.begin());
    while (!val.empty() && iswspace(val.back())) val.pop_back();

    if (name.empty()) return Value();

    Cookie ck;
    ck.value = val;
    ck.expires = -1; // varsayilan: oturum cerezi

    double now = rt.Now();

    for (size_t i = 1; i < parts.size(); i++) {
        std::wstring p = parts[i];
        while (!p.empty() && iswspace(p.front())) p.erase(p.begin());
        while (!p.empty() && iswspace(p.back())) p.pop_back();
        if (p.empty()) continue;

        size_t pEq = p.find(L'=');
        std::wstring attrName = (pEq == std::wstring::npos) ? p : p.substr(0, pEq);
        std::wstring attrVal = (pEq == std::wstring::npos) ? L"" : p.substr(pEq + 1);
        while (!attrName.empty() && iswspace(attrName.front())) attrName.erase(attrName.begin());
        while (!attrName.empty() && iswspace(attrName.back())) attrName.pop_back();
        while (!attrVal.empty() && iswspace(attrVal.front())) attrVal.erase(attrVal.begin());
        while (!attrVal.empty() && iswspace(attrVal.back())) attrVal.pop_back();

        std::wstring lowerAttr = ToLower(attrName);
        if (lowerAttr == L"max-age") {
            int sec = _wtoi(attrVal.c_str());
            if (sec <= 0) ck.expires = 0;
            else ck.expires = now + double(sec) * 1000.0;
        } else if (lowerAttr == L"expires") {
            double parsed = ParseDate(attrVal);
            if (!std::isnan(parsed)) ck.expires = parsed;
        } else if (lowerAttr == L"path") {
            ck.path = attrVal;
        } else if (lowerAttr == L"domain") {
            ck.domain = attrVal;
        } else if (lowerAttr == L"secure") {
            ck.secure = true;
        } else if (lowerAttr == L"httponly") {
            ck.httpOnly = true;
        }
    }

    if (ck.expires >= 0 && ck.expires <= now) {
        rt.cookies.erase(name);
    } else {
        rt.cookies[name] = std::move(ck);
    }
    return Value();
}
NATIVE(Doc_readyState) { UNUSED; return Value::S(rt.inTask && rt.currentScript ? L"loading" : L"complete"); }
NATIVE(Doc_URL) { UNUSED; return Value::S(rt.host ? rt.host->PageUrl() : L"about:blank"); }
NATIVE(Doc_referrer) { UNUSED; return Value::S(L""); }
NATIVE(Doc_domain) { UNUSED; Url u = ParseUrl(rt.host ? rt.host->PageUrl() : L""); return Value::S(u.host); }
NATIVE(Doc_currentScript) { UNUSED; return W(rt, rt.currentScript); }
NATIVE(Doc_activeElement) { UNUSED; return W(rt, BodyElem(rt)); }
NATIVE(Doc_false) { UNUSED; return Value::B(false); }
NATIVE(Doc_true) { UNUSED; return Value::B(true); }
NATIVE(Doc_visible) { UNUSED; return Value::S(L"visible"); }
NATIVE(Doc_charset) { UNUSED; return Value::S(L"UTF-8"); }
NATIVE(Doc_compat) { UNUSED; return Value::S(L"CSS1Compat"); }
NATIVE(Doc_createElement) {
    UNUSED;
    std::wstring t = ToLower(rt.ToStr(A(0)));
    if (t.empty() || t.find_first_of(L" <>\"'/=") != std::wstring::npos) rt.Throw(L"Error", L"InvalidCharacterError: The tag name provided is not a valid name.");
    return W(rt, NewNode(Doc(rt), Node::Elem, t));
}
NATIVE(Doc_createElementNS) { UNUSED; std::vector<Value> a{A(1)}; return Doc_createElement(rt, self, thisV, a); }
NATIVE(Doc_createTextNode) { UNUSED; return W(rt, NewNode(Doc(rt), Node::Text, rt.ToStr(A(0)))); }
NATIVE(Doc_createComment) { UNUSED; return W(rt, NewNode(Doc(rt), Node::Text, L"")); }
NATIVE(Doc_createDocumentFragment) { UNUSED; return W(rt, NewNode(Doc(rt), Node::Elem, L"#fragment")); }
NATIVE(Doc_createEvent) {
    UNUSED;
    std::vector<Value> a{Value::S(L"")};
    return rt.Construct(Value::O(rt.ctors[L"Event"]), a);
}
NATIVE(Doc_getElementById) {
    UNUSED;
    std::wstring id = rt.ToStr(A(0));
    Node* found = nullptr;
    ForEachElem(Doc(rt).root.get(), false, [&](Node* e) { if (e->idAttr == id) { found = e; return false; } return true; });
    return W(rt, found);
}
NATIVE(Doc_getElementsByName) {
    UNUSED;
    std::wstring nm = rt.ToStr(A(0));
    std::vector<Value> v;
    ForEachElem(Doc(rt).root.get(), false, [&](Node* e) { auto* a = e->Attr(L"name"); if (a && *a == nm) v.push_back(W(rt, e)); return true; });
    return Value::O(rt.NewArrayFrom(v));
}
Value Collect(Runtime& rt, const wchar_t* tag, const wchar_t* needAttr) {
    std::vector<Value> v;
    ForEachElem(Doc(rt).root.get(), false, [&](Node* e) { if (e->name == tag && (!needAttr || e->Attr(needAttr))) v.push_back(W(rt, e)); return true; });
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(Doc_scripts) { UNUSED; return Collect(rt, L"script", nullptr); }
NATIVE(Doc_images) { UNUSED; return Collect(rt, L"img", nullptr); }
NATIVE(Doc_links) { UNUSED; return Collect(rt, L"a", L"href"); }
NATIVE(Doc_forms) { UNUSED; return Collect(rt, L"form", nullptr); }
NATIVE(Doc_write) {
    UNUSED;
    std::wstring html;
    for (auto& a : args) html += rt.ToStr(a);
    Document& d = Doc(rt);
    auto frag = ParseFragment(html, d);
    Node* anchor = rt.currentScript;
    Node* parent = anchor && anchor->parent ? anchor->parent : BodyElem(rt);
    if (!parent) parent = d.root.get();
    Node* ref = anchor && anchor->parent ? Sibling(anchor, 1, false) : nullptr;
    std::vector<Node*> raw;
    for (auto& f : frag) { raw.push_back(f.get()); d.detached.push_back(std::move(f)); }
    for (Node* r : raw) { InsertBefore(d, parent, r, ref); }
    Dirty(rt);
    for (Node* r : raw) RunInsertedScripts(rt, r);
    return Value();
}
NATIVE(Doc_writeln) { UNUSED; args.push_back(Value::S(L"\n")); return Doc_write(rt, self, thisV, args); }
NATIVE(Doc_location) { UNUSED; return rt.Get(Value::O(rt.global), L"location"); }
NATIVE(Doc_setLocation) { UNUSED; if (rt.host) rt.pendingNav = rt.host->Resolve(rt.ToStr(A(0))); return Value(); }
NATIVE(Doc_defaultView) { UNUSED; return Value::O(rt.global); }
NATIVE(Doc_implementation) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineMethod(o, L"hasFeature", Doc_true, 0);
    rt.DefineMethod(o, L"createHTMLDocument", Doc_createHTMLDocument, 1);
    return Value::O(o);
}

// ---------------- Window ----------------

NATIVE(Win_innerWidth) { UNUSED; double w = 1280, h = 800; if (rt.host) rt.host->ViewportSize(w, h); return Value::N(std::round(w)); }
NATIVE(Win_innerHeight) { UNUSED; double w = 1280, h = 800; if (rt.host) rt.host->ViewportSize(w, h); return Value::N(std::round(h)); }
NATIVE(Win_zero) { UNUSED; return Value::N(0); }
NATIVE(Win_noop) { UNUSED; return Value(); }
NATIVE(Win_null) { UNUSED; return Value::Null_(); }
NATIVE(Win_alert) {
    UNUSED;
    std::wstring m = L"[alert] " + (args.empty() ? std::wstring() : rt.ToStr(A(0)));
    if (rt.consoleLines.size() < 5000) rt.consoleLines.push_back(m);
    if (rt.host) rt.host->Log(1, m);
    return Value();
}
NATIVE(Win_confirm) { UNUSED; Win_alert(rt, self, thisV, args); return Value::B(false); }
NATIVE(Win_prompt) { UNUSED; Win_alert(rt, self, thisV, args); return Value::Null_(); }
NATIVE(Win_open) {
    UNUSED;
    if (rt.host) rt.host->Log(1, L"Açılır pencere engellendi: " + (args.empty() ? std::wstring() : rt.ToStr(A(0))));
    return Value::Null_();
}
NATIVE(Win_rAF) {
    UNUSED;
    if (!IsCallable(A(0))) return Value::N(0);
    Timer t;
    t.id = rt.nextTimerId++;
    t.due = rt.Now() + 16;
    t.interval = -1;
    t.fn = A(0);
    t.args = {Value::N(rt.Now())};
    if (rt.timers.size() < 10000) rt.timers.push_back(t);
    return Value::N(t.id);
}
NATIVE(Win_idle) {
    UNUSED;
    if (!IsCallable(A(0))) return Value::N(0);
    Obj* dl = rt.NewObject();
    rt.DefineData(dl, L"didTimeout", Value::B(false));
    rt.DefineMethod(dl, L"timeRemaining", [](Runtime&, Obj*, const Value&, std::vector<Value>&) { return Value::N(30); }, 0);
    Timer t;
    t.id = rt.nextTimerId++;
    t.due = rt.Now() + 1;
    t.interval = -1;
    t.fn = A(0);
    t.args = {Value::O(dl)};
    if (rt.timers.size() < 10000) rt.timers.push_back(t);
    return Value::N(t.id);
}
NATIVE(Win_getComputedStyle) {
    UNUSED;
    Node* n = NOpt(A(0));
    Obj* s = rt.NewObject(rt.styleProto);
    s->cls = Cls::StyleDecl;
    s->X().node = n;
    // en sik sorulan hesaplanmis degerler icin makul varsayilanlar
    rt.DefineData(s, L"\x02computed:display", Value::S(n && (n->name == L"span" || n->name == L"a" || n->name == L"b" || n->name == L"i") ? L"inline" : L"block"), false);
    rt.DefineData(s, L"\x02computed:visibility", Value::S(L"visible"), false);
    rt.DefineData(s, L"\x02computed:position", Value::S(L"static"), false);
    rt.DefineData(s, L"\x02computed:opacity", Value::S(L"1"), false);
    return Value::O(s);
}
NATIVE(Win_matchMedia) {
    UNUSED;
    std::wstring q = ToLower(rt.ToStr(A(0)));
    double w = 1280, h = 800;
    if (rt.host) rt.host->ViewportSize(w, h);
    bool m = true;
    auto num = [&](const wchar_t* key, double& out) {
        size_t p = q.find(key);
        if (p == std::wstring::npos) return false;
        p += wcslen(key);
        while (p < q.size() && (q[p] == L':' || q[p] == L' ')) p++;
        out = wcstod(q.c_str() + p, nullptr);
        return true;
    };
    double v;
    if (num(L"max-width", v) && w > v) m = false;
    if (num(L"min-width", v) && w < v) m = false;
    if (q.find(L"prefers-color-scheme: dark") != std::wstring::npos || q.find(L"prefers-color-scheme:dark") != std::wstring::npos) m = false;
    if (q.find(L"print") != std::wstring::npos) m = false;
    if (q.find(L"prefers-reduced-motion: reduce") != std::wstring::npos) m = false;
    Obj* r = rt.NewObject();
    rt.DefineData(r, L"matches", Value::B(m));
    rt.DefineData(r, L"media", Value::S(q));
    for (const wchar_t* k : {L"addListener", L"removeListener", L"addEventListener", L"removeEventListener"}) rt.DefineMethod(r, k, Win_noop, 1);
    return Value::O(r);
}

// location
NATIVE(Loc_href) { UNUSED; return Value::S(rt.host ? rt.host->PageUrl() : L"about:blank"); }
NATIVE(Loc_setHref) { UNUSED; if (rt.host) rt.pendingNav = rt.host->Resolve(rt.ToStr(A(0))); return Value(); }
Url PageUrlParts(Runtime& rt) { return ParseUrl(rt.host ? rt.host->PageUrl() : L"about:blank"); }
NATIVE(Loc_protocol) { UNUSED; Url u = PageUrlParts(rt); return Value::S(u.scheme + L":"); }
NATIVE(Loc_host) { UNUSED; Url u = PageUrlParts(rt); std::wstring o = u.Origin(); size_t p = o.find(L"://"); return Value::S(p == std::wstring::npos ? L"" : o.substr(p + 3)); }
NATIVE(Loc_hostname) { UNUSED; return Value::S(PageUrlParts(rt).host); }
NATIVE(Loc_port) { UNUSED; Url u = PageUrlParts(rt); bool def = (u.scheme == L"https" && u.port == 443) || (u.scheme == L"http" && u.port == 80); return Value::S(def || !u.port ? L"" : std::to_wstring(u.port)); }
NATIVE(Loc_pathname) { UNUSED; Url u = PageUrlParts(rt); size_t q = u.path.find(L'?'); return Value::S(u.path.substr(0, q)); }
NATIVE(Loc_search) { UNUSED; Url u = PageUrlParts(rt); size_t q = u.path.find(L'?'); return Value::S(q == std::wstring::npos ? L"" : u.path.substr(q)); }
NATIVE(Loc_hash) { UNUSED; Url u = PageUrlParts(rt); return Value::S(u.fragment.empty() ? L"" : L"#" + u.fragment); }
NATIVE(Loc_setHash) { UNUSED; return Value(); }
NATIVE(Loc_origin) { UNUSED; return Value::S(PageUrlParts(rt).Origin()); }
NATIVE(Loc_assign) { UNUSED; if (rt.host) rt.pendingNav = rt.host->Resolve(rt.ToStr(A(0))); return Value(); }
NATIVE(Loc_reload) { UNUSED; if (rt.host) rt.pendingNav = rt.host->PageUrl(); return Value(); }

// history
NATIVE(Hist_pushState) { UNUSED; if (rt.host && !A(2).IsNullish()) rt.host->SetUrlNoNav(rt.host->Resolve(rt.ToStr(A(2)))); return Value(); }
NATIVE(Hist_back) { UNUSED; if (rt.host) rt.host->HistoryGo(-1); return Value(); }
NATIVE(Hist_forward) { UNUSED; if (rt.host) rt.host->HistoryGo(1); return Value(); }
NATIVE(Hist_go) { UNUSED; if (rt.host) rt.host->HistoryGo(int(rt.ToNum(A(0)))); return Value(); }

// performance
NATIVE(Perf_now) { UNUSED; static thread_local double base = rt.Now(); return Value::N(rt.Now() - base); }
NATIVE(Perf_entries) { UNUSED; return Value::O(rt.NewArray(0)); }

// Gozlemciler (MutationObserver vb.)
NATIVE(Obs_ctor) {
    UNUSED;
    Obj* proto = nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* o = rt.NewObject(proto);
    rt.DefineData(o, L"\x02cb", A(0), false);
    return Value::O(o);
}
NATIVE(IO_observe) {
    UNUSED;
    // IntersectionObserver: ogeyi hemen "gorunur" kabul et (tembel yuklenen resimler yuklensin)
    Value cb = rt.Get(thisV, L"\x02cb");
    if (!IsCallable(cb) || !NOpt(A(0))) return Value();
    Obj* entry = rt.NewObject();
    rt.DefineData(entry, L"isIntersecting", Value::B(true));
    rt.DefineData(entry, L"intersectionRatio", Value::N(1));
    rt.DefineData(entry, L"target", A(0));
    double x, y, w, h;
    RectOf(rt, NOpt(A(0)), x, y, w, h);
    rt.DefineData(entry, L"boundingClientRect", Value::O(MakeRect(rt, x, y, w, h)));
    Timer t;
    t.id = rt.nextTimerId++;
    t.due = rt.Now() + 1;
    t.interval = -1;
    t.fn = cb;
    t.args = {Value::O(rt.NewArrayFrom({Value::O(entry)})), thisV};
    if (rt.timers.size() < 10000) rt.timers.push_back(t);
    return Value();
}
NATIVE(Obs_takeRecords) { UNUSED; return Value::O(rt.NewArray(0)); }

// URL ve URLSearchParams
void ParseQuery(Runtime& rt, Obj* sp, const std::wstring& q0) {
    std::wstring q = q0;
    if (!q.empty() && q[0] == L'?') q.erase(0, 1);
    size_t st = 0;
    while (st <= q.size() && !q.empty()) {
        size_t e = q.find(L'&', st);
        if (e == std::wstring::npos) e = q.size();
        std::wstring part = q.substr(st, e - st);
        if (!part.empty()) {
            size_t eq = part.find(L'=');
            std::wstring k = part.substr(0, eq), v = eq == std::wstring::npos ? L"" : part.substr(eq + 1);
            for (auto& c : k) if (c == L'+') c = L' ';
            for (auto& c : v) if (c == L'+') c = L' ';
            std::vector<Value> a1{Value::S(k)}, a2{Value::S(v)};
            Value dk = rt.Call(rt.Get(Value::O(rt.global), L"decodeURIComponent"), Value(), a1);
            Value dv = rt.Call(rt.Get(Value::O(rt.global), L"decodeURIComponent"), Value(), a2);
            sp->elems.push_back(Value::O(rt.NewArrayFrom({dk, dv})));
        }
        st = e + 1;
        if (e >= q.size()) break;
    }
}
NATIVE(USP_ctor) {
    UNUSED;
    Obj* proto = nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* sp = rt.NewObject(proto);
    if (A(0).IsStr()) ParseQuery(rt, sp, *A(0).s);
    else if (A(0).IsObj()) {
        if (A(0).o->cls == Cls::Array) {
            for (auto& e : A(0).o->elems) sp->elems.push_back(Value::O(rt.NewArrayFrom({Value::S(rt.ToStr(rt.GetV(e, Value::N(0)))), Value::S(rt.ToStr(rt.GetV(e, Value::N(1))))})));
        } else {
            for (auto& k : rt.OwnKeys(A(0).o, true)) sp->elems.push_back(Value::O(rt.NewArrayFrom({Value::S(k), Value::S(rt.ToStr(rt.Get(A(0), k)))})));
        }
    }
    return Value::O(sp);
}
Obj* ThisUSP(Runtime& rt, const Value& v) { if (v.IsObj()) return v.o; rt.Throw(L"TypeError", L"Illegal invocation"); }
NATIVE(USP_get) {
    UNUSED;
    std::wstring k = rt.ToStr(A(0));
    for (auto& e : ThisUSP(rt, thisV)->elems) if (e.IsObj() && e.o->elems.size() == 2 && rt.ToStr(e.o->elems[0]) == k) return e.o->elems[1];
    return Value::Null_();
}
NATIVE(USP_getAll) {
    UNUSED;
    std::wstring k = rt.ToStr(A(0));
    std::vector<Value> v;
    for (auto& e : ThisUSP(rt, thisV)->elems) if (e.IsObj() && rt.ToStr(e.o->elems[0]) == k) v.push_back(e.o->elems[1]);
    return Value::O(rt.NewArrayFrom(v));
}
NATIVE(USP_has) { UNUSED; return Value::B(!USP_get(rt, self, thisV, args).IsNullish()); }
NATIVE(USP_append) { UNUSED; ThisUSP(rt, thisV)->elems.push_back(Value::O(rt.NewArrayFrom({Value::S(rt.ToStr(A(0))), Value::S(rt.ToStr(A(1)))}))); return Value(); }
NATIVE(USP_delete) {
    UNUSED;
    Obj* sp = ThisUSP(rt, thisV);
    std::wstring k = rt.ToStr(A(0));
    std::vector<Value> keep;
    for (auto& e : sp->elems) if (!(e.IsObj() && rt.ToStr(e.o->elems[0]) == k)) keep.push_back(e);
    sp->elems = keep;
    return Value();
}
NATIVE(USP_set) { UNUSED; USP_delete(rt, self, thisV, args); return USP_append(rt, self, thisV, args); }
NATIVE(USP_toString) {
    UNUSED;
    std::wstring o;
    Value enc = rt.Get(Value::O(rt.global), L"encodeURIComponent");
    for (auto& e : ThisUSP(rt, thisV)->elems) {
        if (!e.IsObj()) continue;
        if (!o.empty()) o += L'&';
        std::vector<Value> a1{e.o->elems[0]}, a2{e.o->elems[1]};
        o += rt.ToStr(rt.Call(enc, Value(), a1)) + L"=" + rt.ToStr(rt.Call(enc, Value(), a2));
    }
    return Value::S(o);
}
NATIVE(USP_forEach) {
    UNUSED;
    for (auto e : ThisUSP(rt, thisV)->elems) {
        if (!e.IsObj()) continue;
        std::vector<Value> a{e.o->elems[1], e.o->elems[0], thisV};
        rt.Call(A(0), A(1), a);
    }
    return Value();
}
NATIVE(USP_entries) { UNUSED; return Value::O(rt.MakeIteratorFromList(ThisUSP(rt, thisV)->elems)); }

NATIVE(URL_ctor) {
    UNUSED;
    std::wstring input = rt.ToStr(A(0));
    std::wstring abs = input;
    if (!A(1).IsUndef()) abs = ResolveUrl(rt.ToStr(A(1)), input);
    else if (!HasScheme(input, nullptr)) rt.Throw(L"TypeError", L"Failed to construct 'URL': Invalid URL");
    Url u = ParseUrl(abs);
    if (!u.ok) rt.Throw(L"TypeError", L"Failed to construct 'URL': Invalid URL");
    Obj* proto = nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* o = rt.NewObject(proto);
    std::wstring path = u.path, search;
    size_t q = path.find(L'?');
    if (q != std::wstring::npos) { search = path.substr(q); path = path.substr(0, q); }
    std::wstring href = u.ToString() + (u.fragment.empty() ? L"" : L"#" + u.fragment);
    std::wstring origin = u.Origin();
    size_t hp = origin.find(L"://");
    rt.DefineData(o, L"href", Value::S(href));
    rt.DefineData(o, L"protocol", Value::S(u.scheme + L":"));
    rt.DefineData(o, L"host", Value::S(hp == std::wstring::npos ? L"" : origin.substr(hp + 3)));
    rt.DefineData(o, L"hostname", Value::S(u.host));
    bool def = (u.scheme == L"https" && u.port == 443) || (u.scheme == L"http" && u.port == 80);
    rt.DefineData(o, L"port", Value::S(def || !u.port ? L"" : std::to_wstring(u.port)));
    rt.DefineData(o, L"pathname", Value::S(path));
    rt.DefineData(o, L"search", Value::S(search));
    rt.DefineData(o, L"hash", Value::S(u.fragment.empty() ? L"" : L"#" + u.fragment));
    rt.DefineData(o, L"origin", Value::S(origin));
    std::vector<Value> spArgs{Value::S(search)};
    rt.DefineData(o, L"searchParams", rt.Construct(Value::O(rt.ctors[L"URLSearchParams"]), spArgs));
    return Value::O(o);
}
NATIVE(URL_toString) { UNUSED; return rt.Get(thisV, L"href"); }

// ---------------- Ag API'leri (Headers, Response, Request, fetch, XMLHttpRequest) ----------------

static std::wstring TrimHeader(const std::wstring& s) {
    size_t i = 0, j = s.size();
    while (i < j && iswspace(s[i])) i++;
    while (j > i && iswspace(s[j - 1])) j--;
    return s.substr(i, j - i);
}

NATIVE(Headers_ctor) {
    UNUSED;
    Obj* proto = rt.ctors[L"Headers"] ? rt.Get(Value::O(rt.ctors[L"Headers"]), L"prototype").o : nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* h = rt.NewObject(proto);
    Value init = A(0);
    if (init.IsObj()) {
        if (init.o->cls == Cls::Array) {
            for (auto& item : init.o->elems) {
                if (item.IsObj() && item.o->cls == Cls::Array && item.o->elems.size() >= 2) {
                    std::vector<Value> appendArgs{item.o->elems[0], item.o->elems[1]};
                    rt.Call(rt.Get(Value::O(h), L"append"), Value::O(h), appendArgs);
                }
            }
        } else {
            for (auto& k : rt.OwnKeys(init.o, true)) {
                std::vector<Value> appendArgs{Value::S(k), rt.Get(init, k)};
                rt.Call(rt.Get(Value::O(h), L"append"), Value::O(h), appendArgs);
            }
        }
    }
    return Value::O(h);
}

NATIVE(Headers_append) {
    UNUSED;
    std::wstring key = TrimHeader(rt.ToStr(A(0)));
    std::wstring val = TrimHeader(rt.ToStr(A(1)));
    if (key.empty()) return Value();
    std::wstring lowerKey = ToLower(key);
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    if (!h) return Value();
    for (auto& item : h->elems) {
        if (item.IsObj() && item.o->elems.size() >= 3 && rt.ToStr(item.o->elems[0]) == lowerKey) {
            std::wstring curVal = rt.ToStr(item.o->elems[2]);
            item.o->elems[2] = Value::S(curVal + L", " + val);
            return Value();
        }
    }
    h->elems.push_back(Value::O(rt.NewArrayFrom({Value::S(lowerKey), Value::S(key), Value::S(val)})));
    return Value();
}

NATIVE(Headers_set) {
    UNUSED;
    std::wstring key = TrimHeader(rt.ToStr(A(0)));
    std::wstring val = TrimHeader(rt.ToStr(A(1)));
    if (key.empty()) return Value();
    std::wstring lowerKey = ToLower(key);
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    if (!h) return Value();
    for (auto& item : h->elems) {
        if (item.IsObj() && item.o->elems.size() >= 3 && rt.ToStr(item.o->elems[0]) == lowerKey) {
            item.o->elems[1] = Value::S(key);
            item.o->elems[2] = Value::S(val);
            return Value();
        }
    }
    h->elems.push_back(Value::O(rt.NewArrayFrom({Value::S(lowerKey), Value::S(key), Value::S(val)})));
    return Value();
}

NATIVE(Headers_get) {
    UNUSED;
    std::wstring lowerKey = ToLower(TrimHeader(rt.ToStr(A(0))));
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    if (!h) return Value::Null_();
    for (auto& item : h->elems) {
        if (item.IsObj() && item.o->elems.size() >= 3 && rt.ToStr(item.o->elems[0]) == lowerKey) {
            return item.o->elems[2];
        }
    }
    return Value::Null_();
}

NATIVE(Headers_has) {
    UNUSED;
    std::wstring lowerKey = ToLower(TrimHeader(rt.ToStr(A(0))));
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    if (!h) return Value::B(false);
    for (auto& item : h->elems) {
        if (item.IsObj() && item.o->elems.size() >= 3 && rt.ToStr(item.o->elems[0]) == lowerKey) {
            return Value::B(true);
        }
    }
    return Value::B(false);
}

NATIVE(Headers_delete) {
    UNUSED;
    std::wstring lowerKey = ToLower(TrimHeader(rt.ToStr(A(0))));
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    if (!h) return Value();
    for (size_t i = 0; i < h->elems.size(); i++) {
        if (h->elems[i].IsObj() && h->elems[i].o->elems.size() >= 3 && rt.ToStr(h->elems[i].o->elems[0]) == lowerKey) {
            h->elems.erase(h->elems.begin() + i);
            break;
        }
    }
    return Value();
}

NATIVE(Headers_forEach) {
    UNUSED;
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    if (!h || !IsCallable(A(0))) return Value();
    Value cb = A(0);
    Value thisArg = A(1).IsUndef() ? thisV : A(1);
    for (auto& item : h->elems) {
        if (item.IsObj() && item.o->elems.size() >= 3) {
            std::vector<Value> callArgs{item.o->elems[2], item.o->elems[0], thisV};
            rt.Call(cb, thisArg, callArgs);
        }
    }
    return Value();
}

NATIVE(Headers_entries) {
    UNUSED;
    Obj* h = thisV.IsObj() ? thisV.o : nullptr;
    std::vector<Value> list;
    if (h) {
        for (auto& item : h->elems) {
            if (item.IsObj() && item.o->elems.size() >= 3) {
                list.push_back(Value::O(rt.NewArrayFrom({item.o->elems[0], item.o->elems[2]})));
            }
        }
    }
    return Value::O(rt.MakeIteratorFromList(list));
}

static void ParseRawHeadersInto(Runtime& rt, Obj* headersObj, const std::wstring& raw) {
    size_t st = 0;
    while (st < raw.size()) {
        size_t e = raw.find(L"\r\n", st);
        if (e == std::wstring::npos) e = raw.find(L'\n', st);
        std::wstring line = (e == std::wstring::npos) ? raw.substr(st) : raw.substr(st, e - st);
        st = (e == std::wstring::npos) ? raw.size() : (raw[e] == L'\r' ? e + 2 : e + 1);
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos && colon > 0) {
            std::wstring k = line.substr(0, colon);
            std::wstring v = line.substr(colon + 1);
            std::vector<Value> args{Value::S(TrimHeader(k)), Value::S(TrimHeader(v))};
            rt.Call(rt.Get(Value::O(headersObj), L"append"), Value::O(headersObj), args);
        }
    }
}

static void ProcessResponseCookies(Runtime& rt, const std::wstring& raw) {
    size_t st = 0;
    while (st < raw.size()) {
        size_t e = raw.find(L"\r\n", st);
        if (e == std::wstring::npos) e = raw.find(L'\n', st);
        std::wstring line = (e == std::wstring::npos) ? raw.substr(st) : raw.substr(st, e - st);
        st = (e == std::wstring::npos) ? raw.size() : (raw[e] == L'\r' ? e + 2 : e + 1);
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos) {
            std::wstring k = ToLower(TrimHeader(line.substr(0, colon)));
            if (k == L"set-cookie") {
                std::vector<Value> args{Value::S(TrimHeader(line.substr(colon + 1)))};
                Doc_setCookie(rt, nullptr, Value(), args);
            }
        }
    }
}

NATIVE(Response_text) {
    UNUSED;
    rt.Put(thisV, L"bodyUsed", Value::B(true));
    Obj* p = rt.NewPromise();
    rt.ResolvePromise(p, rt.Get(thisV, L"\x02bodyText"));
    return Value::O(p);
}

NATIVE(Response_json) {
    UNUSED;
    rt.Put(thisV, L"bodyUsed", Value::B(true));
    Obj* p = rt.NewPromise();
    std::wstring text = rt.ToStr(rt.Get(thisV, L"\x02bodyText"));
    try {
        Value jsonObj = rt.Get(Value::O(rt.global), L"JSON");
        Value parseFn = rt.Get(jsonObj, L"parse");
        std::vector<Value> parseArgs{Value::S(text)};
        Value parsed = rt.Call(parseFn, jsonObj, parseArgs);
        rt.ResolvePromise(p, parsed);
    } catch (JSThrow& t) {
        rt.RejectPromise(p, t.v);
    }
    return Value::O(p);
}

NATIVE(Response_blob) {
    UNUSED;
    rt.Put(thisV, L"bodyUsed", Value::B(true));
    Obj* p = rt.NewPromise();

    std::shared_ptr<std::vector<uint8_t>> rawData;
    if (thisV.IsObj() && thisV.o->x && thisV.o->x->bufferData) {
        rawData = thisV.o->x->bufferData;
    } else {
        std::wstring text = rt.ToStr(rt.Get(thisV, L"\x02bodyText"));
        int sz = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
        rawData = std::make_shared<std::vector<uint8_t>>(sz > 0 ? sz : 0);
        if (sz > 0) {
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), (char*)rawData->data(), sz, nullptr, nullptr);
        }
    }

    Obj* b = rt.NewObject(rt.blobProto ? rt.blobProto : rt.objectProto);
    b->X().bufferData = rawData;
    rt.DefineData(b, L"size", Value::N(double(rawData ? rawData->size() : 0)));
    std::wstring mime = L"text/plain";
    Value hVal = rt.Get(thisV, L"headers");
    if (hVal.IsObj()) {
        std::vector<Value> ctArgs{Value::S(L"content-type")};
        Value ct = rt.Call(rt.Get(hVal, L"get"), hVal, ctArgs);
        if (ct.IsStr()) mime = *ct.s;
    }
    rt.DefineData(b, L"type", Value::S(mime));
    rt.DefineData(b, L"\x02isBlob", Value::B(true), false);
    rt.ResolvePromise(p, Value::O(b));
    return Value::O(p);
}

NATIVE(Response_arrayBuffer) {
    UNUSED;
    rt.Put(thisV, L"bodyUsed", Value::B(true));
    Obj* p = rt.NewPromise();
    if (thisV.IsObj() && thisV.o->x && thisV.o->x->bufferData) {
        auto& src = *thisV.o->x->bufferData;
        Obj* ab = rt.NewArrayBuffer(src.size());
        if (!src.empty() && ab->x && ab->x->bufferData) {
            memcpy(ab->x->bufferData->data(), src.data(), src.size());
        }
        rt.ResolvePromise(p, Value::O(ab));
    } else {
        std::wstring text = rt.ToStr(rt.Get(thisV, L"\x02bodyText"));
        int sz = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
        Obj* ab = rt.NewArrayBuffer(sz > 0 ? size_t(sz) : 0);
        if (sz > 0 && ab->x && ab->x->bufferData) {
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), (char*)ab->x->bufferData->data(), sz, nullptr, nullptr);
        }
        rt.ResolvePromise(p, Value::O(ab));
    }
    return Value::O(p);
}

NATIVE(Response_clone) {
    UNUSED;
    Obj* r = rt.NewObject(rt.ctors[L"Response"] ? rt.Get(Value::O(rt.ctors[L"Response"]), L"prototype").o : nullptr);
    rt.DefineData(r, L"status", rt.Get(thisV, L"status"));
    rt.DefineData(r, L"statusText", rt.Get(thisV, L"statusText"));
    rt.DefineData(r, L"ok", rt.Get(thisV, L"ok"));
    rt.DefineData(r, L"url", rt.Get(thisV, L"url"));
    rt.DefineData(r, L"headers", rt.Get(thisV, L"headers"));
    rt.DefineData(r, L"type", rt.Get(thisV, L"type"));
    rt.DefineData(r, L"redirected", rt.Get(thisV, L"redirected"));
    rt.DefineData(r, L"bodyUsed", Value::B(false));
    rt.DefineData(r, L"\x02bodyText", rt.Get(thisV, L"\x02bodyText"));
    return Value::O(r);
}

NATIVE(Response_ctor) {
    UNUSED;
    Obj* proto = rt.ctors[L"Response"] ? rt.Get(Value::O(rt.ctors[L"Response"]), L"prototype").o : nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* r = rt.NewObject(proto);
    std::wstring body = A(0).IsUndef() ? L"" : rt.ToStr(A(0));
    Value init = A(1);
    double status = 200;
    std::wstring statusText = L"OK";
    Value headersVal;
    if (init.IsObj()) {
        Value st = rt.Get(init, L"status");
        if (st.IsNum()) status = st.n;
        Value stTxt = rt.Get(init, L"statusText");
        if (stTxt.IsStr()) statusText = *stTxt.s;
        headersVal = rt.Get(init, L"headers");
    }
    std::vector<Value> hArgs;
    if (!headersVal.IsUndef()) hArgs.push_back(headersVal);
    Obj* hObj = rt.Construct(Value::O(rt.ctors[L"Headers"]), hArgs).o;
    rt.DefineData(r, L"status", Value::N(status));
    rt.DefineData(r, L"statusText", Value::S(statusText));
    rt.DefineData(r, L"ok", Value::B(status >= 200 && status < 300));
    rt.DefineData(r, L"url", Value::S(L""));
    rt.DefineData(r, L"headers", Value::O(hObj));
    rt.DefineData(r, L"type", Value::S(L"basic"));
    rt.DefineData(r, L"redirected", Value::B(false));
    rt.DefineData(r, L"bodyUsed", Value::B(false));
    rt.DefineData(r, L"\x02bodyText", Value::S(body));
    return Value::O(r);
}

NATIVE(Request_ctor) {
    UNUSED;
    Obj* proto = rt.ctors[L"Request"] ? rt.Get(Value::O(rt.ctors[L"Request"]), L"prototype").o : nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* req = rt.NewObject(proto);
    std::wstring url = rt.ToStr(A(0));
    Value init = A(1);
    std::wstring method = L"GET";
    Value headersVal;
    Value bodyVal;
    if (init.IsObj()) {
        Value m = rt.Get(init, L"method");
        if (m.IsStr()) method = ToUpper(*m.s);
        headersVal = rt.Get(init, L"headers");
        bodyVal = rt.Get(init, L"body");
    }
    std::vector<Value> hArgs;
    if (!headersVal.IsUndef()) hArgs.push_back(headersVal);
    Obj* hObj = rt.Construct(Value::O(rt.ctors[L"Headers"]), hArgs).o;
    rt.DefineData(req, L"url", Value::S(url));
    rt.DefineData(req, L"method", Value::S(method));
    rt.DefineData(req, L"headers", Value::O(hObj));
    rt.DefineData(req, L"body", bodyVal);
    return Value::O(req);
}

NATIVE(Win_fetch) {
    UNUSED;
    Obj* promise = rt.NewPromise();

    std::wstring url;
    Value input = A(0);
    Value init = A(1);

    if (input.IsObj() && rt.HasProperty(input.o, L"url")) {
        url = rt.ToStr(rt.Get(input, L"url"));
    } else {
        url = rt.ToStr(input);
    }

    if (StartsWith(url, L"blob:")) {
        auto it = rt.objectUrls.find(url);
        if (it == rt.objectUrls.end()) {
            rt.RejectPromise(promise, Value::O(rt.NewError(L"TypeError", L"Failed to fetch blob: URL not found")));
            return Value::O(promise);
        }
        Obj* target = it->second;
        std::shared_ptr<std::vector<uint8_t>> rawData;
        std::wstring mime = L"application/octet-stream";
        if (target && target->x && target->x->bufferData) {
            rawData = target->x->bufferData;
        }
        if (target) {
            Value tVal = rt.Get(Value::O(target), L"type");
            if (tVal.IsStr() && !tVal.s->empty()) mime = *tVal.s;
        }
        std::wstring bodyText;
        if (rawData && !rawData->empty()) {
            bodyText = FromUtf8(rawData->data(), rawData->size());
        }
        std::vector<Value> respArgs{Value::S(bodyText)};
        Obj* resp = rt.Construct(Value::O(rt.ctors[L"Response"]), respArgs).o;
        resp->X().bufferData = rawData;
        rt.DefineData(resp, L"status", Value::N(200));
        rt.DefineData(resp, L"statusText", Value::S(L"OK"));
        rt.DefineData(resp, L"ok", Value::B(true));
        rt.DefineData(resp, L"url", Value::S(url));
        rt.DefineData(resp, L"\x02bodyText", Value::S(bodyText));
        Value hVal = rt.Get(Value::O(resp), L"headers");
        if (hVal.IsObj()) {
            std::vector<Value> ctArgs{Value::S(L"content-type"), Value::S(mime)};
            rt.Call(rt.Get(hVal, L"set"), hVal, ctArgs);
        }
        rt.ResolvePromise(promise, Value::O(resp));
        return Value::O(promise);
    }

    if (rt.host && !StartsWith(url, L"http://") && !StartsWith(url, L"https://")) {
        url = rt.host->Resolve(url);
    }

    FetchOptions opt;
    opt.method = L"GET";

    if (init.IsObj()) {
        Value m = rt.Get(init, L"method");
        if (m.IsStr() && !m.s->empty()) opt.method = ToUpper(*m.s);

        Value b = rt.Get(init, L"body");
        if (!b.IsNullish()) {
            std::wstring bs = rt.ToStr(b);
            int sz = WideCharToMultiByte(CP_UTF8, 0, bs.c_str(), int(bs.size()), nullptr, 0, nullptr, nullptr);
            if (sz > 0) {
                opt.body.resize(sz);
                WideCharToMultiByte(CP_UTF8, 0, bs.c_str(), int(bs.size()), &opt.body[0], sz, nullptr, nullptr);
            }
        }

        Value h = rt.Get(init, L"headers");
        if (h.IsObj()) {
            if (h.o->cls == Cls::Array) {
                for (auto& item : h.o->elems) {
                    if (item.IsObj() && item.o->elems.size() >= 2) {
                        opt.extraHeaders += rt.ToStr(item.o->elems[0]) + L": " + rt.ToStr(item.o->elems[1]) + L"\r\n";
                    }
                }
            } else {
                for (auto& k : rt.OwnKeys(h.o, true)) {
                    std::wstring val = rt.ToStr(rt.Get(h, k));
                    if (ToLower(k) == L"content-type") opt.contentType = val;
                    opt.extraHeaders += k + L": " + val + L"\r\n";
                }
            }
        }
    }

    if (!rt.cookies.empty()) {
        std::wstring ckHeader;
        double now = rt.Now();
        for (auto& kv : rt.cookies) {
            if (kv.second.expires < 0 || kv.second.expires > now) {
                if (!ckHeader.empty()) ckHeader += L"; ";
                ckHeader += kv.first + L"=" + kv.second.value;
            }
        }
        if (!ckHeader.empty()) opt.extraHeaders += L"Cookie: " + ckHeader + L"\r\n";
    }

    FetchResult res = Fetch(url, 24 * 1024 * 1024, nullptr, 0, &opt);
    if (!res.ok) {
        rt.RejectPromise(promise, Value::O(rt.NewError(L"TypeError", L"Failed to fetch: " + NetErrorText(res.error))));
    } else {
        ProcessResponseCookies(rt, res.rawHeaders);
        std::wstring bodyText = DecodeBody(res.body, res.contentType);
        std::vector<Value> respArgs{Value::S(bodyText)};
        Obj* resp = rt.Construct(Value::O(rt.ctors[L"Response"]), respArgs).o;
        resp->X().bufferData = std::make_shared<std::vector<uint8_t>>(res.body.begin(), res.body.end());
        rt.DefineData(resp, L"status", Value::N(double(res.status)));
        rt.DefineData(resp, L"statusText", Value::S(res.statusText));
        rt.DefineData(resp, L"ok", Value::B(res.status >= 200 && res.status < 300));
        rt.DefineData(resp, L"url", Value::S(res.finalUrl));
        rt.DefineData(resp, L"\x02bodyText", Value::S(bodyText));
        Value hVal = rt.Get(Value::O(resp), L"headers");
        if (hVal.IsObj()) ParseRawHeadersInto(rt, hVal.o, res.rawHeaders);
        rt.ResolvePromise(promise, Value::O(resp));
    }

    return Value::O(promise);
}

// ---------------- XMLHttpRequest ----------------

static void FireXHREvent(Runtime& rt, Obj* xhr, const std::wstring& type) {
    Obj* ev = MakeEvent(rt, type, false, false);
    rt.Put(Value::O(ev), L"target", Value::O(xhr));
    rt.Put(Value::O(ev), L"currentTarget", Value::O(xhr));
    Value handler = rt.Get(Value::O(xhr), L"on" + type);
    if (IsCallable(handler)) {
        std::vector<Value> args{Value::O(ev)};
        rt.Call(handler, Value::O(xhr), args);
    }
    if (xhr->x) {
        auto ls = xhr->x->listeners;
        for (auto& l : ls) {
            if (l.type == type && IsCallable(l.fn)) {
                std::vector<Value> args{Value::O(ev)};
                rt.Call(l.fn, Value::O(xhr), args);
            }
        }
    }
}

NATIVE(XHR_ctor) {
    UNUSED;
    Obj* proto = rt.ctors[L"XMLHttpRequest"] ? rt.Get(Value::O(rt.ctors[L"XMLHttpRequest"]), L"prototype").o : nullptr;
    if (rt.nativeNewTarget) { Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype"); if (p.IsObj()) proto = p.o; }
    Obj* xhr = rt.NewObject(proto);
    rt.DefineData(xhr, L"readyState", Value::N(0));
    rt.DefineData(xhr, L"status", Value::N(0));
    rt.DefineData(xhr, L"statusText", Value::S(L""));
    rt.DefineData(xhr, L"responseText", Value::S(L""));
    rt.DefineData(xhr, L"response", Value::S(L""));
    rt.DefineData(xhr, L"responseType", Value::S(L""));
    rt.DefineData(xhr, L"responseURL", Value::S(L""));
    rt.DefineData(xhr, L"timeout", Value::N(0));
    rt.DefineData(xhr, L"withCredentials", Value::B(false));
    Obj* upload = rt.NewObject(rt.eventTargetProto);
    rt.DefineData(xhr, L"upload", Value::O(upload));
    return Value::O(xhr);
}

NATIVE(XHR_open) {
    UNUSED;
    Obj* xhr = thisV.IsObj() ? thisV.o : nullptr;
    if (!xhr) rt.Throw(L"TypeError", L"Illegal invocation");
    std::wstring method = ToUpper(TrimHeader(rt.ToStr(A(0))));
    std::wstring url = rt.ToStr(A(1));
    bool isAsync = A(2).IsUndef() ? true : rt.Truthy(A(2));
    rt.Put(thisV, L"\x02method", Value::S(method));
    rt.Put(thisV, L"\x02url", Value::S(url));
    rt.Put(thisV, L"\x02async", Value::B(isAsync));
    rt.Put(thisV, L"\x02headers", Value::O(rt.NewArray(0)));
    rt.Put(thisV, L"readyState", Value::N(1));
    FireXHREvent(rt, xhr, L"readystatechange");
    return Value();
}

NATIVE(XHR_setRequestHeader) {
    UNUSED;
    Obj* xhr = thisV.IsObj() ? thisV.o : nullptr;
    if (!xhr) rt.Throw(L"TypeError", L"Illegal invocation");
    if (rt.ToNum(rt.Get(thisV, L"readyState")) != 1) {
        rt.Throw(L"Error", L"InvalidStateError: XMLHttpRequest is not opened");
    }
    std::wstring k = TrimHeader(rt.ToStr(A(0))), v = TrimHeader(rt.ToStr(A(1)));
    Value hVal = rt.Get(thisV, L"\x02headers");
    if (hVal.IsObj() && hVal.o->cls == Cls::Array) {
        hVal.o->elems.push_back(Value::O(rt.NewArrayFrom({Value::S(k), Value::S(v)})));
    }
    return Value();
}

NATIVE(XHR_send) {
    UNUSED;
    Obj* xhr = thisV.IsObj() ? thisV.o : nullptr;
    if (!xhr) rt.Throw(L"TypeError", L"Illegal invocation");
    if (rt.ToNum(rt.Get(thisV, L"readyState")) != 1) {
        rt.Throw(L"Error", L"InvalidStateError: XMLHttpRequest is not opened");
    }

    std::wstring url = rt.ToStr(rt.Get(thisV, L"\x02url"));
    if (rt.host && !StartsWith(url, L"http://") && !StartsWith(url, L"https://")) {
        url = rt.host->Resolve(url);
    }

    FetchOptions opt;
    opt.method = rt.ToStr(rt.Get(thisV, L"\x02method"));
    if (opt.method.empty()) opt.method = L"GET";

    if (!A(0).IsNullish()) {
        std::wstring bs = rt.ToStr(A(0));
        int sz = WideCharToMultiByte(CP_UTF8, 0, bs.c_str(), int(bs.size()), nullptr, 0, nullptr, nullptr);
        if (sz > 0) {
            opt.body.resize(sz);
            WideCharToMultiByte(CP_UTF8, 0, bs.c_str(), int(bs.size()), &opt.body[0], sz, nullptr, nullptr);
        }
    }

    Value hVal = rt.Get(thisV, L"\x02headers");
    if (hVal.IsObj() && hVal.o->cls == Cls::Array) {
        for (auto& item : hVal.o->elems) {
            if (item.IsObj() && item.o->elems.size() >= 2) {
                std::wstring hk = rt.ToStr(item.o->elems[0]);
                std::wstring hv = rt.ToStr(item.o->elems[1]);
                if (ToLower(hk) == L"content-type") opt.contentType = hv;
                opt.extraHeaders += hk + L": " + hv + L"\r\n";
            }
        }
    }

    if (!rt.cookies.empty()) {
        std::wstring ckHeader;
        double now = rt.Now();
        for (auto& kv : rt.cookies) {
            if (kv.second.expires < 0 || kv.second.expires > now) {
                if (!ckHeader.empty()) ckHeader += L"; ";
                ckHeader += kv.first + L"=" + kv.second.value;
            }
        }
        if (!ckHeader.empty()) opt.extraHeaders += L"Cookie: " + ckHeader + L"\r\n";
    }

    FetchResult res = Fetch(url, 24 * 1024 * 1024, nullptr, 0, &opt);
    if (!res.ok) {
        rt.Put(thisV, L"readyState", Value::N(4));
        rt.Put(thisV, L"status", Value::N(0));
        rt.Put(thisV, L"statusText", Value::S(L""));
        FireXHREvent(rt, xhr, L"readystatechange");
        FireXHREvent(rt, xhr, L"error");
        FireXHREvent(rt, xhr, L"loadend");
    } else {
        ProcessResponseCookies(rt, res.rawHeaders);
        std::wstring bodyText = DecodeBody(res.body, res.contentType);
        rt.Put(thisV, L"readyState", Value::N(2));
        rt.Put(thisV, L"status", Value::N(double(res.status)));
        rt.Put(thisV, L"statusText", Value::S(res.statusText));
        rt.Put(thisV, L"responseURL", Value::S(res.finalUrl));
        rt.Put(thisV, L"\x02rawHeaders", Value::S(res.rawHeaders));
        FireXHREvent(rt, xhr, L"readystatechange");

        rt.Put(thisV, L"readyState", Value::N(3));
        FireXHREvent(rt, xhr, L"readystatechange");

        rt.Put(thisV, L"readyState", Value::N(4));
        rt.Put(thisV, L"responseText", Value::S(bodyText));
        std::wstring rType = rt.ToStr(rt.Get(thisV, L"responseType"));
        if (rType == L"json") {
            try {
                Value jsonObj = rt.Get(Value::O(rt.global), L"JSON");
                Value parseFn = rt.Get(jsonObj, L"parse");
                std::vector<Value> parseArgs{Value::S(bodyText)};
                rt.Put(thisV, L"response", rt.Call(parseFn, jsonObj, parseArgs));
            } catch (...) {
                rt.Put(thisV, L"response", Value::Null_());
            }
        } else if (rType == L"arraybuffer") {
            Obj* ab = rt.NewArrayBufferFrom(res.body.data(), res.body.size());
            rt.Put(thisV, L"response", Value::O(ab));
        } else {
            rt.Put(thisV, L"response", Value::S(bodyText));
        }

        FireXHREvent(rt, xhr, L"readystatechange");
        FireXHREvent(rt, xhr, L"load");
        FireXHREvent(rt, xhr, L"loadend");
    }
    return Value();
}

NATIVE(XHR_abort) {
    UNUSED;
    Obj* xhr = thisV.IsObj() ? thisV.o : nullptr;
    if (!xhr) rt.Throw(L"TypeError", L"Illegal invocation");
    rt.Put(thisV, L"readyState", Value::N(0));
    FireXHREvent(rt, xhr, L"abort");
    FireXHREvent(rt, xhr, L"loadend");
    return Value();
}

NATIVE(XHR_getAllResponseHeaders) {
    UNUSED;
    Value v = rt.Get(thisV, L"\x02rawHeaders");
    return v.IsStr() ? v : Value::S(L"");
}

NATIVE(XHR_getResponseHeader) {
    UNUSED;
    std::wstring name = ToLower(TrimHeader(rt.ToStr(A(0))));
    std::wstring raw = rt.ToStr(rt.Get(thisV, L"\x02rawHeaders"));
    size_t st = 0;
    while (st < raw.size()) {
        size_t e = raw.find(L"\r\n", st);
        if (e == std::wstring::npos) e = raw.find(L'\n', st);
        std::wstring line = (e == std::wstring::npos) ? raw.substr(st) : raw.substr(st, e - st);
        st = (e == std::wstring::npos) ? raw.size() : (raw[e] == L'\r' ? e + 2 : e + 1);
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos) {
            std::wstring k = ToLower(TrimHeader(line.substr(0, colon)));
            if (k == name) {
                return Value::S(TrimHeader(line.substr(colon + 1)));
            }
        }
    }
    return Value::Null_();
}

NATIVE(XHR_overrideMimeType) {
    UNUSED;
    return Value();
}

NATIVE(Illegal_ctor) { UNUSED; rt.Throw(L"TypeError", L"Illegal constructor"); }
NATIVE(Image_ctor) {
    UNUSED;
    Node* n = NewNode(Doc(rt), Node::Elem, L"img");
    if (!A(0).IsUndef()) SetAttr(n, L"width", rt.ToStr(A(0)));
    if (!A(1).IsUndef()) SetAttr(n, L"height", rt.ToStr(A(1)));
    return W(rt, n);
}
NATIVE(AbortCtl_ctor) {
    UNUSED;
    Obj* o = rt.NewObject();
    Obj* sig = rt.NewObject();
    rt.DefineData(sig, L"aborted", Value::B(false));
    for (const wchar_t* k : {L"addEventListener", L"removeEventListener"}) rt.DefineMethod(sig, k, Win_noop, 2);
    rt.DefineData(o, L"signal", Value::O(sig));
    rt.DefineMethod(o, L"abort", [](Runtime& r, Obj*, const Value& t, std::vector<Value>&) { r.Put(r.Get(t, L"signal"), L"aborted", Value::B(true)); return Value(); }, 0);
    return Value::O(o);
}
NATIVE(CE_get) { UNUSED; return Value(); }
NATIVE(Nav_sendBeacon) { UNUSED; return Value::B(false); }

Obj* NativeCtor(Runtime& rt, const wchar_t* name, NativeFn fn, Obj* proto) {
    Obj* c = rt.NewFunction(fn, name, 0, true);
    if (proto) {
        Prop& p = c->props.Put(L"prototype");
        p.v = Value::O(proto);
        p.enumerable = false;
        if (!proto->props.Find(L"constructor")) rt.DefineData(proto, L"constructor", Value::O(c), false);
    }
    rt.DefineData(rt.global, name, Value::O(c), false);
    rt.ctors[name] = c;
    return c;
}

// Olay dagitimi: hedeften yukari dogru, sonra window
bool Dispatch(Runtime& rt, Node* target, Obj* ev) {
    std::wstring type = rt.ToStr(rt.Get(Value::O(ev), L"type"));
    bool bubbles = rt.Truthy(rt.Get(Value::O(ev), L"bubbles"));
    rt.Put(Value::O(ev), L"target", W(rt, target));
    std::vector<Node*> path;
    for (Node* p = target; p && path.size() < 1000; p = p->parent) path.push_back(p);
    auto fire = [&](Obj* host, const Value& thisObj) -> bool {
        rt.Put(Value::O(ev), L"currentTarget", thisObj);
        // on<olay> ozelligi
        Value prop = rt.Get(thisObj, L"on" + type);
        if (!IsCallable(prop) && host->x && host->x->node && host->x->node->kind == Node::Elem) {
            if (auto* code = host->x->node->Attr((L"on" + type).c_str())) {
                // satir ici olay isleyici: bir kez derlenir
                prop = rt.EvalGlobal(L"(function(event){\n" + *code + L"\n})", L"on" + type);
                rt.DefineData(host, L"on" + type, prop, false);
            }
        }
        if (IsCallable(prop)) {
            std::vector<Value> a{Value::O(ev)};
            Value r = rt.Call(prop, thisObj, a);
            if (r.t == Value::Bool && !r.b) ev->x->defaultPrevented = true;
        }
        if (host->x) {
            auto ls = host->x->listeners;
            for (auto& l : ls) {
                if (l.type != type) continue;
                if (l.once) {
                    auto& real = host->x->listeners;
                    for (size_t i = 0; i < real.size(); i++) if (real[i].type == l.type && rt.StrictEq(real[i].fn, l.fn)) { real.erase(real.begin() + long(i)); break; }
                }
                std::vector<Value> a{Value::O(ev)};
                try {
                    if (IsCallable(l.fn)) rt.Call(l.fn, thisObj, a);
                    else if (l.fn.IsObj()) rt.Call(rt.Get(l.fn, L"handleEvent"), l.fn, a);
                } catch (JSThrow& t) {
                    rt.Report(L"olay: " + type, t);
                }
            }
        }
        return !ev->x->stopProp;
    };
    for (size_t i = 0; i < path.size(); i++) {
        Obj* w = rt.WrapNode(path[i]);
        rt.Put(Value::O(ev), L"eventPhase", Value::N(i == 0 ? 2 : 3));
        if (!fire(w, Value::O(w))) return !ev->x->defaultPrevented;
        if (!bubbles) return !ev->x->defaultPrevented;
    }
    if (bubbles || path.empty() || path.back()->kind == Node::Doc) fire(rt.global, Value::O(rt.global));
    return !ev->x->defaultPrevented;
}

Obj* MakeEvent(Runtime& rt, const std::wstring& type, bool bubbles, bool cancelable) {
    std::vector<Value> a{Value::S(type)};
    Obj* opts = rt.NewObject();
    rt.DefineData(opts, L"bubbles", Value::B(bubbles));
    rt.DefineData(opts, L"cancelable", Value::B(cancelable));
    a.push_back(Value::O(opts));
    Value e = rt.Construct(Value::O(rt.ctors[L"Event"]), a);
    rt.Put(e, L"isTrusted", Value::B(true));
    return e.o;
}

} // namespace

// ---------------- Runtime DOM API ----------------

Obj* Runtime::WrapNode(Node* n) {
    auto it = wrappers.find(n);
    if (it != wrappers.end()) return it->second;
    Obj* proto = n->kind == Node::Text ? textProto : n->kind == Node::Doc ? documentProto : elementProto;
    Obj* w = NewObject(proto);
    w->cls = Cls::Node;
    w->X().node = n;
    wrappers[n] = w;
    return w;
}

bool Runtime::DispatchEvent(Node* target, Obj* event) { return Dispatch(*this, target, event); }

static bool RunTaskDispatch(Runtime& rt, Node* target, const std::wstring& type, bool bubbles, bool cancelable, const std::wstring& key) {
    if (!rt.doc) return true;
    bool nested = rt.inTask;
    if (!nested) rt.BeginTask();
    bool ok = true;
    try {
        Obj* ev = MakeEvent(rt, type, bubbles, cancelable);
        if (!key.empty()) rt.DefineData(ev, L"key", Value::S(key));
        if (type == L"click") { rt.DefineData(ev, L"button", Value::N(0)); rt.DefineData(ev, L"which", Value::N(1)); }
        ok = Dispatch(rt, target, ev);
        if (!nested) rt.RunMicrotasks();
    } catch (JSThrow& t) {
        rt.Report(L"olay: " + type, t);
    } catch (JSAbort& a) {
        if (rt.host) rt.host->Log(2, L"Olay işleyici durduruldu: " + a.reason);
        rt.microtasks.clear();
        if (nested) throw;
    }
    if (!nested) { rt.EndTask(); rt.MaybeGC(); }
    return ok;
}

bool Runtime::DispatchClick(Node* target) { return RunTaskDispatch(*this, target, L"click", true, true, L""); }
bool Runtime::DispatchSimple(Node* target, const std::wstring& type, bool bubbles) { return RunTaskDispatch(*this, target, type, bubbles, true, L""); }
bool Runtime::DispatchKey(Node* target, const std::wstring& type, const std::wstring& key) { return RunTaskDispatch(*this, target, type, true, true, key); }

void Runtime::FireLoadEvents() {
    if (!doc) return;
    RunTaskDispatch(*this, doc->root.get(), L"DOMContentLoaded", true, false, L"");
    RunTaskDispatch(*this, doc->root.get(), L"readystatechange", false, false, L"");
    // window 'load'
    BeginTask();
    try {
        Obj* ev = MakeEvent(*this, L"load", false, false);
        Put(Value::O(ev), L"target", WrapNode(doc->root.get()) ? Value::O(WrapNode(doc->root.get())) : Value());
        Value onload = Get(Value::O(global), L"onload");
        if (onload.IsObj() && onload.o->IsCallable()) { std::vector<Value> a{Value::O(ev)}; Call(onload, Value::O(global), a); }
        if (global->x) {
            auto ls = global->x->listeners;
            for (auto& l : ls) {
                if (l.type != L"load") continue;
                std::vector<Value> a{Value::O(ev)};
                try { if (l.fn.IsObj() && l.fn.o->IsCallable()) Call(l.fn, Value::O(global), a); } catch (JSThrow& t) { Report(L"load", t); }
            }
        }
        RunMicrotasks();
    } catch (JSThrow& t) {
        Report(L"load", t);
    } catch (JSAbort& a) {
        if (host) host->Log(2, L"load durduruldu: " + a.reason);
        microtasks.clear();
    }
    EndTask();
    MaybeGC();
}

void Runtime::ScriptLoaded(Node* scriptNode, bool ok) {
    RunTaskDispatch(*this, scriptNode, ok ? L"load" : L"error", false, false, L"");
}

void Runtime::AttachDocument(Document* d, Host* h) {
    doc = d;
    host = h;
    exoticGet = ExoticGet;
    exoticSet = ExoticSet;
    InstallDom(*this);
}

void InstallDom(Runtime& rt) {
    Obj* g = rt.global;
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Acc = [&](Obj* o, const wchar_t* n, NativeFn get, NativeFn set) { rt.DefineAccessor(o, n, get, set); };

    // EventTarget -> Node -> (Element | Text | Document)
    rt.eventTargetProto = rt.NewObject();
    Obj* eventTargetProto = rt.eventTargetProto;
    M(eventTargetProto, L"addEventListener", ET_addEventListener, 2);
    M(eventTargetProto, L"removeEventListener", ET_removeEventListener, 2);
    M(eventTargetProto, L"dispatchEvent", ET_dispatchEvent, 1);
    rt.nodeProto = rt.NewObject(eventTargetProto);
    rt.elementProto = rt.NewObject(rt.nodeProto);
    rt.textProto = rt.NewObject(rt.nodeProto);
    rt.documentProto = rt.NewObject(rt.nodeProto);
    rt.styleProto = rt.NewObject();
    rt.classListProto = rt.NewObject();
    rt.eventProto = rt.NewObject();
    rt.storageProto = rt.NewObject();

    // window de bir EventTarget
    g->proto = eventTargetProto;
    eventTargetProto->proto = rt.objectProto;

    Obj* NP = rt.nodeProto;
    Acc(NP, L"nodeType", Node_nodeType, nullptr);
    Acc(NP, L"nodeName", Node_nodeName, nullptr);
    Acc(NP, L"nodeValue", Node_nodeValue, Node_setNodeValue);
    Acc(NP, L"data", Node_nodeValue, Node_setNodeValue);
    Acc(NP, L"textContent", Node_textContent, Node_setTextContent);
    Acc(NP, L"parentNode", Node_parentNode, nullptr);
    Acc(NP, L"parentElement", Node_parentElement, nullptr);
    Acc(NP, L"childNodes", Node_childNodes, nullptr);
    Acc(NP, L"firstChild", Node_firstChild, nullptr);
    Acc(NP, L"lastChild", Node_lastChild, nullptr);
    Acc(NP, L"nextSibling", Node_nextSibling, nullptr);
    Acc(NP, L"previousSibling", Node_previousSibling, nullptr);
    Acc(NP, L"ownerDocument", Node_ownerDocument, nullptr);
    Acc(NP, L"isConnected", Node_isConnected, nullptr);
    M(NP, L"appendChild", Node_appendChild, 1);
    M(NP, L"insertBefore", Node_insertBefore, 2);
    M(NP, L"removeChild", Node_removeChild, 1);
    M(NP, L"replaceChild", Node_replaceChild, 2);
    M(NP, L"cloneNode", Node_cloneNode, 1);
    M(NP, L"contains", Node_contains, 1);
    M(NP, L"hasChildNodes", Node_hasChildNodes, 0);
    M(NP, L"getRootNode", Node_getRootNode, 0);
    // Element ve Document ortak sorgulari NodeProto'da (DocumentFragment de kullanir)
    M(NP, L"querySelector", El_querySelector, 1);
    M(NP, L"querySelectorAll", El_querySelectorAll, 1);
    M(NP, L"getElementsByTagName", El_getElementsByTagName, 1);
    M(NP, L"getElementsByClassName", El_getElementsByClassName, 1);
    Acc(NP, L"children", El_children, nullptr);
    Acc(NP, L"childElementCount", El_childElementCount, nullptr);
    Acc(NP, L"firstElementChild", El_firstElementChild, nullptr);
    Acc(NP, L"lastElementChild", El_lastElementChild, nullptr);
    M(NP, L"append", El_append, 1);
    M(NP, L"prepend", El_prepend, 1);

    Obj* EP = rt.elementProto;
    Acc(EP, L"tagName", El_tagName, nullptr);
    Acc(EP, L"localName", El_localName, nullptr);
    Acc(EP, L"id", El_id, El_setId);
    Acc(EP, L"className", El_className, El_setClassName);
    Acc(EP, L"classList", El_classList, nullptr);
    Acc(EP, L"style", El_style, nullptr);
    Acc(EP, L"dataset", El_dataset, nullptr);
    Acc(EP, L"attributes", El_attributes, nullptr);
    Acc(EP, L"innerHTML", El_innerHTML, El_setInnerHTML);
    Acc(EP, L"outerHTML", El_outerHTML, El_setOuterHTML);
    Acc(EP, L"innerText", Node_textContent, Node_setTextContent);
    Acc(EP, L"outerText", Node_textContent, nullptr);
    Acc(EP, L"nextElementSibling", Node_nextElementSibling, nullptr);
    Acc(EP, L"previousElementSibling", Node_previousElementSibling, nullptr);
    Acc(EP, L"value", El_value, El_setValue);
    Acc(EP, L"defaultValue", El_value, nullptr);
    Acc(EP, L"checked", El_checked, El_setChecked);
    Acc(EP, L"disabled", El_disabled, El_setDisabled);
    Acc(EP, L"hidden", El_hidden, El_setHidden);
    Acc(EP, L"href", El_href, El_setHref);
    Acc(EP, L"src", El_src, El_setSrc);
    Acc(EP, L"title", El_title, El_setTitle);
    Acc(EP, L"type", El_type, El_setType);
    Acc(EP, L"name", El_name, El_setName);
    Acc(EP, L"text", El_text, Node_setTextContent);
    Acc(EP, L"form", El_form, nullptr);
    Acc(EP, L"elements", El_elements, nullptr);
    Acc(EP, L"options", El_options, nullptr);
    Acc(EP, L"selectedIndex", El_selectedIndex, nullptr);
    Acc(EP, L"contentDocument", El_contentDocument, nullptr);
    Acc(EP, L"contentWindow", El_contentWindow, nullptr);
    Acc(EP, L"offsetWidth", El_offsetWidth, nullptr);
    Acc(EP, L"offsetHeight", El_offsetHeight, nullptr);
    Acc(EP, L"clientWidth", El_offsetWidth, nullptr);
    Acc(EP, L"clientHeight", El_offsetHeight, nullptr);
    Acc(EP, L"scrollWidth", El_offsetWidth, nullptr);
    Acc(EP, L"scrollHeight", El_offsetHeight, nullptr);
    Acc(EP, L"offsetTop", El_offsetTop, nullptr);
    Acc(EP, L"offsetLeft", El_offsetLeft, nullptr);
    Acc(EP, L"offsetParent", El_offsetParent, nullptr);
    Acc(EP, L"scrollTop", El_zero, El_noop);
    Acc(EP, L"scrollLeft", El_zero, El_noop);
    M(EP, L"getAttribute", El_getAttribute, 1);
    M(EP, L"setAttribute", El_setAttribute, 2);
    M(EP, L"removeAttribute", El_removeAttribute, 1);
    M(EP, L"hasAttribute", El_hasAttribute, 1);
    M(EP, L"toggleAttribute", El_toggleAttribute, 1);
    M(EP, L"getAttributeNames", El_getAttributeNames, 0);
    M(EP, L"getAttributeNS", El_getAttribute, 2);
    M(EP, L"setAttributeNS", El_setAttribute, 3);
    M(EP, L"insertAdjacentHTML", El_insertAdjacentHTML, 2);
    M(EP, L"insertAdjacentElement", El_insertAdjacentElement, 2);
    M(EP, L"matches", El_matches, 1);
    M(EP, L"webkitMatchesSelector", El_matches, 1);
    M(EP, L"closest", El_closest, 1);
    M(EP, L"before", El_before, 1);
    M(EP, L"after", El_after, 1);
    M(EP, L"remove", El_remove, 0);
    M(EP, L"replaceWith", El_replaceWith, 1);
    M(EP, L"getBoundingClientRect", El_getBoundingClientRect, 0);
    M(EP, L"getClientRects", El_getClientRects, 0);
    M(EP, L"click", El_click, 0);
    M(EP, L"submit", El_submit, 0);
    M(EP, L"requestSubmit", El_submit, 0);
    M(EP, L"reset", El_reset, 0);
    M(EP, L"namedItem", El_namedItem, 1);
    for (const wchar_t* n : {L"focus", L"blur", L"scrollIntoView", L"scrollTo", L"scrollBy", L"select", L"setSelectionRange", L"setCustomValidity", L"reportValidity", L"releasePointerCapture", L"setPointerCapture", L"animate", L"attachShadow"})
        M(EP, n, El_noop, 0);
    M(EP, L"checkValidity", Doc_true, 0);

    // Media methods and accessors (HTMLMediaElement, HTMLVideoElement, HTMLAudioElement)
    M(EP, L"canPlayType", El_canPlayType, 1);
    M(EP, L"play", El_play, 0);
    M(EP, L"pause", El_pause, 0);
    M(EP, L"load", El_load, 0);
    Acc(EP, L"currentTime", El_currentTime, El_setCurrentTime);
    Acc(EP, L"duration", El_duration, El_setDuration);
    Acc(EP, L"paused", El_paused, nullptr);
    Acc(EP, L"ended", El_ended, nullptr);
    Acc(EP, L"muted", El_muted, El_setMuted);
    Acc(EP, L"volume", El_volume, El_setVolume);
    Acc(EP, L"readyState", El_mediaReadyState, nullptr);
    Acc(EP, L"networkState", El_networkState, nullptr);
    Acc(EP, L"videoWidth", El_videoWidth, nullptr);
    Acc(EP, L"videoHeight", El_videoHeight, nullptr);
    Acc(EP, L"currentSrc", El_currentSrc, nullptr);
    Acc(EP, L"autoplay", El_autoplay, El_setAutoplay);
    Acc(EP, L"loop", El_loop, El_setLoop);
    Acc(EP, L"controls", El_controls, El_setControls);
    Acc(EP, L"playbackRate", El_playbackRate, El_setPlaybackRate);
    Acc(EP, L"defaultPlaybackRate", El_playbackRate, El_setPlaybackRate);
    Acc(EP, L"defaultMuted", El_muted, El_setMuted);
    Acc(EP, L"seeking", El_seeking, nullptr);
    Acc(EP, L"buffered", El_mediaTimeRanges, nullptr);
    Acc(EP, L"played", El_mediaTimeRanges, nullptr);
    Acc(EP, L"seekable", El_mediaTimeRanges, nullptr);

    // on<olay> ozellikleri (ozellik algilama icin: "onclick" in el)
    for (const wchar_t* ev : {L"click", L"dblclick", L"mousedown", L"mouseup", L"mousemove", L"mouseover", L"mouseout", L"mouseenter",
                              L"mouseleave", L"keydown", L"keyup", L"keypress", L"input", L"change", L"submit", L"reset", L"focus",
                              L"blur", L"load", L"error", L"scroll", L"resize", L"touchstart", L"touchend", L"touchmove", L"wheel",
                              L"contextmenu", L"pointerdown", L"pointerup", L"pointermove", L"pointerenter", L"pointerleave",
                              L"animationend", L"transitionend", L"select", L"abort", L"focusin", L"focusout", L"dragstart", L"drop"}) {
        rt.DefineData(NP, std::wstring(L"on") + ev, Value::Null_(), false);
        if (!g->props.Find(std::wstring(L"on") + ev)) rt.DefineData(g, std::wstring(L"on") + ev, Value::Null_(), false);
    }
    for (const wchar_t* ev : {L"beforeunload", L"unload", L"hashchange", L"popstate", L"message", L"online", L"offline", L"pageshow", L"pagehide"})
        rt.DefineData(g, std::wstring(L"on") + ev, Value::Null_(), false);

    // Text: dugum ozellikleri yeterli; Element metotlari yok
    M(rt.textProto, L"remove", El_remove, 0);
    Acc(rt.textProto, L"nextElementSibling", Node_nextElementSibling, nullptr);
    Acc(rt.textProto, L"wholeText", Node_textContent, nullptr);

    Obj* DP = rt.documentProto;
    Acc(DP, L"documentElement", Doc_documentElement, nullptr);
    Acc(DP, L"body", Doc_body, nullptr);
    Acc(DP, L"head", Doc_head, nullptr);
    Acc(DP, L"title", Doc_title, Doc_setTitle);
    Acc(DP, L"cookie", Doc_cookie, Doc_setCookie);
    Acc(DP, L"readyState", Doc_readyState, nullptr);
    Acc(DP, L"URL", Doc_URL, nullptr);
    Acc(DP, L"documentURI", Doc_URL, nullptr);
    Acc(DP, L"referrer", Doc_referrer, nullptr);
    Acc(DP, L"domain", Doc_domain, nullptr);
    Acc(DP, L"currentScript", Doc_currentScript, nullptr);
    Acc(DP, L"activeElement", Doc_activeElement, nullptr);
    Acc(DP, L"hidden", Doc_false, nullptr);
    Acc(DP, L"visibilityState", Doc_visible, nullptr);
    Acc(DP, L"characterSet", Doc_charset, nullptr);
    Acc(DP, L"charset", Doc_charset, nullptr);
    Acc(DP, L"compatMode", Doc_compat, nullptr);
    Acc(DP, L"scripts", Doc_scripts, nullptr);
    Acc(DP, L"images", Doc_images, nullptr);
    Acc(DP, L"links", Doc_links, nullptr);
    Acc(DP, L"forms", Doc_forms, nullptr);
    Acc(DP, L"location", Doc_location, Doc_setLocation);
    Acc(DP, L"defaultView", Doc_defaultView, nullptr);
    Acc(DP, L"implementation", Doc_implementation, nullptr);
    Acc(DP, L"scrollingElement", Doc_documentElement, nullptr);
    M(DP, L"createElement", Doc_createElement, 1);
    M(DP, L"createElementNS", Doc_createElementNS, 2);
    M(DP, L"createTextNode", Doc_createTextNode, 1);
    M(DP, L"createComment", Doc_createComment, 1);
    M(DP, L"createDocumentFragment", Doc_createDocumentFragment, 0);
    M(DP, L"createEvent", Doc_createEvent, 1);
    M(DP, L"getElementById", Doc_getElementById, 1);
    M(DP, L"getElementsByName", Doc_getElementsByName, 1);
    M(DP, L"write", Doc_write, 1);
    M(DP, L"writeln", Doc_writeln, 1);
    M(DP, L"open", Doc_open, 0);
    M(DP, L"close", Doc_close, 0);
    M(DP, L"hasFocus", Doc_true, 0);
    M(DP, L"execCommand", Doc_false, 0);

    // style / classList / storage prototipleri
    M(rt.styleProto, L"setProperty", Style_setProperty, 3);
    M(rt.styleProto, L"getPropertyValue", Style_getPropertyValue, 1);
    M(rt.styleProto, L"removeProperty", Style_removeProperty, 1);
    Obj* CLP = rt.classListProto;
    M(CLP, L"add", CL_add, 1); M(CLP, L"remove", CL_remove, 1); M(CLP, L"contains", CL_contains, 1);
    M(CLP, L"toggle", CL_toggle, 1); M(CLP, L"replace", CL_replace, 2); M(CLP, L"item", CL_item, 1);
    M(CLP, L"forEach", CL_forEach, 1);
    Acc(CLP, L"length", CL_length, nullptr);
    Acc(CLP, L"value", CL_value, nullptr);
    M(CLP, L"toString", CL_value, 0);
    rt.DefineData(CLP, L"\x01Symbol.iterator", Value::O(rt.NewFunction(CL_iter, L"[Symbol.iterator]", 0)), false);
    Obj* SP = rt.storageProto;
    M(SP, L"getItem", St_getItem, 1); M(SP, L"setItem", St_setItem, 2); M(SP, L"removeItem", St_removeItem, 1);
    M(SP, L"clear", St_clear, 0); M(SP, L"key", St_key, 1);
    Acc(SP, L"length", St_length, nullptr);

    // Olaylar
    Obj* EvP = rt.eventProto;
    M(EvP, L"preventDefault", Ev_preventDefault, 0);
    M(EvP, L"stopPropagation", Ev_stopPropagation, 0);
    M(EvP, L"stopImmediatePropagation", Ev_stopPropagation, 0);
    M(EvP, L"composedPath", Ev_composedPath, 0);
    M(EvP, L"initEvent", Ev_initEvent, 3);
    M(EvP, L"initCustomEvent", Ev_initEvent, 4);
    Acc(EvP, L"defaultPrevented", Ev_defaultPrevented, nullptr);
    Obj* EvC = NativeCtor(rt, L"Event", Event_ctor, EvP);
    for (const wchar_t* n : {L"CustomEvent", L"MouseEvent", L"KeyboardEvent", L"UIEvent", L"FocusEvent", L"InputEvent", L"PointerEvent", L"TouchEvent", L"MessageEvent", L"ErrorEvent", L"ProgressEvent", L"SubmitEvent"}) {
        Obj* p = rt.NewObject(EvP);
        Obj* c = NativeCtor(rt, n, Event_ctor, p);
        c->proto = EvC;
    }

    // Kurucular (instanceof icin)
    NativeCtor(rt, L"EventTarget", Illegal_ctor, eventTargetProto);
    NativeCtor(rt, L"Node", Illegal_ctor, NP);
    for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{{L"ELEMENT_NODE", 1}, {L"TEXT_NODE", 3}, {L"COMMENT_NODE", 8}, {L"DOCUMENT_NODE", 9}, {L"DOCUMENT_FRAGMENT_NODE", 11}})
        rt.DefineData(rt.ctors[L"Node"], kv.first, Value::N(kv.second), false);
    NativeCtor(rt, L"Element", Illegal_ctor, EP);
    NativeCtor(rt, L"HTMLElement", Illegal_ctor, EP);
    for (const wchar_t* n : {L"HTMLDivElement", L"HTMLInputElement", L"HTMLAnchorElement", L"HTMLImageElement", L"HTMLFormElement",
                             L"HTMLButtonElement", L"HTMLSelectElement", L"HTMLTextAreaElement", L"HTMLScriptElement", L"HTMLIFrameElement",
                             L"HTMLCanvasElement", L"HTMLVideoElement", L"HTMLAudioElement", L"HTMLMediaElement", L"HTMLTemplateElement", L"HTMLBodyElement",
                             L"HTMLSpanElement", L"HTMLParagraphElement", L"HTMLLinkElement", L"HTMLStyleElement", L"SVGElement"})
        NativeCtor(rt, n, Illegal_ctor, EP);
    NativeCtor(rt, L"Audio", Audio_ctor, EP);
    NativeCtor(rt, L"Text", Illegal_ctor, rt.textProto);
    NativeCtor(rt, L"CharacterData", Illegal_ctor, rt.textProto);
    NativeCtor(rt, L"Document", Illegal_ctor, DP);
    NativeCtor(rt, L"HTMLDocument", Illegal_ctor, DP);
    NativeCtor(rt, L"DocumentFragment", Illegal_ctor, NP);
    NativeCtor(rt, L"Storage", Illegal_ctor, SP);
    NativeCtor(rt, L"Image", Image_ctor, EP);
    NativeCtor(rt, L"AbortController", AbortCtl_ctor, nullptr);

    // MediaElement sabitleri (HAVE_*, NETWORK_*)
    for (const wchar_t* cName : {L"HTMLMediaElement", L"HTMLVideoElement", L"HTMLAudioElement"}) {
        auto it = rt.ctors.find(cName);
        if (it != rt.ctors.end()) {
            Obj* c = it->second;
            rt.DefineData(c, L"HAVE_NOTHING", Value::N(0), false);
            rt.DefineData(c, L"HAVE_METADATA", Value::N(1), false);
            rt.DefineData(c, L"HAVE_CURRENT_DATA", Value::N(2), false);
            rt.DefineData(c, L"HAVE_FUTURE_DATA", Value::N(3), false);
            rt.DefineData(c, L"HAVE_ENOUGH_DATA", Value::N(4), false);
            rt.DefineData(c, L"NETWORK_EMPTY", Value::N(0), false);
            rt.DefineData(c, L"NETWORK_IDLE", Value::N(1), false);
            rt.DefineData(c, L"NETWORK_LOADING", Value::N(2), false);
            rt.DefineData(c, L"NETWORK_NO_SOURCE", Value::N(3), false);
        }
    }

    // Blob
    rt.blobProto = rt.NewObject();
    M(rt.blobProto, L"slice", BlobP_slice, 2);
    M(rt.blobProto, L"text", BlobP_text, 0);
    M(rt.blobProto, L"arrayBuffer", BlobP_arrayBuffer, 0);
    NativeCtor(rt, L"Blob", Blob_ctor, rt.blobProto);

    // MediaSource & SourceBuffer
    rt.mediaSourceProto = rt.NewObject(eventTargetProto);
    M(rt.mediaSourceProto, L"addSourceBuffer", MediaSource_addSourceBuffer, 1);
    M(rt.mediaSourceProto, L"removeSourceBuffer", MediaSource_removeSourceBuffer, 1);
    M(rt.mediaSourceProto, L"endOfStream", MediaSource_endOfStream, 1);
    Obj* msCtor = NativeCtor(rt, L"MediaSource", MediaSource_ctor, rt.mediaSourceProto);
    M(msCtor, L"isTypeSupported", MediaSource_isTypeSupported, 1);
    rt.DefineData(g, L"WebKitMediaSource", Value::O(msCtor), false);

    rt.sourceBufferProto = rt.NewObject(eventTargetProto);
    M(rt.sourceBufferProto, L"appendBuffer", SourceBuffer_appendBuffer, 1);
    M(rt.sourceBufferProto, L"abort", SourceBuffer_abort, 0);
    M(rt.sourceBufferProto, L"remove", SourceBuffer_remove, 2);
    NativeCtor(rt, L"SourceBuffer", Illegal_ctor, rt.sourceBufferProto);

    // document ve window
    Obj* docW = rt.WrapNode(rt.doc->root.get());
    rt.DefineData(g, L"document", Value::O(docW), false);
    for (const wchar_t* n : {L"window", L"self", L"top", L"parent", L"frames"}) rt.DefineData(g, n, Value::O(g), false);
    rt.DefineData(g, L"frameElement", Value::Null_(), false);
    rt.DefineData(g, L"opener", Value::Null_(), false);
    rt.DefineData(g, L"name", Value::S(L""));
    rt.DefineData(g, L"closed", Value::B(false), false);
    rt.DefineData(g, L"isSecureContext", Value::B(true), false);
    rt.DefineData(g, L"origin", Value::S(L""), false);
    Acc(g, L"innerWidth", Win_innerWidth, nullptr);
    Acc(g, L"innerHeight", Win_innerHeight, nullptr);
    Acc(g, L"outerWidth", Win_innerWidth, nullptr);
    Acc(g, L"outerHeight", Win_innerHeight, nullptr);
    rt.DefineData(g, L"devicePixelRatio", Value::N(1));
    for (const wchar_t* n : {L"scrollX", L"scrollY", L"pageXOffset", L"pageYOffset", L"screenX", L"screenY", L"screenLeft", L"screenTop"}) Acc(g, n, Win_zero, nullptr);
    for (const wchar_t* n : {L"scrollTo", L"scrollBy", L"scroll", L"focus", L"blur", L"print", L"close", L"stop", L"postMessage", L"moveTo", L"resizeTo"}) M(g, n, Win_noop, 0);
    M(g, L"alert", Win_alert, 1);
    M(g, L"confirm", Win_confirm, 1);
    M(g, L"prompt", Win_prompt, 2);
    M(g, L"open", Win_open, 3);
    M(g, L"requestAnimationFrame", Win_rAF, 1);
    M(g, L"cancelAnimationFrame", rt.Get(Value::O(g), L"clearTimeout").o->x->native, 1);
    M(g, L"requestIdleCallback", Win_idle, 1);
    M(g, L"cancelIdleCallback", rt.Get(Value::O(g), L"clearTimeout").o->x->native, 1);
    M(g, L"getComputedStyle", Win_getComputedStyle, 1);
    M(g, L"matchMedia", Win_matchMedia, 1);
    M(g, L"getSelection", Win_null, 0);
    M(g, L"writeEmbed", Win_writeEmbed, 1);

    Obj* loc = rt.NewObject();
    Acc(loc, L"href", Loc_href, Loc_setHref);
    Acc(loc, L"protocol", Loc_protocol, nullptr);
    Acc(loc, L"host", Loc_host, nullptr);
    Acc(loc, L"hostname", Loc_hostname, nullptr);
    Acc(loc, L"port", Loc_port, nullptr);
    Acc(loc, L"pathname", Loc_pathname, nullptr);
    Acc(loc, L"search", Loc_search, nullptr);
    Acc(loc, L"hash", Loc_hash, Loc_setHash);
    Acc(loc, L"origin", Loc_origin, nullptr);
    M(loc, L"assign", Loc_assign, 1);
    M(loc, L"replace", Loc_assign, 1);
    M(loc, L"reload", Loc_reload, 0);
    M(loc, L"toString", Loc_href, 0);
    Acc(g, L"location", [](Runtime& r, Obj*, const Value&, std::vector<Value>&) { return r.Get(Value::O(r.global), L"\x02loc"); }, Loc_setHref);
    rt.DefineData(g, L"\x02loc", Value::O(loc), false);

    Obj* nav = rt.NewObject();
    rt.DefineData(g, L"navigator", Value::O(nav), false);
    rt.DefineData(nav, L"userAgent", Value::S(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) merOSShield/0.2 MerEngine/0.2 merJS/0.1"));
    rt.DefineData(nav, L"appName", Value::S(L"Netscape"));
    rt.DefineData(nav, L"appVersion", Value::S(L"5.0 (Windows)"));
    rt.DefineData(nav, L"platform", Value::S(L"Win32"));
    rt.DefineData(nav, L"vendor", Value::S(L""));
    rt.DefineData(nav, L"product", Value::S(L"Gecko"));
    std::vector<std::wstring> langs = i18n::GetLanguageList();
    std::vector<Value> langVals;
    langVals.reserve(langs.size());
    for (const auto& l : langs) langVals.push_back(Value::S(l));
    rt.DefineData(nav, L"language", Value::S(i18n::GetLanguageCode()));
    rt.DefineData(nav, L"languages", Value::O(rt.NewArrayFrom(langVals)));
    rt.DefineData(nav, L"cookieEnabled", Value::B(true));
    rt.DefineData(nav, L"onLine", Value::B(true));
    rt.DefineData(nav, L"doNotTrack", Value::S(L"1"));
    rt.DefineData(nav, L"globalPrivacyControl", Value::B(true));
    rt.DefineData(nav, L"hardwareConcurrency", Value::N(4));
    rt.DefineData(nav, L"maxTouchPoints", Value::N(0));
    rt.DefineData(nav, L"webdriver", Value::B(false));
    M(nav, L"sendBeacon", Nav_sendBeacon, 2);
    M(nav, L"javaEnabled", Doc_false, 0);
    Obj* plugins = rt.NewArray(0);
    rt.DefineData(nav, L"plugins", Value::O(plugins));
    rt.DefineData(nav, L"mimeTypes", Value::O(rt.NewArray(0)));

    Obj* scr = rt.NewObject();
    rt.DefineData(g, L"screen", Value::O(scr), false);
    double vw = 1280, vh = 800;
    if (rt.host) rt.host->ViewportSize(vw, vh);
    for (const wchar_t* n : {L"width", L"availWidth"}) rt.DefineData(scr, n, Value::N(std::max(vw, 1280.0)));
    for (const wchar_t* n : {L"height", L"availHeight"}) rt.DefineData(scr, n, Value::N(std::max(vh, 800.0)));
    rt.DefineData(scr, L"colorDepth", Value::N(24));
    rt.DefineData(scr, L"pixelDepth", Value::N(24));

    Obj* hist = rt.NewObject();
    rt.DefineData(g, L"history", Value::O(hist), false);
    rt.DefineData(hist, L"length", Value::N(1));
    rt.DefineData(hist, L"state", Value::Null_());
    rt.DefineData(hist, L"scrollRestoration", Value::S(L"auto"));
    M(hist, L"pushState", Hist_pushState, 3);
    M(hist, L"replaceState", Hist_pushState, 3);
    M(hist, L"back", Hist_back, 0);
    M(hist, L"forward", Hist_forward, 0);
    M(hist, L"go", Hist_go, 1);

    for (int k = 0; k < 2; k++) {
        Obj* st = rt.NewObject(SP);
        st->cls = Cls::Storage;
        st->X().num = k;
        rt.DefineData(g, k ? L"sessionStorage" : L"localStorage", Value::O(st), false);
    }

    Obj* perf = rt.NewObject();
    rt.DefineData(g, L"performance", Value::O(perf), false);
    M(perf, L"now", Perf_now, 0);
    rt.DefineData(perf, L"timeOrigin", Value::N(0));
    for (const wchar_t* n : {L"mark", L"measure", L"clearMarks", L"clearMeasures"}) M(perf, n, Win_noop, 1);
    for (const wchar_t* n : {L"getEntriesByType", L"getEntriesByName", L"getEntries"}) M(perf, n, Perf_entries, 1);
    Obj* timing = rt.NewObject();
    rt.DefineData(timing, L"navigationStart", Value::N(0));
    rt.DefineData(perf, L"timing", Value::O(timing));

    // Gozlemciler
    for (const wchar_t* n : {L"MutationObserver", L"ResizeObserver", L"IntersectionObserver", L"PerformanceObserver"}) {
        Obj* p = rt.NewObject();
        M(p, L"observe", std::wstring(n) == L"IntersectionObserver" ? IO_observe : Win_noop, 2);
        M(p, L"unobserve", Win_noop, 1);
        M(p, L"disconnect", Win_noop, 0);
        M(p, L"takeRecords", Obs_takeRecords, 0);
        NativeCtor(rt, n, Obs_ctor, p);
    }

    Obj* uspP = rt.NewObject();
    M(uspP, L"get", USP_get, 1); M(uspP, L"getAll", USP_getAll, 1); M(uspP, L"has", USP_has, 1);
    M(uspP, L"append", USP_append, 2); M(uspP, L"delete", USP_delete, 1); M(uspP, L"set", USP_set, 2);
    M(uspP, L"toString", USP_toString, 0); M(uspP, L"forEach", USP_forEach, 1); M(uspP, L"entries", USP_entries, 0);
    rt.DefineData(uspP, L"\x01Symbol.iterator", Value::O(rt.NewFunction(USP_entries, L"[Symbol.iterator]", 0)), false);
    NativeCtor(rt, L"URLSearchParams", USP_ctor, uspP);
    Obj* urlP = rt.NewObject();
    M(urlP, L"toString", URL_toString, 0);
    M(urlP, L"toJSON", URL_toString, 0);
    Obj* urlCtor = NativeCtor(rt, L"URL", URL_ctor, urlP);
    M(urlCtor, L"createObjectURL", URL_createObjectURL, 1);
    M(urlCtor, L"revokeObjectURL", URL_revokeObjectURL, 1);
    rt.DefineData(g, L"webkitURL", Value::O(urlCtor), false);

    Obj* ce = rt.NewObject();
    rt.DefineData(g, L"customElements", Value::O(ce), false);
    M(ce, L"define", Win_noop, 2);
    M(ce, L"get", CE_get, 1);
    M(ce, L"whenDefined", [](Runtime& r, Obj*, const Value&, std::vector<Value>&) { Obj* p = r.NewPromise(); return Value::O(p); }, 1);

    // Headers
    Obj* headersProto = rt.NewObject();
    M(headersProto, L"append", Headers_append, 2);
    M(headersProto, L"set", Headers_set, 2);
    M(headersProto, L"get", Headers_get, 1);
    M(headersProto, L"has", Headers_has, 1);
    M(headersProto, L"delete", Headers_delete, 1);
    M(headersProto, L"forEach", Headers_forEach, 1);
    M(headersProto, L"entries", Headers_entries, 0);
    rt.DefineData(headersProto, L"\x01Symbol.iterator", Value::O(rt.NewFunction(Headers_entries, L"[Symbol.iterator]", 0)), false);
    NativeCtor(rt, L"Headers", Headers_ctor, headersProto);

    // Response
    Obj* responseProto = rt.NewObject();
    M(responseProto, L"text", Response_text, 0);
    M(responseProto, L"json", Response_json, 0);
    M(responseProto, L"blob", Response_blob, 0);
    M(responseProto, L"arrayBuffer", Response_arrayBuffer, 0);
    M(responseProto, L"clone", Response_clone, 0);
    NativeCtor(rt, L"Response", Response_ctor, responseProto);

    // Request
    Obj* requestProto = rt.NewObject();
    NativeCtor(rt, L"Request", Request_ctor, requestProto);

    // fetch
    M(g, L"fetch", Win_fetch, 2);

    // XMLHttpRequest
    Obj* xhrProto = rt.NewObject(rt.eventTargetProto);
    M(xhrProto, L"open", XHR_open, 3);
    M(xhrProto, L"setRequestHeader", XHR_setRequestHeader, 2);
    M(xhrProto, L"send", XHR_send, 1);
    M(xhrProto, L"abort", XHR_abort, 0);
    M(xhrProto, L"getAllResponseHeaders", XHR_getAllResponseHeaders, 0);
    M(xhrProto, L"getResponseHeader", XHR_getResponseHeader, 1);
    M(xhrProto, L"overrideMimeType", XHR_overrideMimeType, 1);

    for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{
             {L"UNSENT", 0}, {L"OPENED", 1}, {L"HEADERS_RECEIVED", 2}, {L"LOADING", 3}, {L"DONE", 4}}) {
        rt.DefineData(xhrProto, kv.first, Value::N(kv.second), false);
    }

    Obj* xhrCtor = NativeCtor(rt, L"XMLHttpRequest", XHR_ctor, xhrProto);
    for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{
             {L"UNSENT", 0}, {L"OPENED", 1}, {L"HEADERS_RECEIVED", 2}, {L"LOADING", 3}, {L"DONE", 4}}) {
        rt.DefineData(xhrCtor, kv.first, Value::N(kv.second), false);
    }
}

} // namespace js
} // namespace mer
