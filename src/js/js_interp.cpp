// merOS Shield - merJS : ağaç-gezen yorumlayıcı
#include <algorithm>
#include <cmath>
#include <set>
#include "js_ast.h"

namespace mer {
namespace js {

namespace {

struct Comp {
    enum T : uint8_t { Normal, Return, Break, Continue };
    T t = Normal;
    Value v;
    const std::wstring* label = nullptr;
};

struct OptShort {}; // ?. kisa devresi

using Labels = std::vector<const std::wstring*>;

int32_t ToInt32(double d) {
    if (!std::isfinite(d) || d == 0) return 0;
    double t = std::trunc(d);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    uint32_t u = uint32_t(m);
    return int32_t(u);
}
uint32_t ToUint32(double d) { return uint32_t(ToInt32(d)); }

enum BindMode { B_Assign = 0, B_Var = 1, B_Let = 2, B_Const = 3, B_Param = 4 };


struct In {
    Runtime& rt;

    [[noreturn]] void Throw(const wchar_t* c, const std::wstring& m) { rt.Throw(c, m); }

    Env* NewEnv(Env* parent) {
        Env* e = rt.Alloc<Env>();
        e->parent = parent;
        return e;
    }

    void Step() { if (++rt.steps > rt.stepBudget) throw JSAbort{L"betik çok uzun sürdü (sonsuz döngü?)"}; }

    // ---------- Degiskenler ----------
    Value GetVar(const std::wstring& n, Env* env) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj && rt.HasProperty(e->withObj, n)) return rt.Get(Value::O(e->withObj), n);
            if (Binding* b = e->Find(n)) return b->v;
        }
        if (rt.HasProperty(rt.global, n)) return rt.Get(Value::O(rt.global), n);
        Throw(L"ReferenceError", n + L" is not defined");
    }

