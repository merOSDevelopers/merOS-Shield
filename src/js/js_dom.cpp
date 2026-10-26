#ifndef NOMINMAX
#define NOMINMAX
#endif
// merOS Shield - merJS : DOM bağlantıları (document, element, olaylar, window, location, storage...)
#include <algorithm>
#include <cmath>
#include <list>
#include <random>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif
#include "../css.h"
#include "../style.h"
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

// ---- Custom Elements ileri bildirimleri ----
void CEAttr(Runtime& rt, Node* n, const std::wstring& name, const Value& oldV, const Value& newV);
void CEInserted(Runtime& rt, Node* n, bool upgradeDisconnected);
void CERemoved(Runtime& rt, Node* n);
void SlotChangeFor(Runtime& rt, Node* lightChild);
int CEDefFor(Runtime& rt, const std::wstring& name);
void CEUpgrade(Runtime& rt, Node* e, int di);
void MONotify(Runtime& rt, Node* target, uint8_t kind, const std::wstring& attrName, const Value& oldV,
              Node* added, Node* removed, Node* prev, Node* next);
void MOChild(Runtime& rt, Node* parent, Node* added, Node* removed, Node* prev, Node* next);
void MOInserted(Runtime& rt, Node* n);
void MOChar(Runtime& rt, Node* n, const std::wstring& old);
Node* Sibling(Node* n, int dir, bool elemOnly);

void SetAttr(Runtime& rt, Node* n, const std::wstring& name, const std::wstring& value) {
    std::wstring k = ToLower(name);
    for (auto& a : n->attrs) if (a.first == k) {
        std::wstring old = a.second;
        a.second = value;
        SyncAttrCache(n);
        if (!rt.moRegs.empty()) MONotify(rt, n, 2, k, Value::S(old), nullptr, nullptr, nullptr, nullptr);
        if (n->ceState == 1) CEAttr(rt, n, k, Value::S(old), Value::S(value));
        return;
    }
    if (n->attrs.size() < 256) n->attrs.emplace_back(k, value);
    SyncAttrCache(n);
    if (!rt.moRegs.empty()) MONotify(rt, n, 2, k, Value::Null_(), nullptr, nullptr, nullptr, nullptr);
    if (n->ceState == 1) CEAttr(rt, n, k, Value::Null_(), Value::S(value));
}
void RemoveAttr(Runtime& rt, Node* n, const std::wstring& name) {
    std::wstring k = ToLower(name);
    for (size_t i = 0; i < n->attrs.size(); i++) if (n->attrs[i].first == k) {
        std::wstring old = n->attrs[i].second;
        n->attrs.erase(n->attrs.begin() + long(i));
        SyncAttrCache(n);
        if (!rt.moRegs.empty()) MONotify(rt, n, 2, k, Value::S(old), nullptr, nullptr, nullptr, nullptr);
        if (n->ceState == 1) CEAttr(rt, n, k, Value::S(old), Value::Null_());
        return;
    }
    SyncAttrCache(n);
}

// Agactan cikarma + disconnectedCallback / slotchange
void RemoveNodeCE(Runtime& rt, Document& d, Node* n) {
    SlotChangeFor(rt, n);
    Node* par = n->parent;
    Node* prev = (par && !rt.moRegs.empty()) ? Sibling(n, -1, false) : nullptr;
    Node* next = (par && !rt.moRegs.empty()) ? Sibling(n, 1, false) : nullptr;
    RemoveNode(d, n);
    CERemoved(rt, n);
    if (par) MOChild(rt, par, nullptr, n, prev, next);
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
        if (!s->Attr(L"src") && Trim(TextContent(s)).empty()) continue;
        SetAttr(rt, s, L"data-merjs-ran", L"1");
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
    if (Contains(Doc(rt).root.get(), n)) {
        RunInsertedScripts(rt, n);
        if (n->kind == Node::Text && n->parent && n->parent->name == L"script" && !n->parent->Attr(L"data-merjs-ran")) RunInsertedScripts(rt, n->parent);
    }
    CEInserted(rt, n, false);
    SlotChangeFor(rt, n);
    MOInserted(rt, n);
    if (Contains(Doc(rt).root.get(), n)) MediaScan(rt, n);
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
        Node* oldPar = rt.moRegs.empty() ? nullptr : c->parent;
        Node* oPrev = oldPar ? Sibling(c, -1, false) : nullptr;
        Node* oNext = oldPar ? Sibling(c, 1, false) : nullptr;
        if (oldPar) SlotChangeFor(rt, c);
        if (!InsertBefore(Doc(rt), parent, c, ref)) { ok = false; continue; }
        if (oldPar) MOChild(rt, oldPar, nullptr, c, oPrev, oNext);
        AfterInsert(rt, c);
    }
    return ok;
}

// ---------------- Node ----------------

NATIVE(Node_nodeType) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Text) return Value::N(3);
    if (n->kind == Node::Comment) return Value::N(8);
    if (n->kind == Node::Doc) return Value::N(9);
    if (n->name == L"#fragment" || n->kind == Node::Shadow) return Value::N(11);
    return Value::N(1);
}
NATIVE(Node_nodeName) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Text) return Value::S(L"#text");
    if (n->kind == Node::Comment) return Value::S(L"#comment");
    if (n->kind == Node::Doc) return Value::S(L"#document");
    if (n->name == L"#fragment" || n->kind == Node::Shadow) return Value::S(L"#document-fragment");
    std::wstring u = n->name;
    for (auto& c : u) if (c >= L'a' && c <= L'z') c = wchar_t(c - 32);
    return Value::S(u);
}
NATIVE(Node_nodeValue) { UNUSED; Node* n = N(rt, thisV); return (n->kind == Node::Text || n->kind == Node::Comment) ? Value::S(n->text) : Value::Null_(); }
NATIVE(Node_setNodeValue) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Text || n->kind == Node::Comment) {
        std::wstring old = n->text;
        n->text = A(0).t == Value::Null ? L"" : rt.ToStr(A(0));
        if (n->kind == Node::Text) Dirty(rt);
        MOChar(rt, n, old);
    }
    return Value();
}
NATIVE(Node_textContent) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Doc) return Value::Null_();
    if (n->kind == Node::Comment) return Value::S(n->text);
    return Value::S(TextContent(n));
}
NATIVE(Node_setTextContent) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring t = A(0).IsNullish() ? L"" : rt.ToStr(A(0));
    if (n->kind == Node::Text || n->kind == Node::Comment) {
        std::wstring old = n->text;
        n->text = t;
        if (n->kind == Node::Text) Dirty(rt);
        MOChar(rt, n, old);
        return Value();
    }
    while (!n->kids.empty()) RemoveNodeCE(rt, Doc(rt), n->kids.back().get());
    if (!t.empty()) {
        Node* tn = NewNode(Doc(rt), Node::Text, t);
        AppendChild(Doc(rt), n, tn);
        MOInserted(rt, tn);
        if (n->name == L"script" && Contains(Doc(rt).root.get(), n) && !n->Attr(L"data-merjs-ran")) RunInsertedScripts(rt, n);
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
// <template>.content icindeki dugumler ayri, etkisiz bir belgeye aittir (tarayicilardaki gibi):
// orada olusturulan/kopyalanan ozel ogeler yukseltilmez.
bool InTemplateContent(Runtime& rt, const Node* n) {
    if (rt.templateFragments.empty()) return false;
    const Node* top = n;
    for (int g = 0; top->parent && g < 100000; g++) top = top->parent;
    return rt.templateFragments.count(const_cast<Node*>(top)) > 0;
}
Node* TemplateDoc(Runtime& rt) {
    if (!rt.templateDoc) {
        std::unique_ptr<Node> d(new Node());
        d->kind = Node::Doc;
        d->id = Doc(rt).count++;
        rt.templateDoc = d.get();
        Doc(rt).detached.push_back(std::move(d));
    }
    return rt.templateDoc;
}
NATIVE(Node_ownerDocument) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind == Node::Doc) return Value::Null_();
    if (InTemplateContent(rt, n)) return W(rt, TemplateDoc(rt));
    return W(rt, Doc(rt).root.get());
}
NATIVE(Node_isConnected) { UNUSED; return Value::B(Contains(Doc(rt).root.get(), N(rt, thisV))); }
NATIVE(Node_hasChildNodes) { UNUSED; return Value::B(!N(rt, thisV)->kids.empty()); }
NATIVE(Node_contains) { UNUSED; Node* o = NOpt(A(0)); return Value::B(o && ContainsLight(N(rt, thisV), o)); }
NATIVE(Node_getRootNode) {
    UNUSED;
    Node* n = N(rt, thisV);
    bool composed = A(0).IsObj() && rt.Truthy(rt.Get(A(0), L"composed"));
    for (int g = 0; g < 100000; g++) {
        if (n->parent) n = n->parent;
        else if (composed && n->kind == Node::Shadow && n->host) n = n->host;
        else break;
    }
    return W(rt, n);
}
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
    RemoveNodeCE(rt, Doc(rt), c);
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
    RemoveNodeCE(rt, Doc(rt), old);
    Dirty(rt);
    return A(1);
}
Node* Clone(Runtime& rt, const Node* n, bool deep) {
    bool txt = n->kind == Node::Text || n->kind == Node::Comment;
    Node* c = NewNode(Doc(rt), txt ? n->kind : Node::Elem, txt ? n->text : n->name);
    if (n->kind == Node::Elem) {
        c->name = n->name;
        c->attrs = n->attrs;
        SyncAttrCache(c);
        if (deep) for (auto& k : n->kids) AppendChild(Doc(rt), c, Clone(rt, k.get(), true));
        // <template> kopyalanirken .content da kopyalanir (spesifikasyon)
        if (deep && n->name == L"template") {
            auto it = rt.templateContent.find(const_cast<Node*>(n));
            if (it != rt.templateContent.end() && it->second) {
                Node* src = it->second;
                Node* f = NewNode(Doc(rt), Node::Elem, L"#fragment");
                rt.templateFragments[f] = true;
                for (auto& k : src->kids) AppendChild(Doc(rt), f, Clone(rt, k.get(), true));
                rt.templateContent[c] = f;
            }
        }
        // tanimli ozel ogenin kopyasi da yukseltilir
        if (c->name.find(L'-') != std::wstring::npos && CEDefFor(rt, c->name) >= 0 && !InTemplateContent(rt, n)) {
            CEReaction r;
            r.kind = 0;
            r.node = c;
            rt.ceQueue.push_back(r);
        }
    }
    return c;
}
NATIVE(Node_cloneNode) { UNUSED; return W(rt, Clone(rt, N(rt, thisV), rt.Truthy(A(0)))); }

// ---------------- Element ----------------

