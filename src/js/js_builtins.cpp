// merOS Shield - merJS : yerleşik nesneler (Object, Function, Array, String, Number, Math, JSON, Promise, Map, Set...)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <cwctype>
#include <set>
#include "js_ast.h"
#include "js_bigint.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mer {
namespace js {

void InstallBuiltins2(Runtime& rt); // RegExp, Date, URI, base64
Value PromiseResolveFnImpl(Runtime&, Obj*, const Value&, std::vector<Value>&);
Value PromiseRejectFnImpl(Runtime&, Obj*, const Value&, std::vector<Value>&);

uint32_t ToUint32JS(double d) {
    if (!std::isfinite(d) || d == 0) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return uint32_t(m);
}

namespace {

#define A(i) (args.size() > (i) ? args[i] : Value())
#define NATIVE(name) Value name(Runtime& rt, Obj* self, const Value& thisV, std::vector<Value>& args)
#define UNUSED (void)self; (void)thisV; (void)args; (void)rt


bool IsCallable(const Value& v) { return v.IsObj() && v.o->IsCallable(); }

double ToInteger(Runtime& rt, const Value& v) {
    double d = rt.ToNum(v);
    if (std::isnan(d)) return 0;
    if (std::isinf(d)) return d;
    return std::trunc(d);
}

// Dizi benzeri nesneyi okuma: gercek dizi icin hizli yol
// Bosluklari (holes) koruyan ham liste: concat/slice/reduce/indexOf gibi bosluk-duyarli metotlar icin
std::vector<Value> ListOf(Runtime& rt, const Value& o);
std::vector<Value> ListRaw(Runtime& rt, const Value& o) {
    if (o.IsObj() && (o.o->cls == Cls::Array || o.o->cls == Cls::Arguments)) return o.o->elems;
    return ListOf(rt, o);
}

std::vector<Value> ListOf(Runtime& rt, const Value& o) {
    if (o.IsObj() && (o.o->cls == Cls::Array || o.o->cls == Cls::Arguments)) {
        std::vector<Value> out = o.o->elems;
        for (auto& v : out) if (v.IsHole()) v = Value();
        return out;
    }
    if (o.IsStr()) return rt.IterateToList(o);
    Obj* obj = rt.ToObject(o);
    double n = ToInteger(rt, rt.Get(Value::O(obj), L"length"));
    if (n < 0) n = 0;
    if (n > 10000000) rt.Throw(L"RangeError", L"Invalid array length");
    std::vector<Value> out;
    out.reserve(size_t(n));
    for (size_t i = 0; i < size_t(n); i++) out.push_back(rt.Get(Value::O(obj), std::to_wstring(i)));
    return out;
}

Obj* ArrThis(Runtime& rt, const Value& thisV, const wchar_t* method) {
    if (thisV.IsObj() && thisV.o->cls == Cls::Array) return thisV.o;
    rt.Throw(L"TypeError", std::wstring(L"Array.prototype.") + method + L" called on non-array");
}

size_t RelIndex(Runtime& rt, const Value& v, size_t len, size_t def) {
    if (v.IsUndef()) return def;
    double d = ToInteger(rt, v);
    if (d < 0) d = std::max(0.0, double(len) + d);
    return size_t(std::min(d, double(len)));
}

// Unicode buyuk/kucuk harf donusumu (dil bagimsiz, JS'teki gibi: 'i' -> 'I')
void CaseMap(std::wstring& s, bool upper) {
#ifdef _WIN32
    if (s.empty()) return;
    std::wstring out(s.size() * 3 + 4, L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE, s.c_str(), int(s.size()), &out[0], int(out.size()), nullptr, nullptr, 0);
    if (n > 0) { out.resize(size_t(n)); s.swap(out); return; }
#endif
    for (auto& c : s) {
        uint32_t u = uint32_t(c);
        if (upper) {
            if (u >= L'a' && u <= L'z') u -= 32;
            else if (u >= 0xE0 && u <= 0xFE && u != 0xF7) u -= 32;
            else if (u == 0xFF) u = 0x178;
            else if (u == 0x131) u = L'I';
            else if (u >= 0x100 && u <= 0x17F && u != 0x130 && u != 0x138 && u != 0x149) {
                bool oddLower = (u >= 0x139 && u <= 0x148) || (u >= 0x179 && u <= 0x17E);
                if (oddLower ? (u % 2 == 0) : (u % 2 == 1)) u -= 1;
            }
            else if (u >= 0x3B1 && u <= 0x3C9 && u != 0x3C2) u -= 32;
            else if (u >= 0x430 && u <= 0x44F) u -= 32;
            else if (u >= 0x450 && u <= 0x45F) u -= 80;
        } else {
            if (u >= L'A' && u <= L'Z') u += 32;
            else if (u >= 0xC0 && u <= 0xDE && u != 0xD7) u += 32;
            else if (u == 0x178) u = 0xFF;
            else if (u == 0x130) u = L'i';
            else if (u >= 0x100 && u <= 0x17F && u != 0x131 && u != 0x138 && u != 0x149) {
                bool oddLower = (u >= 0x139 && u <= 0x148) || (u >= 0x179 && u <= 0x17E);
                if (oddLower ? (u % 2 == 1) : (u % 2 == 0)) u += 1;
            }
            else if (u >= 0x391 && u <= 0x3A9) u += 32;
            else if (u >= 0x410 && u <= 0x42F) u += 32;
            else if (u >= 0x400 && u <= 0x40F) u += 80;
        }
        c = wchar_t(u);
    }
}

std::wstring StrThis(Runtime& rt, const Value& thisV) {
    if (thisV.IsNullish()) rt.Throw(L"TypeError", L"String.prototype method called on null or undefined");
    return rt.ToStr(thisV);
}
// Kopyasiz: ilkel dizgede ayni paylasilan tampon (charCodeAt gibi yontemler her cagrida dizgeyi kopyalamasin)
Str StrThisS(Runtime& rt, const Value& thisV) {
    if (thisV.IsNullish()) rt.Throw(L"TypeError", L"String.prototype method called on null or undefined");
    return rt.ToStrS(thisV);
}

// ================= Object =================

NATIVE(Object_ctor) {
    UNUSED;
    Value v = A(0);
    if (v.IsNullish()) return Value::O(rt.NewObject());
    return Value::O(rt.ToObject(v));
}
NATIVE(Object_keys) {
    UNUSED;
    Obj* o = rt.ToObject(A(0));
    std::vector<Value> out;
    for (auto& k : rt.OwnKeys(o, true)) out.push_back(Value::S(k));
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(Object_values) {
    UNUSED;
    Obj* o = rt.ToObject(A(0));
    std::vector<Value> out;
    for (auto& k : rt.OwnKeys(o, true)) out.push_back(rt.Get(Value::O(o), k));
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(Object_entries) {
    UNUSED;
    Obj* o = rt.ToObject(A(0));
    std::vector<Value> out;
    for (auto& k : rt.OwnKeys(o, true)) out.push_back(Value::O(rt.NewArrayFrom({Value::S(k), rt.Get(Value::O(o), k)})));
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(Object_assign) {
    UNUSED;
    Obj* t = rt.ToObject(A(0));
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i].IsNullish()) continue;
        Obj* s = rt.ToObject(args[i]);
        for (auto& k : rt.OwnKeys(s, true)) rt.Put(Value::O(t), k, rt.Get(Value::O(s), k));
    }
    return Value::O(t);
}
void DefineFromDescriptor(Runtime& rt, Obj* o, const std::wstring& key, const Value& d) {
    if (!d.IsObj()) rt.Throw(L"TypeError", L"Property description must be an object");
    Value get = rt.Get(d, L"get"), set = rt.Get(d, L"set");
    bool enumerable = rt.Truthy(rt.Get(d, L"enumerable"));
    if (IsCallable(get) || IsCallable(set)) {
        Prop& p = o->props.Put(key);
        if (!p.accessor) { p.v = Value(); p.getter = p.setter = nullptr; }
        p.accessor = true;
        if (IsCallable(get)) p.getter = get.o;
        if (IsCallable(set)) p.setter = set.o;
        p.enumerable = enumerable;
        return;
    }
    if (o->cls == Cls::Array && rt.HasOwn(o, key) && key != L"length") { rt.Put(Value::O(o), key, rt.Get(d, L"value")); return; }
    Prop* existing = o->props.Find(key);
    Prop& p = o->props.Put(key);
    if (rt.HasProperty(d.o, L"value") || !existing) p.v = rt.Get(d, L"value");
    p.accessor = false;
    p.getter = p.setter = nullptr;
    p.enumerable = existing && !rt.HasProperty(d.o, L"enumerable") ? existing->enumerable : enumerable;
    p.writable = rt.HasProperty(d.o, L"writable") ? rt.Truthy(rt.Get(d, L"writable")) : (existing ? existing->writable : false);
}
NATIVE(Object_defineProperty) {
    UNUSED;
    if (!A(0).IsObj()) rt.Throw(L"TypeError", L"Object.defineProperty called on non-object");
    DefineFromDescriptor(rt, A(0).o, rt.KeyOf(A(1)), A(2));
    return A(0);
}
NATIVE(Object_defineProperties) {
    UNUSED;
    if (!A(0).IsObj()) rt.Throw(L"TypeError", L"Object.defineProperties called on non-object");
    Obj* ps = rt.ToObject(A(1));
    for (auto& k : rt.OwnKeys(ps, true)) DefineFromDescriptor(rt, A(0).o, k, rt.Get(Value::O(ps), k));
    return A(0);
}
NATIVE(Object_create) {
    UNUSED;
    Value p = A(0);
    if (!p.IsObj() && p.t != Value::Null) rt.Throw(L"TypeError", L"Object prototype may only be an Object or null");
    Obj* o = rt.NewObject();
    o->proto = p.IsObj() ? p.o : nullptr;
    if (A(1).IsObj()) {
        for (auto& k : rt.OwnKeys(A(1).o, true)) DefineFromDescriptor(rt, o, k, rt.Get(A(1), k));
    }
    return Value::O(o);
}
NATIVE(Object_getPrototypeOf) {
    UNUSED;
    Obj* o = rt.ToObject(A(0));
    if (A(0).IsStr()) return Value::O(rt.stringProto);
    return o->proto ? Value::O(o->proto) : Value::Null_();
}
NATIVE(Object_setPrototypeOf) {
    UNUSED;
    if (A(0).IsObj()) {
        Obj* np = A(1).IsObj() ? A(1).o : nullptr;
        for (Obj* p = np; p; p = p->proto) if (p == A(0).o) rt.Throw(L"TypeError", L"Cyclic __proto__ value");
        A(0).o->proto = np;
    }
    return A(0);
}
NATIVE(Object_freeze) { UNUSED; if (A(0).IsObj()) { A(0).o->frozen = true; A(0).o->extensible = false; } return A(0); }
NATIVE(Object_isFrozen) { UNUSED; return Value::B(!A(0).IsObj() || A(0).o->frozen); }
NATIVE(Object_seal) { UNUSED; if (A(0).IsObj()) A(0).o->extensible = false; return A(0); }
NATIVE(Object_isExtensible) { UNUSED; return Value::B(A(0).IsObj() && A(0).o->extensible); }
NATIVE(Object_getOwnPropertyNames) {
    UNUSED;
    Obj* o = rt.ToObject(A(0));
    std::vector<Value> out;
    for (auto& k : rt.OwnKeys(o, false)) out.push_back(Value::S(k));
    if (o->cls == Cls::Array) out.push_back(Value::S(L"length"));
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(Object_getOwnPropertySymbols) { UNUSED; return Value::O(rt.NewArray(0)); }
NATIVE(Object_getOwnPropertyDescriptor) {
    UNUSED;
    Obj* o = rt.ToObject(A(0));
    std::wstring k = rt.KeyOf(A(1));
    if (!rt.HasOwn(o, k)) return Value();
    Obj* d = rt.NewObject();
    Prop* p = o->props.Find(k);
    if (p && p->accessor) {
        rt.DefineData(d, L"get", p->getter ? Value::O(p->getter) : Value());
        rt.DefineData(d, L"set", p->setter ? Value::O(p->setter) : Value());
    } else {
        rt.DefineData(d, L"value", rt.Get(Value::O(o), k));
        rt.DefineData(d, L"writable", Value::B(p ? p->writable && !o->frozen : true));
    }
    rt.DefineData(d, L"enumerable", Value::B(p ? p->enumerable : true));
    rt.DefineData(d, L"configurable", Value::B(!o->frozen));
    return Value::O(d);
}
NATIVE(Object_fromEntries) {
    UNUSED;
    Obj* o = rt.NewObject();
    for (auto& e : rt.IterateToList(A(0))) rt.DefineData(o, rt.KeyOf(rt.GetV(e, Value::N(0))), rt.GetV(e, Value::N(1)));
    return Value::O(o);
}
NATIVE(Object_is) {
    UNUSED;
    Value a = A(0), b = A(1);
    if (a.IsNum() && b.IsNum()) {
        if (std::isnan(a.n) && std::isnan(b.n)) return Value::B(true);
        if (a.n == 0 && b.n == 0) return Value::B(std::signbit(a.n) == std::signbit(b.n));
    }
    return Value::B(rt.StrictEq(a, b));
}
NATIVE(ObjectP_hasOwnProperty) { UNUSED; return Value::B(rt.HasOwn(rt.ToObject(thisV), rt.KeyOf(A(0)))); }
NATIVE(Object_hasOwn) { UNUSED; return Value::B(rt.HasOwn(rt.ToObject(A(0)), rt.KeyOf(A(1)))); }
NATIVE(ObjectP_isPrototypeOf) {
    UNUSED;
    if (!A(0).IsObj() || !thisV.IsObj()) return Value::B(false);
    for (Obj* p = A(0).o->proto; p; p = p->proto) if (p == thisV.o) return Value::B(true);
    return Value::B(false);
}
NATIVE(ObjectP_propertyIsEnumerable) {
    UNUSED;
    Obj* o = rt.ToObject(thisV);
    std::wstring k = rt.KeyOf(A(0));
    Prop* p = o->props.Find(k);
    if (p) return Value::B(p->enumerable);
    return Value::B(rt.HasOwn(o, k));
}
NATIVE(ObjectP_toString) {
    UNUSED;
    if (thisV.IsUndef()) return Value::S(L"[object Undefined]");
    if (thisV.t == Value::Null) return Value::S(L"[object Null]");
    Obj* o = rt.ToObject(thisV);
    Value tag = rt.Get(Value::O(o), L"\x01Symbol.toStringTag");
    if (tag.IsStr()) return Value::S(L"[object " + *tag.s + L"]");
    const wchar_t* n = L"Object";
    switch (o->cls) {
    case Cls::Array: n = L"Array"; break;
    case Cls::Function: n = L"Function"; break;
    case Cls::Error: n = L"Error"; break;
    case Cls::Date: n = L"Date"; break;
    case Cls::RegExp: n = L"RegExp"; break;
    case Cls::BoolBox: n = L"Boolean"; break;
    case Cls::NumBox: n = L"Number"; break;
    case Cls::StrBox: n = L"String"; break;
    case Cls::Arguments: n = L"Arguments"; break;
    case Cls::Map: n = L"Map"; break;
    case Cls::Set: n = L"Set"; break;
    case Cls::Promise: n = L"Promise"; break;
    default: break;
    }
    return Value::S(std::wstring(L"[object ") + n + L"]");
}
NATIVE(ObjectP_valueOf) { UNUSED; return Value::O(rt.ToObject(thisV)); }
NATIVE(ObjectP_toLocaleString) { UNUSED; std::vector<Value> none; return rt.Call(rt.Get(thisV, L"toString"), thisV, none); }
NATIVE(ObjectP_protoGet) { UNUSED; Obj* o = rt.ToObject(thisV); return o->proto ? Value::O(o->proto) : Value::Null_(); }
NATIVE(ObjectP_protoSet) {
    UNUSED;
    if (thisV.IsObj() && (A(0).IsObj() || A(0).t == Value::Null)) {
        Obj* np = A(0).IsObj() ? A(0).o : nullptr;
        for (Obj* p = np; p; p = p->proto) if (p == thisV.o) rt.Throw(L"TypeError", L"Cyclic __proto__ value");
        thisV.o->proto = np;
    }
    return Value();
}

// ================= Function =================

NATIVE(Function_ctor) {
    UNUSED;
    std::wstring params, body;
    for (size_t i = 0; i + 1 < args.size(); i++) { if (i) params += L","; params += rt.ToStr(args[i]); }
    if (!args.empty()) body = rt.ToStr(args.back());
    Value f = rt.EvalGlobal(L"(function anonymous(" + params + L"\n) {\n" + body + L"\n})", L"Function");
    return f;
}
NATIVE(FunctionP_call) {
    UNUSED;
    std::vector<Value> rest;
    if (args.size() > 1) rest.assign(args.begin() + 1, args.end());
    return rt.Call(thisV, A(0), rest);
}
NATIVE(FunctionP_apply) {
    UNUSED;
    std::vector<Value> list;
    if (!A(1).IsNullish()) list = ListOf(rt, A(1));
    return rt.Call(thisV, A(0), list);
}
NATIVE(FunctionP_bind) {
    UNUSED;
    if (!IsCallable(thisV)) rt.Throw(L"TypeError", L"Bind must be called on a function");
    Obj* f = rt.NewObject(rt.functionProto);
    f->cls = Cls::Function;
    ObjExtra& x = f->X();
    x.boundTarget = thisV.o;
    x.boundThis = A(0);
    if (args.size() > 1) x.boundArgs.assign(args.begin() + 1, args.end());
    x.name = L"bound " + (thisV.o->x ? thisV.o->x->name : std::wstring());
    x.ctor = thisV.o->x && (thisV.o->x->ctor || thisV.o->x->classKind);
    return Value::O(f);
}
NATIVE(FunctionP_toString) {
    UNUSED;
    std::wstring n = thisV.IsObj() && thisV.o->x ? thisV.o->x->name : L"";
    return Value::S(L"function " + n + L"() { [native code] }");
}

// ================= Array =================

NATIVE(Array_ctor) {
    UNUSED;
    if (args.size() == 1 && A(0).IsNum()) {
        double n = A(0).n;
        if (n < 0 || n != std::floor(n) || n > 50000000) rt.Throw(L"RangeError", L"Invalid array length");
        Obj* a = rt.NewArray(size_t(n));
        for (auto& v : a->elems) v = Value::Hole();   // Array(n): n bosluk
        return Value::O(a);
    }
    return Value::O(rt.NewArrayFrom(args));
}
NATIVE(Array_isArray) { UNUSED; return Value::B(A(0).IsObj() && A(0).o->cls == Cls::Array); }
NATIVE(Array_from) {
    UNUSED;
    Value src = A(0);
    if (src.IsNullish()) rt.Throw(L"TypeError", L"Array.from requires an array-like object");
    std::vector<Value> list;
    Value itf = src.IsObj() || src.IsStr() ? rt.Get(src, L"\x01Symbol.iterator") : Value();
    if (IsCallable(itf) || src.IsStr()) list = rt.IterateToList(src);
    else list = ListOf(rt, src);
    if (IsCallable(A(1))) {
        for (size_t i = 0; i < list.size(); i++) {
            std::vector<Value> a{list[i], Value::N(double(i))};
            list[i] = rt.Call(A(1), A(2), a);
        }
    }
    return Value::O(rt.NewArrayFrom(list));
}
NATIVE(Array_of) { UNUSED; return Value::O(rt.NewArrayFrom(args)); }

NATIVE(ArrayP_push) {
    UNUSED;
    if (thisV.IsObj() && thisV.o->cls == Cls::Array) {
        Obj* a = thisV.o;
        if (a->frozen) rt.Throw(L"TypeError", L"Cannot add property, object is not extensible");
        if (a->elems.size() + args.size() > 50000000) rt.Throw(L"RangeError", L"Invalid array length");
        a->elems.insert(a->elems.end(), args.begin(), args.end());
        return Value::N(double(a->elems.size()));
    }
    Obj* o = rt.ToObject(thisV);
    double n = ToInteger(rt, rt.Get(Value::O(o), L"length"));
    for (auto& v : args) rt.Put(Value::O(o), Runtime::NumToStr(n++), v);
    rt.Put(Value::O(o), L"length", Value::N(n));
    return Value::N(n);
}
NATIVE(ArrayP_pop) {
    UNUSED;
    Obj* a = ArrThis(rt, thisV, L"pop");
    if (a->elems.empty()) return Value();
    Value v = Unhole(a->elems.back());
    a->elems.pop_back();
    return v;
}
NATIVE(ArrayP_shift) {
    UNUSED;
    Obj* a = ArrThis(rt, thisV, L"shift");
    if (a->elems.empty()) return Value();
    Value v = Unhole(a->elems.front());
    a->elems.erase(a->elems.begin());
    return v;
}
NATIVE(ArrayP_unshift) {
    UNUSED;
    Obj* a = ArrThis(rt, thisV, L"unshift");
    a->elems.insert(a->elems.begin(), args.begin(), args.end());
    return Value::N(double(a->elems.size()));
}
NATIVE(ArrayP_slice) {
    UNUSED;
    std::vector<Value> l = ListRaw(rt, thisV);
    size_t s = RelIndex(rt, A(0), l.size(), 0), e = RelIndex(rt, A(1), l.size(), l.size());
    std::vector<Value> out;
    if (s < e) out.assign(l.begin() + long(s), l.begin() + long(e));
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_splice) {
    UNUSED;
    Obj* a = ArrThis(rt, thisV, L"splice");
    size_t len = a->elems.size();
    size_t start = RelIndex(rt, A(0), len, 0);
    size_t del = args.size() < 1 ? 0 : args.size() < 2 ? len - start : size_t(std::max(0.0, std::min(ToInteger(rt, A(1)), double(len - start))));
    std::vector<Value> removed(a->elems.begin() + long(start), a->elems.begin() + long(start + del));
    a->elems.erase(a->elems.begin() + long(start), a->elems.begin() + long(start + del));
    if (args.size() > 2) a->elems.insert(a->elems.begin() + long(start), args.begin() + 2, args.end());
    return Value::O(rt.NewArrayFrom(removed));
}
NATIVE(ArrayP_concat) {
    UNUSED;
    std::vector<Value> out = ListRaw(rt, thisV);
    for (auto& v : args) {
        if (v.IsObj() && v.o->cls == Cls::Array) out.insert(out.end(), v.o->elems.begin(), v.o->elems.end());   // bosluklar korunur
        else out.push_back(v);
        if (out.size() > 50000000) rt.Throw(L"RangeError", L"Invalid array length");
    }
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_join) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    std::wstring sep = A(0).IsUndef() ? L"," : rt.ToStr(A(0));
    std::wstring out;
    for (size_t i = 0; i < l.size(); i++) {
        if (i) out += sep;
        if (!l[i].IsNullish()) out += rt.ToStr(l[i]);
        if (out.size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
    }
    return Value::S(std::move(out));
}
NATIVE(ArrayP_toString) {
    UNUSED;
    if (thisV.IsObj() && thisV.o->cls == Cls::Array) { std::vector<Value> none; return ArrayP_join(rt, self, thisV, none); }
    return ObjectP_toString(rt, self, thisV, args);
}
NATIVE(ArrayP_reverse) { UNUSED; Obj* a = ArrThis(rt, thisV, L"reverse"); std::reverse(a->elems.begin(), a->elems.end()); return thisV; }
NATIVE(ArrayP_indexOf) {
    UNUSED;
    std::vector<Value> l = ListRaw(rt, thisV);
    for (size_t i = RelIndex(rt, A(1), l.size(), 0); i < l.size(); i++) if (!l[i].IsHole() && rt.StrictEq(l[i], A(0))) return Value::N(double(i));
    return Value::N(-1);
}
NATIVE(ArrayP_lastIndexOf) {
    UNUSED;
    std::vector<Value> l = ListRaw(rt, thisV);
    for (size_t i = l.size(); i-- > 0;) if (!l[i].IsHole() && rt.StrictEq(l[i], A(0))) return Value::N(double(i));
    return Value::N(-1);
}
NATIVE(ArrayP_includes) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    for (size_t i = RelIndex(rt, A(1), l.size(), 0); i < l.size(); i++) if (rt.SameValueZero(l[i], A(0))) return Value::B(true);
    return Value::B(false);
}

// Geri cagirmali dizi metotlari icin ortak yardimci
// skipHoles: forEach/map/filter/some/every bosluklari atlar; find/findIndex atlamaz (spesifikasyon)
template <class F>
void EachCb(Runtime& rt, const Value& thisV, std::vector<Value>& args, F f, bool skipHoles = true) {
    if (!IsCallable(A(0))) rt.Throw(L"TypeError", rt.TypeOf(A(0)) + L" is not a function");
    std::vector<Value> l = ListRaw(rt, thisV);
    bool isArr = thisV.IsObj() && thisV.o->cls == Cls::Array;
    for (size_t i = 0; i < l.size(); i++) {
        // dizi ozgun ise canli oku (geri cagri diziyi degistirebilir)
        Value v = isArr ? (i < thisV.o->elems.size() ? thisV.o->elems[i] : Value::Hole()) : l[i];
        if (v.IsHole()) {
            if (skipHoles) continue;
            v = Value();
        }
        std::vector<Value> a{v, Value::N(double(i)), thisV};
        Value r = rt.Call(A(0), A(1), a);
        if (!f(v, i, r)) break;
    }
}

NATIVE(ArrayP_forEach) { UNUSED; EachCb(rt, thisV, args, [](const Value&, size_t, const Value&) { return true; }); return Value(); }
NATIVE(ArrayP_map) {
    UNUSED;
    // sonuc ayni uzunlukta; kaynaktaki bosluklar sonucta da bosluk kalir
    std::vector<Value> out(ListRaw(rt, thisV).size(), Value::Hole());
    EachCb(rt, thisV, args, [&](const Value&, size_t i, const Value& r) { if (i < out.size()) out[i] = Unhole(r); return true; });
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_filter) {
    UNUSED;
    std::vector<Value> out;
    EachCb(rt, thisV, args, [&](const Value& v, size_t, const Value& r) { if (rt.Truthy(r)) out.push_back(v); return true; });
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_some) {
    UNUSED;
    bool f = false;
    EachCb(rt, thisV, args, [&](const Value&, size_t, const Value& r) { if (rt.Truthy(r)) { f = true; return false; } return true; });
    return Value::B(f);
}
NATIVE(ArrayP_every) {
    UNUSED;
    bool f = true;
    EachCb(rt, thisV, args, [&](const Value&, size_t, const Value& r) { if (!rt.Truthy(r)) { f = false; return false; } return true; });
    return Value::B(f);
}
NATIVE(ArrayP_find) {
    UNUSED;
    Value out;
    EachCb(rt, thisV, args, [&](const Value& v, size_t, const Value& r) { if (rt.Truthy(r)) { out = v; return false; } return true; }, false);
    return out;
}
NATIVE(ArrayP_findIndex) {
    UNUSED;
    double out = -1;
    EachCb(rt, thisV, args, [&](const Value&, size_t i, const Value& r) { if (rt.Truthy(r)) { out = double(i); return false; } return true; }, false);
    return Value::N(out);
}
NATIVE(ArrayP_findLast) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    for (size_t i = l.size(); i-- > 0;) {
        std::vector<Value> a{l[i], Value::N(double(i)), thisV};
        if (rt.Truthy(rt.Call(A(0), A(1), a))) return l[i];
    }
    return Value();
}
NATIVE(ArrayP_findLastIndex) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    for (size_t i = l.size(); i-- > 0;) {
        std::vector<Value> a{l[i], Value::N(double(i)), thisV};
        if (rt.Truthy(rt.Call(A(0), A(1), a))) return Value::N(double(i));
    }
    return Value::N(-1);
}
NATIVE(ArrayP_reduce) {
    UNUSED;
    if (!IsCallable(A(0))) rt.Throw(L"TypeError", L"reduce callback is not a function");
    std::vector<Value> l = ListRaw(rt, thisV);
    size_t i = 0;
    Value acc;
    if (args.size() >= 2) acc = A(1);
    else {
        while (i < l.size() && l[i].IsHole()) i++;
        if (i >= l.size()) rt.Throw(L"TypeError", L"Reduce of empty array with no initial value");
        acc = l[i];
        i++;
    }
    for (; i < l.size(); i++) {
        if (l[i].IsHole()) continue;
        std::vector<Value> a{acc, l[i], Value::N(double(i)), thisV};
        acc = rt.Call(A(0), Value(), a);
    }
    return acc;
}
NATIVE(ArrayP_reduceRight) {
    UNUSED;
    if (!IsCallable(A(0))) rt.Throw(L"TypeError", L"reduceRight callback is not a function");
    std::vector<Value> l = ListRaw(rt, thisV);
    long i = long(l.size()) - 1;
    Value acc;
    if (args.size() >= 2) acc = A(1);
    else {
        while (i >= 0 && l[size_t(i)].IsHole()) i--;
        if (i < 0) rt.Throw(L"TypeError", L"Reduce of empty array with no initial value");
        acc = l[size_t(i)];
        i--;
    }
    for (; i >= 0; i--) {
        if (l[size_t(i)].IsHole()) continue;
        std::vector<Value> a{acc, l[size_t(i)], Value::N(double(i)), thisV};
        acc = rt.Call(A(0), Value(), a);
    }
    return acc;
}
NATIVE(ArrayP_sort) {
    UNUSED;
    Obj* a = ArrThis(rt, thisV, L"sort");
    Value cmp = A(0);
    std::vector<Value> v = a->elems, undefs;
    std::vector<Value> defined;
    size_t holes = 0;
    for (auto& x : v) { if (x.IsHole()) holes++; else (x.IsUndef() ? undefs : defined).push_back(x); }
    if (IsCallable(cmp)) {
        std::stable_sort(defined.begin(), defined.end(), [&](const Value& x, const Value& y) {
            std::vector<Value> ca{x, y};
            double r = rt.ToNum(rt.Call(cmp, Value(), ca));
            return r < 0;
        });
    } else {
        std::vector<std::pair<std::wstring, Value>> keyed;
        for (auto& x : defined) keyed.emplace_back(rt.ToStr(x), x);
        std::stable_sort(keyed.begin(), keyed.end(), [](const std::pair<std::wstring, Value>& x, const std::pair<std::wstring, Value>& y) { return x.first < y.first; });
        defined.clear();
        for (auto& k : keyed) defined.push_back(k.second);
    }
    defined.insert(defined.end(), undefs.begin(), undefs.end());
    defined.insert(defined.end(), holes, Value::Hole());   // bosluklar en sona
    a->elems = defined;
    return thisV;
}
NATIVE(ArrayP_fill) {
    UNUSED;
    Obj* a = ArrThis(rt, thisV, L"fill");
    size_t s = RelIndex(rt, A(1), a->elems.size(), 0), e = RelIndex(rt, A(2), a->elems.size(), a->elems.size());
    for (size_t i = s; i < e; i++) a->elems[i] = A(0);
    return thisV;
}
NATIVE(ArrayP_values) { UNUSED; return Value::O(rt.MakeIteratorFromList(ListOf(rt, thisV))); }
NATIVE(ArrayP_keys) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV), out;
    for (size_t i = 0; i < l.size(); i++) out.push_back(Value::N(double(i)));
    return Value::O(rt.MakeIteratorFromList(out));
}
NATIVE(ArrayP_entries) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV), out;
    for (size_t i = 0; i < l.size(); i++) out.push_back(Value::O(rt.NewArrayFrom({Value::N(double(i)), l[i]})));
    return Value::O(rt.MakeIteratorFromList(out));
}
void Flatten(Runtime& rt, const std::vector<Value>& in, double depth, std::vector<Value>& out) {
    for (auto& v : in) {
        if (v.IsHole()) continue;   // flat bosluklari atar
        if (depth > 0 && v.IsObj() && v.o->cls == Cls::Array) Flatten(rt, v.o->elems, depth - 1, out);
        else out.push_back(v);
        if (out.size() > 50000000) rt.Throw(L"RangeError", L"Invalid array length");
    }
}
NATIVE(ArrayP_flat) {
    UNUSED;
    std::vector<Value> out;
    Flatten(rt, ListRaw(rt, thisV), A(0).IsUndef() ? 1 : std::min(ToInteger(rt, A(0)), 100.0), out);
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_flatMap) {
    UNUSED;
    std::vector<Value> mapped;
    EachCb(rt, thisV, args, [&](const Value&, size_t, const Value& r) { mapped.push_back(r); return true; });
    std::vector<Value> out;
    Flatten(rt, mapped, 1, out);
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_at) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    double i = ToInteger(rt, A(0));
    if (i < 0) i += double(l.size());
    if (i < 0 || i >= double(l.size())) return Value();
    return l[size_t(i)];
}
NATIVE(ArrayP_toReversed) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    std::reverse(l.begin(), l.end());
    return Value::O(rt.NewArrayFrom(l));
}
NATIVE(ArrayP_toSorted) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    Value cmp = A(0);
    if (!cmp.IsUndef() && !IsCallable(cmp)) rt.Throw(L"TypeError", L"The comparison function must be either a function or undefined");
    if (IsCallable(cmp)) {
        std::stable_sort(l.begin(), l.end(), [&](const Value& a, const Value& b) {
            std::vector<Value> args2{a, b};
            double r = rt.ToNum(rt.Call(cmp, Value(), args2));
            return r < 0;
        });
    } else {
        std::stable_sort(l.begin(), l.end(), [&](const Value& a, const Value& b) {
            return rt.ToStr(a) < rt.ToStr(b);
        });
    }
    return Value::O(rt.NewArrayFrom(l));
}
NATIVE(ArrayP_toSpliced) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    double len = double(l.size());
    double start = ToInteger(rt, A(0));
    double actualStart = (start < 0) ? std::max(len + start, 0.0) : std::min(start, len);
    double actualDeleteCount = 0;
    if (args.size() == 1) {
        actualDeleteCount = len - actualStart;
    } else if (args.size() >= 2) {
        double dc = ToInteger(rt, A(1));
        actualDeleteCount = std::max(0.0, std::min(dc, len - actualStart));
    }
    std::vector<Value> out;
    size_t sIdx = size_t(actualStart);
    size_t del = size_t(actualDeleteCount);
    for (size_t i = 0; i < sIdx && i < l.size(); i++) out.push_back(l[i]);
    for (size_t i = 2; i < args.size(); i++) out.push_back(args[i]);
    for (size_t i = sIdx + del; i < l.size(); i++) out.push_back(l[i]);
    return Value::O(rt.NewArrayFrom(out));
}
NATIVE(ArrayP_with) {
    UNUSED;
    std::vector<Value> l = ListOf(rt, thisV);
    double len = double(l.size());
    double relativeIndex = ToInteger(rt, A(0));
    double actualIndex = (relativeIndex >= 0) ? relativeIndex : len + relativeIndex;
    if (actualIndex < 0 || actualIndex >= len) rt.Throw(L"RangeError", L"Invalid index in Array.prototype.with");
    l[size_t(actualIndex)] = A(1);
    return Value::O(rt.NewArrayFrom(l));
}