    bool TryGetVar(const std::wstring& n, Env* env, Value& out) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj && rt.HasProperty(e->withObj, n)) { out = rt.Get(Value::O(e->withObj), n); return true; }
            if (Binding* b = e->Find(n)) { out = b->v; return true; }
        }
        if (rt.HasProperty(rt.global, n)) { out = rt.Get(Value::O(rt.global), n); return true; }
        return false;
    }

    void SetVar(const std::wstring& n, const Value& v, Env* env) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj && rt.HasProperty(e->withObj, n)) { rt.Put(Value::O(e->withObj), n, v); return; }
            if (Binding* b = e->Find(n)) {
                if (b->kind == 2) Throw(L"TypeError", L"Assignment to constant variable.");
                b->v = v;
                return;
            }
        }
        rt.Put(Value::O(rt.global), n, v);
    }

    Env* FuncEnv(Env* env) {
        for (Env* e = env; e; e = e->parent) if (e->hasThis) return e;
        return rt.globalEnv;
    }

    Value ThisOf(Env* env) {
        Env* f = FuncEnv(env);
        if (f->thisUninit) Throw(L"ReferenceError", L"Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
        return f->thisVal;
    }

    Obj* HomeObject(Env* env) {
        Env* f = FuncEnv(env);
        return (f && f->func && f->func->x) ? f->func->x->home : nullptr;
    }

    static void NameFn(const Value& v, const std::wstring& name) {
        if (v.IsObj() && v.o->IsCallable() && v.o->x && v.o->x->name.empty() && v.o->x->fn) v.o->x->name = name;
    }

    // ---------- Fonksiyonlar ----------
    Obj* MakeClosure(Ast* fnNode, Env* env) {
        FuncInfo* fi = fnNode->fi;
        Obj* f = rt.NewObject(rt.functionProto);
        f->cls = Cls::Function;
        ObjExtra& x = f->X();
        x.fn = fnNode;
        x.scope = env;
        x.name = fi->name;
        x.ctor = !fi->arrow && fi->kind == 0 && !fi->isAsync && !fi->isGen;
        return f;
    }

    Value FunctionExpr(Ast* e, Env* env) {
        if (!e->flag2 && e->str && !e->fi->arrow) {
            Env* ne = NewEnv(env);
            Obj* f = MakeClosure(e, ne);
            ne->Define(*e->str, 2).v = Value::O(f);
            return Value::O(f);
        }
        return Value::O(MakeClosure(e, env));
    }

    Value CallFunction(Obj* f, const Value& thisV, std::vector<Value>& args, Obj* newTarget, Value* outThis) {
        Ast* fnNode = f->x->fn;
        FuncInfo* fi = fnNode->fi;
        rt.CheckStack();
        if (++rt.depth > rt.maxDepth) { rt.depth--; Throw(L"RangeError", L"Maximum call stack size exceeded"); }
        struct DG { int& d; ~DG() { d--; } } dg{rt.depth};
        Env* fenv = NewEnv(f->x->scope);
        fenv->isFunc = true;
        if (!fi->arrow) {
            fenv->hasThis = true;
            fenv->func = f;
            fenv->newTarget = newTarget;
            if (f->x->classKind == 2 && newTarget) fenv->thisUninit = true;
            else if (thisV.IsNullish() && !f->x->classKind && fi->kind == 0) fenv->thisVal = Value::O(rt.global);
            else fenv->thisVal = thisV;
            if (fi->usesArgs) {
                Obj* a = rt.NewObject(rt.objectProto);
                a->cls = Cls::Arguments;
                a->elems = args;
                fenv->Define(L"arguments", 0).v = Value::O(a);
            }
        }
        if (fi->simpleParams) {
            for (size_t i = 0; i < fi->params.size(); i++)
                fenv->Define(*fi->params[i]->str, 0).v = i < args.size() ? args[i] : Value();
        } else {
            for (size_t i = 0; i < fi->params.size(); i++) {
                Ast* p = fi->params[i];
                if (p->k == K::Rest) {
                    std::vector<Value> rest;
                    if (i < args.size()) rest.assign(args.begin() + long(i), args.end());
                    Bind(p->a, Value::O(rt.NewArrayFrom(rest)), fenv, B_Param);
                    break;
                }
                Bind(p, i < args.size() ? args[i] : Value(), fenv, B_Param);
            }
        }
        for (auto& n : fi->varNames) if (!fenv->Find(n)) fenv->Define(n, 0);

        if (fi->isGen) {
            std::vector<Value> sink;
            rt.yieldSinks.push_back(&sink);
            try {
                RunBody(fi, fenv, nullptr);
            } catch (...) {
                rt.yieldSinks.pop_back();
                throw;
            }
            rt.yieldSinks.pop_back();
            return Value::O(rt.MakeIteratorFromList(std::move(sink)));
        }
        if (fi->isAsync) {
            Obj* p = rt.NewPromise();
            try {
                Value r = RunBody(fi, fenv, nullptr);
                rt.ResolvePromise(p, r);
            } catch (JSThrow& t) {
                rt.RejectPromise(p, t.v);
            }
            return Value::O(p);
        }
        return RunBody(fi, fenv, outThis);
    }

    Value RunBody(FuncInfo* fi, Env* fenv, Value* outThis) {
        Value result;
        if (fi->exprBody) result = Eval(fi->body, fenv);
        else {
            Comp c = ExecList(fi->body->list, fenv);
            if (c.t == Comp::Return) result = c.v;
        }
        if (outThis) {
            if (fenv->thisUninit && !result.IsObj())
                Throw(L"ReferenceError", L"Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
            *outThis = fenv->thisVal;
        }
        return result;
    }

    void InitFields(Obj* cls, const Value& obj) {
        if (!cls->x || !cls->x->classAst) return;
        Ast* c = cls->x->classAst;
        for (Ast* m : c->list) {
            if (m->isStatic || m->kind != 3) continue;
            std::wstring key = m->flag ? rt.KeyOf(Eval(m->a, cls->x->fieldsEnv)) : *m->a->str;
            Value v;
            if (m->b) {
                Obj* init = MakeClosure(m->b, cls->x->fieldsEnv);
                Value proto = rt.Get(Value::O(cls), L"prototype");
                init->x->home = proto.IsObj() ? proto.o : nullptr;
                std::vector<Value> none;
                v = CallFunction(init, obj, none, nullptr, nullptr);
                NameFn(v, key);
            }
            if (obj.IsObj()) rt.DefineData(obj.o, key, v);
        }
    }

    Value Construct(Obj* fo, std::vector<Value>& args, Obj* newTarget) {
        ObjExtra& x = *fo->x;
        if (x.classKind == 2) {
            Value thisOut, r;
            if (x.fn) r = CallFunction(fo, Value(), args, newTarget, &thisOut);
            else {
                if (!fo->proto || !fo->proto->IsCallable()) Throw(L"TypeError", L"Super constructor is not a constructor");
                thisOut = rt.Construct(Value::O(fo->proto), args, newTarget);
                InitFields(fo, thisOut);
            }
            if (r.IsObj()) return r;
            return thisOut;
        }
        Value proto = rt.Get(Value::O(newTarget), L"prototype");
        Obj* obj = rt.NewObject(proto.IsObj() ? proto.o : rt.objectProto);
        if (x.classKind == 1) InitFields(fo, Value::O(obj));
        if (x.fn) {
            Value r = CallFunction(fo, Value::O(obj), args, newTarget, nullptr);
            if (r.IsObj()) return r;
        }
        return Value::O(obj);
    }

    // ---------- Siniflar ----------
    Value EvalClass(Ast* c, Env* env) {
        Obj* protoParent = rt.objectProto;
        Obj* ctorParent = rt.functionProto;
        if (c->a) {
            Value pv = Eval(c->a, env);
            if (pv.t == Value::Null) protoParent = nullptr;
            else if (pv.IsObj() && pv.o->IsCallable()) {
                ctorParent = pv.o;
                Value pp = rt.Get(pv, L"prototype");
                if (pp.IsObj()) protoParent = pp.o;
                else if (pp.t == Value::Null) protoParent = nullptr;
                else Throw(L"TypeError", L"Class extends value does not have valid prototype property");
            } else Throw(L"TypeError", L"Class extends value " + rt.Inspect(pv) + L" is not a constructor or null");
        }
        Env* cenv = NewEnv(env);
        Obj* F = nullptr;
        if (c->b) F = MakeClosure(c->b, cenv);
        else { F = rt.NewObject(rt.functionProto); F->cls = Cls::Function; F->X(); }
        F->proto = ctorParent;
        F->x->ctor = true;
        F->x->classKind = c->a ? 2 : 1;
        F->x->classAst = c;
        F->x->fieldsEnv = cenv;
        F->x->name = c->str ? *c->str : L"";
        Obj* P = rt.NewObject(rt.objectProto);
        P->proto = protoParent;
        rt.DefineData(P, L"constructor", Value::O(F), false);
        Prop& pp = F->props.Put(L"prototype");
        pp.v = Value::O(P);
        pp.enumerable = false;
        pp.writable = false;
        F->x->home = P;
        if (c->str) cenv->Define(*c->str, 2).v = Value::O(F);
        for (Ast* m : c->list) {
            Obj* target = m->isStatic ? F : P;
            if (m->kind == 3) {
                if (!m->isStatic) continue;
                std::wstring key = m->flag ? rt.KeyOf(Eval(m->a, cenv)) : *m->a->str;
                Value v;
                if (m->b) {
                    Obj* init = MakeClosure(m->b, cenv);
                    init->x->home = F;
                    std::vector<Value> none;
                    v = CallFunction(init, Value::O(F), none, nullptr, nullptr);
                    NameFn(v, key);
                }
                rt.DefineData(F, key, v);
                continue;
            }
            if (m->kind == 4) {
                Obj* blk = MakeClosure(m->b, cenv);
                blk->x->home = F;
                std::vector<Value> none;
                CallFunction(blk, Value::O(F), none, nullptr, nullptr);
                continue;
            }
            std::wstring key = m->flag ? rt.KeyOf(Eval(m->a, cenv)) : *m->a->str;
            Obj* fn = MakeClosure(m->b, cenv);
            fn->x->home = target;
            fn->x->name = key;
            if (m->kind == 0) rt.DefineData(target, key, Value::O(fn), false);
            else {
                Prop& pr = target->props.Put(key);
                if (!pr.accessor) { pr.v = Value(); pr.getter = pr.setter = nullptr; }
                pr.accessor = true;
                pr.enumerable = false;
                if (m->kind == 1) pr.getter = fn; else pr.setter = fn;
            }
        }
        return Value::O(F);
    }

    // ---------- Kaliplar ----------
    void BindName(const std::wstring& n, const Value& v, Env* env, int mode) {
        switch (mode) {
        case B_Assign: case B_Var: SetVar(n, v, env); break;
        case B_Let: env->Define(n, 1).v = v; break;
        case B_Const: env->Define(n, 2).v = v; break;
        default: env->Define(n, 0).v = v; break;
        }
    }

    void Bind(Ast* p, const Value& v, Env* env, int mode) {
        switch (p->k) {
        case K::Ident: BindName(*p->str, v, env, mode); return;
        case K::Member: case K::Index: {
            if (mode != B_Assign) Throw(L"SyntaxError", L"Invalid destructuring target");
            Value o = Eval(p->a, env);
            if (p->k == K::Member) rt.Put(o, *p->str, v);
            else rt.PutV(o, Eval(p->b, env), v);
            return;
        }
        case K::AssignPat: {
            Value val = v;
            if (val.IsUndef()) {
                val = Eval(p->b, env);
                if (p->a->k == K::Ident) NameFn(val, *p->a->str);
            }
            Bind(p->a, val, env, mode);
            return;
        }
        case K::ArrPat: {
            bool hasRest = !p->list.empty() && p->list.back()->k == K::Rest;
            std::vector<Value> items = rt.IterateToList(v, hasRest ? 10000000 : p->list.size());
            for (size_t i = 0; i < p->list.size(); i++) {
                Ast* el = p->list[i];
                if (el->k == K::Hole) continue;
                if (el->k == K::Rest) {
                    std::vector<Value> rest;
                    if (i < items.size()) rest.assign(items.begin() + long(i), items.end());
                    Bind(el->a, Value::O(rt.NewArrayFrom(rest)), env, mode);
                    break;
                }
                Bind(el, i < items.size() ? items[i] : Value(), env, mode);
            }
            return;
        }
        case K::ObjPat: {
            if (v.IsNullish()) Throw(L"TypeError", L"Cannot destructure '" + rt.ToStr(v) + L"' as it is " + rt.ToStr(v) + L".");
            std::set<std::wstring> used;
            for (Ast* pr : p->list) {
                if (pr->k == K::Rest) {
                    Obj* rest = rt.NewObject();
                    if (v.IsObj())
                        for (auto& k : rt.OwnKeys(v.o, true)) if (!used.count(k)) rt.DefineData(rest, k, rt.Get(v, k));
                    Bind(pr->a, Value::O(rest), env, mode);
                    break;
                }
                std::wstring key = pr->flag ? rt.KeyOf(Eval(pr->a, env)) : *pr->a->str;
                used.insert(key);
                Bind(pr->b, rt.Get(v, key), env, mode);
            }
            return;
        }
        default:
            Throw(L"SyntaxError", L"Invalid destructuring target");
        }
    }

    // ---------- Referanslar ----------
    struct Ref { int kind; Value base; std::wstring key; Value keyV; const std::wstring* name; };

    Ref MakeRef(Ast* t, Env* env) {
        Ref r;
        if (t->k == K::Ident) { r.kind = 0; r.name = t->str.get(); return r; }
        if (t->k == K::Member) {
            r.kind = 1;
            r.base = t->a->k == K::Super ? ThisOf(env) : Eval(t->a, env);
            r.key = *t->str;
            return r;
        }
        if (t->k == K::Index) {
            r.kind = 2;
            r.base = Eval(t->a, env);
            r.keyV = Eval(t->b, env);
            return r;
        }
        Throw(L"SyntaxError", L"Invalid assignment target");
    }
    Value RefGet(Ref& r, Env* env) {
        if (r.kind == 0) return GetVar(*r.name, env);
        if (r.kind == 1) return rt.Get(r.base, r.key);
        return rt.GetV(r.base, r.keyV);
    }
    void RefPut(Ref& r, const Value& v, Env* env) {
        if (r.kind == 0) SetVar(*r.name, v, env);
        else if (r.kind == 1) rt.Put(r.base, r.key, v);
        else rt.PutV(r.base, r.keyV, v);
    }

    // ---------- Operatorler ----------
    bool LessThan(const Value& a0, const Value& b0, bool leftFirst, bool& undefinedResult) {
        Value a, b;
        if (leftFirst) { a = rt.ToPrimitive(a0, 1); b = rt.ToPrimitive(b0, 1); }
        else { b = rt.ToPrimitive(b0, 1); a = rt.ToPrimitive(a0, 1); }
        undefinedResult = false;
        if (a.IsStr() && b.IsStr()) return *a.s < *b.s;
        double x = rt.ToNum(a), y = rt.ToNum(b);
        if (std::isnan(x) || std::isnan(y)) { undefinedResult = true; return false; }
        return x < y;
    }

    Value BinOp(uint8_t op, const Value& a, const Value& b) {
        switch (op) {
        case O_Add: {
            if (a.t == Value::Num && b.t == Value::Num) return Value::N(a.n + b.n);
            if (a.IsStr() && b.IsStr()) {
                if (a.s->size() + b.s->size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
                std::wstring r;
                r.reserve(a.s->size() + b.s->size());
                r += *a.s;
                r += *b.s;
                return Value::S(std::move(r));
            }
            Value pa = rt.ToPrimitive(a, 0), pb = rt.ToPrimitive(b, 0);
            if (pa.IsStr() || pb.IsStr()) {
                Str sa = rt.ToStrS(pa), sb = rt.ToStrS(pb);
                if (sa->size() + sb->size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
                return Value::S(*sa + *sb);
            }
            return Value::N(rt.ToNum(pa) + rt.ToNum(pb));
        }
        case O_Sub: return Value::N(rt.ToNum(a) - rt.ToNum(b));
        case O_Mul: return Value::N(rt.ToNum(a) * rt.ToNum(b));
        case O_Div: return Value::N(rt.ToNum(a) / rt.ToNum(b));
        case O_Mod: {
            double x = rt.ToNum(a), y = rt.ToNum(b);
            if (y == 0 || std::isnan(x) || std::isnan(y) || std::isinf(x)) return Value::N(NAN);
            if (std::isinf(y)) return Value::N(x);
            return Value::N(std::fmod(x, y));
        }
        case O_Exp: {
            double x = rt.ToNum(a), y = rt.ToNum(b);
            if (std::isnan(y)) return Value::N(NAN);
            if ((x == 1 || x == -1) && std::isinf(y)) return Value::N(NAN);
            return Value::N(std::pow(x, y));
        }
        case O_Shl: return Value::N(double(int32_t(uint32_t(ToInt32(rt.ToNum(a))) << (ToUint32(rt.ToNum(b)) & 31))));
        case O_Shr: return Value::N(double(ToInt32(rt.ToNum(a)) >> (ToUint32(rt.ToNum(b)) & 31)));
        case O_UShr: return Value::N(double(ToUint32(rt.ToNum(a)) >> (ToUint32(rt.ToNum(b)) & 31)));
        case O_BitAnd: return Value::N(double(ToInt32(rt.ToNum(a)) & ToInt32(rt.ToNum(b))));
        case O_BitOr: return Value::N(double(ToInt32(rt.ToNum(a)) | ToInt32(rt.ToNum(b))));
        case O_BitXor: return Value::N(double(ToInt32(rt.ToNum(a)) ^ ToInt32(rt.ToNum(b))));
        case O_Eq: return Value::B(rt.LooseEq(a, b));
        case O_Ne: return Value::B(!rt.LooseEq(a, b));
        case O_SEq: return Value::B(rt.StrictEq(a, b));
        case O_SNe: return Value::B(!rt.StrictEq(a, b));
        case O_Lt: { bool u; bool r = LessThan(a, b, true, u); return Value::B(!u && r); }
        case O_Gt: { bool u; bool r = LessThan(b, a, false, u); return Value::B(!u && r); }
        case O_Le: { bool u; bool r = LessThan(b, a, false, u); return Value::B(!u && !r); }
        case O_Ge: { bool u; bool r = LessThan(a, b, true, u); return Value::B(!u && !r); }
        case O_In:
            if (!b.IsObj()) Throw(L"TypeError", L"Cannot use 'in' operator to search for '" + rt.KeyOf(a) + L"' in " + rt.ToStr(b));
            return Value::B(rt.HasProperty(b.o, rt.KeyOf(a)));
        case O_InstanceOf: return Value::B(rt.InstanceOf(a, b));
        }
        return Value();
    }

    std::wstring Describe(Ast* c) {
        switch (c->k) {
        case K::Ident: return *c->str;
        case K::Member: return Describe(c->a) + L"." + *c->str;
        case K::This: return L"this";
        case K::Call: return Describe(c->a) + L"(...)";
        case K::Index: return Describe(c->a) + L"[...]";
        default: return L"ifade";
        }
    }

    void EvalArgs(const std::vector<Ast*>& list, Env* env, std::vector<Value>& out) {
        out.reserve(list.size());
        for (Ast* a : list) {
            if (a->k == K::Spread) {
                std::vector<Value> items = rt.IterateToList(Eval(a->a, env));
                if (out.size() + items.size() > 1000000) Throw(L"RangeError", L"Too many arguments");
                out.insert(out.end(), items.begin(), items.end());
            } else out.push_back(Eval(a, env));
        }
    }

    Value EvalMember(Ast* e, Env* env) {
        if (e->a->k == K::Super) {
            Obj* home = HomeObject(env);
            Obj* start = home ? home->proto : nullptr;
            if (!start) return Value();
            std::wstring key = e->k == K::Member ? *e->str : rt.KeyOf(Eval(e->b, env));
            return rt.GetFrom(start, key, ThisOf(env));
        }
        Value o = Eval(e->a, env);
        if (e->flag && o.IsNullish()) throw OptShort{};
        if (e->k == K::Member) return rt.Get(o, *e->str);
        Value k = Eval(e->b, env);
        return rt.GetV(o, k);
    }

    Value EvalCall(Ast* e, Env* env) {
        Ast* c = e->a;
        if (c->k == K::Super) {
            Env* fenv = FuncEnv(env);
            Obj* f = fenv->func;
            if (!f || !f->x || f->x->classKind != 2) Throw(L"SyntaxError", L"'super' keyword unexpected here");
            Obj* parent = f->proto;
            std::vector<Value> args;
            EvalArgs(e->list, env, args);
            if (!parent || !parent->IsCallable()) Throw(L"TypeError", L"Super constructor is not a constructor");
            Value r = rt.Construct(Value::O(parent), args, fenv->newTarget ? fenv->newTarget : f);
            if (!fenv->thisUninit) Throw(L"ReferenceError", L"Super constructor may only be called once");
            fenv->thisVal = r;
            fenv->thisUninit = false;
            InitFields(f, r);
            return Value();
        }
        Value fn, thisV;
        if (c->k == K::Member || c->k == K::Index) {
            if (c->a->k == K::Super) {
                thisV = ThisOf(env);
                fn = EvalMember(c, env);
            } else {
                Value o = Eval(c->a, env);
                if (c->flag && o.IsNullish()) throw OptShort{};
                fn = c->k == K::Member ? rt.Get(o, *c->str) : rt.GetV(o, Eval(c->b, env));
                thisV = o;
            }
        } else {
            fn = Eval(c, env);
        }
        if (e->flag && fn.IsNullish()) throw OptShort{};
        std::vector<Value> args;
        EvalArgs(e->list, env, args);
        if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", Describe(c) + L" is not a function");
        return rt.Call(fn, thisV, args);
    }

    Value Await(const Value& v) {
        Value pv = v;
        if (pv.IsObj() && pv.o->cls != Cls::Promise) {
            Value then = rt.Get(pv, L"then");
            if (then.IsObj() && then.o->IsCallable()) {
                Obj* p = rt.NewPromise();
                rt.ResolvePromise(p, pv);
                pv = Value::O(p);
            } else return pv;
        }
        if (!pv.IsObj() || pv.o->cls != Cls::Promise) return pv;
        Obj* p = pv.o;
        p->x->handled = true;
        for (int guard = 0; p->x->pstate == 0 && !rt.microtasks.empty() && guard < 10000; guard++) rt.RunMicrotasks();
        if (p->x->pstate == 0) Throw(L"Error", L"merJS: await edilen Promise henüz çözülmedi (zamanlayıcı/ağ bekleyen await desteklenmiyor)");
        if (p->x->pstate == 2) throw JSThrow{p->x->pvalue};
        return p->x->pvalue;
    }

    // ---------- Ifade degerlendirme ----------
    Value Eval(Ast* e, Env* env) {
        Step();
        switch (e->k) {
        case K::Num: return Value::N(e->num);
        case K::Str: return Value::S(e->str);
        case K::True: return Value::B(true);
        case K::False: return Value::B(false);
        case K::Null: return Value::Null_();
        case K::Tpl: {
            std::wstring s = *e->cooked[0];
            for (size_t i = 0; i < e->list.size(); i++) {
                Value v = Eval(e->list[i], env);
                if (v.t == Value::Symbol) Throw(L"TypeError", L"Cannot convert a Symbol value to a string");
                s += rt.ToStr(v);
                if (i + 1 < e->cooked.size()) s += *e->cooked[i + 1];
            }
            return Value::S(std::move(s));
        }
        case K::TaggedTpl: {
            Ast* t = e->b;
            std::vector<Value> strs, raws;
            for (auto& s : t->cooked) strs.push_back(Value::S(s));
            for (auto& s : t->raw) raws.push_back(Value::S(s));
            Obj* sa = rt.NewArrayFrom(strs);
            rt.DefineData(sa, L"raw", Value::O(rt.NewArrayFrom(raws)), false);
            std::vector<Value> args{Value::O(sa)};
            for (Ast* x : t->list) args.push_back(Eval(x, env));
            Value fn, thisV;
            if (e->a->k == K::Member) { thisV = Eval(e->a->a, env); fn = rt.Get(thisV, *e->a->str); }
            else fn = Eval(e->a, env);
            if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", Describe(e->a) + L" is not a function");
            return rt.Call(fn, thisV, args);
        }
        case K::Ident: return GetVar(*e->str, env);
        case K::This: return ThisOf(env);
        case K::Super: Throw(L"SyntaxError", L"'super' keyword unexpected here");
        case K::Regex: return Value::O(rt.NewRegExp(*e->str, e->str2));
        case K::Arr: {
            Obj* a = rt.NewArray(0);
            for (Ast* el : e->list) {
                if (el->k == K::Hole) a->elems.push_back(Value());
                else if (el->k == K::Spread) {
                    std::vector<Value> items = rt.IterateToList(Eval(el->a, env));
                    a->elems.insert(a->elems.end(), items.begin(), items.end());
                } else a->elems.push_back(Eval(el, env));
                if (a->elems.size() > 50000000) Throw(L"RangeError", L"Invalid array length");
            }
            return Value::O(a);
        }
        case K::ObjLit: return ObjectLiteral(e, env);
        case K::Func: return FunctionExpr(e, env);
        case K::Class: return EvalClass(e, env);
        case K::Unary: return Unary(e, env);
        case K::Update: {
            Ref r = MakeRef(e->a, env);
            double old = rt.ToNum(RefGet(r, env));
            double nv = e->op == O_Inc ? old + 1 : old - 1;
            RefPut(r, Value::N(nv), env);
            return Value::N(e->flag ? nv : old);
        }
        case K::Bin: {
            Value a = Eval(e->a, env);
            Value b = Eval(e->b, env);
            return BinOp(e->op, a, b);
        }
        case K::Logic: {
            Value a = Eval(e->a, env);
            if (e->op == O_And) return rt.Truthy(a) ? Eval(e->b, env) : a;
            if (e->op == O_Or) return rt.Truthy(a) ? a : Eval(e->b, env);
            return a.IsNullish() ? Eval(e->b, env) : a;
        }
        case K::Assign: return Assign(e, env);
        case K::ImportCall: {
            Value specVal = Eval(e->a, env);
            std::wstring spec = rt.ToStr(specVal);
            std::wstring base = rt.currentModule ? rt.currentModule->url : (rt.host ? rt.host->PageUrl() : L"");
            Obj* p = rt.NewPromise();
            try {
                auto mod = rt.LoadModuleInternal(spec, base);
                rt.ResolvePromise(p, Value::O(mod->namespaceObj));
            } catch (JSThrow& t) {
                rt.RejectPromise(p, t.v);
            } catch (...) {
                rt.RejectPromise(p, Value::O(rt.NewError(L"TypeError", L"Failed to load module: " + spec)));
            }
            return Value::O(p);
        }
        case K::ImportMeta: {
            Obj* meta = rt.NewObject();
            std::wstring u = rt.currentModule ? rt.currentModule->url : (rt.host ? rt.host->PageUrl() : L"about:blank");
            rt.DefineData(meta, L"url", Value::S(u));
            return Value::O(meta);
        }
        case K::Cond: return rt.Truthy(Eval(e->a, env)) ? Eval(e->b, env) : Eval(e->c, env);
        case K::Call:
            if (e->flag2) { try { return EvalCall(e, env); } catch (OptShort&) { return Value(); } }
            return EvalCall(e, env);
        case K::Member:
        case K::Index:
            if (e->flag2) { try { return EvalMember(e, env); } catch (OptShort&) { return Value(); } }
            return EvalMember(e, env);
        case K::New: {
            Value fn = Eval(e->a, env);
            std::vector<Value> args;
            EvalArgs(e->list, env, args);
            if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", Describe(e->a) + L" is not a constructor");
            return rt.Construct(fn, args);
        }
        case K::Seq: {
            Value v;
            for (Ast* x : e->list) v = Eval(x, env);
            return v;
        }
        case K::NewTarget: {
            Env* f = FuncEnv(env);
            return f->newTarget ? Value::O(f->newTarget) : Value();
        }
        case K::Await: return Await(Eval(e->a, env));
        case K::Yield: {
            if (rt.yieldSinks.empty()) Throw(L"SyntaxError", L"yield outside generator");
            std::vector<Value>* sink = rt.yieldSinks.back();
            Value v = e->a ? Eval(e->a, env) : Value();
            if (e->flag) {
                for (auto& x : rt.IterateToList(v, 200000)) sink->push_back(x);
            } else sink->push_back(v);
            if (sink->size() > 200000) throw JSAbort{L"üreteç çok fazla değer üretti"};
            return Value();
        }
        default:
            Throw(L"SyntaxError", L"Unexpected expression");
        }
    }

    Value ObjectLiteral(Ast* e, Env* env) {
        Obj* o = rt.NewObject();
        for (Ast* p : e->list) {
            if (p->k == K::Spread) {
                Value v = Eval(p->a, env);
                if (v.IsObj()) for (auto& k : rt.OwnKeys(v.o, true)) rt.DefineData(o, k, rt.Get(v, k));
                else if (v.IsStr()) for (size_t i = 0; i < v.s->size(); i++) rt.DefineData(o, std::to_wstring(i), Value::S(std::wstring(1, (*v.s)[i])));
                continue;
            }
            std::wstring key = p->flag ? rt.KeyOf(Eval(p->a, env)) : *p->a->str;
            if (p->kind == 1 || p->kind == 2) {
                Obj* fn = MakeClosure(p->b, env);
                fn->x->home = o;
                fn->x->name = key;
                Prop& pr = o->props.Put(key);
                if (!pr.accessor) { pr.v = Value(); pr.getter = pr.setter = nullptr; }
                pr.accessor = true;
                pr.enumerable = true;
                if (p->kind == 1) pr.getter = fn; else pr.setter = fn;
                continue;
            }
            if (p->flag2) {
                Obj* fn = MakeClosure(p->b, env);
                fn->x->home = o;
                fn->x->name = key;
                rt.DefineData(o, key, Value::O(fn));
                continue;
            }
            Value v = Eval(p->b, env);
            if (!p->flag && !p->isStatic && key == L"__proto__") {
                if (v.IsObj()) o->proto = v.o;
                else if (v.t == Value::Null) o->proto = nullptr;
                continue;
            }
            NameFn(v, key);
            rt.DefineData(o, key, v);
        }
        return Value::O(o);
    }

    Value Unary(Ast* e, Env* env) {
        switch (e->op) {
        case O_TypeOf:
            if (e->a->k == K::Ident) {
                Value v;
                if (!TryGetVar(*e->a->str, env, v)) return Value::S(L"undefined");
                return Value::S(rt.TypeOf(v));
            }
            return Value::S(rt.TypeOf(Eval(e->a, env)));
        case O_Delete: {
            Ast* t = e->a;
            if (t->k == K::Member || t->k == K::Index) {
                Value o = Eval(t->a, env);
                std::wstring key = t->k == K::Member ? *t->str : rt.KeyOf(Eval(t->b, env));
                if (o.IsNullish()) Throw(L"TypeError", L"Cannot convert undefined or null to object");
                if (!o.IsObj()) return Value::B(true);
                return Value::B(rt.Delete(o.o, key));
            }
            if (t->k == K::Ident) return Value::B(false);
            Eval(t, env);
            return Value::B(true);
        }
        case O_Void: Eval(e->a, env); return Value();
        case O_Not: return Value::B(!rt.Truthy(Eval(e->a, env)));
        case O_Neg: return Value::N(-rt.ToNum(Eval(e->a, env)));
        case O_Pos: return Value::N(rt.ToNum(Eval(e->a, env)));
        case O_BitNot: return Value::N(double(~ToInt32(rt.ToNum(Eval(e->a, env)))));
        }
        return Value();
    }

    Value Assign(Ast* e, Env* env) {
        Ast* t = e->a;
        if (e->op == O_None) {
            if (t->k == K::ObjPat || t->k == K::ArrPat) {
                Value v = Eval(e->b, env);
                Bind(t, v, env, B_Assign);
                return v;
            }
            if (t->k == K::Ident) {
                Value v = Eval(e->b, env);
                NameFn(v, *t->str);
                SetVar(*t->str, v, env);
                return v;
            }
            if (t->k == K::Member) {
                Value o = t->a->k == K::Super ? ThisOf(env) : Eval(t->a, env);
                Value v = Eval(e->b, env);
                rt.Put(o, *t->str, v);
                return v;
            }
            Value o = Eval(t->a, env);
            Value k = Eval(t->b, env);
            Value v = Eval(e->b, env);
            rt.PutV(o, k, v);
            return v;
        }
        Ref r = MakeRef(t, env);
        Value cur = RefGet(r, env);
        if (e->op == O_And || e->op == O_Or || e->op == O_Nullish) {
            bool assign = e->op == O_And ? rt.Truthy(cur) : e->op == O_Or ? !rt.Truthy(cur) : cur.IsNullish();
            if (!assign) return cur;
            Value v = Eval(e->b, env);
            RefPut(r, v, env);
            return v;
        }
        Value v = BinOp(e->op, cur, Eval(e->b, env));
        RefPut(r, v, env);
        return v;
    }

    // ---------- Deyimler ----------
    void Hoist(const std::vector<Ast*>& list, Env* env) {
        for (Ast* s : list) {
            Ast* target = s;
            if (s->k == K::ExportDecl && s->a && s->a->k == K::Func) target = s->a;
            if (target->k != K::Func || !target->flag2) continue;
            Obj* f = MakeClosure(target, env);
            const std::wstring& n = *target->str;
            if (env->isGlobal) rt.DefineData(rt.global, n, Value::O(f));
            else if (env->isFunc) env->Define(n, 0).v = Value::O(f);
            else {
                env->Define(n, 1).v = Value::O(f);
                for (Env* e = env->parent; e; e = e->parent) {
                    if (e->isGlobal) { rt.Put(Value::O(rt.global), n, Value::O(f)); break; }
                    if (e->isFunc) { if (Binding* b = e->Find(n)) b->v = Value::O(f); break; }
                }
            }
        }
    }

    Comp ExecList(const std::vector<Ast*>& list, Env* env) {
        Hoist(list, env);
        Comp last;
        for (Ast* s : list) {
            Comp c = Exec(s, env, nullptr);
            if (c.t != Comp::Normal) return c;
            if (!c.v.IsUndef()) last.v = c.v;
        }
        return last;
    }

    static bool HasLabel(const Labels* l, const std::wstring* lab) {
        if (!l || !lab) return false;
        for (auto* x : *l) if (*x == *lab) return true;
        return false;
    }

    // 0: donguye devam, 1: donguden cik (normal), 2: yukari ilet
    static int LoopCtl(const Comp& c, const Labels* labels) {
        if (c.t == Comp::Break) return (!c.label || HasLabel(labels, c.label)) ? 1 : 2;
        if (c.t == Comp::Continue) return (!c.label || HasLabel(labels, c.label)) ? 0 : 2;
        if (c.t == Comp::Return) return 2;
        return 0;
    }

    Env* CopyEnv(Env* e) {
        Env* n = NewEnv(e->parent);
        n->vars = e->vars;
        n->idx = e->idx;
        return n;
    }

    void BindLoopVar(Ast* target, const Value& v, Env* env, Env*& iterEnv) {
        if (target->k == K::VarDecl) {
            Ast* pat = target->list[0]->a;
            if (target->kind == 0) { iterEnv = env; Bind(pat, v, env, B_Var); }
            else { iterEnv = NewEnv(env); Bind(pat, v, iterEnv, target->kind == 1 ? B_Let : B_Const); }
        } else {
            iterEnv = env;
            Bind(target, v, env, B_Assign);
        }
    }

    Comp Exec(Ast* s, Env* env, const Labels* labels) {
        Step();
        switch (s->k) {
        case K::ExprStmt: { Comp c; c.v = Eval(s->a, env); return c; }
        case K::VarDecl: {
            for (Ast* d : s->list) {
                if (s->kind == 0) {
                    if (!d->b) continue;
                    Value v = Eval(d->b, env);
                    if (d->a->k == K::Ident) NameFn(v, *d->a->str);
                    Bind(d->a, v, env, B_Var);
                } else {
                    Value v = d->b ? Eval(d->b, env) : Value();
                    if (d->a->k == K::Ident) NameFn(v, *d->a->str);
                    Bind(d->a, v, env, s->kind == 1 ? B_Let : B_Const);
                }
            }
            return Comp();
        }
        case K::Func: return Comp();
        case K::Class: {
            Value c = EvalClass(s, env);
            env->Define(*s->str, 1).v = c;
            return Comp();
        }
        case K::ImportDecl: return Comp();
        case K::ExportDefault: {
            Value v;
            if (s->a->k == K::Func) {
                Obj* f = MakeClosure(s->a, env);
                v = Value::O(f);
                if (s->a->str && !s->a->str->empty()) env->Define(*s->a->str, 1).v = v;
            } else if (s->a->k == K::Class) {
                v = EvalClass(s->a, env);
                if (s->a->str && !s->a->str->empty()) env->Define(*s->a->str, 1).v = v;
            } else {
                v = Eval(s->a, env);
            }
            if (rt.currentModule) {
                rt.currentModule->exports[L"default"] = v;
                rt.DefineData(rt.currentModule->namespaceObj, L"default", v, true);
            }
            return Comp();
        }
        case K::ExportDecl: {
            if (s->a) {
                Exec(s->a, env, nullptr);
                if (rt.currentModule) {
                    if (s->a->k == K::VarDecl) {
                        for (Ast* dc : s->a->list) {
                            if (dc->a->k == K::Ident) {
                                std::wstring name = *dc->a->str;
                                Value val;
                                if (TryGetVar(name, env, val)) {
                                    rt.currentModule->exports[name] = val;
                                    rt.DefineData(rt.currentModule->namespaceObj, name, val, true);
                                }
                            }
                        }
                    } else if (s->a->k == K::Func) {
                        if (s->a->str) {
                            std::wstring name = *s->a->str;
                            Value val;
                            if (!name.empty() && TryGetVar(name, env, val)) {
                                rt.currentModule->exports[name] = val;
                                rt.DefineData(rt.currentModule->namespaceObj, name, val, true);
                            }
                        }
                    } else if (s->a->k == K::Class) {
                        if (s->a->str) {
                            std::wstring name = *s->a->str;
                            Value val;
                            if (!name.empty() && TryGetVar(name, env, val)) {
                                rt.currentModule->exports[name] = val;
                                rt.DefineData(rt.currentModule->namespaceObj, name, val, true);
                            }
                        }
                    }
                }
            }
            if (!s->list.empty()) {
                std::shared_ptr<ModuleRecord> depMod;
                if (s->str && !s->str->empty()) {
                    depMod = rt.LoadModuleInternal(*s->str, rt.currentModule ? rt.currentModule->url : L"");
                }
                for (Ast* sp : s->list) {
                    std::wstring origName = *sp->str;
                    std::wstring expName = sp->str2;
                    Value val;
                    if (depMod) {
                        val = rt.Get(Value::O(depMod->namespaceObj), origName);
                    } else {
                        TryGetVar(origName, env, val);
                    }
                    if (rt.currentModule) {
                        rt.currentModule->exports[expName] = val;
                        rt.DefineData(rt.currentModule->namespaceObj, expName, val, true);
                    }
                }
            }
            return Comp();
        }
        case K::ExportAll: {
            if (rt.currentModule && s->str && !s->str->empty()) {
                auto depMod = rt.LoadModuleInternal(*s->str, rt.currentModule->url);
                if (depMod) {
                    if (!s->str2.empty()) {
                        rt.currentModule->exports[s->str2] = Value::O(depMod->namespaceObj);
                        rt.DefineData(rt.currentModule->namespaceObj, s->str2, Value::O(depMod->namespaceObj), true);
                    } else {
                        for (auto& k : rt.OwnKeys(depMod->namespaceObj, true)) {
                            if (k != L"default") {
                                Value v = rt.Get(Value::O(depMod->namespaceObj), k);
                                rt.currentModule->exports[k] = v;
                                rt.DefineData(rt.currentModule->namespaceObj, k, v, true);
                            }
                        }
                    }
                }
            }
            return Comp();
        }
        case K::Empty: return Comp();
        case K::Block: return ExecList(s->list, s->flag2 ? NewEnv(env) : env);
        case K::Program: {
            for (auto& n : s->fi->varNames)
                if (!rt.HasOwn(rt.global, n)) rt.DefineData(rt.global, n, Value());
            return ExecList(s->list, env);
        }
        case K::If:
            if (rt.Truthy(Eval(s->a, env))) return Exec(s->b, env, nullptr);
            if (s->c) return Exec(s->c, env, nullptr);
            return Comp();
        case K::Return: { Comp c; c.t = Comp::Return; if (s->a) c.v = Eval(s->a, env); return c; }
        case K::Break: { Comp c; c.t = Comp::Break; if (!s->str2.empty()) c.label = &s->str2; return c; }
        case K::Continue: { Comp c; c.t = Comp::Continue; if (!s->str2.empty()) c.label = &s->str2; return c; }
        case K::Throw: throw JSThrow{Eval(s->a, env)};
        case K::Labeled: {
            Labels ls;
            if (labels) ls = *labels;
            ls.push_back(&s->str2);
            Comp c = Exec(s->a, env, &ls);
            if (c.t == Comp::Break && c.label && *c.label == s->str2) return Comp();
            return c;
        }
        case K::While: {
            while (rt.Truthy(Eval(s->a, env))) {
                Comp c = Exec(s->b, env, nullptr);
                int r = LoopCtl(c, labels);
                if (r == 1) break;
                if (r == 2) return c;
            }
            return Comp();
        }
        case K::DoWhile: {
            do {
                Comp c = Exec(s->b, env, nullptr);
                int r = LoopCtl(c, labels);
                if (r == 1) break;
                if (r == 2) return c;
            } while (rt.Truthy(Eval(s->a, env)));
            return Comp();
        }
        case K::For: {
            Env* loopEnv = env;
            bool perIter = s->a && s->a->k == K::VarDecl && s->a->kind != 0;
            if (perIter) loopEnv = NewEnv(env);
            if (s->a) Exec(s->a, loopEnv, nullptr);
            Env* it = perIter ? CopyEnv(loopEnv) : loopEnv;
            while (true) {
                if (s->b && !rt.Truthy(Eval(s->b, it))) break;
                Comp c = Exec(s->d, it, nullptr);
                int r = LoopCtl(c, labels);
                if (r == 1) break;
                if (r == 2) return c;
                if (perIter) it = CopyEnv(it);
                if (s->c) Eval(s->c, it);
            }
            return Comp();
        }
        case K::ForIn: {
            Value o = Eval(s->b, env);
            if (o.IsNullish()) return Comp();
            Obj* obj = rt.ToObject(o);
            std::vector<std::wstring> keys;
            std::set<std::wstring> seen;
            int guard = 0;
            for (Obj* p = obj; p && guard < 1000; p = p->proto, guard++)
                for (auto& k : rt.OwnKeys(p, true)) if (seen.insert(k).second) keys.push_back(k);
            for (auto& k : keys) {
                if (!rt.HasProperty(obj, k)) continue;
                Env* it = nullptr;
                BindLoopVar(s->a, Value::S(k), env, it);
                Comp c = Exec(s->c, it, nullptr);
                int r = LoopCtl(c, labels);
                if (r == 1) break;
                if (r == 2) return c;
            }
            return Comp();
        }
        case K::ForOf: return ForOf(s, env, labels);
        case K::With: {
            Value o = Eval(s->a, env);
            Env* we = NewEnv(env);
            we->withObj = rt.ToObject(o);
            return Exec(s->b, we, labels);
        }
        case K::Try: return Try(s, env);
        case K::Switch: return Switch(s, env, labels);
        default:
            Throw(L"SyntaxError", L"Unexpected statement");
        }
    }

    Comp ForOf(Ast* s, Env* env, const Labels* labels) {
        Value it = Eval(s->b, env);
        auto body = [&](const Value& v, Comp& out) -> int {
            Env* ie = nullptr;
            BindLoopVar(s->a, v, env, ie);
            Comp c = Exec(s->c, ie, nullptr);
            int r = LoopCtl(c, labels);
            if (r == 2) out = c;
            return r;
        };
        Comp out;
        if (it.IsObj() && it.o->cls == Cls::Array) {
            Obj* a = it.o;
            for (size_t i = 0; i < a->elems.size(); i++) {
                int r = body(a->elems[i], out);
                if (r == 1) break;
                if (r == 2) return out;
            }
            return Comp();
        }
        bool simple = it.IsStr() || (it.IsObj() && (it.o->cls == Cls::Map || it.o->cls == Cls::Set || it.o->cls == Cls::Arguments || it.o->cls == Cls::TypedArray));
        if (simple) {
            for (auto& v : rt.IterateToList(it)) {
                int r = body(v, out);
                if (r == 1) break;
                if (r == 2) return out;
            }
            return Comp();
        }
        if (it.IsNullish()) Throw(L"TypeError", rt.ToStr(it) + L" is not iterable");
        Value fn = rt.Get(it, L"\x01Symbol.iterator");
        if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", Describe(s->b) + L" is not iterable");
        std::vector<Value> none;
        Value iter = rt.Call(fn, it, none);
        Value next = rt.Get(iter, L"next");
        while (true) {
            std::vector<Value> n2;
            Value r = rt.Call(next, iter, n2);
            if (!r.IsObj()) Throw(L"TypeError", L"Iterator result is not an object");
            if (rt.Truthy(rt.Get(r, L"done"))) break;
            int ctl = body(rt.Get(r, L"value"), out);
            if (ctl != 0) {
                Value ret = rt.Get(iter, L"return");
                if (ret.IsObj() && ret.o->IsCallable()) { std::vector<Value> n3; rt.Call(ret, iter, n3); }
                if (ctl == 2) return out;
                break;
            }
        }
        return Comp();
    }

    Comp Try(Ast* s, Env* env) {
        Comp result;
        bool pending = false;
        JSThrow pend;
        try {
            result = Exec(s->a, env, nullptr);
        } catch (JSThrow& t) {
            if (s->c) {
                try {
                    Env* cenv = NewEnv(env);
                    if (s->b) Bind(s->b, t.v, cenv, B_Let);
                    result = ExecList(s->c->list, cenv);
                } catch (JSThrow& t2) {
                    pending = true;
                    pend = t2;
                }
            } else {
                pending = true;
                pend = t;
            }
        }
        if (s->d) {
            Comp f = Exec(s->d, env, nullptr);
            if (f.t != Comp::Normal) return f;
        }
        if (pending) throw pend;
        return result;
    }

    Comp Switch(Ast* s, Env* env, const Labels* labels) {
        (void)labels;
        Value d = Eval(s->a, env);
        Env* senv = NewEnv(env);
        for (Ast* c : s->list) Hoist(c->list, senv);
        int start = -1, def = -1;
        for (size_t i = 0; i < s->list.size(); i++) {
            Ast* c = s->list[i];
            if (!c->a) { def = int(i); continue; }
            if (rt.StrictEq(d, Eval(c->a, senv))) { start = int(i); break; }
        }
        if (start < 0) start = def;
        if (start < 0) return Comp();
        for (size_t i = size_t(start); i < s->list.size(); i++) {
            for (Ast* st : s->list[i]->list) {
                Comp c = Exec(st, senv, nullptr);
                if (c.t == Comp::Break && !c.label) return Comp();
                if (c.t != Comp::Normal) return c;
            }
        }
        return Comp();
    }
};

} // namespace

// ---------------- Runtime koprusu ----------------

Value Runtime::EvalAst(Ast* e, Env* env) {
    In in{*this};
    if (e->k == K::Program) return in.Exec(e, env, nullptr).v;
    return in.Eval(e, env);
}

Value Runtime::CallUser(Obj* f, const Value& thisV, std::vector<Value>& args, Obj* newTarget) {
    In in{*this};
    return in.CallFunction(f, thisV, args, newTarget, nullptr);
}

Obj* Runtime::MakeClosure(Ast* fnNode, Env* env) {
    In in{*this};
    return in.MakeClosure(fnNode, env);
}

Value Runtime::Construct(const Value& f, std::vector<Value>& args, Obj* newTarget) {
    if (!f.IsObj() || !f.o->IsCallable()) Throw(L"TypeError", ToStr(f.IsObj() ? Value::S(L"object") : f) + L" is not a constructor");
    Obj* fo = f.o;
    if (!newTarget) newTarget = fo;
    if (fo->x && fo->x->boundTarget) {
        std::vector<Value> a = fo->x->boundArgs;
        a.insert(a.end(), args.begin(), args.end());
        return Construct(Value::O(fo->x->boundTarget), a, newTarget == fo ? fo->x->boundTarget : newTarget);
    }
    if (++steps > stepBudget) throw JSAbort{L"betik çok uzun sürdü"};
    if (fo->x && fo->x->native) {
        if (!fo->x->ctor) Throw(L"TypeError", fo->x->name + L" is not a constructor");
        CheckStack();
        Obj* saved = nativeNewTarget;
        nativeNewTarget = newTarget;
        struct R { Obj*& r; Obj* v; ~R() { r = v; } } restore{nativeNewTarget, saved};
        return fo->x->native(*this, fo, Value(), args);
    }
    if (!fo->x || (!fo->x->ctor && !fo->x->classKind)) Throw(L"TypeError", (fo->x ? fo->x->name : std::wstring(L"ifade")) + L" is not a constructor");
    In in{*this};
    return in.Construct(fo, args, newTarget);
}

} // namespace js
} // namespace mer