NATIVE(El_tagName) { UNUSED; return Node_nodeName(rt, self, thisV, args); }
NATIVE(El_localName) { UNUSED; return Value::S(N(rt, thisV)->name); }
NATIVE(El_id) { UNUSED; return Value::S(N(rt, thisV)->idAttr); }
NATIVE(El_setId) { UNUSED; SetAttr(rt, N(rt, thisV), L"id", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_className) { UNUSED; auto* c = N(rt, thisV)->Attr(L"class"); return Value::S(c ? *c : L""); }
NATIVE(El_setClassName) { UNUSED; SetAttr(rt, N(rt, thisV), L"class", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_getAttribute) { UNUSED; auto* v = N(rt, thisV)->Attr(ToLower(rt.ToStr(A(0))).c_str()); return v ? Value::S(*v) : Value::Null_(); }
NATIVE(El_setAttribute) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring name = ToLower(rt.ToStr(A(0)));
    if (name.empty() || name.find_first_of(L" \"'>/=") != std::wstring::npos) rt.Throw(L"Error", L"InvalidCharacterError: invalid attribute name");
    SetAttr(rt, n, name, rt.ToStr(A(1)));
    if (name == L"value") n->dirtyValue = false;
    Dirty(rt);
    if (name == L"src" && n->name == L"script" && Contains(Doc(rt).root.get(), n) && !n->Attr(L"data-merjs-ran")) RunInsertedScripts(rt, n);
    if (name == L"src" && (n->name == L"video" || n->name == L"audio")) OnMediaSrcChanged(rt, n, rt.ToStr(A(1)), thisV);
    return Value();
}
NATIVE(El_removeAttribute) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring name = ToLower(rt.ToStr(A(0)));
    RemoveAttr(rt, n, name);
    Dirty(rt);
    if (name == L"src" && (n->name == L"video" || n->name == L"audio")) MediaSrcChanged(rt, n);
    return Value();
}
NATIVE(El_hasAttribute) { UNUSED; return Value::B(N(rt, thisV)->Attr(ToLower(rt.ToStr(A(0))).c_str()) != nullptr); }
NATIVE(El_hasAttributes) { UNUSED; return Value::B(!N(rt, thisV)->attrs.empty()); }
NATIVE(El_toggleAttribute) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring k = ToLower(rt.ToStr(A(0)));
    bool has = n->Attr(k.c_str()) != nullptr;
    bool want = A(1).IsUndef() ? !has : rt.Truthy(A(1));
    if (want && !has) SetAttr(rt, n, k, L"");
    if (!want && has) RemoveAttr(rt, n, k);
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
NATIVE(El_innerHTML) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->name == L"template") {
        auto it = rt.templateContent.find(n);
        if (it != rt.templateContent.end() && n->kids.empty()) return Value::S(SerializeHTML(it->second, false));
    }
    return Value::S(SerializeHTML(n, false));
}
NATIVE(El_outerHTML) { UNUSED; return Value::S(SerializeHTML(N(rt, thisV), true)); }
NATIVE(El_setInnerHTML) {
    UNUSED;
    Node* n = N(rt, thisV);
    Document& d = Doc(rt);
    while (!n->kids.empty()) RemoveNodeCE(rt, d, n->kids.back().get());
    auto frag = ParseFragment(rt.ToStr(A(0)), d);
    std::vector<Node*> added;
    for (auto& f : frag) {
        f->parent = n;
        // innerHTML ile eklenen <script> calistirilmaz (tarayici davranisi)
        ForEachElem(f.get(), true, [](Node* e) { if (e->name == L"script") { e->attrs.emplace_back(L"data-merjs-ran", L"1"); } return true; });
        added.push_back(f.get());
        n->kids.push_back(std::move(f));
    }
    for (Node* a : added) { CEInserted(rt, a, true); SlotChangeFor(rt, a); MOInserted(rt, a); }
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
    for (auto& f : frag) {
        ForEachElem(f.get(), true, [](Node* e) { if (e->name == L"script") { e->attrs.emplace_back(L"data-merjs-ran", L"1"); } return true; });
        raw.push_back(f.get());
        d.detached.push_back(std::move(f));
    }
    for (Node* r : raw) InsertBefore(d, n->parent, r, n);
    for (Node* r : raw) { CEInserted(rt, r, true); MOInserted(rt, r); }
    RemoveNodeCE(rt, d, n);
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
    for (auto& f : frag) {
        ForEachElem(f.get(), true, [](Node* e) { if (e->name == L"script") { e->attrs.emplace_back(L"data-merjs-ran", L"1"); } return true; });
        raw.push_back(f.get());
        d.detached.push_back(std::move(f));
    }
    if (pos == L"beforebegin") { if (n->parent) for (Node* r : raw) InsertBefore(d, n->parent, r, n); }
    else if (pos == L"afterbegin") { Node* first = n->kids.empty() ? nullptr : n->kids.front().get(); for (Node* r : raw) InsertBefore(d, n, r, first); }
    else if (pos == L"beforeend") { for (Node* r : raw) AppendChild(d, n, r); }
    else if (pos == L"afterend") { if (n->parent) { Node* nx = Sibling(n, 1, false); for (Node* r : raw) InsertBefore(d, n->parent, r, nx); } }
    else rt.Throw(L"SyntaxError", L"Invalid position for insertAdjacentHTML");
    for (Node* r : raw) { CEInserted(rt, r, true); MOInserted(rt, r); }
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
NATIVE(El_remove) { UNUSED; Node* n = N(rt, thisV); if (n->parent) { RemoveNodeCE(rt, Doc(rt), n); Dirty(rt); } return Value(); }
NATIVE(El_replaceWith) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (!n->parent) return Value();
    Node* p = n->parent;
    for (auto& a : args) InsertNodes(rt, p, FragmentKids(ToNodeArg(rt, a)), n);
    RemoveNodeCE(rt, Doc(rt), n);
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
            if (ov == n->value) SetAttr(rt, e, L"selected", L""); else RemoveAttr(rt, e, L"selected");
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
    if (rt.Truthy(v)) SetAttr(rt, n, a, L""); else RemoveAttr(rt, n, a);
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
NATIVE(El_setHref) { UNUSED; SetAttr(rt, N(rt, thisV), L"href", rt.ToStr(A(0))); Dirty(rt); return Value(); }
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
    SetAttr(rt, n, L"src", rt.ToStr(A(0)));
    Dirty(rt);
    if (n->name == L"script" && Contains(Doc(rt).root.get(), n) && !n->Attr(L"data-merjs-ran")) RunInsertedScripts(rt, n);
    if (n->name == L"video" || n->name == L"audio") OnMediaSrcChanged(rt, n, rt.ToStr(A(0)), thisV);
    return Value();
}
NATIVE(El_title) { UNUSED; return Value::S(AttrOr(N(rt, thisV), L"title")); }
NATIVE(El_setTitle) { UNUSED; SetAttr(rt, N(rt, thisV), L"title", rt.ToStr(A(0))); return Value(); }
NATIVE(El_type) { UNUSED; Node* n = N(rt, thisV); std::wstring t = ToLower(AttrOr(n, L"type", n->name == L"input" ? L"text" : n->name == L"button" ? L"submit" : L"")); return Value::S(t); }
NATIVE(El_setType) { UNUSED; SetAttr(rt, N(rt, thisV), L"type", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_name) { UNUSED; return Value::S(AttrOr(N(rt, thisV), L"name")); }
NATIVE(El_setName) { UNUSED; SetAttr(rt, N(rt, thisV), L"name", rt.ToStr(A(0))); return Value(); }
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

NATIVE(Audio_ctor) {
    UNUSED;
    Node* n = NewNode(Doc(rt), Node::Elem, L"audio");
    if (!args.empty() && !A(0).IsUndef()) {
        SetAttr(rt, n, L"src", rt.ToStr(A(0)));
    }
    return W(rt, n);
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

        static constexpr size_t kMaxBlobBytes = 64u * 1024 * 1024;
        auto appendBytes = [&](const uint8_t* src, size_t len) {
            if (!src || len == 0) return;
            size_t oldSz = blob->x->bufferData->size();
            if (len > kMaxBlobBytes || oldSz > kMaxBlobBytes - len) {
                rt.Throw(L"Error", L"QuotaExceededError: Blob size limit exceeded");
            }
            blob->x->bufferData->resize(oldSz + len);
            memcpy(blob->x->bufferData->data() + oldSz, src, len);
        };
        for (auto& item : list) {
            if (item.IsStr()) {
                std::string u8 = ToUtf8(*item.s);
                appendBytes(reinterpret_cast<const uint8_t*>(u8.data()), u8.size());
            } else if (item.IsObj() && item.o->cls == Cls::ArrayBuffer && item.o->x && item.o->x->bufferData) {
                size_t bufSz = item.o->x->bufferData->size();
                size_t sz = std::min(item.o->x->byteLength, bufSz);
                appendBytes(item.o->x->bufferData->data(), sz);
            } else if (item.IsObj() && (item.o->cls == Cls::TypedArray || item.o->cls == Cls::DataView) && item.o->x && item.o->x->bufferData) {
                size_t bufSz = item.o->x->bufferData->size();
                size_t off = item.o->x->byteOffset;
                if (off <= bufSz) {
                    size_t sz = std::min(item.o->x->byteLength, bufSz - off);
                    appendBytes(item.o->x->bufferData->data() + off, sz);
                }
            } else if (item.IsObj() && rt.Get(item, L"\x02isBlob").b && item.o->x && item.o->x->bufferData) {
                auto& otherBuf = *item.o->x->bufferData;
                appendBytes(otherBuf.data(), otherBuf.size());
            } else {
                std::string u8 = ToUtf8(rt.ToStr(item));
                appendBytes(reinterpret_cast<const uint8_t*>(u8.data()), u8.size());
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
    (void)srcVal; (void)videoElemVal;
    if (n && (n->name == L"video" || n->name == L"audio")) MediaSrcChanged(rt, n);
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
void WriteDecls(Runtime& rt, Node* n, const std::vector<Decl>& d) {
    std::wstring o;
    for (auto& x : d) {
        if (!o.empty()) o += L" ";
        o += x.prop + L": " + x.value + (x.important ? L" !important" : L"") + L";";
    }
    if (o.empty()) RemoveAttr(rt, n, L"style"); else SetAttr(rt, n, L"style", o);
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
    WriteDecls(rt, n, d);
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
    SetAttr(rt, n, L"class", s);
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
        if (k == L"cssText") { SetAttr(rt, n, L"style", rt.ToStr(v)); Dirty(rt); return true; }
        StyleSet(rt, n, Kebab(k), del || v.IsNullish() ? L"" : rt.ToStr(v), false);
        return true;
    }
    if (o->cls == Cls::Dataset) {
        Node* n = o->x->node;
        if (!n) return false;
        if (del) RemoveAttr(rt, n, L"data-" + Kebab(k));
        else SetAttr(rt, n, L"data-" + Kebab(k), rt.ToStr(v));
        Dirty(rt);
        return true;
    }
    if (o->cls == Cls::Storage) {
        if (rt.HasProperty(rt.storageProto, k)) return false;
        auto& m = Store(rt, o);
        if (del) {
            m.erase(k);
        } else {
            std::wstring val = rt.ToStr(v);
            size_t total = 0;
            for (auto& e : m) if (e.first != k) total += e.first.size() + e.second.size();
            if (total + k.size() + val.size() > 5u * 1024 * 1024) rt.Throw(L"Error", L"QuotaExceededError: Storage quota exceeded");
            m[k] = std::move(val);
        }
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
    bool bubbles = false, cancelable = false, composed = false;
    Value detail = Value::Null_();
    if (A(1).IsObj()) {
        bubbles = rt.Truthy(rt.Get(A(1), L"bubbles"));
        cancelable = rt.Truthy(rt.Get(A(1), L"cancelable"));
        composed = rt.Truthy(rt.Get(A(1), L"composed"));
        if (rt.HasProperty(A(1).o, L"detail")) detail = rt.Get(A(1), L"detail");
        for (const wchar_t* k : {L"key", L"code", L"keyCode", L"clientX", L"clientY", L"button", L"ctrlKey", L"shiftKey", L"altKey", L"metaKey"})
            if (rt.HasProperty(A(1).o, k)) rt.DefineData(e, k, rt.Get(A(1), k));
    }
    rt.DefineData(e, L"bubbles", Value::B(bubbles));
    rt.DefineData(e, L"cancelable", Value::B(cancelable));
    rt.DefineData(e, L"composed", Value::B(composed));
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
NATIVE(Ev_composedPath) {
    UNUSED;
    Value p = thisV.IsObj() ? rt.Get(thisV, L"\x02path") : Value();
    if (p.IsObj() && p.o->cls == Cls::Array) return Value::O(rt.NewArrayFrom(p.o->elems));
    return Value::O(rt.NewArray(0));
}
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
    // on<olay> ozelligi (window disindaki duz EventTarget'lar: AbortSignal vb.)
    if (t != rt.global) {
        Value h = rt.Get(Value::O(t), L"on" + type);
        if (IsCallable(h)) {
            std::vector<Value> a{ev};
            try { rt.Call(h, Value::O(t), a); } catch (JSThrow& e) { rt.Report(L"olay: " + type, e); }
        }
    }
    if (t->x) {
        auto ls = t->x->listeners;
        for (auto& l : ls) {
            if (l.type != type) continue;
            if (l.once) {
                auto& real = t->x->listeners;
                for (size_t i = 0; i < real.size(); i++) if (real[i].type == l.type && rt.StrictEq(real[i].fn, l.fn)) { real.erase(real.begin() + long(i)); break; }
            }
            std::vector<Value> a{ev};
            try {
                if (IsCallable(l.fn)) rt.Call(l.fn, Value::O(t), a);
                else if (l.fn.IsObj()) rt.Call(rt.Get(l.fn, L"handleEvent"), l.fn, a);
            } catch (JSThrow& e) {
                rt.Report(L"olay: " + type, e);   // dinleyici hatasi digerlerini durdurmaz
            }
            if (A(0).o->x->stopProp && false) break;
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
Url PageUrlParts(Runtime& rt);

static bool IsSecureCookieContext(Runtime& rt) {
    Url u = PageUrlParts(rt);
    return u.IsSecure() || u.scheme == L"about" || u.host == L"localhost" || u.host == L"127.0.0.1" || u.host == L"[::1]";
}

NATIVE(Doc_cookie) {
    UNUSED;
    double now = rt.Now();
    bool secureCtx = IsSecureCookieContext(rt);
    std::wstring result;
    for (auto it = rt.cookies.begin(); it != rt.cookies.end(); ) {
        if (it->second.expires >= 0 && it->second.expires <= now) {
            it = rt.cookies.erase(it);
        } else {
            if (!it->second.httpOnly && (!it->second.secure || secureCtx)) {
                if (!result.empty()) result += L"; ";
                result += it->first + L"=" + it->second.value;
            }
            ++it;
        }
    }
    return Value::S(result);
}

static void SetCookieInternal(Runtime& rt, const std::wstring& rawStr, bool fromHttp) {
    std::wstring s = rawStr;
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty() || s.size() > 4096) return;
    for (wchar_t c : s) {
        if ((c < 0x20 && c != L'\t') || c == 0x7F) return; // CRLF / kontrol karakteri engeli
    }

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
    if (parts.empty()) return;

    std::wstring pair = parts[0];
    while (!pair.empty() && iswspace(pair.front())) pair.erase(pair.begin());
    while (!pair.empty() && iswspace(pair.back())) pair.pop_back();

    size_t eq = pair.find(L'=');
    if (eq == std::wstring::npos) return;
    std::wstring name = pair.substr(0, eq);
    std::wstring val = pair.substr(eq + 1);
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!val.empty() && iswspace(val.front())) val.erase(val.begin());
    while (!val.empty() && iswspace(val.back())) val.pop_back();

    if (name.empty()) return;

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

    bool secureCtx = IsSecureCookieContext(rt);
    if (!fromHttp && ck.httpOnly) return; // JS HttpOnly cerez olusturamaz
    if (!secureCtx && ck.secure) return;  // Guvensiz baglam Secure cerez olusturamaz
    auto existing = rt.cookies.find(name);
    if (existing != rt.cookies.end()) {
        if (!fromHttp && existing->second.httpOnly) return; // JS HttpOnly cerezi ezemez/silemez
        if (!secureCtx && existing->second.secure) return;  // Guvensiz baglam Secure cerezi ezemez
    }

    if (ck.expires >= 0 && ck.expires <= now) {
        rt.cookies.erase(name);
    } else if (existing != rt.cookies.end() || rt.cookies.size() < 256) {
        rt.cookies[name] = std::move(ck);
    }
}

NATIVE(Doc_setCookie) {
    UNUSED;
    SetCookieInternal(rt, rt.ToStr(A(0)), false);
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
    Node* e = NewNode(Doc(rt), Node::Elem, t);
    bool inert = rt.templateDoc && NOpt(thisV) == rt.templateDoc;
    if (t.find(L'-') != std::wstring::npos && !inert) {
        int di = CEDefFor(rt, t);
        if (di >= 0) CEUpgrade(rt, e, di);
    }
    return W(rt, e);
}
NATIVE(Doc_createElementNS) { UNUSED; std::vector<Value> a{A(1)}; return Doc_createElement(rt, self, thisV, a); }
NATIVE(Doc_createTextNode) { UNUSED; return W(rt, NewNode(Doc(rt), Node::Text, rt.ToStr(A(0)))); }
NATIVE(Doc_createComment) { UNUSED; return W(rt, NewNode(Doc(rt), Node::Comment, A(0).IsUndef() ? L"" : rt.ToStr(A(0)))); }
NATIVE(Doc_importNode) {
    UNUSED;
    Node* n = NOpt(A(0));
    if (!n) rt.Throw(L"TypeError", L"importNode: parameter 1 is not of type 'Node'");
    Node* c = Clone(rt, n, rt.Truthy(A(1)));
    // ana belgeye alinan kopyadaki tanimli ozel ogeler yukseltilir
    if (!(rt.templateDoc && NOpt(thisV) == rt.templateDoc)) CEInserted(rt, c, true);
    return W(rt, c);
}
NATIVE(Doc_adoptNode) { UNUSED; Node* n = NOpt(A(0)); if (n && n->parent) RemoveNodeCE(rt, Doc(rt), n); return A(0); }

// ---- TreeWalker / NodeIterator (onceden sirali gezinme) ----
uint32_t NodeShowBit(const Node* n) {
    switch (n->kind) {
    case Node::Elem: return n->name == L"#fragment" ? 1u << 10 : 1u;
    case Node::Text: return 1u << 2;
    case Node::Comment: return 1u << 7;
    case Node::Doc: return 1u << 8;
    case Node::Shadow: return 1u << 10;
    }
    return 0;
}
// 1 kabul, 2 reddet, 3 atla
int WalkerAccept(Runtime& rt, const Value& self, Node* n) {
    uint32_t show = uint32_t(rt.ToNum(rt.Get(self, L"whatToShow")));
    if (!(show & NodeShowBit(n))) return 3;
    Value f = rt.Get(self, L"filter");
    if (f.IsNullish()) return 1;
    std::vector<Value> a{W(rt, n)};
    Value r = IsCallable(f) ? rt.Call(f, Value(), a) : rt.Call(rt.Get(f, L"acceptNode"), f, a);
    int v = int(rt.ToNum(r));
    return v == 2 || v == 3 ? v : 1;
}
Node* PreorderNext(Node* cur, Node* root, bool skipKids) {
    if (!skipKids && !cur->kids.empty()) return cur->kids.front().get();
    for (Node* c = cur; c && c != root; c = c->parent) {
        if (Node* s = Sibling(c, 1, false)) return s;
    }
    return nullptr;
}
NATIVE(TW_nextNode) {
    UNUSED;
    Node* root = NOpt(rt.Get(thisV, L"root"));
    Node* cur = NOpt(rt.Get(thisV, L"\x02iter").IsUndef() ? rt.Get(thisV, L"currentNode") : rt.Get(thisV, L"\x02ref"));
    if (!root || !cur) return Value::Null_();
    bool iter = !rt.Get(thisV, L"\x02iter").IsUndef();
    bool fresh = iter && rt.Truthy(rt.Get(thisV, L"\x02fresh"));
    Node* n = cur;
    bool skip = false;
    for (int g = 0; g < 10000000; g++) {
        if (fresh) { fresh = false; rt.Put(thisV, L"\x02fresh", Value::B(false)); }
        else {
            n = PreorderNext(n, root, skip);
            if (!n) return Value::Null_();
        }
        int a = WalkerAccept(rt, thisV, n);
        skip = (a == 2 && !iter);
        if (a == 1) {
            rt.Put(thisV, iter ? L"\x02ref" : L"currentNode", W(rt, n));
            if (iter) rt.Put(thisV, L"referenceNode", W(rt, n));
            return W(rt, n);
        }
    }
    return Value::Null_();
}
NATIVE(TW_parentNode) {
    UNUSED;
    Node* root = NOpt(rt.Get(thisV, L"root"));
    Node* n = NOpt(rt.Get(thisV, L"currentNode"));
    while (n && n != root && (n = n->parent)) {
        if (WalkerAccept(rt, thisV, n) == 1) { rt.Put(thisV, L"currentNode", W(rt, n)); return W(rt, n); }
    }
    return Value::Null_();
}
NATIVE(TW_firstChild) {
    UNUSED;
    Node* n = NOpt(rt.Get(thisV, L"currentNode"));
    if (!n) return Value::Null_();
    for (auto& k : n->kids) if (WalkerAccept(rt, thisV, k.get()) == 1) { rt.Put(thisV, L"currentNode", W(rt, k.get())); return W(rt, k.get()); }
    return Value::Null_();
}
NATIVE(TW_nextSibling) {
    UNUSED;
    Node* n = NOpt(rt.Get(thisV, L"currentNode"));
    for (Node* s = n ? Sibling(n, 1, false) : nullptr; s; s = Sibling(s, 1, false))
        if (WalkerAccept(rt, thisV, s) == 1) { rt.Put(thisV, L"currentNode", W(rt, s)); return W(rt, s); }
    return Value::Null_();
}
Value MakeWalker(Runtime& rt, std::vector<Value>& args, bool iterator) {
    Node* root = NOpt(A(0));
    if (!root) rt.Throw(L"TypeError", L"parameter 1 is not of type 'Node'");
    Obj* w = rt.NewObject();
    rt.DefineData(w, L"root", A(0));
    rt.DefineData(w, L"whatToShow", Value::N(A(1).IsUndef() ? 4294967295.0 : double(uint32_t(rt.ToNum(A(1))))));
    rt.DefineData(w, L"filter", A(2).IsUndef() ? Value::Null_() : A(2));
    rt.DefineData(w, L"currentNode", A(0));
    rt.DefineMethod(w, L"nextNode", TW_nextNode, 0);
    if (iterator) {
        rt.DefineData(w, L"\x02iter", Value::B(true), false);
        rt.DefineData(w, L"\x02ref", A(0), false);
        rt.DefineData(w, L"\x02fresh", Value::B(true), false);
        rt.DefineData(w, L"referenceNode", A(0));
        rt.DefineMethod(w, L"detach", [](Runtime&, Obj*, const Value&, std::vector<Value>&) { return Value(); }, 0);
    } else {
        rt.DefineMethod(w, L"parentNode", TW_parentNode, 0);
        rt.DefineMethod(w, L"firstChild", TW_firstChild, 0);
        rt.DefineMethod(w, L"nextSibling", TW_nextSibling, 0);
    }
    return Value::O(w);
}
NATIVE(Doc_createTreeWalker) { UNUSED; return MakeWalker(rt, args, false); }
NATIVE(Doc_createNodeIterator) { UNUSED; return MakeWalker(rt, args, true); }
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
    rt.DefineData(s, L"\x02computed:font-size", Value::S(L"16px"), false);
    rt.DefineData(s, L"\x02computed:line-height", Value::S(L"20px"), false);
    rt.DefineData(s, L"\x02computed:color", Value::S(L"rgb(0, 0, 0)"), false);
    rt.DefineData(s, L"\x02computed:background-color", Value::S(L"rgba(0, 0, 0, 0)"), false);
    rt.DefineData(s, L"\x02computed:box-sizing", Value::S(L"border-box"), false);
    rt.DefineData(s, L"\x02computed:overflow", Value::S(L"visible"), false);
    if (n) {
        double rx = 0, ry = 0, rw = 0, rh = 0;
        RectOf(rt, n, rx, ry, rw, rh);
        rt.DefineData(s, L"\x02computed:width", Value::S(std::to_wstring(int(std::round(rw))) + L"px"), false);
        rt.DefineData(s, L"\x02computed:height", Value::S(std::to_wstring(int(std::round(rh))) + L"px"), false);
    }
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
    rt.DefineData(entry, L"intersectionRect", Value::O(MakeRect(rt, x, y, w, h)));
    double vw = 1280, vh = 800;
    if (rt.host) rt.host->ViewportSize(vw, vh);
    rt.DefineData(entry, L"rootBounds", Value::O(MakeRect(rt, 0, 0, vw, vh)));
    rt.DefineData(entry, L"time", Value::N(rt.Now()));
    Timer t;
    t.id = rt.nextTimerId++;
    t.due = rt.Now() + 1;
    t.interval = -1;
    t.fn = cb;
    t.args = {Value::O(rt.NewArrayFrom({Value::O(entry)})), thisV};
    if (rt.timers.size() < 10000) rt.timers.push_back(t);
    return Value();
}
// ResizeObserver: tarayicilar observe() sonrasi ilk boyutu bir kez bildirir; ayni seyi yapariz
NATIVE(RO_observe) {
    UNUSED;
    Value cb = rt.Get(thisV, L"\x02cb");
    if (!IsCallable(cb) || !NOpt(A(0))) return Value();
    double x, y, w, h;
    RectOf(rt, NOpt(A(0)), x, y, w, h);
    Obj* entry = rt.NewObject();
    rt.DefineData(entry, L"target", A(0));
    rt.DefineData(entry, L"contentRect", Value::O(MakeRect(rt, 0, 0, w, h)));
    for (const wchar_t* k : {L"contentBoxSize", L"borderBoxSize", L"devicePixelContentBoxSize"}) {
        Obj* sz = rt.NewObject();
        rt.DefineData(sz, L"inlineSize", Value::N(w));
        rt.DefineData(sz, L"blockSize", Value::N(h));
        rt.DefineData(entry, k, Value::O(rt.NewArrayFrom({Value::O(sz)})));
    }
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
    std::wstring clean;
    clean.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\0') continue;
        if ((c < 0x20 && c != L'\t') || c == 0x7F) continue;
        clean.push_back(c);
    }
    size_t i = 0, j = clean.size();
    while (i < j && iswspace(clean[i])) i++;
    while (j > i && iswspace(clean[j - 1])) j--;
    return clean.substr(i, j - i);
}

static bool IsForbiddenJsRequestHeader(const std::wstring& lowerName) {
    if (lowerName.empty()) return true;
    for (wchar_t c : lowerName) {
        if (c <= 0x20 || c >= 0x7F || c == L':' || c == L';') return true;
    }
    if (StartsWith(lowerName, L"sec-") || StartsWith(lowerName, L"proxy-")) return true;
    return lowerName == L"accept-charset" || lowerName == L"accept-encoding" ||
           lowerName == L"access-control-request-headers" || lowerName == L"access-control-request-method" ||
           lowerName == L"connection" || lowerName == L"content-length" ||
           lowerName == L"cookie" || lowerName == L"cookie2" ||
           lowerName == L"date" || lowerName == L"dnt" || lowerName == L"expect" ||
           lowerName == L"host" || lowerName == L"keep-alive" || lowerName == L"origin" ||
           lowerName == L"referer" || lowerName == L"te" || lowerName == L"trailer" ||
           lowerName == L"transfer-encoding" || lowerName == L"upgrade" ||
           lowerName == L"user-agent" || lowerName == L"via";
}

static bool IsForbiddenResponseHeaderName(const std::wstring& lowerName) {
    return lowerName == L"set-cookie" || lowerName == L"set-cookie2";
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
            std::wstring k = TrimHeader(line.substr(0, colon));
            if (IsForbiddenResponseHeaderName(ToLower(k))) continue; // Set-Cookie JS'den gizlenir
            std::wstring v = TrimHeader(line.substr(colon + 1));
            std::vector<Value> args{Value::S(k), Value::S(v)};
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
                SetCookieInternal(rt, TrimHeader(line.substr(colon + 1)), true);
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

// ---------------- Ag guvenligi (fetch / XHR) ----------------
// Ayni koken mi? Capraz koken yanitlari yalnizca CORS izniyle sayfaya verilir.
static bool NetSameOrigin(Runtime& rt, const std::wstring& url) {
    if (!rt.host) return false;
    Url a = ParseUrl(url), b = ParseUrl(rt.host->PageUrl());
    return IsSameOrigin(a, b);
}
static std::wstring NetPageOrigin(Runtime& rt) {
    if (!rt.host) return L"null";
    Url b = ParseUrl(rt.host->PageUrl());
    if (!b.ok || (b.scheme != L"http" && b.scheme != L"https")) return L"null";
    return b.Origin();
}
static bool NetRequestAllowed(Runtime& rt, const std::wstring& url) {
    if (!rt.host || !rt.host->CheckRequest(url)) return false;
    Url dst = ParseUrl(url);
    if (!dst.ok || (dst.scheme != L"http" && dst.scheme != L"https")) return false;
    Url page = ParseUrl(rt.host->PageUrl());
    if (page.ok && page.scheme == L"https" && dst.scheme == L"http" && dst.host != L"localhost" && dst.host != L"127.0.0.1" && dst.host != L"[::1]") return false;
    bool pageLocal = !page.ok || page.scheme == L"file" || page.scheme == L"about" || IsPrivateOrLocalHost(page.host);
    if (!pageLocal && IsPrivateOrLocalHost(dst.host)) return false; // SSRF / PNA engeli
    return true;
}
static bool NetCorsAllowed(const std::wstring& rawHeaders, const std::wstring& origin) {
    if (origin.empty()) return false;
    std::wstring low = ToLower(rawHeaders);
    size_t p = low.find(L"\naccess-control-allow-origin:");
    if (p == std::wstring::npos) return false;
    size_t st = p + 30, e = low.find(L'\r', st);
    std::wstring v = Trim(rawHeaders.substr(st, e == std::wstring::npos ? std::wstring::npos : e - st));
    return v == L"*" || (origin != L"null" && ToLower(v) == ToLower(origin));
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
    if (!NetRequestAllowed(rt, url)) {
        rt.RejectPromise(promise, Value::O(rt.NewError(L"TypeError", L"Failed to fetch: blocked by merOS Shield (" + url + L")")));
        return Value::O(promise);
    }
    const bool sameOrigin = NetSameOrigin(rt, url);

    FetchOptions opt;
    opt.method = L"GET";
    if (rt.host) opt.referer = ReferrerFor(rt.host->PageUrl(), url);
    if (!sameOrigin) opt.extraHeaders += L"Origin: " + NetPageOrigin(rt) + L"\r\n";

    auto appendReqHeader = [&](const std::wstring& rawK, const std::wstring& rawV) {
        std::wstring hk = TrimHeader(rawK);
        std::wstring hv = TrimHeader(rawV);
        std::wstring lk = ToLower(hk);
        if (IsForbiddenJsRequestHeader(lk)) return;
        if (lk == L"content-type") opt.contentType = hv;
        opt.extraHeaders += hk + L": " + hv + L"\r\n";
    };

    if (init.IsObj()) {
        Value m = rt.Get(init, L"method");
        if (m.IsStr() && !m.s->empty()) opt.method = ToUpper(TrimHeader(*m.s));

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
                        appendReqHeader(rt.ToStr(item.o->elems[0]), rt.ToStr(item.o->elems[1]));
                    }
                }
            } else if (!h.o->elems.empty() && h.o->elems[0].IsObj() && h.o->elems[0].o->elems.size() >= 3) {
                for (auto& item : h.o->elems) {
                    if (item.IsObj() && item.o->elems.size() >= 3) {
                        appendReqHeader(rt.ToStr(item.o->elems[1]), rt.ToStr(item.o->elems[2]));
                    }
                }
            } else {
                for (auto& k : rt.OwnKeys(h.o, true)) {
                    appendReqHeader(k, rt.ToStr(rt.Get(h, k)));
                }
            }
        }
    }

    if (sameOrigin && !rt.cookies.empty()) {
        std::wstring ckHeader;
        double now = rt.Now();
        bool dstSecure = ParseUrl(url).IsSecure();
        for (auto& kv : rt.cookies) {
            if ((kv.second.expires < 0 || kv.second.expires > now) && (!kv.second.secure || dstSecure)) {
                if (!ckHeader.empty()) ckHeader += L"; ";
                ckHeader += kv.first + L"=" + kv.second.value;
            }
        }
        if (!ckHeader.empty()) opt.extraHeaders += L"Cookie: " + ckHeader + L"\r\n";
    }

    FetchResult res = Fetch(url, 24 * 1024 * 1024, nullptr, 0, &opt);
    if (!res.ok) {
        rt.RejectPromise(promise, Value::O(rt.NewError(L"TypeError", L"Failed to fetch: " + NetErrorText(res.error))));
    } else if (!sameOrigin && !NetCorsAllowed(res.rawHeaders, NetPageOrigin(rt))) {
        rt.RejectPromise(promise, Value::O(rt.NewError(L"TypeError", L"Failed to fetch: CORS policy (no Access-Control-Allow-Origin) " + url)));
    } else {
        if (sameOrigin) ProcessResponseCookies(rt, res.rawHeaders);
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
    if (IsForbiddenJsRequestHeader(ToLower(k))) return Value(); // yasakli basliklari sessizce yoksay (WHATWG XHR spec)
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
    const bool policyOk = NetRequestAllowed(rt, url);
    const bool sameOrigin = NetSameOrigin(rt, url);

    FetchOptions opt;
    opt.method = rt.ToStr(rt.Get(thisV, L"\x02method"));
    if (rt.host) opt.referer = ReferrerFor(rt.host->PageUrl(), url);
    if (!sameOrigin) opt.extraHeaders += L"Origin: " + NetPageOrigin(rt) + L"\r\n";
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
                std::wstring hk = TrimHeader(rt.ToStr(item.o->elems[0]));
                std::wstring hv = TrimHeader(rt.ToStr(item.o->elems[1]));
                std::wstring lk = ToLower(hk);
                if (IsForbiddenJsRequestHeader(lk)) continue;
                if (lk == L"content-type") opt.contentType = hv;
                opt.extraHeaders += hk + L": " + hv + L"\r\n";
            }
        }
    }

    if (sameOrigin && !rt.cookies.empty()) {
        std::wstring ckHeader;
        double now = rt.Now();
        bool dstSecure = ParseUrl(url).IsSecure();
        for (auto& kv : rt.cookies) {
            if ((kv.second.expires < 0 || kv.second.expires > now) && (!kv.second.secure || dstSecure)) {
                if (!ckHeader.empty()) ckHeader += L"; ";
                ckHeader += kv.first + L"=" + kv.second.value;
            }
        }
        if (!ckHeader.empty()) opt.extraHeaders += L"Cookie: " + ckHeader + L"\r\n";
    }

    FetchResult res;
    if (policyOk) res = Fetch(url, 24 * 1024 * 1024, nullptr, 0, &opt);
    if (res.ok && !sameOrigin && !NetCorsAllowed(res.rawHeaders, NetPageOrigin(rt))) res.ok = false; // CORS
    if (!res.ok) {
        rt.Put(thisV, L"readyState", Value::N(4));
        rt.Put(thisV, L"status", Value::N(0));
        rt.Put(thisV, L"statusText", Value::S(L""));
        FireXHREvent(rt, xhr, L"readystatechange");
        FireXHREvent(rt, xhr, L"error");
        FireXHREvent(rt, xhr, L"loadend");
    } else {
        if (sameOrigin) ProcessResponseCookies(rt, res.rawHeaders);
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
    if (!v.IsStr()) return Value::S(L"");
    const std::wstring& raw = *v.s;
    std::wstring filtered;
    size_t st = 0;
    while (st < raw.size()) {
        size_t e = raw.find(L"\r\n", st);
        if (e == std::wstring::npos) e = raw.find(L'\n', st);
        std::wstring line = (e == std::wstring::npos) ? raw.substr(st) : raw.substr(st, e - st);
        st = (e == std::wstring::npos) ? raw.size() : (raw[e] == L'\r' ? e + 2 : e + 1);
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos) {
            std::wstring k = ToLower(TrimHeader(line.substr(0, colon)));
            if (IsForbiddenResponseHeaderName(k)) continue;
            filtered += line + L"\r\n";
        }
    }
    return Value::S(filtered);
}