// ================= String =================

NATIVE(String_ctor) {
    UNUSED;
    std::wstring s;
    if (!args.empty()) s = A(0).t == Value::Symbol ? L"Symbol(" + rt.ToStr(rt.Get(A(0), L"description")) + L")" : rt.ToStr(A(0));
    if (rt.nativeNewTarget) {
        Obj* o = rt.NewObject(rt.stringProto);
        o->cls = Cls::StrBox;
        o->X().str = s;
        return Value::O(o);
    }
    return Value::S(std::move(s));
}
NATIVE(String_fromCharCode) {
    UNUSED;
    std::wstring s;
    for (auto& a : args) s += wchar_t(uint16_t(uint32_t(int64_t(rt.ToNum(a)))));
    return Value::S(std::move(s));
}
NATIVE(String_fromCodePoint) {
    UNUSED;
    std::wstring s;
    for (auto& a : args) {
        double d = rt.ToNum(a);
        if (d < 0 || d > 0x10FFFF || d != std::floor(d)) rt.Throw(L"RangeError", L"Invalid code point");
        uint32_t cp = uint32_t(d);
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) { cp -= 0x10000; s += wchar_t(0xD800 + (cp >> 10)); s += wchar_t(0xDC00 + (cp & 0x3FF)); }
        else s += wchar_t(cp);
    }
    return Value::S(std::move(s));
}
NATIVE(String_raw) {
    UNUSED;
    Value strs = rt.Get(A(0), L"raw");
    std::vector<Value> l = ListOf(rt, strs);
    std::wstring s;
    for (size_t i = 0; i < l.size(); i++) {
        s += rt.ToStr(l[i]);
        if (i + 1 < l.size() && i + 1 < args.size()) s += rt.ToStr(args[i + 1]);
    }
    return Value::S(std::move(s));
}
NATIVE(StringP_toString) {
    UNUSED;
    if (thisV.IsStr()) return thisV;
    if (thisV.IsObj() && thisV.o->cls == Cls::StrBox) return Value::S(thisV.o->x->str);
    rt.Throw(L"TypeError", L"String.prototype.toString requires that 'this' be a String");
}
NATIVE(StringP_charAt) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double i = ToInteger(rt, A(0));
    if (i < 0 || i >= double(s.size())) return Value::S(L"");
    return Value::S(std::wstring(1, s[size_t(i)]));
}
NATIVE(StringP_charCodeAt) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double i = ToInteger(rt, A(0));
    if (i < 0 || i >= double(s.size())) return Value::N(NAN);
    return Value::N(double(uint16_t(s[size_t(i)])));
}
NATIVE(StringP_codePointAt) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double i = ToInteger(rt, A(0));
    if (i < 0 || i >= double(s.size())) return Value();
    uint32_t c = uint32_t(s[size_t(i)]);
    if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && size_t(i) + 1 < s.size()) {
        uint32_t d = uint32_t(s[size_t(i) + 1]);
        if (d >= 0xDC00 && d <= 0xDFFF) c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
    }
    return Value::N(double(c));
}
NATIVE(StringP_indexOf) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_; std::wstring f = rt.ToStr(A(0));
    size_t from = size_t(std::max(0.0, std::min(ToInteger(rt, A(1)), double(s.size()))));
    size_t p = s.find(f, from);
    return Value::N(p == std::wstring::npos ? -1 : double(p));
}
NATIVE(StringP_lastIndexOf) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_; std::wstring f = rt.ToStr(A(0));
    size_t p = s.rfind(f);
    return Value::N(p == std::wstring::npos ? -1 : double(p));
}
NATIVE(StringP_includes) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    if (A(0).IsObj() && A(0).o->cls == Cls::RegExp) rt.Throw(L"TypeError", L"First argument must not be a regular expression");
    std::wstring f = rt.ToStr(A(0));
    size_t from = size_t(std::max(0.0, std::min(ToInteger(rt, A(1)), double(s.size()))));
    return Value::B(s.find(f, from) != std::wstring::npos);
}
NATIVE(StringP_startsWith) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_; std::wstring f = rt.ToStr(A(0));
    size_t pos = size_t(std::max(0.0, std::min(ToInteger(rt, A(1)), double(s.size()))));
    return Value::B(s.compare(pos, f.size(), f) == 0 && pos + f.size() <= s.size());
}
NATIVE(StringP_endsWith) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_; std::wstring f = rt.ToStr(A(0));
    size_t end = A(1).IsUndef() ? s.size() : size_t(std::max(0.0, std::min(ToInteger(rt, A(1)), double(s.size()))));
    if (f.size() > end) return Value::B(false);
    return Value::B(s.compare(end - f.size(), f.size(), f) == 0);
}
NATIVE(StringP_slice) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    size_t a = RelIndex(rt, A(0), s.size(), 0), b = RelIndex(rt, A(1), s.size(), s.size());
    return Value::S(a < b ? s.substr(a, b - a) : L"");
}
NATIVE(StringP_substring) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double a = std::max(0.0, std::min(ToInteger(rt, A(0)), double(s.size())));
    double b = A(1).IsUndef() ? double(s.size()) : std::max(0.0, std::min(ToInteger(rt, A(1)), double(s.size())));
    if (a > b) std::swap(a, b);
    return Value::S(s.substr(size_t(a), size_t(b - a)));
}
NATIVE(StringP_substr) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    size_t a = RelIndex(rt, A(0), s.size(), 0);
    double len = A(1).IsUndef() ? double(s.size()) : std::max(0.0, ToInteger(rt, A(1)));
    return Value::S(s.substr(a, size_t(std::min(len, double(s.size() - a)))));
}
NATIVE(StringP_toUpperCase) { UNUSED; std::wstring s = StrThis(rt, thisV); CaseMap(s, true); return Value::S(std::move(s)); }
NATIVE(StringP_toLowerCase) { UNUSED; std::wstring s = StrThis(rt, thisV); CaseMap(s, false); return Value::S(std::move(s)); }
bool JsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF || c == 0x2028 || c == 0x2029 || (c > 127 && iswspace(c)); }
NATIVE(StringP_trim) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    size_t a = 0, b = s.size();
    while (a < b && JsSpace(s[a])) a++;
    while (b > a && JsSpace(s[b - 1])) b--;
    return Value::S(s.substr(a, b - a));
}
NATIVE(StringP_trimStart) { UNUSED; std::wstring s = StrThis(rt, thisV); size_t a = 0; while (a < s.size() && JsSpace(s[a])) a++; return Value::S(s.substr(a)); }
NATIVE(StringP_trimEnd) { UNUSED; std::wstring s = StrThis(rt, thisV); size_t b = s.size(); while (b > 0 && JsSpace(s[b - 1])) b--; return Value::S(s.substr(0, b)); }
std::wstring Pad(Runtime& rt, const Value& thisV, std::vector<Value>& args, bool start) {
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double target = ToInteger(rt, A(0));
    std::wstring fill = A(1).IsUndef() ? L" " : rt.ToStr(A(1));
    if (target <= double(s.size()) || fill.empty()) return s;
    if (target > 100000000) rt.Throw(L"RangeError", L"Invalid string length");
    std::wstring pad;
    while (pad.size() < size_t(target) - s.size()) pad += fill;
    pad.resize(size_t(target) - s.size());
    return start ? pad + s : s + pad;
}
NATIVE(StringP_padStart) { UNUSED; return Value::S(Pad(rt, thisV, args, true)); }
NATIVE(StringP_padEnd) { UNUSED; return Value::S(Pad(rt, thisV, args, false)); }
NATIVE(StringP_repeat) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double n = ToInteger(rt, A(0));
    if (n < 0 || std::isinf(n)) rt.Throw(L"RangeError", L"Invalid count value");
    if (double(s.size()) * n > 100000000) rt.Throw(L"RangeError", L"Invalid string length");
    std::wstring o;
    for (int i = 0; i < int(n); i++) o += s;
    return Value::S(std::move(o));
}
NATIVE(StringP_concat) { UNUSED; std::wstring s = StrThis(rt, thisV); for (auto& a : args) s += rt.ToStr(a); return Value::S(std::move(s)); }
NATIVE(StringP_at) {
    UNUSED;
    Str sp_ = StrThisS(rt, thisV); const std::wstring& s = *sp_;
    double i = ToInteger(rt, A(0));
    if (i < 0) i += double(s.size());
    if (i < 0 || i >= double(s.size())) return Value();
    return Value::S(std::wstring(1, s[size_t(i)]));
}
NATIVE(StringP_localeCompare) {
    UNUSED;
    std::wstring a = StrThis(rt, thisV), b = rt.ToStr(A(0));
    if (args.size() > 1) {
        Value intl = rt.Get(Value::O(rt.global), L"Intl");
        if (intl.IsObj()) {
            Value colCtor = rt.Get(intl, L"Collator");
            if (colCtor.IsObj() && colCtor.o->IsCallable()) {
                std::vector<Value> colArgs;
                colArgs.push_back(args[1]);
                if (args.size() > 2) colArgs.push_back(args[2]);
                Value col = rt.Call(colCtor, Value::O(rt.global), colArgs);
                if (col.IsObj()) {
                    Value cmp = rt.Get(col, L"compare");
                    if (cmp.IsObj() && cmp.o->IsCallable()) {
                        std::vector<Value> ca{Value::S(a), Value::S(b)};
                        return rt.Call(cmp, col, ca);
                    }
                }
            }
        }
    }
    int r = wcscoll(a.c_str(), b.c_str());
    return Value::N(r < 0 ? -1 : r > 0 ? 1 : 0);
}
NATIVE(StringP_normalize) { UNUSED; return Value::S(StrThis(rt, thisV)); }
NATIVE(StringP_iterator) { UNUSED; return Value::O(rt.MakeIteratorFromList(rt.IterateToList(Value::S(StrThis(rt, thisV))))); }

