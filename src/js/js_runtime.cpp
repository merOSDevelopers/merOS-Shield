#ifndef NOMINMAX
#define NOMINMAX
#endif
// merOS Shield - merJS : nesne modeli, dönüşümler, çöp toplayıcı, özellik erişimi, Promise
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include "../net.h"
#include "../url.h"
#include "../util.h"
#include "js_ast.h"

namespace mer {
namespace js {

// ---------------- PropMap ----------------

Prop* PropMap::Find(const std::wstring& k) {
    if (!idx_.empty()) {
        auto it = idx_.find(k);
        return it == idx_.end() ? nullptr : &list_[it->second].p;
    }
    for (auto& e : list_) if (!e.dead && e.key == k) return &e.p;
    return nullptr;
}

const Prop* PropMap::Find(const std::wstring& k) const { return const_cast<PropMap*>(this)->Find(k); }

Prop& PropMap::Put(const std::wstring& k) {
    if (Prop* p = Find(k)) return *p;
    list_.push_back(E{k, Prop(), false});
    live_++;
    if (!idx_.empty()) idx_[k] = uint32_t(list_.size() - 1);
    else if (list_.size() > 8) Reindex();
    return list_.back().p;
}

bool PropMap::Erase(const std::wstring& k) {
    for (size_t i = 0; i < list_.size(); i++) {
        if (!list_[i].dead && list_[i].key == k) {
            list_[i].dead = true;
            list_[i].p = Prop();
            live_--;
            if (!idx_.empty()) idx_.erase(k);
            if (list_.size() > 16 && live_ * 2 < list_.size()) {
                std::vector<E> nl;
                for (auto& e : list_) if (!e.dead) nl.push_back(std::move(e));
                list_.swap(nl);
                if (list_.size() > 8) Reindex(); else idx_.clear();
            }
            return true;
        }
    }
    return false;
}

void PropMap::Reindex() {
    idx_.clear();
    for (size_t i = 0; i < list_.size(); i++) if (!list_[i].dead) idx_[list_[i].key] = uint32_t(i);
}

static inline void PushV(std::vector<Cell*>& w, const Value& v) { if (v.o) w.push_back(v.o); }

void PropMap::TraceInto(std::vector<Cell*>& w) const {
    for (auto& e : list_) {
        if (e.dead) continue;
        PushV(w, e.p.v);
        if (e.p.getter) w.push_back(e.p.getter);
        if (e.p.setter) w.push_back(e.p.setter);
    }
}

void Obj::Trace(std::vector<Cell*>& w) {
    if (proto) w.push_back(proto);
    props.TraceInto(w);
    for (auto& v : elems) PushV(w, v);
    if (!x) return;
    if (x->scope) w.push_back(x->scope);
    for (auto& v : x->slots) PushV(w, v);
    if (x->home) w.push_back(x->home);
    if (x->fieldsEnv) w.push_back(x->fieldsEnv);
    if (x->parentClass) w.push_back(x->parentClass);
    if (x->boundTarget) w.push_back(x->boundTarget);
    PushV(w, x->boundThis);
    for (auto& v : x->boundArgs) PushV(w, v);
    for (auto& e : x->entries) { PushV(w, e.first); PushV(w, e.second); }
    PushV(w, x->pvalue);
    for (auto& r : x->reactions) { PushV(w, r.onFul); PushV(w, r.onRej); if (r.derived) w.push_back(r.derived); }
    for (auto& l : x->listeners) PushV(w, l.fn);
    if (x->arrayBufferObj) w.push_back(x->arrayBufferObj);
}

Binding* Env::Find(const std::wstring& k) {
    if (!idx.empty()) {
        auto it = idx.find(k);
        return it == idx.end() ? nullptr : &vars[it->second].second;
    }
    for (auto& v : vars) if (v.first == k) return &v.second;
    return nullptr;
}

Binding& Env::Define(const std::wstring& k, uint8_t kind) {
    if (Binding* b = Find(k)) { if (kind > b->kind) b->kind = kind; return *b; }
    vars.emplace_back(k, Binding());
    vars.back().second.kind = kind;
    if (!idx.empty()) idx[k] = uint32_t(vars.size() - 1);
    else if (vars.size() > 12) for (size_t i = 0; i < vars.size(); i++) idx[vars[i].first] = uint32_t(i);
    return vars.back().second;
}

void Env::Trace(std::vector<Cell*>& w) {
    if (parent) w.push_back(parent);
    if (withObj) w.push_back(withObj);
    PushV(w, thisVal);
    if (func) w.push_back(func);
    if (newTarget) w.push_back(newTarget);
    for (auto& v : vars) PushV(w, v.second.v);
}

// ---------------- Runtime ----------------

Runtime::Runtime() {
    objectProto = Alloc<Obj>();
    functionProto = Alloc<Obj>();
    functionProto->proto = objectProto;
    functionProto->cls = Cls::Function;
    functionProto->X();
    global = Alloc<Obj>();
    global->proto = objectProto;
    globalEnv = Alloc<Env>();
    globalEnv->isGlobal = true;
    globalEnv->isFunc = true;
    globalEnv->hasThis = true;
    globalEnv->thisVal = Value::O(global);
    InstallBuiltins(*this);
    InstallIntl(*this);
}

Runtime::~Runtime() {
    for (Cell* c : cells_) delete c;
    cells_.clear();
}

double Runtime::Now() const {
    using namespace std::chrono;
    return double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

void Runtime::CheckStack() {
    char probe = 0;
    uintptr_t here = reinterpret_cast<uintptr_t>(&probe);
    if (stackBase && (stackBase > here ? stackBase - here : here - stackBase) > stackLimit)
        Throw(L"RangeError", L"Maximum call stack size exceeded");
}

Obj* Runtime::NewObject(Obj* proto) {
    if (cells_.size() > maxCells) throw JSAbort{L"bellek sınırı aşıldı"};
    Obj* o = Alloc<Obj>();
    o->proto = proto ? proto : objectProto;
    return o;
}

Obj* Runtime::NewArray(size_t n) {
    Obj* a = NewObject(arrayProto);
    a->cls = Cls::Array;
    a->elems.resize(n);
    return a;
}

Obj* Runtime::NewArrayFrom(const std::vector<Value>& v) {
    Obj* a = NewArray(0);
    a->elems = v;
    return a;
}

Obj* Runtime::NewArrayBuffer(size_t byteLength) {
    Obj* ab = NewObject(arrayBufferProto);
    ab->cls = Cls::ArrayBuffer;
    ab->X().bufferData = std::make_shared<std::vector<uint8_t>>(byteLength, 0);
    ab->x->byteLength = byteLength;
    return ab;
}

Obj* Runtime::NewArrayBufferFrom(const void* data, size_t byteLength) {
    Obj* ab = NewArrayBuffer(byteLength);
    if (data && byteLength > 0 && ab->x && ab->x->bufferData) {
        memcpy(ab->x->bufferData->data(), data, byteLength);
    }
    return ab;
}

Obj* Runtime::NewTypedArray(TypedKind kind, size_t length) {
    size_t elSize = ElementSize(kind);
    size_t byteLen = length * elSize;
    Obj* proto = nullptr;
    const wchar_t* ctorName = TypedKindName(kind);
    auto it = ctors.find(ctorName);
    if (it != ctors.end()) {
        Value p = Get(Value::O(it->second), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    if (!proto) proto = typedArrayProto;
    Obj* ta = NewObject(proto);
    ta->cls = Cls::TypedArray;
    ta->X().bufferData = std::make_shared<std::vector<uint8_t>>(byteLen, 0);
    ta->x->byteOffset = 0;
    ta->x->byteLength = byteLen;
    ta->x->typedLength = length;
    ta->x->typedKind = kind;
    return ta;
}

Obj* Runtime::NewTypedArrayFromBuffer(TypedKind kind, Obj* bufferObj, size_t byteOffset, size_t length) {
    size_t elSize = ElementSize(kind);
    size_t byteLen = length * elSize;
    Obj* proto = nullptr;
    const wchar_t* ctorName = TypedKindName(kind);
    auto it = ctors.find(ctorName);
    if (it != ctors.end()) {
        Value p = Get(Value::O(it->second), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    if (!proto) proto = typedArrayProto;
    Obj* ta = NewObject(proto);
    ta->cls = Cls::TypedArray;
    if (bufferObj && bufferObj->x) {
        ta->X().bufferData = bufferObj->x->bufferData;
        ta->x->arrayBufferObj = bufferObj;
    } else {
        ta->X().bufferData = std::make_shared<std::vector<uint8_t>>(byteLen, 0);
    }
    ta->x->byteOffset = byteOffset;
    ta->x->byteLength = byteLen;
    ta->x->typedLength = length;
    ta->x->typedKind = kind;
    return ta;
}

Obj* Runtime::NewFunction(NativeFn fn, const std::wstring& name, int length, bool ctor) {
    Obj* f = NewObject(functionProto);
    f->cls = Cls::Function;
    f->X().native = fn;
    f->x->name = name;
    f->x->ctor = ctor;
    f->x->num = length;
    return f;
}

Obj* Runtime::NewError(const wchar_t* ctorName, const std::wstring& msg) {
    Obj* proto = errorProto;
    auto it = ctors.find(ctorName);
    if (it != ctors.end()) {
        Value p = Get(Value::O(it->second), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* e = NewObject(proto);
    e->cls = Cls::Error;
    DefineData(e, L"message", Value::S(msg), false);
    DefineData(e, L"stack", Value::S(std::wstring(ctorName) + L": " + msg), false);
    return e;
}

void Runtime::Throw(const wchar_t* ctorName, const std::wstring& msg) {
    throw JSThrow{Value::O(NewError(ctorName, msg))};
}

void Runtime::DefineData(Obj* o, const std::wstring& key, const Value& v, bool enumerable) {
    Prop& p = o->props.Put(key);
    p.v = v;
    p.accessor = false;
    p.getter = p.setter = nullptr;
    p.enumerable = enumerable;
    p.writable = true;
}

void Runtime::DefineAccessor(Obj* o, const std::wstring& key, NativeFn getter, NativeFn setter) {
    Prop& p = o->props.Put(key);
    p.accessor = true;
    p.enumerable = false;
    p.getter = getter ? NewFunction(getter, L"get " + key, 0) : nullptr;
    p.setter = setter ? NewFunction(setter, L"set " + key, 1) : nullptr;
}

void Runtime::DefineMethod(Obj* o, const std::wstring& key, NativeFn fn, int length) {
    DefineData(o, key, Value::O(NewFunction(fn, key, length)), false);
}

// ---------------- Donusumler ----------------

bool Runtime::Truthy(const Value& v) const {
    switch (v.t) {
    case Value::Undef: case Value::Null: return false;
    case Value::Bool: return v.b;
    case Value::Num: return !(v.n == 0 || std::isnan(v.n));
    case Value::String: return !v.s->empty();
    default: return true;
    }
}

std::wstring Runtime::NumToStr(double d) {
    if (std::isnan(d)) return L"NaN";
    if (d == 0) return L"0";
    if (std::isinf(d)) return d > 0 ? L"Infinity" : L"-Infinity";
    std::wstring sign = d < 0 ? L"-" : L"";
    double a = std::fabs(d);
    if (a < 1e21 && a == std::floor(a)) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.0f", a);
        return sign + buf;
    }
    // En kisa geri-donusumlu basamaklar
    char buf[64];
    int prec = 1;
    for (; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, a);
        if (strtod(buf, nullptr) == a) break;
    }
    if (prec > 17) prec = 17;
    snprintf(buf, sizeof buf, "%.*e", prec - 1, a);
    std::string m(buf);
    size_t e = m.find('e');
    std::string mant = m.substr(0, e);
    int exp10 = atoi(m.c_str() + e + 1);
    std::string digits;
    for (char c : mant) if (c >= '0' && c <= '9') digits += c;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int k = int(digits.size()), n = exp10 + 1;
    std::string out;
    if (k <= n && n <= 21) { out = digits + std::string(size_t(n - k), '0'); }
    else if (0 < n && n <= 21) { out = digits.substr(0, size_t(n)) + "." + digits.substr(size_t(n)); }
    else if (-6 < n && n <= 0) { out = "0." + std::string(size_t(-n), '0') + digits; }
    else {
        out = digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        out += "e";
        out += (n - 1 >= 0) ? "+" : "-";
        out += std::to_string(std::abs(n - 1));
    }
    return sign + std::wstring(out.begin(), out.end());
}

static double StrToNum(const std::wstring& s0) {
    size_t a = 0, b = s0.size();
    while (a < b && (iswspace(s0[a]) || s0[a] == 0xFEFF || s0[a] == 0xA0)) a++;
    while (b > a && (iswspace(s0[b - 1]) || s0[b - 1] == 0xFEFF || s0[b - 1] == 0xA0)) b--;
    std::wstring s = s0.substr(a, b - a);
    if (s.empty()) return 0;
    if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X' || s[1] == L'o' || s[1] == L'O' || s[1] == L'b' || s[1] == L'B')) {
        int base = (s[1] == L'x' || s[1] == L'X') ? 16 : (s[1] == L'o' || s[1] == L'O') ? 8 : 2;
        double v = 0;
        for (size_t i = 2; i < s.size(); i++) {
            int d = -1;
            wchar_t c = s[i];
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
            if (d < 0 || d >= base) return NAN;
            v = v * base + d;
        }
        return v;
    }
    if (s == L"Infinity" || s == L"+Infinity") return INFINITY;
    if (s == L"-Infinity") return -INFINITY;
    for (wchar_t c : s) if (!((c >= L'0' && c <= L'9') || c == L'.' || c == L'e' || c == L'E' || c == L'+' || c == L'-')) return NAN;
    std::string n(s.begin(), s.end());
    char* end = nullptr;
    double v = strtod(n.c_str(), &end);
    if (end != n.c_str() + n.size()) return NAN;
    return v;
}

Value Runtime::ToPrimitive(const Value& v, int hint) {
    if (!v.IsObj()) return v;
    if (v.o->cls == Cls::Date && hint == 0) hint = 2;
    const wchar_t* order[2] = {L"valueOf", L"toString"};
    if (hint == 2) std::swap(order[0], order[1]);
    for (auto name : order) {
        Value f = Get(v, name);
        if (f.IsObj() && f.o->IsCallable()) {
            std::vector<Value> none;
            Value r = Call(f, v, none);
            if (!r.IsObj()) return r;
        }
    }
    Throw(L"TypeError", L"Cannot convert object to primitive value");
}

double Runtime::ToNum(const Value& v) {
    switch (v.t) {
    case Value::Undef: return NAN;
    case Value::Null: return 0;
    case Value::Bool: return v.b ? 1 : 0;
    case Value::Num: return v.n;
    case Value::String: return StrToNum(*v.s);
    case Value::Symbol: Throw(L"TypeError", L"Cannot convert a Symbol value to a number");
    case Value::Object: return ToNum(ToPrimitive(v, 1));
    }
    return NAN;
}

Str Runtime::ToStrS(const Value& v) {
    if (v.t == Value::String) return v.s;
    return std::make_shared<const std::wstring>(ToStr(v));
}

std::wstring Runtime::ToStr(const Value& v) {
    switch (v.t) {
    case Value::Undef: return L"undefined";
    case Value::Null: return L"null";
    case Value::Bool: return v.b ? L"true" : L"false";
    case Value::Num: return NumToStr(v.n);
    case Value::String: return *v.s;
    case Value::Symbol: Throw(L"TypeError", L"Cannot convert a Symbol value to a string");
    case Value::Object: return ToStr(ToPrimitive(v, 2));
    }
    return L"";
}

std::wstring Runtime::KeyOf(const Value& v) {
    if (v.t == Value::String || v.t == Value::Symbol) return *v.s;
    if (v.t == Value::Num) return NumToStr(v.n);
    if (v.t == Value::Object) { Value p = ToPrimitive(v, 2); return KeyOf(p); }
    return ToStr(v);
}

Obj* Runtime::ToObject(const Value& v) {
    switch (v.t) {
    case Value::Object: return v.o;
    case Value::Undef: case Value::Null: Throw(L"TypeError", L"Cannot convert undefined or null to object");
    case Value::String: { Obj* o = NewObject(stringProto); o->cls = Cls::StrBox; o->X().str = *v.s; return o; }
    case Value::Num: { Obj* o = NewObject(numberProto); o->cls = Cls::NumBox; o->X().num = v.n; return o; }
    case Value::Bool: { Obj* o = NewObject(booleanProto); o->cls = Cls::BoolBox; o->X().num = v.b ? 1 : 0; return o; }
    case Value::Symbol: { Obj* o = NewObject(symbolProto); o->cls = Cls::Symbol_; o->X().str = *v.s; return o; }
    }
    return NewObject();
}

bool Runtime::StrictEq(const Value& a, const Value& b) const {
    if (a.t != b.t) return false;
    switch (a.t) {
    case Value::Undef: case Value::Null: return true;
    case Value::Bool: return a.b == b.b;
    case Value::Num: return a.n == b.n;
    case Value::String: case Value::Symbol: return a.s == b.s || *a.s == *b.s;
    case Value::Object: return a.o == b.o;
    }
    return false;
}

bool Runtime::SameValueZero(const Value& a, const Value& b) const {
    if (a.t == Value::Num && b.t == Value::Num && std::isnan(a.n) && std::isnan(b.n)) return true;
    return StrictEq(a, b);
}

bool Runtime::LooseEq(const Value& a, const Value& b) {
    if (a.t == b.t) return StrictEq(a, b);
    if (a.IsNullish() && b.IsNullish()) return true;
    if (a.IsNullish() || b.IsNullish()) return false;
    if (a.t == Value::Num && b.t == Value::String) return a.n == ToNum(b);
    if (a.t == Value::String && b.t == Value::Num) return ToNum(a) == b.n;
    if (a.t == Value::Bool) return LooseEq(Value::N(a.b ? 1 : 0), b);
    if (b.t == Value::Bool) return LooseEq(a, Value::N(b.b ? 1 : 0));
    if ((a.t == Value::Num || a.t == Value::String || a.t == Value::Symbol) && b.IsObj()) return LooseEq(a, ToPrimitive(b, 0));
    if (a.IsObj() && (b.t == Value::Num || b.t == Value::String || b.t == Value::Symbol)) return LooseEq(ToPrimitive(a, 0), b);
    return false;
}

std::wstring Runtime::TypeOf(const Value& v) const {
    switch (v.t) {
    case Value::Undef: return L"undefined";
    case Value::Null: return L"object";
    case Value::Bool: return L"boolean";
    case Value::Num: return L"number";
    case Value::String: return L"string";
    case Value::Symbol: return L"symbol";
    case Value::Object: return v.o->IsCallable() ? L"function" : L"object";
    }
    return L"undefined";
}

bool Runtime::InstanceOf(const Value& v, const Value& c) {
    if (!c.IsObj() || !c.o->IsCallable()) Throw(L"TypeError", L"Right-hand side of 'instanceof' is not callable");
    Obj* f = c.o;
    while (f->x && f->x->boundTarget) f = f->x->boundTarget;
    if (!v.IsObj()) return false;
    Value p = Get(Value::O(f), L"prototype");
    if (!p.IsObj()) return false;
    int guard = 0;
    for (Obj* o = v.o->proto; o && guard < 100000; o = o->proto, guard++) if (o == p.o) return true;
    return false;
}

std::wstring Runtime::MapKey(const Value& v) {
    switch (v.t) {
    case Value::Undef: return L"u";
    case Value::Null: return L"n";
    case Value::Bool: return v.b ? L"t" : L"f";
    case Value::Num: return std::isnan(v.n) ? L"dNaN" : L"d" + NumToStr(v.n == 0 ? 0 : v.n);
    case Value::String: return L"s" + *v.s;
    case Value::Symbol: return L"y" + *v.s;
    case Value::Object: { wchar_t buf[32]; swprintf(buf, 32, L"o%p", static_cast<void*>(v.o)); return buf; }
    }
    return L"?";
}

// ---------------- Ozellik erisimi ----------------

static bool ParseIndex(const std::wstring& k, uint32_t& out) {
    if (k.empty() || k.size() > 10) return false;
    if (k.size() > 1 && k[0] == L'0') return false;
    uint64_t v = 0;
    for (wchar_t c : k) { if (c < L'0' || c > L'9') return false; v = v * 10 + uint64_t(c - L'0'); }
    if (v >= 4294967295ULL) return false;
    out = uint32_t(v);
    return true;
}

static bool NumIndex(const Value& k, uint32_t& out) {
    if (k.t != Value::Num) return false;
    double d = k.n;
    if (!(d >= 0) || d >= 4294967295.0 || d != std::floor(d)) return false;
    out = uint32_t(d);
    return true;
}

static Value GetChain(Runtime& rt, Obj* start, const std::wstring& key, const Value& recv) {
    int guard = 0;
    for (Obj* p = start; p && guard < 100000; p = p->proto, guard++) {
        if (p->cls == Cls::Array || p->cls == Cls::Arguments) {
            if (key == L"length") return Value::N(double(p->elems.size()));
            uint32_t idx;
            if (ParseIndex(key, idx)) {
                if (idx < p->elems.size()) return p->elems[idx];
                continue;
            }
        } else if (p->cls == Cls::TypedArray && p->x) {
            if (key == L"length") return Value::N(double(p->x->typedLength));
            if (key == L"byteLength") return Value::N(double(p->x->byteLength));
            if (key == L"byteOffset") return Value::N(double(p->x->byteOffset));
            if (key == L"buffer") {
                if (!p->x->arrayBufferObj) {
                    Obj* ab = rt.NewObject(rt.arrayBufferProto);
                    ab->cls = Cls::ArrayBuffer;
                    ab->X().bufferData = p->x->bufferData;
                    ab->x->byteLength = p->x->bufferData ? p->x->bufferData->size() : 0;
                    p->x->arrayBufferObj = ab;
                }
                return Value::O(p->x->arrayBufferObj);
            }
            uint32_t idx;
            if (ParseIndex(key, idx)) {
                if (idx < p->x->typedLength) return GetTypedElement(*p->x, idx);
                continue;
            }
        } else if (p->cls == Cls::ArrayBuffer && p->x) {
            if (key == L"byteLength") return Value::N(double(p->x->byteLength));
        } else if (p->cls == Cls::DataView && p->x) {
            if (key == L"byteLength") return Value::N(double(p->x->byteLength));
            if (key == L"byteOffset") return Value::N(double(p->x->byteOffset));
            if (key == L"buffer") return p->x->arrayBufferObj ? Value::O(p->x->arrayBufferObj) : Value();
        } else if (p->cls == Cls::StrBox && p->x) {
            if (key == L"length") return Value::N(double(p->x->str.size()));
            uint32_t idx;
            if (ParseIndex(key, idx) && idx < p->x->str.size()) return Value::S(std::wstring(1, p->x->str[idx]));
        }
        if (rt.exoticGet && p->cls >= Cls::StyleDecl && p->cls <= Cls::Storage) {
            Value v;
            if (rt.exoticGet(rt, p, key, v)) return v;
        }
        if (Prop* pr = p->props.Find(key)) {
            if (pr->accessor) {
                if (!pr->getter) return Value();
                std::vector<Value> none;
                return rt.Call(Value::O(pr->getter), recv, none);
            }
            return pr->v;
        }
        if (p->cls == Cls::Function && p->x) {
            if (key == L"prototype" && p->x->fn && p->x->ctor) {
                Obj* proto = rt.NewObject();
                rt.DefineData(proto, L"constructor", Value::O(p), false);
                Prop& pp = p->props.Put(L"prototype");
                pp.v = Value::O(proto);
                pp.enumerable = false;
                return pp.v;
            }
            if (key == L"name") return Value::S(p->x->name);
            if (key == L"length") {
                if (p->x->fn && p->x->fn->fi) {
                    int n = 0;
                    for (auto* pa : p->x->fn->fi->params) { if (pa->k == K::Ident || pa->k == K::ObjPat || pa->k == K::ArrPat) n++; else break; }
                    return Value::N(n);
                }
                return Value::N(p->x->num);
            }
        }
    }
    return Value();
}

Value Runtime::GetFrom(Obj* start, const std::wstring& key, const Value& recv) { return GetChain(*this, start, key, recv); }

Value Runtime::Get(const Value& o, const std::wstring& key) {
    switch (o.t) {
    case Value::Undef:
    case Value::Null:
        Throw(L"TypeError", L"Cannot read properties of " + std::wstring(o.t == Value::Undef ? L"undefined" : L"null") + L" (reading '" + key + L"')");
    case Value::String: {
        if (key == L"length") return Value::N(double(o.s->size()));
        uint32_t idx;
        if (ParseIndex(key, idx)) return idx < o.s->size() ? Value::S(std::wstring(1, (*o.s)[idx])) : Value();
        return GetChain(*this, stringProto, key, o);
    }
    case Value::Num: return GetChain(*this, numberProto, key, o);
    case Value::Bool: return GetChain(*this, booleanProto, key, o);
    case Value::Symbol:
        if (key == L"description") { size_t p = o.s->find(L':', 1); return Value::S(p == std::wstring::npos ? L"" : o.s->substr(p + 1)); }
        return GetChain(*this, symbolProto, key, o);
    case Value::Object: return GetChain(*this, o.o, key, o);
    }
    return Value();
}

Value Runtime::GetV(const Value& o, const Value& key) {
    uint32_t idx;
    if (NumIndex(key, idx)) {
        if (o.IsObj() && (o.o->cls == Cls::Array || o.o->cls == Cls::Arguments) && idx < o.o->elems.size()) return o.o->elems[idx];
        if (o.IsObj() && o.o->cls == Cls::TypedArray && o.o->x && idx < o.o->x->typedLength) return GetTypedElement(*o.o->x, idx);
        if (o.IsStr()) return idx < o.s->size() ? Value::S(std::wstring(1, (*o.s)[idx])) : Value();
    }
    if (o.IsNullish()) Get(o, key.t == Value::String ? *key.s : L"?");
    return Get(o, KeyOf(key));
}

static void ArraySetLength(Runtime& rt, Obj* a, const Value& v) {
    double n = rt.ToNum(v);
    if (!(n >= 0) || n != std::floor(n) || n > 50000000) rt.Throw(L"RangeError", L"Invalid array length");
    a->elems.resize(size_t(n));
}

void Runtime::Put(const Value& o, const std::wstring& key, const Value& v) {
    if (o.IsNullish()) Throw(L"TypeError", L"Cannot set properties of " + std::wstring(o.t == Value::Undef ? L"undefined" : L"null") + L" (setting '" + key + L"')");
    if (!o.IsObj()) return;
    Obj* obj = o.o;
    if (obj->frozen) return;
    if (obj->cls == Cls::Array) {
        if (key == L"length") { ArraySetLength(*this, obj, v); return; }
        uint32_t idx;
        if (ParseIndex(key, idx)) {
            if (idx < obj->elems.size()) { obj->elems[idx] = v; return; }
            if (idx < obj->elems.size() + 1000000 && idx < 50000000) { obj->elems.resize(size_t(idx) + 1); obj->elems[idx] = v; return; }
            Throw(L"RangeError", L"Invalid array length");
        }
    } else if (obj->cls == Cls::Arguments) {
        uint32_t idx;
        if (ParseIndex(key, idx) && idx < obj->elems.size()) { obj->elems[idx] = v; return; }
    } else if (obj->cls == Cls::TypedArray && obj->x) {
        uint32_t idx;
        if (ParseIndex(key, idx)) {
            if (idx < obj->x->typedLength) SetTypedElement(*this, *obj->x, idx, v);
            return;
        }
        if (key == L"length" || key == L"byteLength" || key == L"byteOffset" || key == L"buffer") return;
    } else if (obj->cls == Cls::ArrayBuffer && obj->x) {
        if (key == L"byteLength") return;
    } else if (obj->cls == Cls::DataView && obj->x) {
        if (key == L"byteLength" || key == L"byteOffset" || key == L"buffer") return;
    }
    if (exoticSet && obj->cls >= Cls::StyleDecl && obj->cls <= Cls::Storage && exoticSet(*this, obj, key, v)) return;
    int guard = 0;
    for (Obj* p = obj; p && guard < 100000; p = p->proto, guard++) {
        if (Prop* pr = p->props.Find(key)) {
            if (pr->accessor) {
                if (pr->setter) { std::vector<Value> a{v}; Call(Value::O(pr->setter), o, a); }
                return;
            }
            if (p == obj) { if (pr->writable) pr->v = v; return; }
            if (!pr->writable) return;
            break;
        }
    }
    if (!obj->extensible) return;
    if (obj->props.Size() > 2000000) throw JSAbort{L"nesne cok buyuk"};
    Prop& np = obj->props.Put(key);
    np.v = v;
}

void Runtime::PutV(const Value& o, const Value& key, const Value& v) {
    uint32_t idx;
    if (o.IsObj() && o.o->cls == Cls::Array && !o.o->frozen && NumIndex(key, idx)) {
        Obj* a = o.o;
        if (idx < a->elems.size()) { a->elems[idx] = v; return; }
        if (idx < a->elems.size() + 1000000 && idx < 50000000) { a->elems.resize(size_t(idx) + 1); a->elems[idx] = v; return; }
    }
    if (o.IsObj() && o.o->cls == Cls::TypedArray && !o.o->frozen && NumIndex(key, idx)) {
        if (o.o->x && idx < o.o->x->typedLength) SetTypedElement(*this, *o.o->x, idx, v);
        return;
    }
    Put(o, KeyOf(key), v);
}

bool Runtime::HasOwn(Obj* o, const std::wstring& key) {
    if (o->cls == Cls::Array || o->cls == Cls::Arguments) {
        if (key == L"length") return true;
        uint32_t idx;
        if (ParseIndex(key, idx)) return idx < o->elems.size();
    }
    if (o->cls == Cls::TypedArray && o->x) {
        if (key == L"length" || key == L"byteLength" || key == L"byteOffset" || key == L"buffer") return true;
        uint32_t idx;
        if (ParseIndex(key, idx)) return idx < o->x->typedLength;
    }
    if (o->cls == Cls::ArrayBuffer && o->x) {
        if (key == L"byteLength") return true;
    }
    if (o->cls == Cls::DataView && o->x) {
        if (key == L"byteLength" || key == L"byteOffset" || key == L"buffer") return true;
    }
    if (o->cls == Cls::StrBox && o->x) {
        if (key == L"length") return true;
        uint32_t idx;
        if (ParseIndex(key, idx)) return idx < o->x->str.size();
    }
    if (exoticGet && o->cls >= Cls::StyleDecl && o->cls <= Cls::Storage) { Value v; if (exoticGet(*this, o, key, v)) return !v.IsUndef(); }
    return o->props.Find(key) != nullptr;
}

bool Runtime::HasProperty(Obj* o, const std::wstring& key) {
    int guard = 0;
    for (Obj* p = o; p && guard < 100000; p = p->proto, guard++) {
        if (HasOwn(p, key)) return true;
        if (p->cls == Cls::Function && p->x && (key == L"prototype" || key == L"name" || key == L"length")) return true;
    }
    return false;
}

bool Runtime::Delete(Obj* o, const std::wstring& key) {
    if (o->frozen) return false;
    if (o->cls == Cls::Array) {
        uint32_t idx;
        if (ParseIndex(key, idx)) {
            if (idx < o->elems.size()) {
                if (idx + 1 == o->elems.size()) o->elems[idx] = Value();
                else o->elems[idx] = Value();
            }
            return true;
        }
    }
    if (exoticSet && o->cls >= Cls::StyleDecl && o->cls <= Cls::Storage) { Value u; exoticSet(*this, o, L"\x02del:" + key, u); }
    o->props.Erase(key);
    return true;
}

std::vector<std::wstring> Runtime::OwnKeys(Obj* o, bool onlyEnum) {
    std::vector<std::wstring> keys;
    if (o->cls == Cls::Array || o->cls == Cls::Arguments) {
        for (size_t i = 0; i < o->elems.size(); i++) keys.push_back(std::to_wstring(i));
    } else if (o->cls == Cls::TypedArray && o->x) {
        for (size_t i = 0; i < o->x->typedLength; i++) keys.push_back(std::to_wstring(i));
    } else if (o->cls == Cls::StrBox && o->x) {
        for (size_t i = 0; i < o->x->str.size(); i++) keys.push_back(std::to_wstring(i));
    } else if (o->cls == Cls::Dataset || o->cls == Cls::Storage || o->cls == Cls::StyleDecl) {
        Value v;
        if (exoticGet && exoticGet(*this, o, L"\x02keys", v) && v.IsObj())
            for (auto& e : v.o->elems) if (e.IsStr()) keys.push_back(*e.s);
    }
    // Dizin gibi gorunen anahtarlar once (sayisal sirayla)
    std::vector<std::pair<uint32_t, std::wstring>> ints;
    std::vector<std::wstring> strs;
    o->props.ForEach([&](const std::wstring& k, const Prop& p) {
        if (onlyEnum && !p.enumerable) return;
        if (!k.empty() && k[0] == L'\x01') return; // semboller
        uint32_t idx;
        if (ParseIndex(k, idx)) ints.emplace_back(idx, k);
        else strs.push_back(k);
    });
    std::sort(ints.begin(), ints.end());
    for (auto& i : ints) keys.push_back(i.second);
    for (auto& s : strs) keys.push_back(s);
    return keys;
}

// ---------------- Cagri ----------------

Value Runtime::Call(const Value& f, const Value& thisV, std::vector<Value>& args) {
    if (!f.IsObj() || !f.o->IsCallable()) Throw(L"TypeError", TypeOf(f) == L"undefined" ? L"undefined is not a function" : ToStr(f.IsObj() ? Value::S(L"object") : f) + L" is not a function");
    Obj* fo = f.o;
    if (fo->x && fo->x->boundTarget) {
        std::vector<Value> a = fo->x->boundArgs;
        a.insert(a.end(), args.begin(), args.end());
        return Call(Value::O(fo->x->boundTarget), fo->x->boundThis, a);
    }
    if (++steps > stepBudget) throw JSAbort{L"betik çok uzun sürdü"};
    if (fo->x && fo->x->native) {
        CheckStack();
        Obj* saved = nativeNewTarget;
        nativeNewTarget = nullptr;
        struct R { Obj*& r; Obj* v; ~R() { r = v; } } restore{nativeNewTarget, saved};
        return fo->x->native(*this, fo, thisV, args);
    }
    if (fo->x && fo->x->classKind) Throw(L"TypeError", L"Class constructor " + fo->x->name + L" cannot be invoked without 'new'");
    if (!fo->x || !fo->x->fn) return Value();
    return CallUser(fo, thisV, args, nullptr);
}

std::vector<Value> Runtime::IterateToList(const Value& it, size_t limit) {
    std::vector<Value> out;
    if (it.IsStr()) {
        const std::wstring& s = *it.s;
        for (size_t i = 0; i < s.size(); i++) {
            if (sizeof(wchar_t) == 2 && s[i] >= 0xD800 && s[i] <= 0xDBFF && i + 1 < s.size()) { out.push_back(Value::S(s.substr(i, 2))); i++; }
            else out.push_back(Value::S(std::wstring(1, s[i])));
        }
        return out;
    }
    if (it.IsObj()) {
        Obj* o = it.o;
        if (o->cls == Cls::Array && Get(Value::O(arrayProto), L"\x01Symbol.iterator").o == Get(it, L"\x01Symbol.iterator").o) return o->elems;
        if (o->cls == Cls::Arguments) return o->elems;
        if (o->cls == Cls::TypedArray && o->x) {
            for (size_t i = 0; i < o->x->typedLength; i++) out.push_back(GetTypedElement(*o->x, i));
            return out;
        }
        if ((o->cls == Cls::Map || o->cls == Cls::Set) && o->x) {
            for (size_t i = 0; i < o->x->entries.size(); i++) {
                if (o->x->dead[i]) continue;
                if (o->cls == Cls::Set) out.push_back(o->x->entries[i].first);
                else out.push_back(Value::O(NewArrayFrom({o->x->entries[i].first, o->x->entries[i].second})));
            }
            return out;
        }
    }
    if (it.IsNullish()) Throw(L"TypeError", ToStr(it) + L" is not iterable");
    Value fn = Get(it, L"\x01Symbol.iterator");
    if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", L"object is not iterable");
    std::vector<Value> none;
    Value iter = Call(fn, it, none);
    Value next = Get(iter, L"next");
    while (out.size() < limit) {
        std::vector<Value> n2;
        Value r = Call(next, iter, n2);
        if (!r.IsObj()) Throw(L"TypeError", L"Iterator result is not an object");
        if (Truthy(Get(r, L"done"))) break;
        out.push_back(Get(r, L"value"));
    }
    return out;
}

// ---------------- Promise ve gorev kuyrugu ----------------

void Runtime::Enqueue(const Value& fn, std::vector<Value> args) {
    if (microtasks.size() > 2000000) throw JSAbort{L"mikro görev kuyruğu taştı"};
    microtasks.emplace_back(fn, std::move(args));
}

Obj* Runtime::NewPromise() {
    Obj* p = NewObject(promiseProto);
    p->cls = Cls::Promise;
    p->X();
    return p;
}

static Value ReactionJob(Runtime& rt, Obj* self, const Value&, std::vector<Value>&) {
    // slots: 0 handler, 1 derived, 2 value, 3 reddedildi mi
    Value handler = self->x->slots[0];
    Obj* derived = self->x->slots[1].o;
    Value v = self->x->slots[2];
    bool rejected = self->x->slots[3].b;
    if (handler.IsObj() && handler.o->IsCallable()) {
        try {
            std::vector<Value> a{v};
            Value r = rt.Call(handler, Value(), a);
            if (derived) rt.ResolvePromise(derived, r);
        } catch (JSThrow& t) {
            if (derived) rt.RejectPromise(derived, t.v);
        }
    } else if (derived) {
        if (rejected) rt.RejectPromise(derived, v);
        else rt.ResolvePromise(derived, v);
    }
    return Value();
}

static void TriggerReactions(Runtime& rt, Obj* p) {
    auto reactions = std::move(p->x->reactions);
    p->x->reactions.clear();
    for (auto& r : reactions) {
        Obj* job = rt.NewFunction(ReactionJob, L"", 0);
        job->x->slots = {p->x->pstate == 1 ? r.onFul : r.onRej, Value::O(r.derived), p->x->pvalue, Value::B(p->x->pstate == 2)};
        rt.Enqueue(Value::O(job), {});
    }
}

Value PromiseResolveFnImpl(Runtime& rt, Obj* self, const Value&, std::vector<Value>& args) {
    Obj* p = self->x->slots[0].o;
    Obj* flag = self->x->slots[1].o;
    if (flag->x->num) return Value();
    flag->x->num = 1;
    rt.ResolvePromise(p, args.empty() ? Value() : args[0]);
    return Value();
}
Value PromiseRejectFnImpl(Runtime& rt, Obj* self, const Value&, std::vector<Value>& args) {
    Obj* p = self->x->slots[0].o;
    Obj* flag = self->x->slots[1].o;
    if (flag->x->num) return Value();
    flag->x->num = 1;
    rt.RejectPromise(p, args.empty() ? Value() : args[0]);
    return Value();
}

static Value ThenableJob(Runtime& rt, Obj* self, const Value&, std::vector<Value>&) {
    Obj* p = self->x->slots[0].o;
    Value thenable = self->x->slots[1], then = self->x->slots[2];
    Obj* flag = rt.NewObject();
    flag->X().num = 0;
    Obj* res = rt.NewFunction(PromiseResolveFnImpl, L"", 1);
    res->x->slots = {Value::O(p), Value::O(flag)};
    Obj* rej = rt.NewFunction(PromiseRejectFnImpl, L"", 1);
    rej->x->slots = {Value::O(p), Value::O(flag)};
    try {
        std::vector<Value> a{Value::O(res), Value::O(rej)};
        rt.Call(then, thenable, a);
    } catch (JSThrow& t) {
        if (!flag->x->num) { flag->x->num = 1; rt.RejectPromise(p, t.v); }
    }
    return Value();
}

void Runtime::ResolvePromise(Obj* p, const Value& v) {
    if (!p->x || p->x->pstate != 0) return;
    if (v.IsObj() && v.o == p) { RejectPromise(p, Value::O(NewError(L"TypeError", L"Chaining cycle detected for promise"))); return; }
    if (v.IsObj()) {
        Value then;
        try { then = Get(v, L"then"); } catch (JSThrow& t) { RejectPromise(p, t.v); return; }
        if (then.IsObj() && then.o->IsCallable()) {
            Obj* job = NewFunction(ThenableJob, L"", 0);
            job->x->slots = {Value::O(p), v, then};
            Enqueue(Value::O(job), {});
            return;
        }
    }
    p->x->pstate = 1;
    p->x->pvalue = v;
    TriggerReactions(*this, p);
}

void Runtime::RejectPromise(Obj* p, const Value& v) {
    if (!p->x || p->x->pstate != 0) return;
    p->x->pstate = 2;
    p->x->pvalue = v;
    if (p->x->reactions.empty() && !p->x->handled) unhandled.push_back(p);
    TriggerReactions(*this, p);
}

void Runtime::PromiseThen(Obj* p, const Value& onFul, const Value& onRej, Obj* derived) {
    p->x->handled = true;
    Reaction r{onFul, onRej, derived};
    if (p->x->pstate == 0) { p->x->reactions.push_back(r); return; }
    Obj* job = NewFunction(ReactionJob, L"", 0);
    job->x->slots = {p->x->pstate == 1 ? onFul : onRej, Value::O(derived), p->x->pvalue, Value::B(p->x->pstate == 2)};
    Enqueue(Value::O(job), {});
}

void Runtime::RunMicrotasks() {
    size_t guard = 0;
    while (!microtasks.empty() && guard++ < 5000000) {
        std::vector<std::pair<Value, std::vector<Value>>> batch;
        batch.swap(microtasks);
        for (size_t i = 0; i < batch.size(); i++) {
            try {
                Call(batch[i].first, Value(), batch[i].second);
            } catch (JSThrow& t) {
                Report(L"mikro görev", t);
            } catch (JSAbort&) {
                // kalan isleri birak ve yeniden firlat
                for (size_t k = i + 1; k < batch.size(); k++) microtasks.push_back(batch[k]);
                throw;
            }
        }
    }
    for (Obj* p : unhandled) {
        if (p->x && !p->x->handled && p->x->pstate == 2) {
            JSThrow t{p->x->pvalue};
            Report(L"Yakalanmamış Promise reddi", t);
        }
    }
    unhandled.clear();
}

// ---------------- Gorevler, zamanlayicilar, GC ----------------

void Runtime::BeginTask() {
    steps = 0;
    depth = 0;
    char probe = 0;
    stackBase = reinterpret_cast<uintptr_t>(&probe);
    inTask = true;
}

void Runtime::EndTask() {
    inTask = false;
    stackBase = 0;
}

void Runtime::Report(const std::wstring& where, const JSThrow& t) {
    std::wstring msg;
    try {
        if (t.v.IsObj() && t.v.o->cls == Cls::Error) {
            std::wstring name = ToStr(Get(t.v, L"name"));
            std::wstring m = ToStr(Get(t.v, L"message"));
            msg = name + L": " + m;
        } else msg = L"Uncaught " + Inspect(t.v);
    } catch (...) {
        msg = L"Uncaught (hata)";
    }
    std::wstring line = L"[" + where + L"] " + msg;
    if (consoleLines.size() < 5000) consoleLines.push_back(L"✖ " + line);
    if (host) host->Log(2, line);
}

double Runtime::NextTimerDue() const {
    double best = -1;
    for (auto& t : timers) if (best < 0 || t.due < best) best = t.due;
    return best;
}

void Runtime::RunDueTimers(double now) {
    // her cagrida en fazla 64 zamanlayici (sonsuz dongu koruma)
    for (int round = 0; round < 64; round++) {
        size_t pick = size_t(-1);
        for (size_t i = 0; i < timers.size(); i++)
            if (timers[i].due <= now && (pick == size_t(-1) || timers[i].due < timers[pick].due)) pick = i;
        if (pick == size_t(-1)) break;
        Timer t = timers[pick];
        if (t.interval >= 0) timers[pick].due = now + std::max(t.interval, 4.0);
        else timers.erase(timers.begin() + long(pick));
        BeginTask();
        try {
            if (t.fn.IsObj() && t.fn.o->IsCallable()) {
                std::vector<Value> a = t.args;
                Call(t.fn, Value(), a);
            } else if (t.fn.IsStr()) {
                EndTask();
                RunScript(*t.fn.s, L"setTimeout");
                BeginTask();
            }
            RunMicrotasks();
        } catch (JSThrow& e) {
            Report(L"zamanlayıcı", e);
        } catch (JSAbort& a) {
            if (host) host->Log(2, L"Zamanlayıcı durduruldu: " + a.reason);
            timers.clear();
        }
        EndTask();
    }
    MaybeGC();
}

void Runtime::MaybeGC() {
    if (inTask || !microtasks.empty()) return;
    if (cells_.size() < 100000 || cells_.size() < lastGcCells_ * 2) return;
    Collect();
}

void Runtime::Collect() {
    std::vector<Cell*> work;
    auto root = [&](Cell* c) { if (c) work.push_back(c); };
    Obj* protos[] = {objectProto, functionProto, arrayProto, stringProto, numberProto, booleanProto, errorProto,
                     regexpProto, dateProto, promiseProto, mapProto, setProto, symbolProto, iteratorProto,
                     arrayBufferProto, typedArrayProto, dataViewProto,
                     eventTargetProto, nodeProto, elementProto, textProto, documentProto, styleProto, classListProto, eventProto, storageProto,
                     mediaSourceProto, sourceBufferProto, blobProto, global};
    for (Obj* p : protos) root(p);
    root(globalEnv);
    for (auto& c : ctors) root(c.second);
    for (auto& w : wrappers) root(w.second);
    for (auto& u : objectUrls) root(u.second);
    for (auto& t : timers) { PushV(work, t.fn); for (auto& a : t.args) PushV(work, a); }
    for (auto& m : microtasks) { PushV(work, m.first); for (auto& a : m.second) PushV(work, a); }
    for (Obj* p : unhandled) root(p);
    for (auto& m : modules) {
        if (m.second) {
            root(m.second->namespaceObj);
            root(m.second->env);
            for (auto& exp : m.second->exports) PushV(work, exp.second);
        }
    }
    while (!work.empty()) {
        Cell* c = work.back();
        work.pop_back();
        if (c->marked) continue;
        c->marked = true;
        c->Trace(work);
    }
    size_t w = 0;
    for (size_t i = 0; i < cells_.size(); i++) {
        Cell* c = cells_[i];
        if (c->marked) { c->marked = false; cells_[w++] = c; }
        else delete c;
    }
    cells_.resize(w);
    lastGcCells_ = w;
}

// ---------------- Betik calistirma ----------------

bool Runtime::RunScript(const std::wstring& src, const std::wstring& name, Node* scriptNode) {
    std::unique_ptr<Script> sc(new Script());
    sc->name = name;
    try {
        ParseProgram(src, *sc);
    } catch (SyntaxErr& e) {
        std::wstring m = L"[" + name + L":" + std::to_wstring(e.line) + L"] SyntaxError: " + e.msg;
        if (consoleLines.size() < 5000) consoleLines.push_back(L"✖ " + m);
        if (host) host->Log(2, m);
        return false;
    }
    Script* s = sc.get();
    scripts_.push_back(std::move(sc));
    Node* savedScript = currentScript;
    currentScript = scriptNode;
    bool nested = inTask;
    if (!nested) BeginTask();
    bool ok = true;
    try {
        EvalAst(s->root, globalEnv);
        if (!nested) RunMicrotasks();
    } catch (JSThrow& t) {
        Report(name, t);
        ok = false;
    } catch (JSAbort& a) {
        std::wstring m = L"[" + name + L"] Betik durduruldu: " + a.reason;
        if (consoleLines.size() < 5000) consoleLines.push_back(L"✖ " + m);
        if (host) host->Log(2, m);
        microtasks.clear();
        ok = false;
        if (nested) { currentScript = savedScript; throw; }
    }
    currentScript = savedScript;
    if (!nested) { EndTask(); MaybeGC(); }
    return ok;
}

static bool ReadModuleSource(const std::wstring& url, std::wstring& outText) {
    if (url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0) {
        FetchResult res = Fetch(url, 4 * 1024 * 1024);
        if (res.ok && res.status < 400) {
            outText = DecodeBody(res.body, res.contentType);
            return true;
        }
        return false;
    }
    std::wstring filePath = url;
    if (filePath.rfind(L"file:///", 0) == 0) {
        filePath = filePath.substr(8);
    } else if (filePath.rfind(L"file://", 0) == 0) {
        filePath = filePath.substr(7);
    }
    for (auto& ch : filePath) {
        if (ch == L'/') ch = L'\\';
    }
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(hFile, nullptr);
    if (sz == INVALID_FILE_SIZE || sz > 8 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }
    std::string buf(sz, '\0');
    DWORD read = 0;
    if (!ReadFile(hFile, &buf[0], sz, &read, nullptr) || read != sz) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);
    outText = FromUtf8(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    return true;
}

std::shared_ptr<ModuleRecord> Runtime::LoadModuleInternal(const std::wstring& url, const std::wstring& referrer) {
    std::wstring resolved;
    if (referrer.empty()) {
        if (doc && host) resolved = host->Resolve(url);
        else resolved = url;
    } else {
        resolved = ResolveUrl(referrer, url);
    }

    auto it = modules.find(resolved);
    if (it != modules.end()) {
        return it->second;
    }

    auto mod = std::make_shared<ModuleRecord>();
    mod->url = resolved;
    mod->namespaceObj = NewObject(nullptr);
    mod->env = Alloc<Env>();
    mod->env->parent = globalEnv;
    modules[resolved] = mod;

    std::wstring srcText;
    if (!ReadModuleSource(resolved, srcText)) {
        Throw(L"TypeError", L"Failed to fetch module: " + resolved);
    }

    std::unique_ptr<Script> sc(new Script());
    sc->name = resolved;
    try {
        ParseProgram(srcText, *sc);
    } catch (SyntaxErr& e) {
        Throw(L"SyntaxError", e.msg + L" (" + resolved + L":" + std::to_wstring(e.line) + L")");
    }

    Script* s = sc.get();
    scripts_.push_back(std::move(sc));

    auto savedMod = currentModule;
    currentModule = mod;
    mod->evaluating = true;
    try {
        EvalAst(s->root, mod->env);
        mod->evaluated = true;
        mod->evaluating = false;
        for (const auto& exp : mod->exports) {
            DefineData(mod->namespaceObj, exp.first, exp.second, true);
        }
    } catch (...) {
        mod->evaluating = false;
        currentModule = savedMod;
        throw;
    }
    currentModule = savedMod;
    return mod;
}

std::shared_ptr<ModuleRecord> Runtime::LoadModule(const std::wstring& url, Node* scriptNode) {
    Node* savedScript = currentScript;
    currentScript = scriptNode;
    bool nested = inTask;
    if (!nested) BeginTask();
    std::shared_ptr<ModuleRecord> res;
    try {
        res = LoadModuleInternal(url, host ? host->PageUrl() : L"");
        if (!nested) RunMicrotasks();
    } catch (JSThrow& t) {
        Report(url, t);
    } catch (JSAbort& a) {
        std::wstring m = L"[" + url + L"] Modül durduruldu: " + a.reason;
        if (consoleLines.size() < 5000) consoleLines.push_back(L"✖ " + m);
        if (host) host->Log(2, m);
        microtasks.clear();
        if (nested) { currentScript = savedScript; throw; }
    }
    currentScript = savedScript;
    if (!nested) { EndTask(); MaybeGC(); }
    return res;
}

bool Runtime::RunModule(const std::wstring& src, const std::wstring& url, Node* scriptNode) {
    std::wstring modUrl = url.empty() ? (host ? host->PageUrl() : L"inline:module") : url;
    auto it = modules.find(modUrl);
    std::shared_ptr<ModuleRecord> mod;
    if (it != modules.end()) {
        mod = it->second;
    } else {
        mod = std::make_shared<ModuleRecord>();
        mod->url = modUrl;
        mod->namespaceObj = NewObject(nullptr);
        mod->env = Alloc<Env>();
        mod->env->parent = globalEnv;
        modules[modUrl] = mod;
    }

    std::unique_ptr<Script> sc(new Script());
    sc->name = modUrl;
    try {
        ParseProgram(src, *sc);
    } catch (SyntaxErr& e) {
        std::wstring m = L"[" + modUrl + L":" + std::to_wstring(e.line) + L"] SyntaxError: " + e.msg;
        if (consoleLines.size() < 5000) consoleLines.push_back(L"✖ " + m);
        if (host) host->Log(2, m);
        return false;
    }
    Script* s = sc.get();
    scripts_.push_back(std::move(sc));

    Node* savedScript = currentScript;
    currentScript = scriptNode;
    auto savedMod = currentModule;
    currentModule = mod;
    bool nested = inTask;
    if (!nested) BeginTask();
    bool ok = true;
    try {
        mod->evaluating = true;
        EvalAst(s->root, mod->env);
        mod->evaluated = true;
        mod->evaluating = false;
        for (const auto& exp : mod->exports) {
            DefineData(mod->namespaceObj, exp.first, exp.second, true);
        }
        if (!nested) RunMicrotasks();
    } catch (JSThrow& t) {
        Report(modUrl, t);
        ok = false;
    } catch (JSAbort& a) {
        std::wstring m = L"[" + modUrl + L"] Modül durduruldu: " + a.reason;
        if (consoleLines.size() < 5000) consoleLines.push_back(L"✖ " + m);
        if (host) host->Log(2, m);
        microtasks.clear();
        ok = false;
        if (nested) { currentScript = savedScript; currentModule = savedMod; throw; }
    }
    currentScript = savedScript;
    currentModule = savedMod;
    if (!nested) { EndTask(); MaybeGC(); }
    return ok;
}

Value Runtime::EvalGlobal(const std::wstring& src, const std::wstring& name) {
    std::unique_ptr<Script> sc(new Script());
    sc->name = name;
    try {
        ParseProgram(src, *sc);
    } catch (SyntaxErr& e) {
        Throw(L"SyntaxError", e.msg + L" (" + name + L":" + std::to_wstring(e.line) + L")");
    }
    Script* s = sc.get();
    scripts_.push_back(std::move(sc));
    return EvalAst(s->root, globalEnv);
}

bool Runtime::EvalForTest(const std::wstring& src, std::wstring& result) {
    std::unique_ptr<Script> sc(new Script());
    sc->name = L"test";
    try {
        ParseProgram(src, *sc);
    } catch (SyntaxErr& e) {
        result = L"SyntaxError: " + e.msg + L" (satır " + std::to_wstring(e.line) + L")";
        return false;
    }
    Script* s = sc.get();
    scripts_.push_back(std::move(sc));
    BeginTask();
    bool ok = true;
    auto savedMod = currentModule;
    if (!currentModule) {
        auto mod = std::make_shared<ModuleRecord>();
        mod->url = L"test:module";
        mod->namespaceObj = NewObject(nullptr);
        mod->env = globalEnv;
        currentModule = mod;
    }
    try {
        Value v = EvalAst(s->root, globalEnv);
        RunMicrotasks();
        Value r = Get(Value::O(global), L"result");
        result = r.IsUndef() ? Inspect(v) : (r.IsStr() ? *r.s : Inspect(r));
    } catch (JSThrow& t) {
        try {
            result = t.v.IsObj() ? ToStr(Get(t.v, L"name")) + L": " + ToStr(Get(t.v, L"message")) : Inspect(t.v);
        } catch (...) { result = L"?"; }
        ok = false;
    } catch (JSAbort& a) {
        result = L"ABORT: " + a.reason;
        ok = false;
    }
    currentModule = savedMod;
    EndTask();
    return ok;
}

std::wstring Runtime::Inspect(const Value& v, int d) {
    switch (v.t) {
    case Value::String: return d == 0 ? *v.s : L"\"" + *v.s + L"\"";
    case Value::Symbol: return L"Symbol(" + ToStr(Get(v, L"description")) + L")";
    case Value::Object: {
        Obj* o = v.o;
        if (d > 2) return o->cls == Cls::Array ? L"[Array]" : L"[Object]";
        if (o->IsCallable()) return L"[Function: " + (o->x ? o->x->name : std::wstring()) + L"]";
        if (o->cls == Cls::Error) return ToStr(Get(v, L"name")) + L": " + ToStr(Get(v, L"message"));
        if (o->cls == Cls::Array) {
            std::wstring s = L"[";
            for (size_t i = 0; i < o->elems.size() && i < 100; i++) { if (i) s += L", "; s += Inspect(o->elems[i], d + 1); }
            if (o->elems.size() > 100) s += L", …";
            return s + L"]";
        }
        if (o->cls == Cls::Node && o->x && o->x->node) return L"<" + Inspect(Get(v, L"nodeName"), 1) + L">";
        if (o->cls == Cls::Promise) return L"Promise";
        if (o->cls == Cls::Date) return ToStr(v);
        if (o->cls == Cls::TypedArray && o->x) {
            std::wstring s = TypedKindName(o->x->typedKind);
            s += L" [";
            for (size_t i = 0; i < o->x->typedLength && i < 100; i++) {
                if (i) s += L", ";
                s += NumToStr(GetTypedElementNum(*o->x, i));
            }
            if (o->x->typedLength > 100) s += L", …";
            return s + L"]";
        }
        if (o->cls == Cls::ArrayBuffer && o->x) {
            return L"ArrayBuffer { byteLength: " + std::to_wstring(o->x->byteLength) + L" }";
        }
        if (o->cls == Cls::DataView && o->x) {
            return L"DataView { byteLength: " + std::to_wstring(o->x->byteLength) + L", byteOffset: " + std::to_wstring(o->x->byteOffset) + L" }";
        }
        std::wstring s = L"{";
        int n = 0;
        for (auto& k : OwnKeys(o, true)) {
            if (n++) s += L", ";
            if (n > 30) { s += L"…"; break; }
            Value pv;
            try { pv = Get(v, k); } catch (...) {}
            s += k + L": " + Inspect(pv, d + 1);
        }
        return s + L"}";
    }
    default:
        return ToStr(v);
    }
}

} // namespace js
} // namespace mer