NATIVE(XHR_getResponseHeader) {
    UNUSED;
    std::wstring name = ToLower(TrimHeader(rt.ToStr(A(0))));
    if (IsForbiddenResponseHeaderName(name)) return Value::Null_();
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
    if (!A(0).IsUndef()) SetAttr(rt, n, L"width", rt.ToStr(A(0)));
    if (!A(1).IsUndef()) SetAttr(rt, n, L"height", rt.ToStr(A(1)));
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
NATIVE(Nav_sendBeacon) { UNUSED; return Value::B(false); }

static void FillSecureRandomBytes(uint8_t* dst, size_t len) {
    if (!dst || len == 0) return;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return;
    }
#endif
    std::random_device rd;
    for (size_t i = 0; i < len; i++) {
        dst[i] = static_cast<uint8_t>(rd() & 0xFF);
    }
}

NATIVE(Crypto_randomUUID) {
    UNUSED;
    uint64_t r1 = 0, r2 = 0;
    uint8_t bytes[16];
    FillSecureRandomBytes(bytes, sizeof(bytes));
    std::memcpy(&r1, bytes, 8);
    std::memcpy(&r2, bytes + 8, 8);
    r1 = (r1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    r2 = (r2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    wchar_t buf[64];
    swprintf_s(buf, L"%08x-%04x-%04x-%02x%02x-%012llx",
               uint32_t(r1 >> 32),
               uint16_t(r1 >> 16),
               uint16_t(r1),
               uint8_t(r2 >> 56),
               uint8_t(r2 >> 48),
               r2 & 0xFFFFFFFFFFFFULL);
    return Value::S(std::wstring(buf));
}

NATIVE(Crypto_getRandomValues) {
    UNUSED;
    Value arr = A(0);
    if (!arr.IsObj() || arr.o->cls != Cls::TypedArray || !arr.o->x || !arr.o->x->bufferData) {
        rt.Throw(L"TypeMismatchError", L"getRandomValues requires a TypedArray");
    }
    size_t byteLen = arr.o->x->byteLength;
    size_t byteOff = arr.o->x->byteOffset;
    size_t bufSz = arr.o->x->bufferData->size();
    if (byteLen > 65536) rt.Throw(L"QuotaExceededError", L"The requested length exceeds 65,536 bytes");
    if (byteOff > bufSz || byteLen > bufSz - byteOff) {
        rt.Throw(L"RangeError", L"TypedArray view out of bounds");
    }
    if (byteLen > 0) {
        FillSecureRandomBytes(arr.o->x->bufferData->data() + byteOff, byteLen);
    }
    return arr;
}

NATIVE(Clip_writeText) {
    UNUSED;
    std::wstring text = rt.ToStr(A(0));
    if (text.size() > 1024 * 1024) text.resize(1024 * 1024);
    rt.DefineData(rt.global, L"\x02clip", Value::S(std::move(text)), false);
    Obj* p = rt.NewPromise();
    rt.ResolvePromise(p, Value());
    return Value::O(p);
}

NATIVE(Clip_readText) {
    UNUSED;
    Value v = rt.Get(Value::O(rt.global), L"\x02clip");
    std::wstring text = v.IsStr() ? *v.s : L"";
    Obj* p = rt.NewPromise();
    rt.ResolvePromise(p, Value::S(std::move(text)));
    return Value::O(p);
}

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
    // composed olaylar (kullanici olaylari ve composed:true) shadow root'tan host'a gecer;
    // shadow disindaki dinleyiciler hedefi host olarak gorur (retargeting).
    bool composed = rt.Truthy(rt.Get(Value::O(ev), L"composed")) || rt.Truthy(rt.Get(Value::O(ev), L"isTrusted"));
    std::vector<Node*> path, targets;
    Node* tgt = target;
    for (Node* p = target; p && path.size() < 1000;) {
        path.push_back(p);
        targets.push_back(tgt);
        if (p->parent) p = p->parent;
        else if (p->kind == Node::Shadow && p->host && composed) { p = p->host; tgt = p; }
        else p = nullptr;
    }
    {
        std::vector<Value> pv;
        for (Node* x : path) if (x->kind != Node::Shadow || x->shadowMode == 1) pv.push_back(W(rt, x));
        if (bubbles || path.empty() || path.back()->kind == Node::Doc) pv.push_back(Value::O(rt.global));
        rt.DefineData(ev, L"\x02path", Value::O(rt.NewArrayFrom(pv)), false);
    }
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
        if (i == 0 || targets[i] != targets[i - 1]) rt.Put(Value::O(ev), L"target", W(rt, targets[i]));
        rt.Put(Value::O(ev), L"eventPhase", Value::N(i == 0 ? 2 : 3));
        if (!fire(w, Value::O(w))) return !ev->x->defaultPrevented;
        if (!bubbles) return !ev->x->defaultPrevented;
    }
    if (bubbles || path.empty() || path.back()->kind == Node::Doc) fire(rt.global, Value::O(rt.global));
    return !ev->x->defaultPrevented;
}

// ======================================================================
//  Custom Elements + Shadow DOM
// ======================================================================

bool IsConnectedNode(Runtime& rt, const Node* n) { return rt.doc && Contains(rt.doc->root.get(), n); }

// Shadow dahil agac sirasi: oge, kendi shadow agaci, sonra light cocuklari
template <class F>
void ForEachComposed(Node* root, F f) {
    std::vector<Node*> st{root};
    size_t guard = 0;
    while (!st.empty() && guard++ < size_t(kMaxNodes)) {
        Node* c = st.back();
        st.pop_back();
        if (c->kind == Node::Elem) f(c);
        for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
        if (c->shadow) st.push_back(c->shadow.get());
    }
}

int CEDefFor(Runtime& rt, const std::wstring& name) {
    auto it = rt.ceByName.find(name);
    if (it == rt.ceByName.end() || rt.ceDefs[size_t(it->second)].extends) return -1;
    return it->second;
}

bool CEObserves(Runtime& rt, Node* n, const std::wstring& name) {
    if (n->ceState != 1 || n->ceDef < 0 || size_t(n->ceDef) >= rt.ceDefs.size()) return false;
    const CEDef& d = rt.ceDefs[size_t(n->ceDef)];
    if (!IsCallable(d.attrChanged)) return false;
    for (auto& o : d.observed) if (o == name) return true;
    return false;
}

void CEAttr(Runtime& rt, Node* n, const std::wstring& name, const Value& oldV, const Value& newV) {
    if (!CEObserves(rt, n, name)) return;
    CEReaction r;
    r.kind = 3;
    r.node = n;
    r.name = name;
    r.oldV = oldV;
    r.newV = newV;
    rt.ceQueue.push_back(r);
}

void CEEnqueue(Runtime& rt, uint8_t kind, Node* n) {
    if (rt.ceQueue.size() > 2000000) throw JSAbort{L"özel öge tepki kuyruğu taştı"};
    CEReaction r;
    r.kind = kind;
    r.node = n;
    rt.ceQueue.push_back(r);
}

// Geri cagirmayi calistir (hatalar raporlanir, yayilmaz)
void CEInvoke(Runtime& rt, Node* e, uint8_t kind, const std::wstring& name, const Value& oldV, const Value& newV) {
    if (e->ceState != 1 || e->ceDef < 0 || size_t(e->ceDef) >= rt.ceDefs.size()) return;
    const CEDef& d = rt.ceDefs[size_t(e->ceDef)];
    Value fn = kind == 1 ? d.connected : kind == 2 ? d.disconnected : d.attrChanged;
    if (!IsCallable(fn)) return;
    if (kind == 1 && !IsConnectedNode(rt, e)) return;
    std::vector<Value> a;
    if (kind == 3) a = {Value::S(name), oldV, newV, Value::Null_()};
    try {
        rt.Call(fn, W(rt, e), a);
    } catch (JSThrow& t) {
        rt.Report(L"customElements: " + e->name, t);
    }
}