// ================= Number / Boolean / Symbol =================

NATIVE(Number_ctor) {
    UNUSED;
    Value prim = args.empty() ? Value::N(0) : rt.ToPrimitive(A(0), 1);
    double n = prim.IsBig() ? BigOf(prim).ToDouble() : (args.empty() ? 0 : rt.ToNum(prim));
    if (rt.nativeNewTarget) {
        Obj* o = rt.NewObject(rt.numberProto);
        o->cls = Cls::NumBox;
        o->X().num = n;
        return Value::O(o);
    }
    return Value::N(n);
}
double ThisNum(Runtime& rt, const Value& thisV) {
    if (thisV.IsNum()) return thisV.n;
    if (thisV.IsObj() && thisV.o->cls == Cls::NumBox) return thisV.o->x->num;
    rt.Throw(L"TypeError", L"Number.prototype method called on incompatible receiver");
}
NATIVE(NumberP_toString) {
    UNUSED;
    double d = ThisNum(rt, thisV);
    int radix = A(0).IsUndef() ? 10 : int(ToInteger(rt, A(0)));
    if (radix < 2 || radix > 36) rt.Throw(L"RangeError", L"toString() radix must be between 2 and 36");
    if (radix == 10 || std::isnan(d) || std::isinf(d)) return Value::S(Runtime::NumToStr(d));
    bool neg = d < 0;
    d = std::fabs(d);
    double ip = std::floor(d), fp = d - ip;
    std::wstring s;
    const wchar_t* dig = L"0123456789abcdefghijklmnopqrstuvwxyz";
    if (ip == 0) s = L"0";
    while (ip >= 1 && s.size() < 1100) { s.insert(s.begin(), dig[int(std::fmod(ip, radix))]); ip = std::floor(ip / radix); }
    if (fp > 0) {
        s += L'.';
        for (int k = 0; k < 20 && fp > 0; k++) { fp *= radix; int dg = int(fp); s += dig[dg]; fp -= dg; }
    }
    return Value::S(neg ? L"-" + s : s);
}
NATIVE(NumberP_toFixed) {
    UNUSED;
    double d = ThisNum(rt, thisV);
    int f = int(ToInteger(rt, A(0)));
    if (f < 0 || f > 100) rt.Throw(L"RangeError", L"toFixed() digits argument must be between 0 and 100");
    if (std::fabs(d) >= 1e21 || std::isnan(d)) return Value::S(Runtime::NumToStr(d));
    wchar_t buf[160];
    swprintf(buf, 160, L"%.*f", f, d);
    std::wstring r = buf;
    if (r == L"-0" || (r.size() > 1 && r[0] == L'-' && r.find_first_not_of(L"-0.") == std::wstring::npos)) r.erase(0, 1);
    return Value::S(r);
}
NATIVE(NumberP_toPrecision) {
    UNUSED;
    double d = ThisNum(rt, thisV);
    if (A(0).IsUndef() || std::isnan(d) || std::isinf(d)) return Value::S(Runtime::NumToStr(d));
    int p = int(ToInteger(rt, A(0)));
    if (p < 1 || p > 100) rt.Throw(L"RangeError", L"toPrecision() argument must be between 1 and 100");
    wchar_t buf[200];
    int e = d == 0 ? 0 : int(std::floor(std::log10(std::fabs(d))));
    if (e < -6 || e >= p) swprintf(buf, 200, L"%.*e", p - 1, d);
    else swprintf(buf, 200, L"%.*f", std::max(0, p - 1 - e), d);
    std::wstring r = buf;
    size_t ep = r.find(L'e');
    if (ep != std::wstring::npos) {
        // JS bicimi: 1.23e+5 (bastaki sifirlar yok)
        std::wstring mant = r.substr(0, ep), ex = r.substr(ep + 1);
        wchar_t sign = ex[0];
        ex = ex.substr(1);
        while (ex.size() > 1 && ex[0] == L'0') ex.erase(0, 1);
        r = mant + L"e" + sign + ex;
    }
    return Value::S(r);
}
NATIVE(NumberP_valueOf) { UNUSED; return Value::N(ThisNum(rt, thisV)); }
NATIVE(NumberP_toLocaleString) {
    UNUSED;
    double d = ThisNum(rt, thisV);
    if (!args.empty()) {
        Value intl = rt.Get(Value::O(rt.global), L"Intl");
        if (intl.IsObj()) {
            Value nfCtor = rt.Get(intl, L"NumberFormat");
            if (nfCtor.IsObj() && nfCtor.o->IsCallable()) {
                std::vector<Value> nfArgs;
                nfArgs.push_back(args[0]);
                if (args.size() > 1) nfArgs.push_back(args[1]);
                Value nf = rt.Call(nfCtor, Value::O(rt.global), nfArgs);
                if (nf.IsObj()) {
                    Value fmt = rt.Get(nf, L"format");
                    if (fmt.IsObj() && fmt.o->IsCallable()) {
                        std::vector<Value> fa{Value::N(d)};
                        return rt.Call(fmt, nf, fa);
                    }
                }
            }
        }
    }
    if (std::isnan(d) || std::isinf(d)) return Value::S(Runtime::NumToStr(d));
    wchar_t buf[128];
    swprintf(buf, 128, L"%.3f", std::fabs(d));
    std::wstring s = buf;
    size_t dot = s.find(L'.');
    std::wstring ip = s.substr(0, dot), fp = s.substr(dot + 1);
    while (!fp.empty() && fp.back() == L'0') fp.pop_back();
    std::wstring g;
    for (size_t i = 0; i < ip.size(); i++) { if (i && (ip.size() - i) % 3 == 0) g += L','; g += ip[i]; }
    return Value::S((d < 0 ? L"-" : L"") + g + (fp.empty() ? L"" : L"." + fp));
}
NATIVE(Number_isInteger) { UNUSED; return Value::B(A(0).IsNum() && std::isfinite(A(0).n) && A(0).n == std::floor(A(0).n)); }
NATIVE(Number_isSafeInteger) { UNUSED; return Value::B(A(0).IsNum() && std::isfinite(A(0).n) && A(0).n == std::floor(A(0).n) && std::fabs(A(0).n) <= 9007199254740991.0); }
NATIVE(Number_isFinite) { UNUSED; return Value::B(A(0).IsNum() && std::isfinite(A(0).n)); }
NATIVE(Number_isNaN) { UNUSED; return Value::B(A(0).IsNum() && std::isnan(A(0).n)); }
NATIVE(Global_isNaN) { UNUSED; return Value::B(std::isnan(rt.ToNum(A(0)))); }
NATIVE(Global_isFinite) { UNUSED; return Value::B(std::isfinite(rt.ToNum(A(0)))); }
NATIVE(Global_parseInt) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0));
    size_t i = 0;
    while (i < s.size() && JsSpace(s[i])) i++;
    bool neg = false;
    if (i < s.size() && (s[i] == L'+' || s[i] == L'-')) { neg = s[i] == L'-'; i++; }
    int radix = int(ToInteger(rt, A(1)));
    if (radix == 0) {
        radix = 10;
        if (i + 1 < s.size() && s[i] == L'0' && (s[i + 1] == L'x' || s[i + 1] == L'X')) { radix = 16; i += 2; }
    } else if (radix == 16 && i + 1 < s.size() && s[i] == L'0' && (s[i + 1] == L'x' || s[i + 1] == L'X')) i += 2;
    if (radix < 2 || radix > 36) return Value::N(NAN);
    double v = 0;
    bool any = false;
    for (; i < s.size(); i++) {
        wchar_t c = s[i];
        int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'z') ? c - L'a' + 10 : (c >= L'A' && c <= L'Z') ? c - L'A' + 10 : 99;
        if (d >= radix) break;
        v = v * radix + d;
        any = true;
    }
    if (!any) return Value::N(NAN);
    return Value::N(neg ? -v : v);
}
NATIVE(Global_parseFloat) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0));
    size_t i = 0;
    while (i < s.size() && JsSpace(s[i])) i++;
    std::wstring t = s.substr(i);
    if (t.compare(0, 8, L"Infinity") == 0 || t.compare(0, 9, L"+Infinity") == 0) return Value::N(INFINITY);
    if (t.compare(0, 9, L"-Infinity") == 0) return Value::N(-INFINITY);
    std::string n;
    size_t k = 0;
    if (k < t.size() && (t[k] == L'+' || t[k] == L'-')) n += char(t[k++]);
    bool dot = false, digits = false;
    while (k < t.size()) {
        wchar_t c = t[k];
        if (c >= L'0' && c <= L'9') { n += char(c); digits = true; }
        else if (c == L'.' && !dot) { n += '.'; dot = true; }
        else break;
        k++;
    }
    if (digits && k < t.size() && (t[k] == L'e' || t[k] == L'E')) {
        std::string e = "e";
        size_t k2 = k + 1;
        if (k2 < t.size() && (t[k2] == L'+' || t[k2] == L'-')) e += char(t[k2++]);
        bool ed = false;
        while (k2 < t.size() && t[k2] >= L'0' && t[k2] <= L'9') { e += char(t[k2++]); ed = true; }
        if (ed) n += e;
    }
    if (!digits) return Value::N(NAN);
    return Value::N(strtod(n.c_str(), nullptr));
}
NATIVE(Boolean_ctor) {
    UNUSED;
    bool b = rt.Truthy(A(0));
    if (rt.nativeNewTarget) {
        Obj* o = rt.NewObject(rt.booleanProto);
        o->cls = Cls::BoolBox;
        o->X().num = b ? 1 : 0;
        return Value::O(o);
    }
    return Value::B(b);
}
NATIVE(BooleanP_valueOf) {
    UNUSED;
    if (thisV.t == Value::Bool) return thisV;
    if (thisV.IsObj() && thisV.o->cls == Cls::BoolBox) return Value::B(thisV.o->x->num != 0);
    rt.Throw(L"TypeError", L"Boolean.prototype.valueOf requires that 'this' be a Boolean");
}
NATIVE(BooleanP_toString) { UNUSED; std::vector<Value> none; return Value::S(BooleanP_valueOf(rt, self, thisV, none).b ? L"true" : L"false"); }