// Elemani tanima gore yukselt: prototipi degistir, kurucuyu calistir, sonra
// gozlenen ozniteliklerle attributeChangedCallback ve bagliysa connectedCallback.
void CEUpgrade(Runtime& rt, Node* e, int di) {
    if (e->ceState != 0 || di < 0 || size_t(di) >= rt.ceDefs.size()) return;
    Obj* ctor = rt.ceDefs[size_t(di)].ctor;
    Obj* proto = rt.ceDefs[size_t(di)].proto;
    std::wstring name = rt.ceDefs[size_t(di)].name;
    Obj* w = rt.WrapNode(e);
    Obj* oldProto = w->proto;
    w->proto = proto;
    e->ceDef = int16_t(di);
    rt.ceConstruct.emplace_back(di, e);
    bool ok = true;
    try {
        std::vector<Value> none;
        Value r = rt.Construct(Value::O(ctor), none, ctor);
        if (!r.IsObj() || r.o != w) {
            ok = false;
            JSThrow t{Value::O(rt.NewError(L"TypeError", L"Custom element constructor for '" + name + L"' did not return the element (super() eksik?)"))};
            rt.Report(L"customElements", t);
        }
    } catch (JSThrow& t) {
        ok = false;
        rt.Report(L"customElements: " + name, t);
    } catch (...) {
        rt.ceConstruct.pop_back();
        throw;
    }
    rt.ceConstruct.pop_back();
    Dirty(rt);
    if (!ok) {
        e->ceState = 2;
        e->ceDef = -1;
        w->proto = oldProto;
        return;
    }
    e->ceState = 1;
    auto attrs = e->attrs;
    for (auto& a : attrs)
        if (CEObserves(rt, e, a.first)) CEInvoke(rt, e, 3, a.first, Value::Null_(), Value::S(a.second));
    if (IsConnectedNode(rt, e)) {
        e->ceConnected = true;
        CEInvoke(rt, e, 1, L"", Value(), Value());
    }
}

void CEInserted(Runtime& rt, Node* n, bool upgradeDisconnected) {
    if (rt.ceDefs.empty()) return;
    bool conn = IsConnectedNode(rt, n);
    ForEachComposed(n, [&](Node* e) {
        if (e->ceState == 1) {
            if (conn && !e->ceConnected) { e->ceConnected = true; CEEnqueue(rt, 1, e); }
        } else if (e->ceState == 0 && (conn || upgradeDisconnected) && e->name.find(L'-') != std::wstring::npos) {
            if (CEDefFor(rt, e->name) >= 0) CEEnqueue(rt, 0, e);
        }
    });
}

void CERemoved(Runtime& rt, Node* n) {
    if (rt.ceDefs.empty() || IsConnectedNode(rt, n)) return;
    ForEachComposed(n, [&](Node* e) {
        if (e->ceState == 1 && e->ceConnected) { e->ceConnected = false; CEEnqueue(rt, 2, e); }
    });
}

// ---- slotchange (mikro gorevde, slot basina bir kez) ----
Value SlotChangeJob(Runtime& rt, Obj*, const Value&, std::vector<Value>&) {
    std::vector<Node*> list;
    list.swap(rt.slotChangePending);
    for (Node* s : list) {
        if (!IsConnectedNode(rt, s)) continue;
        Obj* ev = MakeEvent(rt, L"slotchange", true, false);
        rt.Put(Value::O(ev), L"isTrusted", Value::B(false));
        Dispatch(rt, s, ev);
    }
    return Value();
}

void SlotChangeFor(Runtime& rt, Node* lightChild) {
    if (!lightChild || !lightChild->parent || !lightChild->parent->shadow) return;
    Node* slot = const_cast<Node*>(AssignedSlot(lightChild));
    if (!slot) return;
    for (Node* p : rt.slotChangePending) if (p == slot) return;
    rt.slotChangePending.push_back(slot);
    if (rt.slotChangePending.size() == 1) rt.Enqueue(Value::O(rt.NewFunction(SlotChangeJob, L"", 0)), {});
}

// ---- HTMLElement kurucusu: class X extends HTMLElement ----
NATIVE(HTMLElement_ctor) {
    UNUSED;
    Obj* nt = rt.nativeNewTarget;
    if (!nt) rt.Throw(L"TypeError", L"Illegal constructor");
    int di = -1;
    for (size_t i = 0; i < rt.ceDefs.size(); i++) if (rt.ceDefs[i].ctor == nt) { di = int(i); break; }
    if (di < 0) rt.Throw(L"TypeError", L"Illegal constructor: bu sınıf customElements.define ile tanımlanmamış");
    // Yukseltme: kurulmakta olan mevcut ogeyi dondur
    if (!rt.ceConstruct.empty() && rt.ceConstruct.back().first == di && rt.ceConstruct.back().second) {
        Node* e = rt.ceConstruct.back().second;
        rt.ceConstruct.back().second = nullptr;
        return Value::O(rt.WrapNode(e));
    }
    // Dogrudan: new X()
    Node* e = NewNode(Doc(rt), Node::Elem, rt.ceDefs[size_t(di)].name);
    Obj* w = rt.WrapNode(e);
    Value pr = rt.Get(Value::O(nt), L"prototype");
    w->proto = pr.IsObj() ? pr.o : rt.ceDefs[size_t(di)].proto;
    e->ceState = 1;
    e->ceDef = int16_t(di);
    return Value::O(w);
}

bool ValidCEName(const std::wstring& n) {
    if (n.size() < 2 || n[0] < L'a' || n[0] > L'z' || n.find(L'-') == std::wstring::npos) return false;
    for (wchar_t c : n) {
        if (c >= L'A' && c <= L'Z') return false;
        if (c < 0x80 && !((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.' || c == L'_')) return false;
    }
    for (const wchar_t* r : {L"annotation-xml", L"color-profile", L"font-face", L"font-face-src", L"font-face-uri",
                             L"font-face-format", L"font-face-name", L"missing-glyph"})
        if (n == r) return false;
    return true;
}

NATIVE(CE_define) {
    UNUSED;
    std::wstring name = rt.ToStr(A(0));
    if (!ValidCEName(name)) rt.Throw(L"SyntaxError", L"Failed to execute 'define' on 'CustomElementRegistry': \"" + name + L"\" is not a valid custom element name");
    if (rt.ceByName.count(name)) rt.Throw(L"Error", L"NotSupportedError: the name \"" + name + L"\" has already been used with this registry");
    if (!IsCallable(A(1))) rt.Throw(L"TypeError", L"Failed to execute 'define' on 'CustomElementRegistry': The provided value is not a constructor.");
    Obj* ctor = A(1).o;
    for (auto& d : rt.ceDefs) if (d.ctor == ctor) rt.Throw(L"Error", L"NotSupportedError: this constructor has already been used with this registry");
    Value pr = rt.Get(A(1), L"prototype");
    if (!pr.IsObj()) rt.Throw(L"TypeError", L"The 'prototype' property of the constructor is not an object");
    CEDef d;
    d.name = name;
    d.ctor = ctor;
    d.proto = pr.o;
    d.connected = rt.Get(pr, L"connectedCallback");
    d.disconnected = rt.Get(pr, L"disconnectedCallback");
    d.adopted = rt.Get(pr, L"adoptedCallback");
    d.attrChanged = rt.Get(pr, L"attributeChangedCallback");
    if (IsCallable(d.attrChanged)) {
        Value obs = rt.Get(A(1), L"observedAttributes");
        if (!obs.IsNullish()) for (auto& v : rt.IterateToList(obs, 10000)) d.observed.push_back(ToLower(rt.ToStr(v)));
    }
    if (A(2).IsObj()) {
        Value ex = rt.Get(A(2), L"extends");
        if (!ex.IsNullish()) d.extends = true;
    }
    int di = int(rt.ceDefs.size());
    rt.ceDefs.push_back(d);
    rt.ceByName[name] = di;
    // Belgede zaten bulunan ogeleri (shadow agaclari dahil, belge sirasiyla) yukselt
    if (rt.doc && !d.extends) {
        ForEachComposed(rt.doc->root.get(), [&](Node* e) {
            if (e->ceState == 0 && e->name == name) CEEnqueue(rt, 0, e);
        });
    }
    auto it = rt.ceWhenDefined.find(name);
    if (it != rt.ceWhenDefined.end()) {
        Obj* p = it->second;
        rt.ceWhenDefined.erase(it);
        rt.ResolvePromise(p, Value::O(ctor));
    }
    return Value();
}

NATIVE(CE_get) {
    UNUSED;
    auto it = rt.ceByName.find(rt.ToStr(A(0)));
    return it == rt.ceByName.end() ? Value() : Value::O(rt.ceDefs[size_t(it->second)].ctor);
}

NATIVE(CE_getName) {
    UNUSED;
    for (auto& d : rt.ceDefs) if (A(0).IsObj() && d.ctor == A(0).o) return Value::S(d.name);
    return Value::Null_();
}

NATIVE(CE_whenDefined) {
    UNUSED;
    std::wstring name = rt.ToStr(A(0));
    Obj* p;
    auto it = rt.ceByName.find(name);
    if (it != rt.ceByName.end()) {
        p = rt.NewPromise();
        rt.ResolvePromise(p, Value::O(rt.ceDefs[size_t(it->second)].ctor));
        return Value::O(p);
    }
    if (!ValidCEName(name)) {
        p = rt.NewPromise();
        rt.RejectPromise(p, Value::O(rt.NewError(L"SyntaxError", L"\"" + name + L"\" is not a valid custom element name")));
        return Value::O(p);
    }
    auto w = rt.ceWhenDefined.find(name);
    if (w != rt.ceWhenDefined.end()) return Value::O(w->second);
    p = rt.NewPromise();
    rt.ceWhenDefined[name] = p;
    return Value::O(p);
}

NATIVE(CE_upgrade) {
    UNUSED;
    Node* root = NOpt(A(0));
    if (!root) return Value();
    std::vector<Node*> list;
    ForEachComposed(root, [&](Node* e) { if (e->ceState == 0) list.push_back(e); });
    for (Node* e : list) { int di = CEDefFor(rt, e->name); if (di >= 0) CEUpgrade(rt, e, di); }
    return Value();
}

// ---- Shadow DOM API ----
NATIVE(El_attachShadow) {
    UNUSED;
    Node* h = N(rt, thisV);
    if (h->kind != Node::Elem) rt.Throw(L"Error", L"NotSupportedError: This element does not support attachShadow");
    if (h->shadow) rt.Throw(L"Error", L"NotSupportedError: Failed to execute 'attachShadow' on 'Element': Shadow root cannot be created on a host which already hosts a shadow tree.");
    std::wstring mode = L"open";
    if (A(0).IsObj()) { Value m = rt.Get(A(0), L"mode"); if (!m.IsUndef()) mode = rt.ToStr(m); }
    if (mode != L"open" && mode != L"closed") rt.Throw(L"TypeError", L"Failed to execute 'attachShadow' on 'Element': The provided value '" + mode + L"' is not a valid enum value of type ShadowRootMode.");
    Node* s = AttachShadowRoot(Doc(rt), h, mode == L"closed" ? 2 : 1);
    Dirty(rt);
    return W(rt, s);
}
NATIVE(El_shadowRoot) {
    UNUSED;
    Node* h = N(rt, thisV);
    return (h->shadow && h->shadow->shadowMode == 1) ? W(rt, h->shadow.get()) : Value::Null_();
}
NATIVE(SR_host) { UNUSED; Node* s = N(rt, thisV); return s->kind == Node::Shadow ? W(rt, s->host) : Value::Null_(); }
NATIVE(SR_mode) { UNUSED; Node* s = N(rt, thisV); return Value::S(s->shadowMode == 2 ? L"closed" : L"open"); }
NATIVE(SR_null) { UNUSED; return Value::Null_(); }
NATIVE(SR_false) { UNUSED; return Value::B(false); }
NATIVE(SR_named) { UNUSED; return Value::S(L"named"); }
NATIVE(SR_getElementById) {
    UNUSED;
    Node* s = N(rt, thisV);
    std::wstring id = rt.ToStr(A(0));
    Node* found = nullptr;
    ForEachElem(s, false, [&](Node* e) { if (e->idAttr == id) { found = e; return false; } return true; });
    return W(rt, found);
}

NATIVE(El_assignedSlot) {
    UNUSED;
    Node* n = N(rt, thisV);
    const Node* s = AssignedSlot(n);
    // kapali shadow root'taki slot disariya gosterilmez
    if (s) { const Node* r = ShadowRootOf(s); if (r && r->shadowMode == 2) s = nullptr; }
    return W(rt, const_cast<Node*>(s));
}
NATIVE(El_slot) { UNUSED; auto* v = N(rt, thisV)->Attr(L"slot"); return Value::S(v ? *v : L""); }
NATIVE(El_setSlot) { UNUSED; Node* n = N(rt, thisV); SetAttr(rt, n, L"slot", rt.ToStr(A(0))); SlotChangeFor(rt, n); Dirty(rt); return Value(); }

void FlattenAssigned(const Node* slot, bool flatten, std::vector<const Node*>& out, int depth) {
    std::vector<const Node*> v;
    AssignedNodes(slot, v);
    if (v.empty() && flatten) for (auto& k : slot->kids) v.push_back(k.get());
    for (const Node* x : v) {
        if (flatten && depth < 16 && x->kind == Node::Elem && x->name == L"slot" && ShadowRootOf(x)) FlattenAssigned(x, true, out, depth + 1);
        else out.push_back(x);
    }
}
Value AssignedImpl(Runtime& rt, const Value& thisV, std::vector<Value>& args, bool elemsOnly) {
    Node* n = N(rt, thisV);
    std::vector<Value> r;
    if (n->name == L"slot") {
        bool flatten = A(0).IsObj() && rt.Truthy(rt.Get(A(0), L"flatten"));
        std::vector<const Node*> v;
        FlattenAssigned(n, flatten, v, 0);
        for (const Node* x : v) if (!elemsOnly || x->kind == Node::Elem) r.push_back(W(rt, const_cast<Node*>(x)));
    }
    return Value::O(rt.NewArrayFrom(r));
}
NATIVE(El_assignedNodes) { UNUSED; return AssignedImpl(rt, thisV, args, false); }
NATIVE(El_assignedElements) { UNUSED; return AssignedImpl(rt, thisV, args, true); }

// <template>.content: sablon cocuklari bir belge parcasina tasinir
NATIVE(El_templateContent) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->name != L"template") return Value();
    Node*& frag = rt.templateContent[n];
    if (!frag) { frag = NewNode(Doc(rt), Node::Elem, L"#fragment"); rt.templateFragments[frag] = true; }
    if (!n->kids.empty()) {
        for (auto& k : TemplateNodes(n, Doc(rt))) { k->parent = frag; frag->kids.push_back(std::move(k)); }
        Dirty(rt);
    }
    return W(rt, frag);
}

// ---- Kurulabilir stil sayfasi (CSSStyleSheet) ----
// Not: adoptedStyleSheets bilerek sunulmaz; kutuphaneler (Lit vb.) shadow root'a <style> ekler.
NATIVE(CSS_ctor) {
    UNUSED;
    Obj* o = rt.NewObject(rt.nativeNewTarget ? rt.Get(Value::O(rt.nativeNewTarget), L"prototype").o : nullptr);
    rt.DefineData(o, L"\x02css", Value::S(L""), false);
    rt.DefineData(o, L"cssRules", Value::O(rt.NewArray(0)));
    rt.DefineData(o, L"disabled", Value::B(false));
    return Value::O(o);
}
void CssSetText(Runtime& rt, const Value& self, const std::wstring& t) {
    rt.Put(self, L"\x02css", Value::S(t));
    std::vector<Value> rules;
    StyleSheet tmp;
    tmp.Add(t);
    for (auto& r : tmp.rules) {
        Obj* ro = rt.NewObject();
        std::wstring decl;
        for (auto& d : *r.decls) decl += d.prop + L": " + d.value + (d.important ? L" !important" : L"") + L"; ";
        rt.DefineData(ro, L"cssText", Value::S(decl));
        rules.push_back(Value::O(ro));
    }
    rt.Put(self, L"cssRules", Value::O(rt.NewArrayFrom(rules)));
}
NATIVE(CSS_replaceSync) { UNUSED; CssSetText(rt, thisV, rt.ToStr(A(0))); return Value(); }
NATIVE(CSS_replace) { UNUSED; CssSetText(rt, thisV, rt.ToStr(A(0))); Obj* p = rt.NewPromise(); rt.ResolvePromise(p, thisV); return Value::O(p); }
NATIVE(CSS_insertRule) {
    UNUSED;
    std::wstring cur = rt.ToStr(rt.Get(thisV, L"\x02css"));
    CssSetText(rt, thisV, cur + L"\n" + rt.ToStr(A(0)));
    return Value::N(0);
}
NATIVE(CSS_text) { UNUSED; return rt.Get(thisV, L"\x02css"); }

// ---- <canvas> 2D baglami (cizim yok; renk ayristirma ve ozellik algilama icin) ----
// Kutuphaneler 1x1 tuvali renk normallestirmek icin kullanir: fillStyle -> fillRect -> getImageData.
NATIVE(Ctx_fillRect) {
    UNUSED;
    uint32_t c = 0;
    Value fs = rt.Get(thisV, L"fillStyle");
    if (!fs.IsStr() || !ParseColor(ToLower(Trim(*fs.s)), c)) c = 0xFF000000;
    rt.Put(thisV, L"\x02px", Value::N(double(c)));
    return Value();
}
NATIVE(Ctx_clearRect) { UNUSED; rt.Put(thisV, L"\x02px", Value::N(0)); return Value(); }
NATIVE(Ctx_getImageData) {
    UNUSED;
    double w = std::max(1.0, std::min(4096.0, rt.ToNum(A(2)))), h = std::max(1.0, std::min(4096.0, rt.ToNum(A(3))));
    if (std::isnan(w)) w = 1;
    if (std::isnan(h)) h = 1;
    size_t n = size_t(w) * size_t(h);
    Obj* arr = rt.NewTypedArray(TypedKind::Uint8Clamped, n * 4);
    Value px = rt.Get(thisV, L"\x02px");
    uint32_t c = px.IsNum() ? uint32_t(px.n) : 0;
    for (size_t i = 0; i < n; i++) {
        SetTypedElement(rt, *arr->x, i * 4 + 0, Value::N((c >> 16) & 255));
        SetTypedElement(rt, *arr->x, i * 4 + 1, Value::N((c >> 8) & 255));
        SetTypedElement(rt, *arr->x, i * 4 + 2, Value::N(c & 255));
        SetTypedElement(rt, *arr->x, i * 4 + 3, Value::N((c >> 24) & 255));
    }
    Obj* img = rt.NewObject();
    rt.DefineData(img, L"width", Value::N(w));
    rt.DefineData(img, L"height", Value::N(h));
    rt.DefineData(img, L"data", Value::O(arr));
    return Value::O(img);
}
NATIVE(Ctx_measureText) {
    UNUSED;
    Obj* m = rt.NewObject();
    rt.DefineData(m, L"width", Value::N(double(rt.ToStr(A(0)).size()) * 6.5));
    return Value::O(m);
}
NATIVE(Ctx_noop) { UNUSED; return Value(); }
NATIVE(El_getContext) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring kind = ToLower(rt.ToStr(A(0)));
    if (n->name != L"canvas" || kind != L"2d") return Value::Null_();   // webgl vb. desteklenmiyor
    Value cached = rt.Get(thisV, L"\x02ctx");
    if (cached.IsObj()) return cached;
    Obj* c = rt.NewObject(rt.ctors.count(L"CanvasRenderingContext2D") ? rt.Get(Value::O(rt.ctors[L"CanvasRenderingContext2D"]), L"prototype").o : nullptr);
    rt.DefineData(c, L"canvas", thisV);
    for (const wchar_t* k : {L"fillStyle", L"strokeStyle", L"shadowColor"}) rt.DefineData(c, k, Value::S(L"#000000"));
    rt.DefineData(c, L"font", Value::S(L"10px sans-serif"));
    rt.DefineData(c, L"globalAlpha", Value::N(1));
    rt.DefineData(c, L"lineWidth", Value::N(1));
    rt.DefineData(c, L"textAlign", Value::S(L"start"));
    rt.DefineData(c, L"textBaseline", Value::S(L"alphabetic"));
    rt.DefineData(c, L"globalCompositeOperation", Value::S(L"source-over"));
    rt.DefineData(thisV.o, L"\x02ctx", Value::O(c), false);
    return Value::O(c);
}
NATIVE(El_toDataURL) { UNUSED; return Value::S(L"data:,"); }