NATIVE(Symbol_ctor) {
    UNUSED;
    if (rt.nativeNewTarget) rt.Throw(L"TypeError", L"Symbol is not a constructor");
    std::wstring desc = A(0).IsUndef() ? L"" : rt.ToStr(A(0));
    Value v;
    v.t = Value::Symbol;
    v.s = std::make_shared<const std::wstring>(L"\x01#" + std::to_wstring(++rt.symbolCounter) + L":" + desc);
    return v;
}
NATIVE(Symbol_for) {
    UNUSED;
    Value v;
    v.t = Value::Symbol;
    v.s = std::make_shared<const std::wstring>(L"\x01reg:" + rt.ToStr(A(0)));
    return v;
}
NATIVE(SymbolP_toString) {
    UNUSED;
    Value s = thisV;
    if (s.IsObj() && s.o->cls == Cls::Symbol_) { s = Value(); s.t = Value::Symbol; s.s = std::make_shared<const std::wstring>(thisV.o->x->str); }
    if (s.t != Value::Symbol) rt.Throw(L"TypeError", L"not a symbol");
    return Value::S(L"Symbol(" + rt.ToStr(rt.Get(s, L"description")) + L")");
}
Value WellKnown(const wchar_t* name) {
    Value v;
    v.t = Value::Symbol;
    v.s = std::make_shared<const std::wstring>(std::wstring(L"\x01Symbol.") + name);
    return v;
}

// ================= Error =================

NATIVE(Error_ctor) {
    UNUSED;
    Obj* proto = nullptr;
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    if (!proto) {
        Value p = rt.Get(Value::O(self), L"prototype");
        proto = p.IsObj() ? p.o : rt.errorProto;
    }
    Obj* e = rt.NewObject(proto);
    e->cls = Cls::Error;
    if (!A(0).IsUndef()) rt.DefineData(e, L"message", Value::S(rt.ToStr(A(0))), false);
    if (A(1).IsObj() && rt.HasProperty(A(1).o, L"cause")) rt.DefineData(e, L"cause", rt.Get(A(1), L"cause"), false);
    std::wstring nm = rt.ToStr(rt.Get(Value::O(e), L"name"));
    rt.FillErrorStack(e, nm + L": " + rt.ToStr(rt.Get(Value::O(e), L"message")));
    return Value::O(e);
}
NATIVE(ErrorP_toString) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"Error.prototype.toString called on non-object");
    std::wstring n = rt.Get(thisV, L"name").IsUndef() ? L"Error" : rt.ToStr(rt.Get(thisV, L"name"));
    std::wstring m = rt.Get(thisV, L"message").IsUndef() ? L"" : rt.ToStr(rt.Get(thisV, L"message"));
    if (n.empty()) return Value::S(m);
    if (m.empty()) return Value::S(n);
    return Value::S(n + L": " + m);
}
NATIVE(Error_capture) { UNUSED; return Value(); }

// ================= Math =================

std::mt19937_64& Rng() {
    static thread_local std::mt19937_64 g(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9E3779B97F4A7C15ULL);
    return g;
}
#define MATH1(nm, expr) NATIVE(Math_##nm) { UNUSED; double x = rt.ToNum(A(0)); (void)x; return Value::N(expr); }
MATH1(abs, std::fabs(x))
MATH1(floor, std::floor(x))
MATH1(ceil, std::ceil(x))
MATH1(trunc, std::trunc(x))
MATH1(sqrt, std::sqrt(x))
MATH1(cbrt, std::cbrt(x))
MATH1(sin, std::sin(x))
MATH1(cos, std::cos(x))
MATH1(tan, std::tan(x))
MATH1(asin, std::asin(x))
MATH1(acos, std::acos(x))
MATH1(atan, std::atan(x))
MATH1(sinh, std::sinh(x))
MATH1(cosh, std::cosh(x))
MATH1(tanh, std::tanh(x))
MATH1(exp, std::exp(x))
MATH1(expm1, std::expm1(x))
MATH1(log, std::log(x))
MATH1(log2, std::log2(x))
MATH1(log10, std::log10(x))
MATH1(log1p, std::log1p(x))
MATH1(fround, double(float(x)))
MATH1(sign, std::isnan(x) ? NAN : x > 0 ? 1.0 : x < 0 ? -1.0 : x)
MATH1(round, std::isfinite(x) ? std::floor(x + 0.5) : x)
MATH1(clz32, [&] { uint32_t u = ToUint32JS(x); int n = 0; if (!u) return 32.0; while (!(u & 0x80000000u)) { u <<= 1; n++; } return double(n); }())
NATIVE(Math_atan2) { UNUSED; return Value::N(std::atan2(rt.ToNum(A(0)), rt.ToNum(A(1)))); }
NATIVE(Math_pow) {
    UNUSED;
    double x = rt.ToNum(A(0)), y = rt.ToNum(A(1));
    if (std::isnan(y) || ((x == 1 || x == -1) && std::isinf(y))) return Value::N(NAN);
    return Value::N(std::pow(x, y));
}
NATIVE(Math_imul) {
    UNUSED;
    uint32_t a = ToUint32JS(rt.ToNum(A(0))), b = ToUint32JS(rt.ToNum(A(1)));
    return Value::N(double(int32_t(a * b)));
}
NATIVE(Math_max) {
    UNUSED;
    double r = -INFINITY;
    for (auto& a : args) { double x = rt.ToNum(a); if (std::isnan(x)) return Value::N(NAN); if (x > r || (x == 0 && r == 0 && !std::signbit(x))) r = x; }
    return Value::N(r);
}
NATIVE(Math_min) {
    UNUSED;
    double r = INFINITY;
    for (auto& a : args) { double x = rt.ToNum(a); if (std::isnan(x)) return Value::N(NAN); if (x < r || (x == 0 && r == 0 && std::signbit(x))) r = x; }
    return Value::N(r);
}
NATIVE(Math_hypot) { UNUSED; double s = 0; for (auto& a : args) { double x = rt.ToNum(a); s += x * x; } return Value::N(std::sqrt(s)); }
NATIVE(Math_random) { UNUSED; return Value::N(double(Rng()() >> 11) * (1.0 / 9007199254740992.0)); }