NATIVE(Text_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Failed to construct 'Text': Please use the 'new' operator");
    return W(rt, NewNode(Doc(rt), Node::Text, A(0).IsUndef() ? L"" : rt.ToStr(A(0))));
}
NATIVE(Comment_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Failed to construct 'Comment': Please use the 'new' operator");
    Node* c = NewNode(Doc(rt), Node::Comment, A(0).IsUndef() ? L"" : rt.ToStr(A(0)));
    Obj* w = rt.WrapNode(c);
    if (rt.commentProto) w->proto = rt.commentProto;
    return Value::O(w);
}
NATIVE(DocFrag_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Failed to construct 'DocumentFragment': Please use the 'new' operator");
    return W(rt, NewNode(Doc(rt), Node::Elem, L"#fragment"));
}

void InstallShadowAndCE(Runtime& rt) {
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Acc = [&](Obj* o, const wchar_t* n, NativeFn get, NativeFn set) { rt.DefineAccessor(o, n, get, set); };
    Obj* EP = rt.elementProto;
    M(EP, L"attachShadow", El_attachShadow, 1);
    Acc(EP, L"shadowRoot", El_shadowRoot, nullptr);
    Acc(EP, L"assignedSlot", El_assignedSlot, nullptr);
    Acc(EP, L"slot", El_slot, El_setSlot);
    M(EP, L"assignedNodes", El_assignedNodes, 1);
    M(EP, L"assignedElements", El_assignedElements, 1);
    Acc(EP, L"content", El_templateContent, nullptr);
    if (rt.textProto) Acc(rt.textProto, L"assignedSlot", El_assignedSlot, nullptr);

    Obj* SRP = rt.NewObject(EP);
    rt.shadowRootProto = SRP;
    Acc(SRP, L"host", SR_host, nullptr);
    Acc(SRP, L"mode", SR_mode, nullptr);
    Acc(SRP, L"activeElement", SR_null, nullptr);
    Acc(SRP, L"delegatesFocus", SR_false, nullptr);
    Acc(SRP, L"clonable", SR_false, nullptr);
    Acc(SRP, L"serializable", SR_false, nullptr);
    Acc(SRP, L"slotAssignment", SR_named, nullptr);
    Acc(SRP, L"tagName", SR_null, nullptr);
    Acc(SRP, L"shadowRoot", SR_null, nullptr);
    M(SRP, L"getElementById", SR_getElementById, 1);
    rt.DefineData(SRP, L"\x01Symbol.toStringTag", Value::S(L"ShadowRoot"), false);
    NativeCtor(rt, L"ShadowRoot", Illegal_ctor, SRP);
    NativeCtor(rt, L"HTMLSlotElement", Illegal_ctor, EP);
    NativeCtor(rt, L"HTMLUnknownElement", Illegal_ctor, EP);

    Obj* ceProto = rt.NewObject();
    M(ceProto, L"define", CE_define, 2);
    M(ceProto, L"get", CE_get, 1);
    M(ceProto, L"getName", CE_getName, 1);
    M(ceProto, L"whenDefined", CE_whenDefined, 1);
    M(ceProto, L"upgrade", CE_upgrade, 1);
    NativeCtor(rt, L"CustomElementRegistry", Illegal_ctor, ceProto);

    // Window: window nesnesinin prototipi (ShadyDOM gibi polyfill'ler Window.prototype'i yamar)
    Obj* winP = rt.NewObject(rt.eventTargetProto ? rt.eventTargetProto : rt.objectProto);
    NativeCtor(rt, L"Window", Illegal_ctor, winP);
    rt.global->proto = winP;

    // <canvas>
    M(EP, L"getContext", El_getContext, 1);
    M(EP, L"toDataURL", El_toDataURL, 0);
    Obj* ctxP = rt.NewObject();
    M(ctxP, L"fillRect", Ctx_fillRect, 4);
    M(ctxP, L"clearRect", Ctx_clearRect, 4);
    M(ctxP, L"getImageData", Ctx_getImageData, 4);
    M(ctxP, L"measureText", Ctx_measureText, 1);
    for (const wchar_t* n : {L"beginPath", L"closePath", L"moveTo", L"lineTo", L"arc", L"arcTo", L"rect", L"fill", L"stroke", L"fillText",
                             L"strokeText", L"strokeRect", L"save", L"restore", L"translate", L"rotate", L"scale", L"setTransform",
                             L"resetTransform", L"transform", L"drawImage", L"putImageData", L"clip", L"bezierCurveTo", L"quadraticCurveTo",
                             L"setLineDash", L"ellipse", L"roundRect"})
        M(ctxP, n, Ctx_noop, 0);
    NativeCtor(rt, L"CanvasRenderingContext2D", Illegal_ctor, ctxP);
    NativeCtor(rt, L"HTMLCanvasElement", Illegal_ctor, EP);

    Obj* cssP = rt.NewObject();
    M(cssP, L"replaceSync", CSS_replaceSync, 1);
    M(cssP, L"replace", CSS_replace, 1);
    M(cssP, L"insertRule", CSS_insertRule, 2);
    M(cssP, L"deleteRule", [](Runtime&, Obj*, const Value&, std::vector<Value>&) { return Value(); }, 1);
    NativeCtor(rt, L"CSSStyleSheet", CSS_ctor, cssP);
    NativeCtor(rt, L"StyleSheet", Illegal_ctor, cssP);
    rt.DefineData(rt.global, L"customElements", Value::O(rt.NewObject(ceProto)), false);
}

// ======================================================================
//  Ek DOM API'leri (Polymer / ShadyDOM / YouTube'un kullandiklari)
// ======================================================================

// Agac sirasi icin kok -> dugum yolu (light agac)
std::vector<const Node*> PathFromRoot(const Node* n) {
    std::vector<const Node*> p;
    for (const Node* c = n; c && p.size() < 100000; c = c->parent) p.push_back(c);
    std::reverse(p.begin(), p.end());
    return p;
}
size_t ChildIndex(const Node* n) {
    if (!n->parent) return 0;
    auto& k = n->parent->kids;
    for (size_t i = 0; i < k.size(); i++) if (k[i].get() == n) return i;
    return 0;
}
NATIVE(Node_compareDocumentPosition) {
    UNUSED;
    Node* a = N(rt, thisV);
    Node* b = NOpt(A(0));
    if (!b) rt.Throw(L"TypeError", L"parameter 1 is not of type 'Node'");
    if (a == b) return Value::N(0);
    auto pa = PathFromRoot(a), pb = PathFromRoot(b);
    if (pa.front() != pb.front()) return Value::N(1 | 32 | (a < b ? 4 : 2));   // DISCONNECTED | IMPLEMENTATION_SPECIFIC
    size_t i = 0;
    while (i < pa.size() && i < pb.size() && pa[i] == pb[i]) i++;
    if (i == pa.size()) return Value::N(16 | 4);   // b, a'nin torunu: CONTAINED_BY | FOLLOWING
    if (i == pb.size()) return Value::N(8 | 2);    // b, a'nin atasi: CONTAINS | PRECEDING
    return Value::N(ChildIndex(pb[i]) > ChildIndex(pa[i]) ? 4 : 2);
}
bool NodesEqual(const Node* a, const Node* b, int depth) {
    if (a->kind != b->kind || depth > 400) return false;
    if (a->kind == Node::Text || a->kind == Node::Comment) return a->text == b->text;
    if (a->name != b->name || a->attrs.size() != b->attrs.size() || a->kids.size() != b->kids.size()) return false;
    for (auto& x : a->attrs) { const std::wstring* v = b->Attr(x.first.c_str()); if (!v || *v != x.second) return false; }
    for (size_t i = 0; i < a->kids.size(); i++) if (!NodesEqual(a->kids[i].get(), b->kids[i].get(), depth + 1)) return false;
    return true;
}
NATIVE(Node_isEqualNode) { UNUSED; Node* b = NOpt(A(0)); return Value::B(b && NodesEqual(N(rt, thisV), b, 0)); }
NATIVE(Node_isSameNode) { UNUSED; return Value::B(NOpt(A(0)) == N(rt, thisV)); }
NATIVE(Node_normalize) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::vector<Node*> st{n};
    while (!st.empty()) {
        Node* c = st.back();
        st.pop_back();
        for (size_t i = 0; i < c->kids.size();) {
            Node* k = c->kids[i].get();
            if (k->kind == Node::Text) {
                if (k->text.empty()) { RemoveNodeCE(rt, Doc(rt), k); continue; }
                while (i + 1 < c->kids.size() && c->kids[i + 1]->kind == Node::Text) {
                    k->text += c->kids[i + 1]->text;
                    RemoveNodeCE(rt, Doc(rt), c->kids[i + 1].get());
                }
            } else st.push_back(k);
            i++;
        }
    }
    Dirty(rt);
    return Value();
}
NATIVE(Node_baseURI) { UNUSED; return Value::S(rt.host ? rt.host->PageUrl() : L"about:blank"); }
NATIVE(Node_lookupNamespaceURI) { UNUSED; return A(0).IsNullish() ? Value::S(L"http://www.w3.org/1999/xhtml") : Value::Null_(); }
NATIVE(El_namespaceURI) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (n->kind != Node::Elem) return Value::Null_();
    for (const Node* p = n; p; p = p->parent) {
        if (p->name == L"svg") return Value::S(L"http://www.w3.org/2000/svg");
        if (p->name == L"math") return Value::S(L"http://www.w3.org/1998/Math/MathML");
        if (p->name == L"foreignobject") break;
    }
    return Value::S(L"http://www.w3.org/1999/xhtml");
}
NATIVE(El_prefix) { UNUSED; return Value::Null_(); }
// *NS ozniteliklerde ad alani yok sayilir (yerel ad kullanilir)
std::wstring LocalName(const std::wstring& q) { size_t c = q.find(L':'); return c == std::wstring::npos ? q : q.substr(c + 1); }
NATIVE(El_getAttributeNS) { UNUSED; auto* v = N(rt, thisV)->Attr(ToLower(LocalName(rt.ToStr(A(1)))).c_str()); return v ? Value::S(*v) : Value::Null_(); }
NATIVE(El_setAttributeNS) { UNUSED; Node* n = N(rt, thisV); SetAttr(rt, n, rt.ToStr(A(1)).find(L"xlink:") == 0 ? rt.ToStr(A(1)) : LocalName(rt.ToStr(A(1))), rt.ToStr(A(2))); Dirty(rt); return Value(); }
NATIVE(El_removeAttributeNS) { UNUSED; RemoveAttr(rt, N(rt, thisV), LocalName(rt.ToStr(A(1)))); Dirty(rt); return Value(); }
NATIVE(El_hasAttributeNS) { UNUSED; return Value::B(N(rt, thisV)->Attr(ToLower(LocalName(rt.ToStr(A(1)))).c_str()) != nullptr); }
NATIVE(El_getAttributeNode) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring k = ToLower(rt.ToStr(A(0)));
    auto* v = n->Attr(k.c_str());
    if (!v) return Value::Null_();
    Obj* o = rt.NewObject(rt.ctors.count(L"Attr") ? rt.Get(Value::O(rt.ctors[L"Attr"]), L"prototype").o : nullptr);
    rt.DefineData(o, L"name", Value::S(k));
    rt.DefineData(o, L"localName", Value::S(k));
    rt.DefineData(o, L"value", Value::S(*v));
    rt.DefineData(o, L"nodeValue", Value::S(*v));
    rt.DefineData(o, L"namespaceURI", Value::Null_());
    rt.DefineData(o, L"prefix", Value::Null_());
    rt.DefineData(o, L"specified", Value::B(true));
    rt.DefineData(o, L"nodeType", Value::N(2));
    rt.DefineData(o, L"ownerElement", thisV);
    return Value::O(o);
}
NATIVE(El_insertAdjacentText) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::wstring pos = ToLower(rt.ToStr(A(0)));
    Node* t = NewNode(Doc(rt), Node::Text, rt.ToStr(A(1)));
    if (pos == L"beforebegin" && n->parent) InsertNodes(rt, n->parent, {t}, n);
    else if (pos == L"afterbegin") InsertNodes(rt, n, {t}, n->kids.empty() ? nullptr : n->kids.front().get());
    else if (pos == L"beforeend") InsertNodes(rt, n, {t}, nullptr);
    else if (pos == L"afterend" && n->parent) InsertNodes(rt, n->parent, {t}, Sibling(n, 1, false));
    else if (pos != L"beforebegin" && pos != L"afterend") rt.Throw(L"SyntaxError", L"Invalid position for insertAdjacentText");
    return Value();
}
NATIVE(El_replaceChildren) {
    UNUSED;
    Node* n = N(rt, thisV);
    std::vector<Node*> add;
    for (auto& a : args) for (Node* x : FragmentKids(ToNodeArg(rt, a))) add.push_back(x);
    while (!n->kids.empty()) RemoveNodeCE(rt, Doc(rt), n->kids.back().get());
    InsertNodes(rt, n, add, nullptr);
    Dirty(rt);
    return Value();
}
NATIVE(El_tabIndex) {
    UNUSED;
    Node* n = N(rt, thisV);
    if (auto* t = n->Attr(L"tabindex")) { double v = rt.ToNum(Value::S(*t)); if (!std::isnan(v)) return Value::N(std::trunc(v)); }
    const std::wstring& t = n->name;
    bool focusable = t == L"input" || t == L"button" || t == L"select" || t == L"textarea" || (t == L"a" && n->Attr(L"href")) || t == L"iframe";
    return Value::N(focusable ? 0 : -1);
}
NATIVE(El_setTabIndex) { UNUSED; SetAttr(rt, N(rt, thisV), L"tabindex", Runtime::NumToStr(std::trunc(rt.ToNum(A(0))))); return Value(); }
NATIVE(El_dir) { UNUSED; auto* v = N(rt, thisV)->Attr(L"dir"); return Value::S(v ? *v : L""); }
NATIVE(El_setDir) { UNUSED; SetAttr(rt, N(rt, thisV), L"dir", rt.ToStr(A(0))); Dirty(rt); return Value(); }
NATIVE(El_lang) { UNUSED; auto* v = N(rt, thisV)->Attr(L"lang"); return Value::S(v ? *v : L""); }
NATIVE(El_setLang) { UNUSED; SetAttr(rt, N(rt, thisV), L"lang", rt.ToStr(A(0))); return Value(); }
NATIVE(El_inert) { UNUSED; return Value::B(N(rt, thisV)->Attr(L"inert") != nullptr); }
NATIVE(El_setInert) { UNUSED; Node* n = N(rt, thisV); if (rt.Truthy(A(0))) SetAttr(rt, n, L"inert", L""); else RemoveAttr(rt, n, L"inert"); return Value(); }
NATIVE(El_contentEditable) { UNUSED; auto* v = N(rt, thisV)->Attr(L"contenteditable"); return Value::S(v ? (v->empty() ? L"true" : *v) : L"inherit"); }
NATIVE(El_isContentEditable) {
    UNUSED;
    for (Node* p = N(rt, thisV); p; p = p->parent) if (auto* v = p->Attr(L"contenteditable")) return Value::B(*v != L"false");
    return Value::B(false);
}
NATIVE(El_getElementsByTagNameNS) { UNUSED; std::vector<Value> a{A(1)}; return El_getElementsByTagName(rt, self, thisV, a); }
NATIVE(El_emptyArray) { UNUSED; return Value::O(rt.NewArray(0)); }
NATIVE(El_rejectPromise) {
    UNUSED;
    Obj* p = rt.NewPromise();
    rt.RejectPromise(p, Value::O(rt.NewError(L"TypeError", L"merOS Shield: bu islem desteklenmiyor")));
    return Value::O(p);
}

// ---- Text / CharacterData ----
Node* TextNode(Runtime& rt, const Value& v) {
    Node* n = N(rt, v);
    if (n->kind != Node::Text && n->kind != Node::Comment) rt.Throw(L"TypeError", L"Illegal invocation");
    return n;
}
size_t Off(Runtime& rt, const Value& v, size_t len) {
    double d = rt.ToNum(v);
    if (std::isnan(d) || d < 0) d = 0;
    if (d > double(len)) rt.Throw(L"Error", L"IndexSizeError: The offset is larger than the data's length.");
    return size_t(d);
}
NATIVE(Text_length) { UNUSED; return Value::N(double(TextNode(rt, thisV)->text.size())); }
NATIVE(Text_appendData) { UNUSED; Node* n = TextNode(rt, thisV); std::wstring old = n->text; n->text += rt.ToStr(A(0)); Dirty(rt); MOChar(rt, n, old); return Value(); }
NATIVE(Text_insertData) { UNUSED; Node* n = TextNode(rt, thisV); std::wstring old = n->text; n->text.insert(Off(rt, A(0), n->text.size()), rt.ToStr(A(1))); Dirty(rt); MOChar(rt, n, old); return Value(); }
NATIVE(Text_deleteData) {
    UNUSED;
    Node* n = TextNode(rt, thisV);
    size_t o = Off(rt, A(0), n->text.size());
    n->text.erase(o, size_t(std::max(0.0, rt.ToNum(A(1)))));
    Dirty(rt);
    return Value();
}
NATIVE(Text_replaceData) {
    UNUSED;
    Node* n = TextNode(rt, thisV);
    size_t o = Off(rt, A(0), n->text.size());
    n->text.replace(o, size_t(std::max(0.0, rt.ToNum(A(1)))), rt.ToStr(A(2)));
    Dirty(rt);
    return Value();
}
NATIVE(Text_substringData) {
    UNUSED;
    Node* n = TextNode(rt, thisV);
    size_t o = Off(rt, A(0), n->text.size());
    return Value::S(n->text.substr(o, size_t(std::max(0.0, rt.ToNum(A(1))))));
}
NATIVE(Text_splitText) {
    UNUSED;
    Node* n = TextNode(rt, thisV);
    size_t o = Off(rt, A(0), n->text.size());
    Node* t = NewNode(Doc(rt), Node::Text, n->text.substr(o));
    n->text.erase(o);
    if (n->parent) InsertNodes(rt, n->parent, {t}, Sibling(n, 1, false));
    Dirty(rt);
    return W(rt, t);
}