// ================= JSON =================

struct JsonParser {
    Runtime& rt;
    const std::wstring& s;
    size_t i = 0;
    int depth = 0;
    [[noreturn]] void Err() { rt.Throw(L"SyntaxError", L"Unexpected token in JSON at position " + std::to_wstring(i)); }
    void Ws() { while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n' || s[i] == L'\r')) i++; }
    Value Parse() {
        Ws();
        if (i >= s.size()) Err();
        if (++depth > 500) rt.Throw(L"RangeError", L"JSON nesting too deep");
        struct D { int& d; ~D() { d--; } } dd{depth};
        wchar_t c = s[i];
        if (c == L'{') {
            i++;
            Obj* o = rt.NewObject();
            Ws();
            if (i < s.size() && s[i] == L'}') { i++; return Value::O(o); }
            while (true) {
                Ws();
                if (i >= s.size() || s[i] != L'"') Err();
                std::wstring k = Str();
                Ws();
                if (i >= s.size() || s[i] != L':') Err();
                i++;
                rt.DefineData(o, k, Parse());
                Ws();
                if (i < s.size() && s[i] == L',') { i++; continue; }
                if (i < s.size() && s[i] == L'}') { i++; return Value::O(o); }
                Err();
            }
        }
        if (c == L'[') {
            i++;
            Obj* a = rt.NewArray(0);
            Ws();
            if (i < s.size() && s[i] == L']') { i++; return Value::O(a); }
            while (true) {
                a->elems.push_back(Parse());
                Ws();
                if (i < s.size() && s[i] == L',') { i++; continue; }
                if (i < s.size() && s[i] == L']') { i++; return Value::O(a); }
                Err();
            }
        }
        if (c == L'"') return Value::S(Str());
        if (s.compare(i, 4, L"true") == 0) { i += 4; return Value::B(true); }
        if (s.compare(i, 5, L"false") == 0) { i += 5; return Value::B(false); }
        if (s.compare(i, 4, L"null") == 0) { i += 4; return Value::Null_(); }
        if (c == L'-' || (c >= L'0' && c <= L'9')) {
            size_t st = i;
            if (s[i] == L'-') i++;
            while (i < s.size() && ((s[i] >= L'0' && s[i] <= L'9') || s[i] == L'.' || s[i] == L'e' || s[i] == L'E' || s[i] == L'+' || s[i] == L'-')) i++;
            wchar_t* end = nullptr;
            double v = wcstod(s.c_str() + st, &end);
            if (end != s.c_str() + i) Err();
            return Value::N(v);
        }
        Err();
    }
    std::wstring Str() {
        i++;
        std::wstring o;
        while (true) {
            if (i >= s.size()) Err();
            wchar_t c = s[i++];
            if (c == L'"') break;
            if (c == L'\\') {
                if (i >= s.size()) Err();
                wchar_t e = s[i++];
                switch (e) {
                case L'n': o += L'\n'; break;
                case L't': o += L'\t'; break;
                case L'r': o += L'\r'; break;
                case L'b': o += L'\b'; break;
                case L'f': o += L'\f'; break;
                case L'u': {
                    if (i + 4 > s.size()) Err();
                    unsigned v = 0;
                    for (int k = 0; k < 4; k++) {
                        wchar_t h = s[i++];
                        v = v * 16 + unsigned((h >= L'0' && h <= L'9') ? h - L'0' : (h >= L'a' && h <= L'f') ? h - L'a' + 10 : (h >= L'A' && h <= L'F') ? h - L'A' + 10 : 99);
                        if (v >= 0x10000 * 16) Err();
                    }
                    o += wchar_t(v);
                    break;
                }
                default: o += e;
                }
            } else if (c < 0x20) Err();
            else o += c;
        }
        return o;
    }
};

NATIVE(JSON_parse) {
    UNUSED;
    std::wstring src = rt.ToStr(A(0));
    JsonParser p{rt, src};
    Value v = p.Parse();
    p.Ws();
    if (p.i != src.size()) p.Err();
    if (IsCallable(A(1))) {
        Obj* holder = rt.NewObject();
        rt.DefineData(holder, L"", v);
        std::function<Value(const Value&, const std::wstring&)> walk = [&](const Value& h, const std::wstring& key) -> Value {
            Value val = rt.Get(h, key);
            if (val.IsObj()) {
                for (auto& k : rt.OwnKeys(val.o, true)) {
                    Value nv = walk(val, k);
                    if (nv.IsUndef()) rt.Delete(val.o, k);
                    else rt.Put(val, k, nv);
                }
            }
            std::vector<Value> a{Value::S(key), val};
            return rt.Call(A(1), h, a);
        };
        return walk(Value::O(holder), L"");
    }
    return v;
}

std::wstring JsonQuote(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        case L'\b': o += L"\\b"; break;
        case L'\f': o += L"\\f"; break;
        default:
            if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", unsigned(c)); o += b; }
            else o += c;
        }
    }
    return o + L"\"";
}

struct JsonWriter {
    Runtime& rt;
    Value replacer;
    std::vector<std::wstring> allow;
    bool hasAllow = false;
    std::wstring gap;
    std::vector<Obj*> stack;
    bool Write(const Value& holder, const std::wstring& key, Value v, const std::wstring& indent, std::wstring& out) {
        if (v.IsObj()) {
            Value tj = rt.Get(v, L"toJSON");
            if (IsCallable(tj)) { std::vector<Value> a{Value::S(key)}; v = rt.Call(tj, v, a); }
        }
        if (IsCallable(replacer)) { std::vector<Value> a{Value::S(key), v}; v = rt.Call(replacer, holder, a); }
        if (v.IsObj()) {
            if (v.o->cls == Cls::NumBox) v = Value::N(v.o->x->num);
            else if (v.o->cls == Cls::StrBox) v = Value::S(v.o->x->str);
            else if (v.o->cls == Cls::BoolBox) v = Value::B(v.o->x->num != 0);
        }
        if (v.IsBig()) {
            Value tj = rt.Get(v, L"toJSON");
            if (IsCallable(tj)) { std::vector<Value> a{Value::S(key)}; v = rt.Call(tj, v, a); }
            if (v.IsBig()) rt.Throw(L"TypeError", L"Do not know how to serialize a BigInt");
        }
        switch (v.t) {
        case Value::Null: out += L"null"; return true;
        case Value::Bool: out += v.b ? L"true" : L"false"; return true;
        case Value::Num: out += std::isfinite(v.n) ? Runtime::NumToStr(v.n) : L"null"; return true;
        case Value::String: out += JsonQuote(*v.s); return true;
        case Value::Undef: case Value::Symbol: return false;
        case Value::Object: break;
        }
        if (v.o->IsCallable()) return false;
        for (Obj* o : stack) if (o == v.o) rt.Throw(L"TypeError", L"Converting circular structure to JSON");
        if (stack.size() > 400) rt.Throw(L"RangeError", L"JSON nesting too deep");
        if (out.size() > 128u * 1024 * 1024) throw JSAbort{L"JSON çok büyük"};
        stack.push_back(v.o);
        std::wstring inner = indent + gap;
        if (v.o->cls == Cls::Array) {
            out += L'[';
            for (size_t i = 0; i < v.o->elems.size(); i++) {
                if (i) out += L',';
                if (!gap.empty()) out += L"\n" + inner;
                if (!Write(v, std::to_wstring(i), v.o->elems[i], inner, out)) out += L"null";
            }
            if (!gap.empty() && !v.o->elems.empty()) out += L"\n" + indent;
            out += L']';
        } else {
            out += L'{';
            bool first = true;
            std::vector<std::wstring> keys = hasAllow ? allow : rt.OwnKeys(v.o, true);
            for (auto& k : keys) {
                std::wstring part;
                if (!Write(v, k, rt.Get(v, k), inner, part)) continue;
                if (!first) out += L',';
                first = false;
                if (!gap.empty()) out += L"\n" + inner;
                out += JsonQuote(k) + (gap.empty() ? L":" : L": ") + part;
            }
            if (!gap.empty() && !first) out += L"\n" + indent;
            out += L'}';
        }
        stack.pop_back();
        return true;
    }
};

NATIVE(JSON_stringify) {
    UNUSED;
    JsonWriter w{rt, Value(), {}, false, L"", {}};
    if (IsCallable(A(1))) w.replacer = A(1);
    else if (A(1).IsObj() && A(1).o->cls == Cls::Array) {
        w.hasAllow = true;
        for (auto& v : A(1).o->elems) w.allow.push_back(rt.ToStr(v));
    }
    if (A(2).IsNum()) w.gap = std::wstring(size_t(std::max(0.0, std::min(10.0, A(2).n))), L' ');
    else if (A(2).IsStr()) w.gap = A(2).s->substr(0, 10);
    Obj* holder = rt.NewObject();
    rt.DefineData(holder, L"", A(0));
    std::wstring out;
    if (!w.Write(Value::O(holder), L"", A(0), L"", out)) return Value();
    return Value::S(std::move(out));
}

// ================= console =================

Value ConsoleOut(Runtime& rt, std::vector<Value>& args, int level, const wchar_t* prefix) {
    std::wstring line = prefix;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) line += L' ';
        try { line += rt.Inspect(args[i]); } catch (JSThrow&) { line += L"[?]"; }
        if (line.size() > 4000) { line.resize(4000); line += L"…"; break; }
    }
    if (rt.consoleLines.size() < 5000) rt.consoleLines.push_back(line);
    if (rt.host) rt.host->Log(level, line);
    return Value();
}
NATIVE(Console_log) { UNUSED; return ConsoleOut(rt, args, 0, L""); }
NATIVE(Console_warn) { UNUSED; return ConsoleOut(rt, args, 1, L"⚠ "); }
NATIVE(Console_error) { UNUSED; return ConsoleOut(rt, args, 2, L"✖ "); }
NATIVE(Console_noop) { UNUSED; return Value(); }
NATIVE(Console_assert) {
    UNUSED;
    if (rt.Truthy(A(0))) return Value();
    std::vector<Value> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    rest.insert(rest.begin(), Value::S(L"Assertion failed:"));
    return ConsoleOut(rt, rest, 2, L"✖ ");
}

// ================= Zamanlayicilar ve genel =================

Value AddTimer(Runtime& rt, std::vector<Value>& args, bool interval) {
    if (rt.timers.size() > 10000) return Value::N(0);
    Timer t;
    t.id = rt.nextTimerId++;
    double ms = std::max(0.0, rt.ToNum(A(1)));
    if (std::isnan(ms)) ms = 0;
    t.due = rt.Now() + ms;
    t.interval = interval ? std::max(ms, 4.0) : -1;
    t.fn = A(0);
    if (args.size() > 2) t.args.assign(args.begin() + 2, args.end());
    rt.timers.push_back(t);
    return Value::N(t.id);
}
NATIVE(Global_setTimeout) { UNUSED; return AddTimer(rt, args, false); }
NATIVE(Global_setInterval) { UNUSED; return AddTimer(rt, args, true); }
NATIVE(Global_clearTimeout) {
    UNUSED;
    int id = int(rt.ToNum(A(0)));
    for (size_t i = 0; i < rt.timers.size(); i++) if (rt.timers[i].id == id) { rt.timers.erase(rt.timers.begin() + long(i)); break; }
    return Value();
}
NATIVE(Global_queueMicrotask) {
    UNUSED;
    if (!IsCallable(A(0))) rt.Throw(L"TypeError", L"queueMicrotask requires a function");
    rt.Enqueue(A(0), {});
    return Value();
}

static Value CloneValueDeep(Runtime& rt, const Value& v, std::unordered_map<Obj*, Obj*>& seen) {
    if (!v.IsObj()) return v;
    Obj* o = v.o;
    if (seen.count(o)) return Value::O(seen[o]);

    if (o->cls == Cls::Array) {
        Obj* clone = rt.NewArray();
        seen[o] = clone;
        for (auto& item : o->elems) clone->elems.push_back(CloneValueDeep(rt, item, seen));
        return Value::O(clone);
    }
    if (o->cls == Cls::Date && o->x) {
        Obj* clone = rt.NewObject(rt.dateProto);
        clone->cls = Cls::Date;
        clone->X().num = o->x->num;
        seen[o] = clone;
        return Value::O(clone);
    }
    if (o->cls == Cls::RegExp && o->x) {
        Obj* clone = rt.NewObject(rt.regexpProto);
        clone->cls = Cls::RegExp;
        clone->X().str = o->x->str;
        clone->X().flags = o->x->flags;
        clone->X().re = o->x->re;
        seen[o] = clone;
        return Value::O(clone);
    }
    if (o->cls == Cls::Map && o->x) {
        Obj* clone = rt.NewObject(rt.mapProto);
        clone->cls = Cls::Map;
        seen[o] = clone;
        for (size_t i = 0; i < o->x->entries.size(); i++) {
            if (i < o->x->dead.size() && o->x->dead[i]) continue;
            Value ck = CloneValueDeep(rt, o->x->entries[i].first, seen);
            Value cv = CloneValueDeep(rt, o->x->entries[i].second, seen);
            clone->X().entries.push_back({ck, cv});
            clone->X().dead.push_back(false);
        }
        return Value::O(clone);
    }
    if (o->cls == Cls::Set && o->x) {
        Obj* clone = rt.NewObject(rt.setProto);
        clone->cls = Cls::Set;
        seen[o] = clone;
        for (size_t i = 0; i < o->x->entries.size(); i++) {
            if (i < o->x->dead.size() && o->x->dead[i]) continue;
            Value ck = CloneValueDeep(rt, o->x->entries[i].first, seen);
            clone->X().entries.push_back({ck, ck});
            clone->X().dead.push_back(false);
        }
        return Value::O(clone);
    }
    if (o->cls == Cls::ArrayBuffer && o->x && o->x->bufferData) {
        Obj* clone = rt.NewObject(rt.arrayBufferProto);
        clone->cls = Cls::ArrayBuffer;
        clone->X().bufferData = std::make_shared<std::vector<uint8_t>>(*o->x->bufferData);
        clone->X().byteLength = o->x->byteLength;
        seen[o] = clone;
        return Value::O(clone);
    }
    if (o->cls == Cls::TypedArray && o->x) {
        Value bufClone = CloneValueDeep(rt, Value::O(o->x->arrayBufferObj), seen);
        Obj* clone = rt.NewObject(o->proto);
        clone->cls = Cls::TypedArray;
        clone->X().arrayBufferObj = bufClone.o;
        clone->X().bufferData = bufClone.o->x->bufferData;
        clone->X().byteOffset = o->x->byteOffset;
        clone->X().byteLength = o->x->byteLength;
        clone->X().typedLength = o->x->typedLength;
        clone->X().typedKind = o->x->typedKind;
        seen[o] = clone;
        return Value::O(clone);
    }
    if (o->cls == Cls::Object) {
        Obj* clone = rt.NewObject(rt.objectProto);
        seen[o] = clone;
        for (auto& k : rt.OwnKeys(o, true)) {
            clone->props.Put(k).v = CloneValueDeep(rt, rt.Get(Value::O(o), k), seen);
        }
        return Value::O(clone);
    }
    rt.Throw(L"DOMException", L"DataCloneError: could not clone object");
}