// ---- Range: createContextualFragment ve temel sinirlar ----
NATIVE(Range_createContextualFragment) {
    UNUSED;
    Document& d = Doc(rt);
    Node* frag = NewNode(d, Node::Elem, L"#fragment");
    for (auto& f : ParseFragment(rt.ToStr(A(0)), d)) { f->parent = frag; frag->kids.push_back(std::move(f)); }
    return W(rt, frag);
}
NATIVE(Range_set) {
    UNUSED;
    std::wstring nm = self && self->x ? self->x->name : L"";
    bool start = nm.find(L"Start") != std::wstring::npos || nm == L"selectNode" || nm == L"selectNodeContents";
    Value node = A(0);
    double off = A(1).IsUndef() ? 0 : rt.ToNum(A(1));
    if (nm == L"selectNodeContents") {
        Node* n = NOpt(node);
        rt.Put(thisV, L"startContainer", node); rt.Put(thisV, L"startOffset", Value::N(0));
        rt.Put(thisV, L"endContainer", node); rt.Put(thisV, L"endOffset", Value::N(n ? double(n->kind == Node::Text ? n->text.size() : n->kids.size()) : 0));
    } else if (nm == L"selectNode") {
        Node* n = NOpt(node);
        Value par = n && n->parent ? W(rt, n->parent) : Value::Null_();
        double idx = n ? double(ChildIndex(n)) : 0;
        rt.Put(thisV, L"startContainer", par); rt.Put(thisV, L"startOffset", Value::N(idx));
        rt.Put(thisV, L"endContainer", par); rt.Put(thisV, L"endOffset", Value::N(idx + 1));
    } else if (start) {
        rt.Put(thisV, L"startContainer", node); rt.Put(thisV, L"startOffset", Value::N(off));
    } else {
        rt.Put(thisV, L"endContainer", node); rt.Put(thisV, L"endOffset", Value::N(off));
    }
    rt.Put(thisV, L"collapsed", Value::B(rt.StrictEq(rt.Get(thisV, L"startContainer"), rt.Get(thisV, L"endContainer")) &&
                                         rt.StrictEq(rt.Get(thisV, L"startOffset"), rt.Get(thisV, L"endOffset"))));
    rt.Put(thisV, L"commonAncestorContainer", rt.Get(thisV, L"startContainer"));
    return Value();
}
NATIVE(Range_collapse) {
    UNUSED;
    bool toStart = rt.Truthy(A(0));
    if (toStart) { rt.Put(thisV, L"endContainer", rt.Get(thisV, L"startContainer")); rt.Put(thisV, L"endOffset", rt.Get(thisV, L"startOffset")); }
    else { rt.Put(thisV, L"startContainer", rt.Get(thisV, L"endContainer")); rt.Put(thisV, L"startOffset", rt.Get(thisV, L"endOffset")); }
    rt.Put(thisV, L"collapsed", Value::B(true));
    return Value();
}
NATIVE(Range_rect) {
    UNUSED;
    Obj* r = rt.NewObject();
    for (const wchar_t* k : {L"x", L"y", L"top", L"left", L"right", L"bottom", L"width", L"height"}) rt.DefineData(r, k, Value::N(0));
    return Value::O(r);
}
Obj* NewRange(Runtime& rt) {
    Obj* proto = rt.ctors.count(L"Range") ? rt.Get(Value::O(rt.ctors[L"Range"]), L"prototype").o : nullptr;
    Obj* r = rt.NewObject(proto);
    Value docW = W(rt, Doc(rt).root.get());
    for (const wchar_t* k : {L"startContainer", L"endContainer", L"commonAncestorContainer"}) rt.DefineData(r, k, docW);
    rt.DefineData(r, L"startOffset", Value::N(0));
    rt.DefineData(r, L"endOffset", Value::N(0));
    rt.DefineData(r, L"collapsed", Value::B(true));
    return r;
}
NATIVE(Range_ctor) { UNUSED; return Value::O(NewRange(rt)); }
NATIVE(Doc_createRange) { UNUSED; return Value::O(NewRange(rt)); }
NATIVE(Range_clone) {
    UNUSED;
    Obj* r = NewRange(rt);
    for (const wchar_t* k : {L"startContainer", L"endContainer", L"commonAncestorContainer", L"startOffset", L"endOffset", L"collapsed"})
        rt.Put(Value::O(r), k, rt.Get(thisV, k));
    return Value::O(r);
}
NATIVE(Range_toString) {
    UNUSED;
    Node* s = NOpt(rt.Get(thisV, L"startContainer"));
    Node* e = NOpt(rt.Get(thisV, L"endContainer"));
    if (s && s == e && s->kind == Node::Text) {
        size_t a = size_t(std::max(0.0, rt.ToNum(rt.Get(thisV, L"startOffset")))), b = size_t(std::max(0.0, rt.ToNum(rt.Get(thisV, L"endOffset"))));
        if (a < b && b <= s->text.size()) return Value::S(s->text.substr(a, b - a));
    }
    return Value::S(L"");
}