NATIVE(Global_structuredClone) {
    UNUSED;
    if (args.empty()) rt.Throw(L"TypeError", L"structuredClone requires at least 1 argument");
    std::unordered_map<Obj*, Obj*> seen;
    return CloneValueDeep(rt, A(0), seen);
}

NATIVE(Global_eval) {
    UNUSED;
    if (!A(0).IsStr()) return A(0);
    return rt.EvalGlobal(*A(0).s, L"eval");
}

// ================= Promise =================

NATIVE(Promise_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Promise constructor cannot be invoked without 'new'");
    if (!IsCallable(A(0))) rt.Throw(L"TypeError", L"Promise resolver is not a function");
    Obj* p = rt.NewPromise();
    Value np = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
    if (np.IsObj()) p->proto = np.o;
    Obj* flag = rt.NewObject();
    flag->X().num = 0;
    Obj* rs = rt.NewFunction(rt.ctors[L"__resolveFn"]->x->native, L"resolve", 1);
    rs->x->slots = {Value::O(p), Value::O(flag)};
    Obj* rj = rt.NewFunction(rt.ctors[L"__rejectFn"]->x->native, L"reject", 1);
    rj->x->slots = {Value::O(p), Value::O(flag)};
    try {
        std::vector<Value> a{Value::O(rs), Value::O(rj)};
        rt.Call(A(0), Value(), a);
    } catch (JSThrow& t) {
        if (!flag->x->num) { flag->x->num = 1; rt.RejectPromise(p, t.v); }
    }
    return Value::O(p);
}
Obj* ThisPromise(Runtime& rt, const Value& thisV) {
    if (thisV.IsObj() && thisV.o->cls == Cls::Promise) return thisV.o;
    rt.Throw(L"TypeError", L"Method Promise.prototype.then called on incompatible receiver");
}
NATIVE(PromiseP_then) {
    UNUSED;
    Obj* p = ThisPromise(rt, thisV);
    Obj* d = rt.NewPromise();
    rt.PromiseThen(p, IsCallable(A(0)) ? A(0) : Value(), IsCallable(A(1)) ? A(1) : Value(), d);
    return Value::O(d);
}
NATIVE(PromiseP_catch) {
    UNUSED;
    std::vector<Value> a{Value(), A(0)};
    Value then = rt.Get(thisV, L"then");
    return rt.Call(then, thisV, a);
}
NATIVE(Finally_ful) {
    UNUSED;
    std::vector<Value> none;
    rt.Call(self->x->slots[0], Value(), none);
    return A(0);
}
NATIVE(Finally_rej) {
    UNUSED;
    std::vector<Value> none;
    rt.Call(self->x->slots[0], Value(), none);
    throw JSThrow{A(0)};
}
NATIVE(PromiseP_finally) {
    UNUSED;
    Obj* p = ThisPromise(rt, thisV);
    Obj* d = rt.NewPromise();
    if (!IsCallable(A(0))) { rt.PromiseThen(p, Value(), Value(), d); return Value::O(d); }
    Obj* f1 = rt.NewFunction(Finally_ful, L"", 1);
    f1->x->slots = {A(0)};
    Obj* f2 = rt.NewFunction(Finally_rej, L"", 1);
    f2->x->slots = {A(0)};
    rt.PromiseThen(p, Value::O(f1), Value::O(f2), d);
    return Value::O(d);
}
NATIVE(Promise_resolve) {
    UNUSED;
    if (A(0).IsObj() && A(0).o->cls == Cls::Promise) return A(0);
    Obj* p = rt.NewPromise();
    rt.ResolvePromise(p, A(0));
    return Value::O(p);
}
NATIVE(Promise_reject) {
    UNUSED;
    Obj* p = rt.NewPromise();
    rt.RejectPromise(p, A(0));
    return Value::O(p);
}
// Promise.all / allSettled / race / any icin ortak toplayici
NATIVE(Combinator_ful) {
    UNUSED;
    // slots: 0 sonuc dizisi, 1 kalan sayaci (nesne), 2 indeks, 3 hedef promise, 4 mod
    Obj* arr = self->x->slots[0].o;
    Obj* cnt = self->x->slots[1].o;
    size_t idx = size_t(self->x->slots[2].n);
    Obj* target = self->x->slots[3].o;
    int mode = int(self->x->slots[4].n);
    if (self->x->num) return Value();
    self->x->num = 1;
    Value v = A(0);
    if (mode == 1) {
        Obj* r = rt.NewObject();
        rt.DefineData(r, L"status", Value::S(L"fulfilled"));
        rt.DefineData(r, L"value", v);
        v = Value::O(r);
    }
    if (mode == 2) { rt.ResolvePromise(target, v); return Value(); }
    arr->elems[idx] = v;
    if (--cnt->x->num == 0) rt.ResolvePromise(target, Value::O(arr));
    return Value();
}
NATIVE(Combinator_rej) {
    UNUSED;
    Obj* arr = self->x->slots[0].o;
    Obj* cnt = self->x->slots[1].o;
    size_t idx = size_t(self->x->slots[2].n);
    Obj* target = self->x->slots[3].o;
    int mode = int(self->x->slots[4].n);
    if (self->x->num) return Value();
    self->x->num = 1;
    if (mode == 1) {
        Obj* r = rt.NewObject();
        rt.DefineData(r, L"status", Value::S(L"rejected"));
        rt.DefineData(r, L"reason", A(0));
        arr->elems[idx] = Value::O(r);
        if (--cnt->x->num == 0) rt.ResolvePromise(target, Value::O(arr));
        return Value();
    }
    if (mode == 3) {
        arr->elems[idx] = A(0);
        if (--cnt->x->num == 0) rt.RejectPromise(target, Value::O(rt.NewError(L"Error", L"All promises were rejected")));
        return Value();
    }
    rt.RejectPromise(target, A(0));
    return Value();
}
Value Combinator(Runtime& rt, std::vector<Value>& args, int mode) {
    // mode: 0 all, 1 allSettled, 2 race, 3 any
    Obj* target = rt.NewPromise();
    std::vector<Value> items = rt.IterateToList(A(0));
    Obj* arr = rt.NewArray(items.size());
    Obj* cnt = rt.NewObject();
    cnt->X().num = double(items.size());
    if (items.empty()) {
        if (mode == 2) return Value::O(target);
        if (mode == 3) rt.RejectPromise(target, Value::O(rt.NewError(L"Error", L"All promises were rejected")));
        else rt.ResolvePromise(target, Value::O(arr));
        return Value::O(target);
    }
    for (size_t i = 0; i < items.size(); i++) {
        Obj* p = nullptr;
        if (items[i].IsObj() && items[i].o->cls == Cls::Promise) p = items[i].o;
        else { p = rt.NewPromise(); rt.ResolvePromise(p, items[i]); }
        Obj* f = rt.NewFunction(mode == 3 ? Combinator_rej : Combinator_ful, L"", 1);
        Obj* r = rt.NewFunction(mode == 3 ? Combinator_ful : Combinator_rej, L"", 1);
        std::vector<Value> slots{Value::O(arr), Value::O(cnt), Value::N(double(i)), Value::O(target), Value::N(mode)};
        f->x->slots = slots;
        r->x->slots = slots;
        f->x->num = 0; // 'zaten cagrildi' bayragi olarak kullanilir
        r->x->num = 0;
        if (mode == 3) {
            // any: ilk basari kazanir (ful = race modu), hepsi reddedilirse hata
            Obj* win = rt.NewFunction(Combinator_ful, L"", 1);
            win->x->slots = {Value::O(arr), Value::O(cnt), Value::N(double(i)), Value::O(target), Value::N(2)};
            win->x->num = 0;
            rt.PromiseThen(p, Value::O(win), Value::O(f), nullptr);
        } else {
            rt.PromiseThen(p, Value::O(f), Value::O(r), nullptr);
        }
    }
    return Value::O(target);
}
NATIVE(Promise_all) { UNUSED; return Combinator(rt, args, 0); }
NATIVE(Promise_allSettled) { UNUSED; return Combinator(rt, args, 1); }
NATIVE(Promise_race) { UNUSED; return Combinator(rt, args, 2); }
NATIVE(Promise_any) { UNUSED; return Combinator(rt, args, 3); }
NATIVE(Promise_withResolvers) {
    UNUSED;
    Obj* p = rt.NewPromise();
    Obj* res = rt.NewObject();
    res->props.Put(L"promise").v = Value::O(p);

    Obj* flag = rt.NewObject();
    flag->X().num = 0;

    Obj* rf = rt.NewFunction(PromiseResolveFnImpl, L"resolve", 1);
    rf->X().slots = {Value::O(p), Value::O(flag)};
    res->props.Put(L"resolve").v = Value::O(rf);

    Obj* jf = rt.NewFunction(PromiseRejectFnImpl, L"reject", 1);
    jf->X().slots = {Value::O(p), Value::O(flag)};
    res->props.Put(L"reject").v = Value::O(jf);

    return Value::O(res);
}

// ================= Map / Set =================

Obj* ThisColl(Runtime& rt, const Value& thisV, Cls c, const wchar_t* name) {
    if (thisV.IsObj() && thisV.o->cls == c) return thisV.o;
    rt.Throw(L"TypeError", std::wstring(L"Method ") + name + L" called on incompatible receiver");
}
void CollCompact(Obj* m) {
    ObjExtra& x = *m->x;
    if (x.entries.size() < 32 || x.index.size() * 2 > x.entries.size()) return;
    std::vector<std::pair<Value, Value>> ne;
    for (size_t i = 0; i < x.entries.size(); i++) if (!x.dead[i]) ne.push_back(x.entries[i]);
    x.entries.swap(ne);
    x.dead.assign(x.entries.size(), false);
    x.index.clear();
}
void CollReindex(Runtime& rt, Obj* m) {
    ObjExtra& x = *m->x;
    if (!x.index.empty() || x.entries.empty()) return;
    for (size_t i = 0; i < x.entries.size(); i++) if (!x.dead[i]) x.index[rt.MapKey(x.entries[i].first)] = i;
}
long CollFind(Runtime& rt, Obj* m, const Value& k) {
    CollReindex(rt, m);
    auto it = m->x->index.find(rt.MapKey(k));
    return it == m->x->index.end() ? -1 : long(it->second);
}
void CollSet(Runtime& rt, Obj* m, const Value& k, const Value& v) {
    long i = CollFind(rt, m, k);
    if (i >= 0) { m->x->entries[size_t(i)].second = v; return; }
    if (m->x->entries.size() > 20000000) rt.Throw(L"RangeError", L"Map maximum size exceeded");
    Value key = k;
    if (key.IsNum() && key.n == 0) key.n = 0;
    m->x->entries.emplace_back(key, v);
    m->x->dead.push_back(false);
    m->x->index[rt.MapKey(key)] = m->x->entries.size() - 1;
}
bool CollDelete(Runtime& rt, Obj* m, const Value& k) {
    long i = CollFind(rt, m, k);
    if (i < 0) return false;
    m->x->dead[size_t(i)] = true;
    m->x->entries[size_t(i)] = {Value(), Value()};
    m->x->index.erase(rt.MapKey(k));
    CollCompact(m);
    return true;
}
size_t CollSize(Obj* m) { size_t n = 0; for (bool d : m->x->dead) if (!d) n++; return n; }