void InstallDomExtras(Runtime& rt) {
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Acc = [&](Obj* o, const wchar_t* n, NativeFn get, NativeFn set) { rt.DefineAccessor(o, n, get, set); };
    Obj* NP = rt.nodeProto;
    Obj* EP = rt.elementProto;
    M(NP, L"compareDocumentPosition", Node_compareDocumentPosition, 1);
    M(NP, L"isEqualNode", Node_isEqualNode, 1);
    M(NP, L"isSameNode", Node_isSameNode, 1);
    M(NP, L"normalize", Node_normalize, 0);
    M(NP, L"lookupNamespaceURI", Node_lookupNamespaceURI, 1);
    M(NP, L"isDefaultNamespace", [](Runtime&, Obj*, const Value&, std::vector<Value>& a) { return Value::B(a.empty() || a[0].IsNullish()); }, 1);
    Acc(NP, L"baseURI", Node_baseURI, nullptr);
    for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{{L"DOCUMENT_POSITION_DISCONNECTED", 1}, {L"DOCUMENT_POSITION_PRECEDING", 2},
             {L"DOCUMENT_POSITION_FOLLOWING", 4}, {L"DOCUMENT_POSITION_CONTAINS", 8}, {L"DOCUMENT_POSITION_CONTAINED_BY", 16},
             {L"DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 32}}) {
        rt.DefineData(NP, kv.first, Value::N(kv.second), false);
        if (rt.ctors.count(L"Node")) rt.DefineData(rt.ctors[L"Node"], kv.first, Value::N(kv.second), false);
    }
    Acc(EP, L"namespaceURI", El_namespaceURI, nullptr);
    Acc(EP, L"prefix", El_prefix, nullptr);
    M(EP, L"getAttributeNS", El_getAttributeNS, 2);
    M(EP, L"setAttributeNS", El_setAttributeNS, 3);
    M(EP, L"removeAttributeNS", El_removeAttributeNS, 2);
    M(EP, L"hasAttributeNS", El_hasAttributeNS, 2);
    M(EP, L"getAttributeNode", El_getAttributeNode, 1);
    M(EP, L"insertAdjacentText", El_insertAdjacentText, 2);
    M(EP, L"replaceChildren", El_replaceChildren, 0);
    M(EP, L"getElementsByTagNameNS", El_getElementsByTagNameNS, 2);
    M(rt.documentProto, L"getElementsByTagNameNS", El_getElementsByTagNameNS, 2);
    M(rt.documentProto, L"replaceChildren", El_replaceChildren, 0);
    M(rt.documentProto, L"createRange", Doc_createRange, 0);
    M(rt.documentProto, L"compareDocumentPosition", Node_compareDocumentPosition, 1);
    Acc(EP, L"tabIndex", El_tabIndex, El_setTabIndex);
    Acc(EP, L"dir", El_dir, El_setDir);
    Acc(EP, L"lang", El_lang, El_setLang);
    Acc(EP, L"inert", El_inert, El_setInert);
    Acc(EP, L"contentEditable", El_contentEditable, nullptr);
    Acc(EP, L"isContentEditable", El_isContentEditable, nullptr);
    M(EP, L"getAnimations", El_emptyArray, 0);
    M(EP, L"getClientRects", El_emptyArray, 0);
    M(EP, L"requestFullscreen", El_rejectPromise, 0);
    M(EP, L"requestPointerLock", El_rejectPromise, 0);
    Obj* TP = rt.textProto;
    Acc(TP, L"length", Text_length, nullptr);
    M(TP, L"appendData", Text_appendData, 1);
    M(TP, L"insertData", Text_insertData, 2);
    M(TP, L"deleteData", Text_deleteData, 2);
    M(TP, L"replaceData", Text_replaceData, 3);
    M(TP, L"substringData", Text_substringData, 2);
    M(TP, L"splitText", Text_splitText, 1);
    Obj* RP = rt.NewObject();
    for (const wchar_t* n : {L"setStart", L"setEnd", L"setStartBefore", L"setStartAfter", L"setEndBefore", L"setEndAfter", L"selectNode", L"selectNodeContents"})
        M(RP, n, Range_set, 2);
    M(RP, L"collapse", Range_collapse, 1);
    M(RP, L"cloneRange", Range_clone, 0);
    M(RP, L"createContextualFragment", Range_createContextualFragment, 1);
    M(RP, L"getBoundingClientRect", Range_rect, 0);
    M(RP, L"getClientRects", El_emptyArray, 0);
    M(RP, L"toString", Range_toString, 0);
    for (const wchar_t* n : {L"detach", L"deleteContents", L"insertNode", L"surroundContents"}) M(RP, n, Ctx_noop, 1);
    NativeCtor(rt, L"Range", Range_ctor, RP);
    NativeCtor(rt, L"AbstractRange", Illegal_ctor, RP);
    NativeCtor(rt, L"StaticRange", Illegal_ctor, RP);
    // instanceof / prototip yamalari icin kurucular (DOM listeleri merJS'te dizi olarak doner)
    Obj* listP = rt.NewObject(rt.arrayProto);
    NativeCtor(rt, L"NodeList", Illegal_ctor, listP);
    NativeCtor(rt, L"HTMLCollection", Illegal_ctor, rt.NewObject(rt.arrayProto));
    NativeCtor(rt, L"NamedNodeMap", Illegal_ctor, rt.NewObject(rt.arrayProto));
    NativeCtor(rt, L"DOMTokenList", Illegal_ctor, rt.classListProto);
    NativeCtor(rt, L"Attr", Illegal_ctor, rt.NewObject(rt.nodeProto));
    NativeCtor(rt, L"Selection", Illegal_ctor, rt.NewObject());
    NativeCtor(rt, L"FontFace", Illegal_ctor, rt.NewObject());
    NativeCtor(rt, L"FontFaceSet", Illegal_ctor, rt.NewObject(rt.eventTargetProto));
}

// Saf JS ile yazilabilen tamamlayicilar (yerlesik baslangic betigi)
// MSVC tek dize sabitini ~16 KB ile sinirlar (C2026): betik parcalar halinde tutulur
const wchar_t* const kDomPreludeParts[] = {
LR"JS((function () {
  'use strict';
  var g = globalThis;
  function def(o, k, v) { if (!(k in o)) Object.defineProperty(o, k, { value: v, writable: true, configurable: true }); }
  def(Object, 'getOwnPropertyDescriptors', function (o) {
    var r = {}, ks = Object.getOwnPropertyNames(o);
    if (Object.getOwnPropertySymbols) ks = ks.concat(Object.getOwnPropertySymbols(o));
    for (var i = 0; i < ks.length; i++) { var d = Object.getOwnPropertyDescriptor(o, ks[i]); if (d) r[ks[i]] = d; }
    return r;
  });
  if (typeof g.WeakRef === 'undefined') {
    g.WeakRef = function WeakRef(t) { if (!(this instanceof WeakRef)) throw new TypeError("Constructor WeakRef requires 'new'"); this._t = t; };
    g.WeakRef.prototype.deref = function () { return this._t; };
  }
  if (typeof g.FinalizationRegistry === 'undefined') {
    g.FinalizationRegistry = function FinalizationRegistry(cb) { this._cb = cb; };
    g.FinalizationRegistry.prototype.register = function () {};
    g.FinalizationRegistry.prototype.unregister = function () { return false; };
  }
  var css = g.CSS || (g.CSS = {});
  def(css, 'escape', function (v) {
    var s = String(v), out = '', n = s.length, first = s.charCodeAt(0);
    for (var i = 0; i < n; i++) {
      var c = s.charCodeAt(i);
      if (c === 0) { out += '�'; continue; }
      if ((c >= 1 && c <= 31) || c === 127 || (i === 0 && c >= 48 && c <= 57) || (i === 1 && c >= 48 && c <= 57 && first === 45)) { out += '\\' + c.toString(16) + ' '; continue; }
      if (i === 0 && n === 1 && c === 45) { out += '\\' + s.charAt(i); continue; }
      if (c >= 128 || c === 45 || c === 95 || (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)) { out += s.charAt(i); continue; }
      out += '\\' + s.charAt(i);
    }
    return out;
  });
  def(css, 'supports', function (p, v) {
    var t = (arguments.length > 1 ? p + ':' + v : String(p)).toLowerCase();
    return !/subgrid|masonry|anchor\(|-moz-|-ms-|paint\(|element\(/.test(t);
  });
  def(css, 'registerProperty', function () {});
  // DOMException
  if (typeof g.DOMException !== 'function') {
    var codes = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5, NoModificationAllowedError: 7,
      NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14,
      InvalidAccessError: 15, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22,
      TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25 };
    var DE = function DOMException(message, name) {
      if (!(this instanceof DE)) throw new TypeError("Failed to construct 'DOMException': Please use the 'new' operator");
      var e = new Error(message === undefined ? '' : String(message));
      Object.defineProperty(this, 'message', { value: e.message, configurable: true, writable: true });
)JS",
LR"JS(      Object.defineProperty(this, 'name', { value: name === undefined ? 'Error' : String(name), configurable: true, writable: true });
      Object.defineProperty(this, 'code', { value: codes[this.name] || 0, configurable: true });
      Object.defineProperty(this, 'stack', { value: e.stack, configurable: true, writable: true });
    };
    DE.prototype = Object.create(Error.prototype, { constructor: { value: DE, writable: true, configurable: true } });
    DE.prototype.toString = function () { return this.name + ': ' + this.message; };
    for (var cn in codes) DE[cn.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR')] = codes[cn];
    g.DOMException = DE;
  }
  // AbortController / AbortSignal (olaylar, reason, throwIfAborted, abort/timeout/any)
  (function () {
    var AS = function AbortSignal() { throw new TypeError('Illegal constructor'); };
    AS.prototype = Object.create(EventTarget.prototype, { constructor: { value: AS, writable: true, configurable: true } });
    function mk() { var s = new EventTarget(); Object.setPrototypeOf(s, AS.prototype); s.aborted = false; s.reason = undefined; s.onabort = null; return s; }
    function doAbort(s, reason) {
      if (s.aborted) return;
      s.aborted = true;
      s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
      s.dispatchEvent(new Event('abort'));
    }
    AS.prototype.throwIfAborted = function () { if (this.aborted) throw this.reason; };
    AS.abort = function (r) { var s = mk(); doAbort(s, r); return s; };
    AS.timeout = function (ms) { var s = mk(); setTimeout(function () { doAbort(s, new DOMException('signal timed out', 'TimeoutError')); }, ms); return s; };
    AS.any = function (list) {
      var s = mk(), arr = Array.from(list);
      for (var i = 0; i < arr.length; i++) if (arr[i].aborted) { doAbort(s, arr[i].reason); return s; }
      arr.forEach(function (x) { x.addEventListener('abort', function () { doAbort(s, x.reason); }); });
      return s;
    };
    var AC = function AbortController() {
      if (!(this instanceof AC)) throw new TypeError("Failed to construct 'AbortController': Please use the 'new' operator");
      Object.defineProperty(this, 'signal', { value: mk(), enumerable: true, configurable: true });
    };
    AC.prototype.abort = function (r) { doAbort(this.signal, r); };
    g.AbortSignal = AS;
    g.AbortController = AC;
  })();
  // MessageChannel / MessagePort / window.postMessage / BroadcastChannel (gorev olarak, eszamansiz teslim)
  (function () {
    function msgEvent(data, origin, source, ports) {
      var e = new Event('message');
      e.data = data; e.origin = origin || ''; e.lastEventId = ''; e.source = source || null; e.ports = ports || [];
      return e;
    }
    function clone(d) { try { return typeof structuredClone === 'function' ? structuredClone(d) : d; } catch (x) { return d; } }
)JS",
LR"JS(    var MP = function MessagePort() { throw new TypeError('Illegal constructor'); };
    MP.prototype = Object.create(EventTarget.prototype, { constructor: { value: MP, writable: true, configurable: true } });
    function mkPort() { var p = new EventTarget(); Object.setPrototypeOf(p, MP.prototype); p.onmessage = null; p.onmessageerror = null; p._other = null; p._closed = false; return p; }
    MP.prototype.postMessage = function (data) {
      var target = this._other;
      if (!target || this._closed || target._closed) return;
      var d = clone(data);
      setTimeout(function () { if (!target._closed) target.dispatchEvent(msgEvent(d)); }, 0);
    };
    MP.prototype.start = function () {};
    MP.prototype.close = function () { this._closed = true; };
    var MC = function MessageChannel() {
      if (!(this instanceof MC)) throw new TypeError("Failed to construct 'MessageChannel': Please use the 'new' operator");
      var a = mkPort(), b = mkPort();
      a._other = b; b._other = a;
      Object.defineProperty(this, 'port1', { value: a, enumerable: true });
      Object.defineProperty(this, 'port2', { value: b, enumerable: true });
    };
    g.MessagePort = MP;
    g.MessageChannel = MC;
    g.postMessage = function (data, targetOrigin) {
      var d = clone(data), origin = (g.location && g.location.origin) || '';
      setTimeout(function () { g.dispatchEvent(msgEvent(d, origin, g)); if (typeof g.onmessage === 'function') g.onmessage(msgEvent(d, origin, g)); }, 0);
    };
    var chans = {};
    var BC = function BroadcastChannel(name) {
      if (!(this instanceof BC)) throw new TypeError("Failed to construct 'BroadcastChannel': Please use the 'new' operator");
      var t = new EventTarget(); Object.setPrototypeOf(t, BC.prototype);
      t.name = String(name); t.onmessage = null; t._closed = false;
      (chans[t.name] || (chans[t.name] = [])).push(t);
      return t;
    };
    BC.prototype = Object.create(EventTarget.prototype, { constructor: { value: BC, writable: true, configurable: true } });
    BC.prototype.postMessage = function (data) {
      var self = this, d = clone(data);
      (chans[this.name] || []).forEach(function (c) { if (c !== self && !c._closed) setTimeout(function () { c.dispatchEvent(msgEvent(d)); }, 0); });
    };
    BC.prototype.close = function () { this._closed = true; var l = chans[this.name] || []; var i = l.indexOf(this); if (i >= 0) l.splice(i, 1); };
    g.BroadcastChannel = BC;
  })();
  // Secim (Selection) ve yazi tipleri (document.fonts)
  var sel = Object.create(Selection.prototype, {
    rangeCount: { value: 0, writable: true }, isCollapsed: { value: true, writable: true }, type: { value: 'None', writable: true },
    anchorNode: { value: null, writable: true }, focusNode: { value: null, writable: true }, anchorOffset: { value: 0, writable: true }, focusOffset: { value: 0, writable: true }
  });
)JS",
LR"JS(  ['removeAllRanges', 'addRange', 'removeRange', 'collapse', 'collapseToStart', 'collapseToEnd', 'selectAllChildren', 'extend', 'setBaseAndExtent', 'empty', 'modify']
    .forEach(function (m) { sel[m] = function () {}; });
  sel.getRangeAt = function () { throw new Error('IndexSizeError: There is no range at this index'); };
  sel.containsNode = function () { return false; };
  sel.toString = function () { return ''; };
  g.getSelection = function () { return sel; };
  if (g.document) {
    document.getSelection = g.getSelection;
    var fonts = Object.create(FontFaceSet.prototype);
    fonts.status = 'loaded';
    fonts.size = 0;
    fonts.ready = Promise.resolve(fonts);
    fonts.check = function () { return true; };
    fonts.load = function () { return Promise.resolve([]); };
    fonts.add = function () { return fonts; };
    fonts.delete = function () { return false; };
    fonts.clear = function () {};
    fonts.forEach = function () {};
    fonts.onloadingdone = null;
    Object.defineProperty(document, 'fonts', { value: fonts, configurable: true });
    if (!('styleSheets' in document)) Object.defineProperty(document, 'styleSheets', { get: function () { return []; }, configurable: true });
    if (!('dir' in document)) Object.defineProperty(document, 'dir', { get: function () { var h = document.documentElement; return h ? (h.getAttribute('dir') || '') : ''; }, configurable: true });
    if (!document.elementFromPoint) document.elementFromPoint = function () { return null; };
    if (!document.elementsFromPoint) document.elementsFromPoint = function () { return []; };
  }
  // el.attributes: item() / getNamedItem()
  var ad = Object.getOwnPropertyDescriptor(Element.prototype, 'attributes');
  if (ad && ad.get) {
    var nativeGet = ad.get;
    Object.defineProperty(Element.prototype, 'attributes', { configurable: true, get: function () {
      var a = nativeGet.call(this);
      a.item = function (i) { return this[i] || null; };
      a.getNamedItem = function (n) { n = String(n).toLowerCase(); for (var i = 0; i < this.length; i++) if (this[i].name === n) return this[i]; return null; };
      return a;
    } });
  }
  // part (::part) belirtec listesi: classList benzeri basit nesne
  if (!('part' in Element.prototype)) Object.defineProperty(Element.prototype, 'part', { configurable: true, get: function () {
    var el = this, list = function () { return (el.getAttribute('part') || '').split(/\s+/).filter(Boolean); };
    return { get length() { return list().length; }, contains: function (t) { return list().indexOf(t) >= 0; },
      add: function () { var l = list(); for (var i = 0; i < arguments.length; i++) if (l.indexOf(arguments[i]) < 0) l.push(arguments[i]); el.setAttribute('part', l.join(' ')); },
      remove: function () { var r = [].slice.call(arguments); el.setAttribute('part', list().filter(function (x) { return r.indexOf(x) < 0; }).join(' ')); },
)JS",
LR"JS(      toString: function () { return list().join(' '); }, get value() { return list().join(' '); } };
  } });
})();
)JS",
// --- Streams (kaynak: tests/js/streams_prelude.js) ---
LR"JS(// WHATWG Streams (sade): ReadableStream, WritableStream, TransformStream, TextDecoderStream, TextEncoderStream
// ve Response.body. merJS prelude'una gomulur (js_dom.cpp kDomPreludeParts); bu dosya kaynak/test kopyasidir.
(function () {
  var g = typeof globalThis !== 'undefined' ? globalThis : this;
  if (typeof g.ReadableStream !== 'undefined') return;
  function resolved(v) { return Promise.resolve(v); }
  function Deferred() { var d = this; this.promise = new Promise(function (r, j) { d.resolve = r; d.reject = j; }); }

  function ReadableStreamDefaultController(stream) { this._s = stream; }
  Object.defineProperty(ReadableStreamDefaultController.prototype, 'desiredSize', { configurable: true, get: function () {
    var s = this._s; if (s._state === 'errored') return null; if (s._state === 'closed') return 0;
    return s._hwm - s._queue.length; } });
  ReadableStreamDefaultController.prototype.enqueue = function (chunk) {
    var s = this._s;
    if (s._closeRequested || s._state !== 'readable') throw new TypeError('Cannot enqueue a chunk into a closed ReadableStream');
    if (s._reads.length) s._reads.shift().resolve({ value: chunk, done: false });
    else s._queue.push(chunk);
    s._pullIfNeeded();
  };
  ReadableStreamDefaultController.prototype.close = function () {
    var s = this._s;
    if (s._closeRequested || s._state !== 'readable') throw new TypeError('Cannot close a closed ReadableStream');
    s._closeRequested = true;
    if (!s._queue.length) s._finishClose();
  };
  ReadableStreamDefaultController.prototype.error = function (e) { this._s._error(e); };

  function ReadableStream(source, strategy) {
    if (!(this instanceof ReadableStream)) throw new TypeError("Failed to construct 'ReadableStream': Please use the 'new' operator");
    source = source || {};
    this._src = source; this._state = 'readable'; this._queue = []; this._reads = []; this._reader = null;
    this._closeRequested = false; this._pulling = false; this._pullAgain = false; this._started = false; this._err = undefined;
    this._hwm = strategy && typeof strategy.highWaterMark === 'number' ? strategy.highWaterMark : 1;
    this._ctl = new ReadableStreamDefaultController(this);
    var self = this;
    var r;
    try { r = typeof source.start === 'function' ? source.start.call(source, this._ctl) : undefined; } catch (e) { this._error(e); return; }
    resolved(r).then(function () { self._started = true; self._pullIfNeeded(); }, function (e) { self._error(e); });
)JS",
LR"JS(  }
  ReadableStream.prototype._pullIfNeeded = function () {
    var self = this;
    if (!this._started || this._state !== 'readable' || this._closeRequested || typeof this._src.pull !== 'function') return;
    if (this._queue.length >= this._hwm && !this._reads.length) return;
    if (this._pulling) { this._pullAgain = true; return; }
    this._pulling = true;
    var r;
    try { r = this._src.pull.call(this._src, this._ctl); } catch (e) { this._error(e); return; }
    resolved(r).then(function () {
      self._pulling = false;
      if (self._pullAgain) { self._pullAgain = false; self._pullIfNeeded(); }
    }, function (e) { self._error(e); });
  };
  ReadableStream.prototype._finishClose = function () {
    if (this._state !== 'readable') return;
    this._state = 'closed';
    while (this._reads.length) this._reads.shift().resolve({ value: undefined, done: true });
    if (this._reader) this._reader._closedD.resolve();
  };
  ReadableStream.prototype._error = function (e) {
    if (this._state !== 'readable') return;
    this._state = 'errored'; this._err = e; this._queue = [];
    while (this._reads.length) this._reads.shift().reject(e);
    if (this._reader) { this._reader._closedD.reject(e); this._reader._closedD.promise.catch(function () {}); }
  };
  ReadableStream.prototype._read = function () {
    if (this._state === 'errored') return Promise.reject(this._err);
    if (this._queue.length) {
      var v = this._queue.shift();
      if (this._closeRequested && !this._queue.length) this._finishClose();
      else this._pullIfNeeded();
      return resolved({ value: v, done: false });
    }
    if (this._state === 'closed') return resolved({ value: undefined, done: true });
    var d = new Deferred(); this._reads.push(d); this._pullIfNeeded();
    return d.promise;
  };
  ReadableStream.prototype._cancel = function (reason) {
    if (this._state === 'closed') return resolved();
    if (this._state === 'errored') return Promise.reject(this._err);
    this._queue = []; this._closeRequested = true; this._finishClose();
    var c = this._src.cancel;
    try { return resolved(typeof c === 'function' ? c.call(this._src, reason) : undefined).then(function () {}); } catch (e) { return Promise.reject(e); }
  };
  Object.defineProperty(ReadableStream.prototype, 'locked', { configurable: true, get: function () { return !!this._reader; } });
  ReadableStream.prototype.getReader = function (opts) {
)JS",
LR"JS(    if (opts && opts.mode === 'byob') throw new TypeError("BYOB okuyucu desteklenmiyor");
    return new ReadableStreamDefaultReader(this);
  };
  ReadableStream.prototype.cancel = function (reason) {
    if (this._reader) return Promise.reject(new TypeError('Cannot cancel a locked stream'));
    return this._cancel(reason);
  };
  ReadableStream.prototype.tee = function () {
    var reader = this.getReader(), c1, c2, canceled = 0;
    function pull() {
      return reader.read().then(function (r) {
        if (r.done) { try { c1.close(); } catch (e) {} try { c2.close(); } catch (e) {} return; }
        try { c1.enqueue(r.value); } catch (e) {} try { c2.enqueue(r.value); } catch (e) {}
      });
    }
    var a = new ReadableStream({ start: function (c) { c1 = c; }, pull: pull, cancel: function (r) { if (++canceled === 2) return reader.cancel(r); } });
    var b = new ReadableStream({ start: function (c) { c2 = c; }, pull: pull, cancel: function (r) { if (++canceled === 2) return reader.cancel(r); } });
    return [a, b];
  };
  ReadableStream.prototype.pipeTo = function (dest, opts) {
    var reader = this.getReader(), writer = dest.getWriter();
    opts = opts || {};
    return new Promise(function (resolve, reject) {
      function step() {
        reader.read().then(function (r) {
          if (r.done) {
            reader.releaseLock();
            if (opts.preventClose) { writer.releaseLock(); resolve(); } else writer.close().then(resolve, reject);
            return;
          }
          writer.write(r.value).then(step, function (e) { reader.cancel(e); reject(e); });
        }, function (e) { if (!opts.preventAbort) writer.abort(e); reject(e); });
      }
      step();
    });
  };
  ReadableStream.prototype.pipeThrough = function (pair, opts) {
    this.pipeTo(pair.writable, opts).catch(function () {});
    return pair.readable;
  };
  ReadableStream.prototype.values = function (opts) {
    var reader = this.getReader(), keep = opts && opts.preventCancel;
    var it = {
      next: function () { return reader.read().then(function (r) { if (r.done) reader.releaseLock(); return r; }); },
      'return': function (v) { if (!keep) reader.cancel(v); reader.releaseLock(); return resolved({ value: v, done: true }); }
    };
    it[Symbol.asyncIterator] = function () { return it; };
    return it;
  };
  if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;
)JS",
LR"JS(  ReadableStream.from = function (iterable) {
    var it = iterable[Symbol.asyncIterator] ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
    return new ReadableStream({ pull: function (c) {
      return resolved(it.next()).then(function (r) { if (r.done) c.close(); else return resolved(r.value).then(function (v) { c.enqueue(v); }); });
    } });
  };

  function ReadableStreamDefaultReader(stream) {
    if (stream._reader) throw new TypeError('ReadableStreamDefaultReader constructor can only accept readable streams that are not yet locked to a reader');
    this._s = stream; stream._reader = this; this._closedD = new Deferred();
    if (stream._state === 'closed') this._closedD.resolve();
    else if (stream._state === 'errored') { this._closedD.reject(stream._err); this._closedD.promise.catch(function () {}); }
  }
  Object.defineProperty(ReadableStreamDefaultReader.prototype, 'closed', { configurable: true, get: function () { return this._closedD.promise; } });
  ReadableStreamDefaultReader.prototype.read = function () {
    if (!this._s) return Promise.reject(new TypeError('This readable stream reader has been released and cannot be used to read from its previous owner stream'));
    return this._s._read();
  };
  ReadableStreamDefaultReader.prototype.releaseLock = function () {
    if (!this._s) return;
    var s = this._s;
    while (s._reads.length) s._reads.shift().reject(new TypeError('Releasing reader'));
    s._reader = null; this._s = null;
  };
  ReadableStreamDefaultReader.prototype.cancel = function (r) { return this._s ? this._s._cancel(r) : Promise.reject(new TypeError('released')); };

  function WritableStreamDefaultController(s) { this._s = s; }
  WritableStreamDefaultController.prototype.error = function (e) { this._s._fail(e); };
  function WritableStream(sink) {
    sink = sink || {};
    this._sink = sink; this._state = 'writable'; this._writer = null; this._chain = resolved(); this._err = undefined;
    this._ctl = new WritableStreamDefaultController(this);
    var self = this;
    try { this._chain = resolved(typeof sink.start === 'function' ? sink.start.call(sink, this._ctl) : undefined); }
    catch (e) { this._state = 'errored'; this._err = e; }
    this._chain.catch(function (e) { self._fail(e); });
  }
  WritableStream.prototype._fail = function (e) { if (this._state === 'errored') return; this._state = 'errored'; this._err = e; };
  WritableStream.prototype._enqueue = function (fn) {
    var self = this;
)JS",
LR"JS(    var p = this._chain.then(function () { if (self._state === 'errored') throw self._err; return fn(); });
    this._chain = p.catch(function (e) { self._fail(e); });
    return p;
  };
  Object.defineProperty(WritableStream.prototype, 'locked', { configurable: true, get: function () { return !!this._writer; } });
  WritableStream.prototype.getWriter = function () { return new WritableStreamDefaultWriter(this); };
  WritableStream.prototype.close = function () { var s = this, k = s._sink; s._state = 'closing'; return s._enqueue(function () { return typeof k.close === 'function' ? k.close.call(k) : undefined; }).then(function () { s._state = 'closed'; }); };
  WritableStream.prototype.abort = function (r) { var k = this._sink; this._fail(r); return resolved(typeof k.abort === 'function' ? k.abort.call(k, r) : undefined).then(function () {}); };
  function WritableStreamDefaultWriter(s) {
    if (s._writer) throw new TypeError('WritableStream is locked');
    this._s = s; s._writer = this;
  }
  WritableStreamDefaultWriter.prototype.write = function (chunk) {
    var s = this._s, k = s._sink;
    if (s._state !== 'writable') return Promise.reject(s._err || new TypeError('stream is not writable'));
    return s._enqueue(function () { return typeof k.write === 'function' ? k.write.call(k, chunk, s._ctl) : undefined; });
  };
  WritableStreamDefaultWriter.prototype.close = function () { return this._s.close(); };
  WritableStreamDefaultWriter.prototype.abort = function (r) { return this._s.abort(r); };
  WritableStreamDefaultWriter.prototype.releaseLock = function () { if (this._s) { this._s._writer = null; this._s = null; } };
  Object.defineProperty(WritableStreamDefaultWriter.prototype, 'ready', { configurable: true, get: function () { return this._s ? this._s._chain : resolved(); } });
  Object.defineProperty(WritableStreamDefaultWriter.prototype, 'closed', { configurable: true, get: function () { var s = this._s; return s ? s._chain.then(function () { if (s._state === 'errored') throw s._err; }) : resolved(); } });
  Object.defineProperty(WritableStreamDefaultWriter.prototype, 'desiredSize', { configurable: true, get: function () { return 1; } });

  function TransformStream(tr) {
    tr = tr || {};
    var rc, self = this;
    this.readable = new ReadableStream({ start: function (c) { rc = c; } });
    var ctl = {
      enqueue: function (c) { rc.enqueue(c); },
      error: function (e) { rc.error(e); },
)JS",
LR"JS(      terminate: function () { try { rc.close(); } catch (e) {} },
      get desiredSize() { return rc.desiredSize; }
    };
    if (typeof tr.start === 'function') tr.start.call(tr, ctl);
    this.writable = new WritableStream({
      write: function (chunk) { return typeof tr.transform === 'function' ? tr.transform.call(tr, chunk, ctl) : ctl.enqueue(chunk); },
      close: function () { return resolved(typeof tr.flush === 'function' ? tr.flush.call(tr, ctl) : undefined).then(function () { try { rc.close(); } catch (e) {} }); },
      abort: function (r) { rc.error(r); }
    });
    void self;
  }

  function TextDecoderStream(label, opts) {
    var dec = new TextDecoder(label, opts);
    var ts = new TransformStream({
      transform: function (chunk, c) { var s = dec.decode(chunk, { stream: true }); if (s) c.enqueue(s); },
      flush: function (c) { var s = dec.decode(); if (s) c.enqueue(s); }
    });
    this.readable = ts.readable; this.writable = ts.writable; this.encoding = dec.encoding;
  }
  function TextEncoderStream() {
    var enc = new TextEncoder();
    var ts = new TransformStream({ transform: function (chunk, c) { c.enqueue(enc.encode(String(chunk))); } });
    this.readable = ts.readable; this.writable = ts.writable; this.encoding = 'utf-8';
  }

  g.ReadableStream = ReadableStream;
  g.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
  g.ReadableStreamDefaultController = ReadableStreamDefaultController;
  g.WritableStream = WritableStream;
  g.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
  g.TransformStream = TransformStream;
  g.TextDecoderStream = TextDecoderStream;
  g.TextEncoderStream = TextEncoderStream;
  g.ByteLengthQueuingStrategy = function (o) { this.highWaterMark = o && o.highWaterMark; this.size = function (c) { return c.byteLength; }; };
  g.CountQueuingStrategy = function (o) { this.highWaterMark = o && o.highWaterMark; this.size = function () { return 1; }; };

  // Response.body: govde tek parca halinde akistan okunur
  if (typeof Response !== 'undefined' && Response.prototype && !Object.getOwnPropertyDescriptor(Response.prototype, 'body')) {
    Object.defineProperty(Response.prototype, 'body', { configurable: true, get: function () {
      if (this._merBody) return this._merBody;
      var resp = this;
      this._merBody = new ReadableStream({
        start: function (c) {
)JS",
LR"JS(          return resp.arrayBuffer().then(function (b) { if (b.byteLength) c.enqueue(new Uint8Array(b)); c.close(); }, function (e) { c.error(e); });
        }
      });
      return this._merBody;
    } });
  }
})();

)JS",
};
std::wstring DomPrelude() {
    std::wstring s;
    for (const wchar_t* p : kDomPreludeParts) s += p;
    return s;
}