Obj* NewColl(Runtime& rt, Cls c) {
    Obj* proto = c == Cls::Map ? rt.mapProto : rt.setProto;
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* m = rt.NewObject(proto);
    m->cls = c;
    m->X();
    return m;
}
NATIVE(Map_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Constructor Map requires 'new'");
    Obj* m = NewColl(rt, Cls::Map);
    if (!A(0).IsNullish())
        for (auto& e : rt.IterateToList(A(0))) CollSet(rt, m, rt.GetV(e, Value::N(0)), rt.GetV(e, Value::N(1)));
    return Value::O(m);
}
NATIVE(Set_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) rt.Throw(L"TypeError", L"Constructor Set requires 'new'");
    Obj* m = NewColl(rt, Cls::Set);
    if (!A(0).IsNullish())
        for (auto& e : rt.IterateToList(A(0))) CollSet(rt, m, e, e);
    return Value::O(m);
}
NATIVE(MapP_get) { UNUSED; Obj* m = ThisColl(rt, thisV, Cls::Map, L"Map.prototype.get"); long i = CollFind(rt, m, A(0)); return i < 0 ? Value() : m->x->entries[size_t(i)].second; }
NATIVE(MapP_set) { UNUSED; Obj* m = ThisColl(rt, thisV, Cls::Map, L"Map.prototype.set"); CollSet(rt, m, A(0), A(1)); return thisV; }
NATIVE(MapP_has) { UNUSED; Obj* m = thisV.IsObj() && (thisV.o->cls == Cls::Map || thisV.o->cls == Cls::Set) ? thisV.o : ThisColl(rt, thisV, Cls::Map, L"has"); return Value::B(CollFind(rt, m, A(0)) >= 0); }
NATIVE(MapP_delete) { UNUSED; Obj* m = thisV.IsObj() && (thisV.o->cls == Cls::Map || thisV.o->cls == Cls::Set) ? thisV.o : ThisColl(rt, thisV, Cls::Map, L"delete"); return Value::B(CollDelete(rt, m, A(0))); }
NATIVE(MapP_clear) { UNUSED; Obj* m = thisV.IsObj() && (thisV.o->cls == Cls::Map || thisV.o->cls == Cls::Set) ? thisV.o : ThisColl(rt, thisV, Cls::Map, L"clear"); m->x->entries.clear(); m->x->dead.clear(); m->x->index.clear(); return Value(); }
NATIVE(MapP_size) { UNUSED; Obj* m = thisV.IsObj() && (thisV.o->cls == Cls::Map || thisV.o->cls == Cls::Set) ? thisV.o : ThisColl(rt, thisV, Cls::Map, L"size"); return Value::N(double(CollSize(m))); }
NATIVE(SetP_add) { UNUSED; Obj* m = ThisColl(rt, thisV, Cls::Set, L"Set.prototype.add"); CollSet(rt, m, A(0), A(0)); return thisV; }
NATIVE(MapP_forEach) {
    UNUSED;
    Obj* m = thisV.IsObj() && (thisV.o->cls == Cls::Map || thisV.o->cls == Cls::Set) ? thisV.o : ThisColl(rt, thisV, Cls::Map, L"forEach");
    for (size_t i = 0; i < m->x->entries.size(); i++) {
        if (m->x->dead[i]) continue;
        auto e = m->x->entries[i];
        std::vector<Value> a{e.second, e.first, thisV};
        rt.Call(A(0), A(1), a);
    }
    return Value();
}
Value CollIter(Runtime& rt, const Value& thisV, int kind) {
    Obj* m = thisV.IsObj() && (thisV.o->cls == Cls::Map || thisV.o->cls == Cls::Set) ? thisV.o : ThisColl(rt, thisV, Cls::Map, L"iterator");
    std::vector<Value> out;
    for (size_t i = 0; i < m->x->entries.size(); i++) {
        if (m->x->dead[i]) continue;
        auto& e = m->x->entries[i];
        if (kind == 0) out.push_back(e.first);
        else if (kind == 1) out.push_back(e.second);
        else out.push_back(Value::O(rt.NewArrayFrom({e.first, e.second})));
    }
    return Value::O(rt.MakeIteratorFromList(out));
}
NATIVE(MapP_keys) { UNUSED; return CollIter(rt, thisV, 0); }
NATIVE(MapP_values) { UNUSED; return CollIter(rt, thisV, 1); }
NATIVE(MapP_entries) { UNUSED; return CollIter(rt, thisV, 2); }

// ================= Yineleyici =================

NATIVE(IterP_next) {
    UNUSED;
    if (!thisV.IsObj()) rt.Throw(L"TypeError", L"next called on non-object");
    Obj* it = thisV.o;
    size_t i = size_t(it->X().num);
    Obj* r = rt.NewObject();
    if (i < it->elems.size()) {
        rt.DefineData(r, L"value", it->elems[i]);
        rt.DefineData(r, L"done", Value::B(false));
        it->x->num = double(i + 1);
    } else {
        rt.DefineData(r, L"value", Value());
        rt.DefineData(r, L"done", Value::B(true));
        it->elems.clear();
    }
    return Value::O(r);
}
NATIVE(IterP_self) { UNUSED; return thisV; }
NATIVE(IterP_return) {
    UNUSED;
    if (thisV.IsObj()) { thisV.o->elems.clear(); thisV.o->X().num = 0; }
    Obj* r = rt.NewObject();
    rt.DefineData(r, L"value", A(0));
    rt.DefineData(r, L"done", Value::B(true));
    return Value::O(r);
}

// ================= Reflect =================
NATIVE(Reflect_ownKeys) { UNUSED; return Object_getOwnPropertyNames(rt, self, thisV, args); }
NATIVE(Reflect_has) { UNUSED; if (!A(0).IsObj()) rt.Throw(L"TypeError", L"Reflect.has called on non-object"); return Value::B(rt.HasProperty(A(0).o, rt.KeyOf(A(1)))); }
NATIVE(Reflect_get) { UNUSED; return rt.Get(A(0), rt.KeyOf(A(1))); }
NATIVE(Reflect_set) { UNUSED; rt.Put(A(0), rt.KeyOf(A(1)), A(2)); return Value::B(true); }
NATIVE(Reflect_apply) { UNUSED; std::vector<Value> l = ListOf(rt, A(2)); return rt.Call(A(0), A(1), l); }
NATIVE(Reflect_construct) { UNUSED; std::vector<Value> l = ListOf(rt, A(1)); return rt.Construct(A(0), l, A(2).IsObj() ? A(2).o : nullptr); }
NATIVE(Reflect_deleteProperty) { UNUSED; if (!A(0).IsObj()) return Value::B(false); return Value::B(rt.Delete(A(0).o, rt.KeyOf(A(1)))); }

// ================= Kurulum yardimcilari =================

Obj* MakeCtor(Runtime& rt, const wchar_t* name, NativeFn fn, int length, Obj* proto) {
    Obj* c = rt.NewFunction(fn, name, length, true);
    if (proto) {
        Prop& p = c->props.Put(L"prototype");
        p.v = Value::O(proto);
        p.enumerable = false;
        p.writable = false;
        rt.DefineData(proto, L"constructor", Value::O(c), false);
    }
    rt.DefineData(rt.global, name, Value::O(c), false);
    rt.ctors[name] = c;
    return c;
}

} // namespace

Obj* Runtime::MakeIteratorFromList(std::vector<Value> list) {
    Obj* it = NewObject(iteratorProto);
    it->elems = std::move(list);
    it->X().num = 0;
    return it;
}

// ================= BigInt =================
BigNum ThisBig(Runtime& rt, const Value& v) {
    if (v.IsBig()) return BigOf(v);
    if (v.IsObj()) { Value b = rt.Get(v, L"\x02big"); if (b.IsBig()) return BigOf(b); }
    rt.Throw(L"TypeError", L"BigInt.prototype method called on incompatible receiver");
}
Value ToBigIntV(Runtime& rt, const Value& v0) {
    Value v = rt.ToPrimitive(v0, 1);
    switch (v.t) {
    case Value::BigInt: return v;
    case Value::Bool: return BigVal(BigNum::FromI64(v.b ? 1 : 0));
    case Value::String: {
        BigNum b;
        if (!BigParse(*v.s, b)) rt.Throw(L"SyntaxError", L"Cannot convert " + *v.s + L" to a BigInt");
        return BigVal(b);
    }
    case Value::Num:
        if (!std::isfinite(v.n) || v.n != std::trunc(v.n))
            rt.Throw(L"RangeError", L"The number " + Runtime::NumToStr(v.n) + L" cannot be converted to a BigInt because it is not an integer");
        return BigVal(BigNum::FromDouble(v.n));
    default:
        rt.Throw(L"TypeError", L"Cannot convert " + rt.ToStr(v.t == Value::Symbol ? Value::S(L"Symbol") : v) + L" to a BigInt");
    }
}
NATIVE(BigInt_fn) {
    UNUSED;
    if (rt.nativeNewTarget) rt.Throw(L"TypeError", L"BigInt is not a constructor");
    return ToBigIntV(rt, A(0));
}
size_t BitsArg(Runtime& rt, const Value& v) {
    double d = ToInteger(rt, v);
    if (d < 0 || d > 9007199254740991.0) rt.Throw(L"RangeError", L"Invalid value: not (convertible to) a safe integer");
    if (d > 1000000) rt.Throw(L"RangeError", L"Maximum BigInt size exceeded");
    return size_t(d);
}
NATIVE(BigInt_asUintN) { UNUSED; size_t bits = BitsArg(rt, A(0)); return BigVal(BigAsUintN(BigOf(ToBigIntV(rt, A(1))), bits)); }
NATIVE(BigInt_asIntN) { UNUSED; size_t bits = BitsArg(rt, A(0)); return BigVal(BigAsIntN(BigOf(ToBigIntV(rt, A(1))), bits)); }
NATIVE(BigIntP_toString) {
    UNUSED;
    BigNum b = ThisBig(rt, thisV);
    int radix = A(0).IsUndef() ? 10 : int(ToInteger(rt, A(0)));
    if (radix < 2 || radix > 36) rt.Throw(L"RangeError", L"toString() radix must be between 2 and 36");
    return Value::S(BigToString(b, radix));
}
NATIVE(BigIntP_valueOf) { UNUSED; return BigVal(ThisBig(rt, thisV)); }

void InstallBigInt(Runtime& rt) {
    rt.bigintProto = rt.NewObject(rt.objectProto);
    Obj* c = rt.NewFunction(BigInt_fn, L"BigInt", 1, true);
    Prop& pp = c->props.Put(L"prototype");
    pp.v = Value::O(rt.bigintProto);
    pp.enumerable = false;
    pp.writable = false;
    rt.DefineData(rt.bigintProto, L"constructor", Value::O(c), false);
    rt.DefineMethod(c, L"asUintN", BigInt_asUintN, 2);
    rt.DefineMethod(c, L"asIntN", BigInt_asIntN, 2);
    rt.DefineMethod(rt.bigintProto, L"toString", BigIntP_toString, 0);
    rt.DefineMethod(rt.bigintProto, L"toLocaleString", BigIntP_toString, 0);
    rt.DefineMethod(rt.bigintProto, L"valueOf", BigIntP_valueOf, 0);
    rt.DefineData(rt.bigintProto, L"\x01Symbol.toStringTag", Value::S(L"BigInt"), false);
    rt.DefineData(rt.global, L"BigInt", Value::O(c), false);
    rt.ctors[L"BigInt"] = c;
}