// ======================================================================
//  MutationObserver (DOM degisikliklerini mikro gorevde bildirir)
// ======================================================================
// flags: 1 childList, 2 attributes, 4 characterData, 8 subtree, 16 attributeOldValue, 32 characterDataOldValue

Value MODeliver(Runtime& rt, Obj*, const Value&, std::vector<Value>&) {
    rt.moScheduled = false;
    std::vector<Obj*> list;
    list.swap(rt.moPending);
    for (Obj* o : list) {
        Value q = rt.Get(Value::O(o), L"\x02q");
        if (!q.IsObj() || q.o->elems.empty()) continue;
        rt.DefineData(o, L"\x02q", Value::O(rt.NewArray(0)), false);
        Value cb = rt.Get(Value::O(o), L"\x02cb");
        if (!IsCallable(cb)) continue;
        std::vector<Value> a{q, Value::O(o)};
        try {
            rt.Call(cb, Value::O(o), a);
        } catch (JSThrow& t) {
            rt.Report(L"MutationObserver", t);
        }
    }
    return Value();
}

void MONotify(Runtime& rt, Node* target, uint8_t kind, const std::wstring& attrName, const Value& oldV,
              Node* added, Node* removed, Node* prev, Node* next) {
    if (rt.moRegs.empty() || !target) return;
    std::vector<std::pair<Obj*, bool>> hits;
    for (auto& r : rt.moRegs) {
        if (!(r.flags & kind)) continue;
        if (r.target != target && !((r.flags & 8) && ContainsLight(r.target, target))) continue;
        if (kind == 2 && !r.filter.empty() && std::find(r.filter.begin(), r.filter.end(), attrName) == r.filter.end()) continue;
        bool wantOld = kind == 2 ? (r.flags & 16) != 0 : kind == 4 ? (r.flags & 32) != 0 : false;
        bool merged = false;
        for (auto& h : hits) if (h.first == r.observer) { h.second = h.second || wantOld; merged = true; }
        if (!merged) hits.emplace_back(r.observer, wantOld);
    }
    for (auto& h : hits) {
        Obj* rec = rt.NewObject(rt.ctors.count(L"MutationRecord") ? rt.Get(Value::O(rt.ctors[L"MutationRecord"]), L"prototype").o : nullptr);
        rt.DefineData(rec, L"type", Value::S(kind == 1 ? L"childList" : kind == 2 ? L"attributes" : L"characterData"));
        rt.DefineData(rec, L"target", W(rt, target));
        std::vector<Value> ad, rm;
        if (added) ad.push_back(W(rt, added));
        if (removed) rm.push_back(W(rt, removed));
        rt.DefineData(rec, L"addedNodes", Value::O(rt.NewArrayFrom(ad)));
        rt.DefineData(rec, L"removedNodes", Value::O(rt.NewArrayFrom(rm)));
        rt.DefineData(rec, L"previousSibling", W(rt, prev));
        rt.DefineData(rec, L"nextSibling", W(rt, next));
        rt.DefineData(rec, L"attributeName", kind == 2 ? Value::S(attrName) : Value::Null_());
        rt.DefineData(rec, L"attributeNamespace", Value::Null_());
        rt.DefineData(rec, L"oldValue", h.second ? oldV : Value::Null_());
        Value q = rt.Get(Value::O(h.first), L"\x02q");
        if (!q.IsObj()) { q = Value::O(rt.NewArray(0)); rt.DefineData(h.first, L"\x02q", q, false); }
        if (q.o->elems.size() < 100000) q.o->elems.push_back(Value::O(rec));
        if (std::find(rt.moPending.begin(), rt.moPending.end(), h.first) == rt.moPending.end()) rt.moPending.push_back(h.first);
        if (!rt.moScheduled) {
            rt.moScheduled = true;
            rt.Enqueue(Value::O(rt.NewFunction(MODeliver, L"", 0)), {});
        }
    }
}

void MOChild(Runtime& rt, Node* parent, Node* added, Node* removed, Node* prev, Node* next) {
    if (rt.moRegs.empty()) return;
    MONotify(rt, parent, 1, L"", Value::Null_(), added, removed, prev, next);
}
void MOInserted(Runtime& rt, Node* n) {
    if (rt.moRegs.empty() || !n->parent) return;
    MOChild(rt, n->parent, n, nullptr, Sibling(n, -1, false), Sibling(n, 1, false));
}
void MOChar(Runtime& rt, Node* n, const std::wstring& old) {
    if (rt.moRegs.empty()) return;
    MONotify(rt, n, 4, L"", Value::S(old), nullptr, nullptr, nullptr, nullptr);
}

NATIVE(MO_observe) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Illegal invocation");
    Node* t = NOpt(A(0));
    if (!t) rt.Throw(L"TypeError", L"Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'.");
    uint8_t f = 0;
    std::vector<std::wstring> filter;
    if (A(1).IsObj()) {
        Value o = A(1);
        auto has = [&](const wchar_t* k) { return !rt.Get(o, k).IsUndef(); };
        auto tr = [&](const wchar_t* k) { return rt.Truthy(rt.Get(o, k)); };
        if (tr(L"childList")) f |= 1;
        if (tr(L"attributes")) f |= 2;
        if (tr(L"characterData")) f |= 4;
        if (tr(L"subtree")) f |= 8;
        if (tr(L"attributeOldValue")) f |= 16;
        if (tr(L"characterDataOldValue")) f |= 32;
        Value af = rt.Get(o, L"attributeFilter");
        if (!af.IsUndef()) for (auto& v : rt.IterateToList(af, 10000)) filter.push_back(ToLower(rt.ToStr(v)));
        if (!has(L"attributes") && (has(L"attributeOldValue") || has(L"attributeFilter"))) f |= 2;
        if (!has(L"characterData") && has(L"characterDataOldValue")) f |= 4;
    }
    if (!(f & 7)) rt.Throw(L"TypeError", L"Failed to execute 'observe' on 'MutationObserver': The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
    for (auto& r : rt.moRegs) if (r.observer == thisV.o && r.target == t) { r.flags = f; r.filter = filter; return Value(); }
    MORegistration r;
    r.observer = thisV.o;
    r.target = t;
    r.flags = f;
    r.filter = filter;
    rt.moRegs.push_back(r);
    return Value();
}
NATIVE(MO_disconnect) {
    UNUSED;
    if (!thisV.IsObj()) return Value();
    auto& v = rt.moRegs;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const MORegistration& r) { return r.observer == thisV.o; }), v.end());
    rt.DefineData(thisV.o, L"\x02q", Value::O(rt.NewArray(0)), false);
    return Value();
}
NATIVE(MO_takeRecords) {
    UNUSED;
    if (!thisV.IsObj()) return Value::O(rt.NewArray(0));
    Value q = rt.Get(thisV, L"\x02q");
    rt.DefineData(thisV.o, L"\x02q", Value::O(rt.NewArray(0)), false);
    return q.IsObj() ? q : Value::O(rt.NewArray(0));
}
NATIVE(MO_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Failed to construct 'MutationObserver': Please use the 'new' operator");
    if (!IsCallable(A(0))) rt.Throw(L"TypeError", L"Failed to construct 'MutationObserver': parameter 1 is not of type 'MutationCallback'.");
    Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
    Obj* o = rt.NewObject(p.IsObj() ? p.o : nullptr);
    rt.DefineData(o, L"\x02cb", A(0), false);
    rt.DefineData(o, L"\x02q", Value::O(rt.NewArray(0)), false);
    return Value::O(o);
}
void InstallMutationObserver(Runtime& rt) {
    Obj* p = rt.NewObject();
    rt.DefineMethod(p, L"observe", MO_observe, 2);
    rt.DefineMethod(p, L"disconnect", MO_disconnect, 0);
    rt.DefineMethod(p, L"takeRecords", MO_takeRecords, 0);
    Obj* c = NativeCtor(rt, L"MutationObserver", MO_ctor, p);
    rt.DefineData(rt.global, L"WebKitMutationObserver", Value::O(c), false);
    NativeCtor(rt, L"MutationRecord", Illegal_ctor, rt.NewObject());
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
    Obj* proto = n->kind == Node::Text ? textProto : n->kind == Node::Doc ? documentProto
               : (n->kind == Node::Comment && commentProto) ? commentProto
               : (n->kind == Node::Shadow && shadowRootProto) ? shadowRootProto : elementProto;
    Obj* w = NewObject(proto);
    w->cls = Cls::Node;
    w->X().node = n;
    wrappers[n] = w;
    return w;
}

bool Runtime::DispatchEvent(Node* target, Obj* event) { return Dispatch(*this, target, event); }

// Ozel oge tepki kuyrugunu bosalt (DOM cagrisi sonunda). Ic ice cagrilar ayni dongude islenir.
void Runtime::FlushCEReactions() {
    if (ceFlushing) return;
    ceFlushing = true;
    struct G { Runtime& r; ~G() { r.ceFlushing = false; } } g{*this};
    try {
        size_t guard = 0;
        while (ceHead < ceQueue.size() && guard++ < 1000000) {
            CEReaction r = ceQueue[ceHead++];
            if (r.kind == 0) {
                if (r.node->ceState == 0) CEUpgrade(*this, r.node, CEDefFor(*this, r.node->name));
            } else {
                CEInvoke(*this, r.node, r.kind, r.name, r.oldV, r.newV);
            }
        }
    } catch (...) {
        ceQueue.clear();
        ceHead = 0;
        throw;
    }
    ceQueue.clear();
    ceHead = 0;
}

static bool RunTaskDispatch(Runtime& rt, Node* target, const std::wstring& type, bool bubbles, bool cancelable, const std::wstring& key) {
    if (!rt.doc) return true;
    bool nested = rt.inTask;
    if (!nested) rt.BeginTask();
    bool ok = true;
    try {
        Obj* ev = MakeEvent(rt, type, bubbles, cancelable);
        if (!key.empty()) rt.DefineData(ev, L"key", Value::S(key));
        if (type == L"click") { rt.DefineData(ev, L"button", Value::N(0)); rt.DefineData(ev, L"which", Value::N(1)); }
        if (type == L"click" && rt.evClientX >= 0) {
            double cx = rt.evClientX, cy = rt.evClientY;
            rt.DefineData(ev, L"clientX", Value::N(cx));
            rt.DefineData(ev, L"clientY", Value::N(cy));
            rt.DefineData(ev, L"x", Value::N(cx));
            rt.DefineData(ev, L"y", Value::N(cy));
            rt.DefineData(ev, L"pageX", Value::N(rt.evPageX));
            rt.DefineData(ev, L"pageY", Value::N(rt.evPageY));
            rt.DefineData(ev, L"screenX", Value::N(cx));
            rt.DefineData(ev, L"screenY", Value::N(cy));
            double rx = 0, ry = 0, rw = 0, rh = 0;
            if (rt.host && rt.host->NodeRect(target, rx, ry, rw, rh)) {
                rt.DefineData(ev, L"offsetX", Value::N(cx - rx));
                rt.DefineData(ev, L"offsetY", Value::N(cy - ry));
            }
        }
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
bool Runtime::DispatchClickAt(Node* target, double clientX, double clientY, double pageX, double pageY) {
    evClientX = clientX; evClientY = clientY; evPageX = pageX; evPageY = pageY;
    bool ok = RunTaskDispatch(*this, target, L"click", true, true, L"");
    evClientX = evClientY = evPageX = evPageY = -1;
    return ok;
}
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
    if (doc) MediaScan(*this, doc->root.get());
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
    M(EP, L"hasAttributes", El_hasAttributes, 0);
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
    for (const wchar_t* n : {L"focus", L"blur", L"scrollIntoView", L"scrollTo", L"scrollBy", L"select", L"setSelectionRange", L"setCustomValidity", L"reportValidity", L"releasePointerCapture", L"setPointerCapture", L"animate"})
        M(EP, n, El_noop, 0);
    M(EP, L"checkValidity", Doc_true, 0);


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
    M(DP, L"importNode", Doc_importNode, 2);
    M(DP, L"adoptNode", Doc_adoptNode, 1);
    M(DP, L"createTreeWalker", Doc_createTreeWalker, 3);
    M(DP, L"createNodeIterator", Doc_createNodeIterator, 3);
    {
        Obj* nf = rt.NewObject();
        for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{{L"FILTER_ACCEPT", 1}, {L"FILTER_REJECT", 2}, {L"FILTER_SKIP", 3},
                 {L"SHOW_ALL", 4294967295.0}, {L"SHOW_ELEMENT", 1}, {L"SHOW_TEXT", 4}, {L"SHOW_COMMENT", 128}, {L"SHOW_DOCUMENT", 256},
                 {L"SHOW_DOCUMENT_FRAGMENT", 1024}, {L"SHOW_PROCESSING_INSTRUCTION", 64}})
            rt.DefineData(nf, kv.first, Value::N(kv.second), false);
        rt.DefineData(rt.global, L"NodeFilter", Value::O(nf), false);
    }
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
    NativeCtor(rt, L"EventTarget", [](Runtime& r, Obj*, const Value&, std::vector<Value>&) -> Value {
        if (!r.nativeNewTarget) r.Throw(L"TypeError", L"Failed to construct 'EventTarget': Please use the 'new' operator");
        Value p = r.Get(Value::O(r.nativeNewTarget), L"prototype");
        Obj* o = r.NewObject(p.IsObj() ? p.o : r.eventTargetProto);
        o->X();
        return Value::O(o);
    }, eventTargetProto);
    NativeCtor(rt, L"Node", Illegal_ctor, NP);
    for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{{L"ELEMENT_NODE", 1}, {L"ATTRIBUTE_NODE", 2}, {L"TEXT_NODE", 3}, {L"PROCESSING_INSTRUCTION_NODE", 7}, {L"COMMENT_NODE", 8}, {L"DOCUMENT_NODE", 9}, {L"DOCUMENT_FRAGMENT_NODE", 11}})
        rt.DefineData(rt.ctors[L"Node"], kv.first, Value::N(kv.second), false);
    NativeCtor(rt, L"Element", Illegal_ctor, EP);
    NativeCtor(rt, L"HTMLElement", HTMLElement_ctor, EP);
    InstallShadowAndCE(rt);
    InstallDomExtras(rt);
    for (const wchar_t* n : {L"HTMLDivElement", L"HTMLInputElement", L"HTMLAnchorElement", L"HTMLImageElement", L"HTMLFormElement",
                             L"HTMLButtonElement", L"HTMLSelectElement", L"HTMLTextAreaElement", L"HTMLScriptElement", L"HTMLIFrameElement",
                             L"HTMLCanvasElement", L"HTMLVideoElement", L"HTMLAudioElement", L"HTMLMediaElement", L"HTMLTemplateElement", L"HTMLBodyElement",
                             L"HTMLSpanElement", L"HTMLParagraphElement", L"HTMLLinkElement", L"HTMLStyleElement", L"SVGElement"})
        NativeCtor(rt, n, Illegal_ctor, EP);
    NativeCtor(rt, L"Audio", Audio_ctor, EP);
    NativeCtor(rt, L"Text", Text_ctor, rt.textProto);
    rt.commentProto = rt.NewObject(rt.textProto);
    NativeCtor(rt, L"Comment", Comment_ctor, rt.commentProto);
    NativeCtor(rt, L"CDATASection", Illegal_ctor, rt.NewObject(rt.textProto));
    NativeCtor(rt, L"ProcessingInstruction", Illegal_ctor, rt.NewObject(rt.commentProto));
    NativeCtor(rt, L"CharacterData", Illegal_ctor, rt.textProto);
    NativeCtor(rt, L"Document", Illegal_ctor, DP);
    NativeCtor(rt, L"HTMLDocument", Illegal_ctor, DP);
    NativeCtor(rt, L"DocumentFragment", DocFrag_ctor, NP);
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

    // Medya: HTMLMediaElement + MediaSource/SourceBuffer (js_media.cpp)
    InstallMedia(rt, EP);

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
    for (const wchar_t* n : {L"scrollTo", L"scrollBy", L"scroll", L"focus", L"blur", L"print", L"close", L"stop", L"moveTo", L"resizeTo"}) M(g, n, Win_noop, 0);
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
    rt.DefineData(nav, L"userAgent", Value::S(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 merOSShield/0.3"));
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

    Obj* clipProto = rt.NewObject();
    M(clipProto, L"writeText", Clip_writeText, 1);
    M(clipProto, L"readText", Clip_readText, 0);
    NativeCtor(rt, L"Clipboard", Illegal_ctor, clipProto);
    Obj* clip = rt.NewObject(clipProto);
    rt.DefineData(nav, L"clipboard", Value::O(clip), false);

    Obj* cryptoProto = rt.NewObject();
    M(cryptoProto, L"randomUUID", Crypto_randomUUID, 0);
    M(cryptoProto, L"getRandomValues", Crypto_getRandomValues, 1);
    NativeCtor(rt, L"Crypto", Illegal_ctor, cryptoProto);
    Obj* cryptoObj = rt.NewObject(cryptoProto);
    rt.DefineData(g, L"crypto", Value::O(cryptoObj), false);

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
    InstallMutationObserver(rt);
    for (const wchar_t* n : {L"ResizeObserver", L"IntersectionObserver", L"PerformanceObserver"}) {
        Obj* p = rt.NewObject();
        std::wstring nm(n);
        M(p, L"observe", nm == L"IntersectionObserver" ? IO_observe : nm == L"ResizeObserver" ? RO_observe : Win_noop, 2);
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

    // Saf JS tamamlayicilar (Selection, document.fonts, CSS.escape, WeakRef ...)
    rt.RunScript(DomPrelude(), L"merOS-baslangic");
}

// js_media.cpp icin: duz nesneye (MediaSource, SourceBuffer) olay gonder (gorev olarak; hatalar raporlanir)
void JsDispatchObj(Runtime& rt, Obj* target, const std::wstring& type) {
    bool nested = rt.inTask;
    if (!nested) rt.BeginTask();
    try {
        DispatchSimpleToObj(rt, target, type);
        if (!nested) rt.RunMicrotasks();
    } catch (JSThrow& t) {
        rt.Report(L"olay: " + type, t);
    } catch (JSAbort& a) {
        if (rt.host) rt.host->Log(2, L"Olay işleyici durduruldu: " + a.reason);
        rt.microtasks.clear();
        if (nested) throw;
    }
    if (!nested) rt.EndTask();
}
Obj* JsNativeCtor(Runtime& rt, const wchar_t* name, NativeFn fn, Obj* proto) { return NativeCtor(rt, name, fn, proto); }

} // namespace js
} // namespace mer