void InstallBuiltins(Runtime& rt) {
    Obj* g = rt.global;
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };

    // Prototipler
    rt.arrayProto = rt.NewObject(rt.objectProto);
    rt.stringProto = rt.NewObject(rt.objectProto);
    rt.stringProto->cls = Cls::StrBox;
    rt.stringProto->X();
    rt.numberProto = rt.NewObject(rt.objectProto);
    rt.booleanProto = rt.NewObject(rt.objectProto);
    rt.errorProto = rt.NewObject(rt.objectProto);
    rt.regexpProto = rt.NewObject(rt.objectProto);
    rt.dateProto = rt.NewObject(rt.objectProto);
    rt.promiseProto = rt.NewObject(rt.objectProto);
    rt.mapProto = rt.NewObject(rt.objectProto);
    rt.setProto = rt.NewObject(rt.objectProto);
    rt.symbolProto = rt.NewObject(rt.objectProto);
    rt.iteratorProto = rt.NewObject(rt.objectProto);

    // Object
    Obj* OC = MakeCtor(rt, L"Object", Object_ctor, 1, rt.objectProto);
    M(OC, L"keys", Object_keys, 1);
    M(OC, L"values", Object_values, 1);
    M(OC, L"entries", Object_entries, 1);
    M(OC, L"assign", Object_assign, 2);
    M(OC, L"create", Object_create, 2);
    M(OC, L"defineProperty", Object_defineProperty, 3);
    M(OC, L"defineProperties", Object_defineProperties, 2);
    M(OC, L"getPrototypeOf", Object_getPrototypeOf, 1);
    M(OC, L"setPrototypeOf", Object_setPrototypeOf, 2);
    M(OC, L"freeze", Object_freeze, 1);
    M(OC, L"isFrozen", Object_isFrozen, 1);
    M(OC, L"seal", Object_seal, 1);
    M(OC, L"isSealed", Object_isFrozen, 1);
    M(OC, L"preventExtensions", Object_seal, 1);
    M(OC, L"isExtensible", Object_isExtensible, 1);
    M(OC, L"getOwnPropertyNames", Object_getOwnPropertyNames, 1);
    M(OC, L"getOwnPropertySymbols", Object_getOwnPropertySymbols, 1);
    M(OC, L"getOwnPropertyDescriptor", Object_getOwnPropertyDescriptor, 2);
    M(OC, L"fromEntries", Object_fromEntries, 1);
    M(OC, L"is", Object_is, 2);
    M(OC, L"hasOwn", Object_hasOwn, 2);
    Obj* OP = rt.objectProto;
    M(OP, L"hasOwnProperty", ObjectP_hasOwnProperty, 1);
    M(OP, L"isPrototypeOf", ObjectP_isPrototypeOf, 1);
    M(OP, L"propertyIsEnumerable", ObjectP_propertyIsEnumerable, 1);
    M(OP, L"toString", ObjectP_toString, 0);
    M(OP, L"toLocaleString", ObjectP_toLocaleString, 0);
    M(OP, L"valueOf", ObjectP_valueOf, 0);
    rt.DefineAccessor(OP, L"__proto__", ObjectP_protoGet, ObjectP_protoSet);

    // Function
    MakeCtor(rt, L"Function", Function_ctor, 1, rt.functionProto);
    M(rt.functionProto, L"call", FunctionP_call, 1);
    M(rt.functionProto, L"apply", FunctionP_apply, 2);
    M(rt.functionProto, L"bind", FunctionP_bind, 1);
    M(rt.functionProto, L"toString", FunctionP_toString, 0);

    // Symbol (yineleyici anahtari diger kurulumlardan once)
    Obj* SC = MakeCtor(rt, L"Symbol", Symbol_ctor, 0, rt.symbolProto);
    for (const wchar_t* w : {L"iterator", L"asyncIterator", L"hasInstance", L"toPrimitive", L"toStringTag", L"species", L"isConcatSpreadable", L"unscopables", L"match", L"replace", L"search", L"split"})
        rt.DefineData(SC, w, WellKnown(w), false);
    M(SC, L"for", Symbol_for, 1);
    M(rt.symbolProto, L"toString", SymbolP_toString, 0);
    const std::wstring kIter = L"\x01Symbol.iterator";

    // Array
    Obj* AC = MakeCtor(rt, L"Array", Array_ctor, 1, rt.arrayProto);
    M(AC, L"isArray", Array_isArray, 1);
    M(AC, L"from", Array_from, 1);
    M(AC, L"of", Array_of, 0);
    Obj* AP = rt.arrayProto;
    M(AP, L"push", ArrayP_push, 1); M(AP, L"pop", ArrayP_pop, 0); M(AP, L"shift", ArrayP_shift, 0);
    M(AP, L"unshift", ArrayP_unshift, 1); M(AP, L"slice", ArrayP_slice, 2); M(AP, L"splice", ArrayP_splice, 2);
    M(AP, L"concat", ArrayP_concat, 1); M(AP, L"join", ArrayP_join, 1); M(AP, L"reverse", ArrayP_reverse, 0);
    M(AP, L"indexOf", ArrayP_indexOf, 1); M(AP, L"lastIndexOf", ArrayP_lastIndexOf, 1); M(AP, L"includes", ArrayP_includes, 1);
    M(AP, L"forEach", ArrayP_forEach, 1); M(AP, L"map", ArrayP_map, 1); M(AP, L"filter", ArrayP_filter, 1);
    M(AP, L"some", ArrayP_some, 1); M(AP, L"every", ArrayP_every, 1); M(AP, L"find", ArrayP_find, 1);
    M(AP, L"findIndex", ArrayP_findIndex, 1); M(AP, L"findLast", ArrayP_findLast, 1); M(AP, L"findLastIndex", ArrayP_findLastIndex, 1);
    M(AP, L"reduce", ArrayP_reduce, 1); M(AP, L"reduceRight", ArrayP_reduceRight, 1); M(AP, L"sort", ArrayP_sort, 1);
    M(AP, L"fill", ArrayP_fill, 1); M(AP, L"keys", ArrayP_keys, 0); M(AP, L"values", ArrayP_values, 0);
    M(AP, L"entries", ArrayP_entries, 0); M(AP, L"flat", ArrayP_flat, 0); M(AP, L"flatMap", ArrayP_flatMap, 1);
    M(AP, L"at", ArrayP_at, 1);
    M(AP, L"toReversed", ArrayP_toReversed, 0);
    M(AP, L"toSorted", ArrayP_toSorted, 1);
    M(AP, L"toSpliced", ArrayP_toSpliced, 2);
    M(AP, L"with", ArrayP_with, 2);
    M(AP, L"toString", ArrayP_toString, 0); M(AP, L"toLocaleString", ArrayP_toString, 0);
    rt.DefineData(AP, kIter, rt.Get(Value::O(AP), L"values"), false);

    // String
    Obj* STC = MakeCtor(rt, L"String", String_ctor, 1, rt.stringProto);
    M(STC, L"fromCharCode", String_fromCharCode, 1);
    M(STC, L"fromCodePoint", String_fromCodePoint, 1);
    M(STC, L"raw", String_raw, 1);
    Obj* SP = rt.stringProto;
    M(SP, L"toString", StringP_toString, 0); M(SP, L"valueOf", StringP_toString, 0);
    M(SP, L"charAt", StringP_charAt, 1); M(SP, L"charCodeAt", StringP_charCodeAt, 1); M(SP, L"codePointAt", StringP_codePointAt, 1);
    M(SP, L"indexOf", StringP_indexOf, 1); M(SP, L"lastIndexOf", StringP_lastIndexOf, 1); M(SP, L"includes", StringP_includes, 1);
    M(SP, L"startsWith", StringP_startsWith, 1); M(SP, L"endsWith", StringP_endsWith, 1); M(SP, L"slice", StringP_slice, 2);
    M(SP, L"substring", StringP_substring, 2); M(SP, L"substr", StringP_substr, 2);
    M(SP, L"toUpperCase", StringP_toUpperCase, 0); M(SP, L"toLowerCase", StringP_toLowerCase, 0);
    M(SP, L"toLocaleUpperCase", StringP_toUpperCase, 0); M(SP, L"toLocaleLowerCase", StringP_toLowerCase, 0);
    M(SP, L"trim", StringP_trim, 0); M(SP, L"trimStart", StringP_trimStart, 0); M(SP, L"trimEnd", StringP_trimEnd, 0);
    M(SP, L"trimLeft", StringP_trimStart, 0); M(SP, L"trimRight", StringP_trimEnd, 0);
    M(SP, L"padStart", StringP_padStart, 2); M(SP, L"padEnd", StringP_padEnd, 2); M(SP, L"repeat", StringP_repeat, 1);
    M(SP, L"concat", StringP_concat, 1); M(SP, L"at", StringP_at, 1); M(SP, L"localeCompare", StringP_localeCompare, 1);
    M(SP, L"normalize", StringP_normalize, 0);
    rt.DefineData(SP, kIter, Value::O(rt.NewFunction(StringP_iterator, L"[Symbol.iterator]", 0)), false);

    // Number / Boolean
    Obj* NC = MakeCtor(rt, L"Number", Number_ctor, 1, rt.numberProto);
    M(NC, L"isInteger", Number_isInteger, 1); M(NC, L"isSafeInteger", Number_isSafeInteger, 1);
    M(NC, L"isFinite", Number_isFinite, 1); M(NC, L"isNaN", Number_isNaN, 1);
    M(NC, L"parseFloat", Global_parseFloat, 1); M(NC, L"parseInt", Global_parseInt, 2);
    rt.DefineData(NC, L"MAX_SAFE_INTEGER", Value::N(9007199254740991.0), false);
    rt.DefineData(NC, L"MIN_SAFE_INTEGER", Value::N(-9007199254740991.0), false);
    rt.DefineData(NC, L"EPSILON", Value::N(2.220446049250313e-16), false);
    rt.DefineData(NC, L"MAX_VALUE", Value::N(1.7976931348623157e308), false);
    rt.DefineData(NC, L"MIN_VALUE", Value::N(5e-324), false);
    rt.DefineData(NC, L"POSITIVE_INFINITY", Value::N(INFINITY), false);
    rt.DefineData(NC, L"NEGATIVE_INFINITY", Value::N(-INFINITY), false);
    rt.DefineData(NC, L"NaN", Value::N(NAN), false);
    M(rt.numberProto, L"toString", NumberP_toString, 1); M(rt.numberProto, L"toFixed", NumberP_toFixed, 1);
    M(rt.numberProto, L"toPrecision", NumberP_toPrecision, 1); M(rt.numberProto, L"valueOf", NumberP_valueOf, 0);
    M(rt.numberProto, L"toLocaleString", NumberP_toLocaleString, 0);
    MakeCtor(rt, L"Boolean", Boolean_ctor, 1, rt.booleanProto);
    M(rt.booleanProto, L"valueOf", BooleanP_valueOf, 0); M(rt.booleanProto, L"toString", BooleanP_toString, 0);

    // Errors
    Obj* EC = MakeCtor(rt, L"Error", Error_ctor, 1, rt.errorProto);
    rt.DefineData(rt.errorProto, L"name", Value::S(L"Error"), false);
    rt.DefineData(rt.errorProto, L"message", Value::S(L""), false);
    M(rt.errorProto, L"toString", ErrorP_toString, 0);
    M(EC, L"captureStackTrace", Error_capture, 1);
    for (const wchar_t* n : {L"TypeError", L"RangeError", L"SyntaxError", L"ReferenceError", L"EvalError", L"URIError", L"AggregateError"}) {
        Obj* p = rt.NewObject(rt.errorProto);
        rt.DefineData(p, L"name", Value::S(n), false);
        Obj* c = MakeCtor(rt, n, Error_ctor, 1, p);
        c->proto = EC;
    }

    // Math
    Obj* Mth = rt.NewObject();
    rt.DefineData(g, L"Math", Value::O(Mth), false);
    struct { const wchar_t* n; NativeFn f; int l; } mf[] = {
        {L"abs", Math_abs, 1}, {L"floor", Math_floor, 1}, {L"ceil", Math_ceil, 1}, {L"trunc", Math_trunc, 1},
        {L"sqrt", Math_sqrt, 1}, {L"cbrt", Math_cbrt, 1}, {L"sin", Math_sin, 1}, {L"cos", Math_cos, 1},
        {L"tan", Math_tan, 1}, {L"asin", Math_asin, 1}, {L"acos", Math_acos, 1}, {L"atan", Math_atan, 1},
        {L"sinh", Math_sinh, 1}, {L"cosh", Math_cosh, 1}, {L"tanh", Math_tanh, 1}, {L"exp", Math_exp, 1},
        {L"expm1", Math_expm1, 1}, {L"log", Math_log, 1}, {L"log2", Math_log2, 1}, {L"log10", Math_log10, 1},
        {L"log1p", Math_log1p, 1}, {L"fround", Math_fround, 1}, {L"sign", Math_sign, 1}, {L"round", Math_round, 1},
        {L"clz32", Math_clz32, 1}, {L"atan2", Math_atan2, 2}, {L"pow", Math_pow, 2}, {L"imul", Math_imul, 2},
        {L"max", Math_max, 2}, {L"min", Math_min, 2}, {L"hypot", Math_hypot, 2}, {L"random", Math_random, 0}};
    for (auto& m : mf) M(Mth, m.n, m.f, m.l);
    rt.DefineData(Mth, L"PI", Value::N(3.141592653589793), false);
    rt.DefineData(Mth, L"E", Value::N(2.718281828459045), false);
    rt.DefineData(Mth, L"LN2", Value::N(0.6931471805599453), false);
    rt.DefineData(Mth, L"LN10", Value::N(2.302585092994046), false);
    rt.DefineData(Mth, L"LOG2E", Value::N(1.4426950408889634), false);
    rt.DefineData(Mth, L"LOG10E", Value::N(0.4342944819032518), false);
    rt.DefineData(Mth, L"SQRT2", Value::N(1.4142135623730951), false);
    rt.DefineData(Mth, L"SQRT1_2", Value::N(0.7071067811865476), false);

    // JSON
    Obj* J = rt.NewObject();
    rt.DefineData(g, L"JSON", Value::O(J), false);
    M(J, L"parse", JSON_parse, 2);
    M(J, L"stringify", JSON_stringify, 3);

    // console
    Obj* C = rt.NewObject();
    rt.DefineData(g, L"console", Value::O(C), false);
    M(C, L"log", Console_log, 0); M(C, L"info", Console_log, 0); M(C, L"debug", Console_log, 0);
    M(C, L"dir", Console_log, 0); M(C, L"table", Console_log, 0); M(C, L"trace", Console_log, 0);
    M(C, L"warn", Console_warn, 0); M(C, L"error", Console_error, 0); M(C, L"assert", Console_assert, 0);
    for (const wchar_t* n : {L"group", L"groupCollapsed", L"groupEnd", L"time", L"timeEnd", L"timeLog", L"count", L"countReset", L"profile", L"profileEnd", L"clear"}) M(C, n, Console_noop, 0);

    // Genel fonksiyonlar
    rt.DefineData(g, L"globalThis", Value::O(g), false);
    rt.DefineData(g, L"NaN", Value::N(NAN), false);
    rt.DefineData(g, L"Infinity", Value::N(INFINITY), false);
    rt.DefineData(g, L"undefined", Value(), false);
    for (auto* n : {L"NaN", L"Infinity", L"undefined"}) g->props.Find(n)->writable = false;
    M(g, L"parseInt", Global_parseInt, 2);
    M(g, L"parseFloat", Global_parseFloat, 1);
    M(g, L"isNaN", Global_isNaN, 1);
    M(g, L"isFinite", Global_isFinite, 1);
    M(g, L"setTimeout", Global_setTimeout, 2);
    M(g, L"setInterval", Global_setInterval, 2);
    M(g, L"clearTimeout", Global_clearTimeout, 1);
    M(g, L"clearInterval", Global_clearTimeout, 1);
    M(g, L"queueMicrotask", Global_queueMicrotask, 1);
    M(g, L"structuredClone", Global_structuredClone, 1);
    M(g, L"eval", Global_eval, 1);

    // Promise
    Obj* PC = MakeCtor(rt, L"Promise", Promise_ctor, 1, rt.promiseProto);
    M(PC, L"resolve", Promise_resolve, 1); M(PC, L"reject", Promise_reject, 1); M(PC, L"all", Promise_all, 1);
    M(PC, L"allSettled", Promise_allSettled, 1); M(PC, L"race", Promise_race, 1); M(PC, L"any", Promise_any, 1);
    M(PC, L"withResolvers", Promise_withResolvers, 0);
    M(rt.promiseProto, L"then", PromiseP_then, 2); M(rt.promiseProto, L"catch", PromiseP_catch, 1);
    M(rt.promiseProto, L"finally", PromiseP_finally, 1);
    rt.DefineData(rt.promiseProto, L"\x01Symbol.toStringTag", Value::S(L"Promise"), false);

    // Map / Set (WeakMap/WeakSet ayni uygulama: zayif referans yok, sayfa omru boyunca tutulur)
    for (const wchar_t* n : {L"Map", L"WeakMap"}) {
        Obj* mp = std::wstring(n) == L"Map" ? rt.mapProto : rt.NewObject(rt.mapProto);
        MakeCtor(rt, n, Map_ctor, 0, mp);
    }
    for (const wchar_t* n : {L"Set", L"WeakSet"}) {
        Obj* sp = std::wstring(n) == L"Set" ? rt.setProto : rt.NewObject(rt.setProto);
        MakeCtor(rt, n, Set_ctor, 0, sp);
    }
    Obj* MP = rt.mapProto;
    M(MP, L"get", MapP_get, 1); M(MP, L"set", MapP_set, 2); M(MP, L"has", MapP_has, 1); M(MP, L"delete", MapP_delete, 1);
    M(MP, L"clear", MapP_clear, 0); M(MP, L"forEach", MapP_forEach, 1); M(MP, L"keys", MapP_keys, 0);
    M(MP, L"values", MapP_values, 0); M(MP, L"entries", MapP_entries, 0);
    rt.DefineAccessor(MP, L"size", MapP_size, nullptr);
    rt.DefineData(MP, kIter, rt.Get(Value::O(MP), L"entries"), false);
    Obj* SPr = rt.setProto;
    M(SPr, L"add", SetP_add, 1); M(SPr, L"has", MapP_has, 1); M(SPr, L"delete", MapP_delete, 1);
    M(SPr, L"clear", MapP_clear, 0); M(SPr, L"forEach", MapP_forEach, 1); M(SPr, L"values", MapP_keys, 0);
    M(SPr, L"keys", MapP_keys, 0); M(SPr, L"entries", MapP_entries, 0);
    rt.DefineAccessor(SPr, L"size", MapP_size, nullptr);
    rt.DefineData(SPr, kIter, rt.Get(Value::O(SPr), L"values"), false);

    // Yineleyici
    M(rt.iteratorProto, L"next", IterP_next, 0);
    M(rt.iteratorProto, L"return", IterP_return, 1);
    rt.DefineData(rt.iteratorProto, kIter, Value::O(rt.NewFunction(IterP_self, L"[Symbol.iterator]", 0)), false);

    // Reflect
    Obj* R = rt.NewObject();
    rt.DefineData(g, L"Reflect", Value::O(R), false);
    M(R, L"ownKeys", Reflect_ownKeys, 1); M(R, L"has", Reflect_has, 2); M(R, L"get", Reflect_get, 2);
    M(R, L"set", Reflect_set, 3); M(R, L"apply", Reflect_apply, 3); M(R, L"construct", Reflect_construct, 2);
    M(R, L"getPrototypeOf", Object_getPrototypeOf, 1); M(R, L"setPrototypeOf", Object_setPrototypeOf, 2);
    M(R, L"defineProperty", Object_defineProperty, 3); M(R, L"deleteProperty", Reflect_deleteProperty, 2);
    M(R, L"getOwnPropertyDescriptor", Object_getOwnPropertyDescriptor, 2);

    // Promise ic yardimcilari (GC koku olarak ctors tablosunda)
    rt.ctors[L"__resolveFn"] = rt.NewFunction(PromiseResolveFnImpl, L"resolve", 1);
    rt.ctors[L"__rejectFn"] = rt.NewFunction(PromiseRejectFnImpl, L"reject", 1);

    InstallBuiltins2(rt);
}

} // namespace js
} // namespace mer
