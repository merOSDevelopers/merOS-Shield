// merOS Shield - merJS : ağaç-gezen yorumlayıcı
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include "js_ast.h"
#include "js_bigint.h"

namespace mer {
namespace js {
// JS cagri cercevesi: error.stack icin kim, nereden cagrildi (istisnada da geri alinir)
struct CallFrameGuard {
    Runtime& rt;
    CallFrameGuard(Runtime& r, const FuncInfo* fi) : rt(r) {
        rt.callStack.push_back(Runtime::CallRec{fi, rt.curLine, rt.curFile});
        if (fi && fi->file) rt.curFile = fi->file;
    }
    ~CallFrameGuard() {
        rt.curFile = rt.callStack.back().callerFile;
        rt.curLine = rt.callStack.back().callLine;
        rt.callStack.pop_back();
    }
};

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
    if (d >= -2147483648.0 && d <= 2147483647.0) return int32_t(d);   // hizli yol (NaN burada elenmez, asagida)
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
    bool completion_ = true;   // deyim tamamlanma degerleri gerekli mi (eval/program); fonksiyon govdelerinde degil

    [[noreturn]] void Throw(const wchar_t* c, const std::wstring& m) { rt.Throw(c, m); }

    Env* NewEnv(Env* parent) {
        Env* e = rt.Alloc<Env>();
        e->parent = parent;
        return e;
    }

    void Step() { rt.CountStep(); }

    // ---------- Degiskenler ----------
    static bool HasWith(Env* env) { for (Env* e = env; e; e = e->parent) if (e->withObj) return true; return false; }
    // with kapsami olmayan zincirde baglama (yoksa nullptr)
    Binding* FindPlain(Atom a, Env* env) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj) return nullptr;
            if (Binding* b = e->Find(a)) return b;
        }
        return nullptr;
    }

    // Degisken erisimi: kapsam zinciri atomla (isaretci karsilastirmasi), sonra global nesne
    Value GetVar(Atom a, Env* env) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj && rt.HasProperty(e->withObj, *a)) return rt.Get(Value::O(e->withObj), *a);
            if (Binding* b = e->Find(a)) return b->v;
        }
        if (rt.HasProperty(rt.global, *a)) return rt.Get(Value::O(rt.global), *a);
        Throw(L"ReferenceError", *a + L" is not defined");
    }
    Value GetVar(const std::wstring& n, Env* env) { return GetVar(AtomOf(n), env); }

    bool TryGetVar(Atom a, Env* env, Value& out) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj && rt.HasProperty(e->withObj, *a)) { out = rt.Get(Value::O(e->withObj), *a); return true; }
            if (Binding* b = e->Find(a)) { out = b->v; return true; }
        }
        if (rt.HasProperty(rt.global, *a)) { out = rt.Get(Value::O(rt.global), *a); return true; }
        return false;
    }
    bool TryGetVar(const std::wstring& n, Env* env, Value& out) { return TryGetVar(AtomOf(n), env, out); }

    void SetVar(Atom a, const Value& v, Env* env) {
        for (Env* e = env; e; e = e->parent) {
            if (e->withObj && rt.HasProperty(e->withObj, *a)) { rt.Put(Value::O(e->withObj), *a, v); return; }
            if (Binding* b = e->Find(a)) {
                if (b->kind == 2) Throw(L"TypeError", L"Assignment to constant variable.");
                b->v = v;
                return;
            }
        }
        rt.Put(Value::O(rt.global), *a, v);
    }
    void SetVar(const std::wstring& n, const Value& v, Env* env) { SetVar(AtomOf(n), v, env); }

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
        if (fi->lazy) {
            // tembel govde: ilk cagrida ayristir
            try { EnsureParsed(fi); }
            catch (SyntaxErr& se) { Throw(L"SyntaxError", se.msg + L" (sat\u0131r " + std::to_wstring(se.line) + L")"); }
        }
        rt.CheckStack();
        if (++rt.depth > rt.maxDepth) { rt.depth--; Throw(L"RangeError", L"Maximum call stack size exceeded"); }
        struct DG { int& d; ~DG() { d--; } } dg{rt.depth};
        CallFrameGuard cfg(rt, fi);
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
                static const Atom kArguments = AtomOf(L"arguments");
                fenv->Define(kArguments, 0).v = Value::O(a);
            }
        }
        if (fi->simpleParams) {
            for (size_t i = 0; i < fi->params.size(); i++)
                fenv->Define(NodeAtom(fi->params[i]), 0).v = i < args.size() ? args[i] : Value();
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
        if (fi->varAtoms.size() != fi->varNames.size()) {
            fi->varAtoms.clear();
            for (auto& n : fi->varNames) fi->varAtoms.push_back(AtomOf(n));
        }
        for (Atom a : fi->varAtoms) if (!fenv->Find(a)) fenv->Define(a, 0);

        if (fi->isGen || fi->isAsync) return StartCoroutine(f, fenv);
        return RunBody(fi, fenv, outThis);
    }

    Value StartCoroutine(Obj* f, Env* fenv);   // asagida (coroutine makinesi)

    Value RunBody(FuncInfo* fi, Env* fenv, Value* outThis) {
        Value result;
        if (fi->exprBody) result = Eval(fi->body, fenv);
        else {
            struct CG { bool& r; bool s; ~CG() { r = s; } } cg{completion_, completion_};
            completion_ = false;   // fonksiyon govdesinde deyim degerleri tutulmaz (return disinda)
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
    void BindName(const std::wstring& n, const Value& v, Env* env, int mode) { BindName(AtomOf(n), v, env, mode); }
    void BindName(Atom n, const Value& v, Env* env, int mode) {
        switch (mode) {
        case B_Assign: case B_Var: SetVar(n, v, env); break;
        case B_Let: env->Define(n, 1).v = v; break;
        case B_Const: env->Define(n, 2).v = v; break;
        default: env->Define(n, 0).v = v; break;
        }
    }

    void Bind(Ast* p, const Value& v, Env* env, int mode) {
        switch (p->k) {
        case K::Ident: BindName(NodeAtom(p), v, env, mode); return;
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
    struct Ref { int kind; Value base; std::wstring key; Value keyV; Atom name; };

    Ref MakeRef(Ast* t, Env* env) {
        Ref r;
        if (t->k == K::Ident) { r.kind = 0; r.name = NodeAtom(t); return r; }
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
        if (r.kind == 0) return GetVar(r.name, env);
        if (r.kind == 1) return rt.Get(r.base, r.key);
        return rt.GetV(r.base, r.keyV);
    }
    void RefPut(Ref& r, const Value& v, Env* env) {
        if (r.kind == 0) SetVar(r.name, v, env);
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
        if (a.IsBig() || b.IsBig()) {
            // BigInt <-> BigInt / Number / String: matematiksel karsilastirma
            auto toBig = [&](const Value& v, BigNum& o, double& d, int& kind) {   // kind 0 big, 1 sayi, 2 tanimsiz
                if (v.IsBig()) { o = BigOf(v); kind = 0; return; }
                if (v.IsStr()) { if (BigParse(*v.s, o)) { kind = 0; return; } kind = 2; return; }
                d = rt.ToNum(v);
                kind = std::isnan(d) ? 2 : 1;
            };
            BigNum x, y;
            double dx = 0, dy = 0;
            int kx, ky;
            toBig(a, x, dx, kx);
            toBig(b, y, dy, ky);
            if (kx == 2 || ky == 2) { undefinedResult = true; return false; }
            auto cmpBD = [](const BigNum& p, double d) -> int {
                if (std::isinf(d)) return d > 0 ? -1 : 1;
                double f = std::floor(d);
                int c = BigCmp(p, BigNum::FromDouble(f));
                if (c) return c;
                return d > f ? -1 : 0;
            };
            int c = (kx == 0 && ky == 0) ? BigCmp(x, y) : kx == 0 ? cmpBD(x, dy) : -cmpBD(y, dx);
            return c < 0;
        }
        double x = rt.ToNum(a), y = rt.ToNum(b);
        if (std::isnan(x) || std::isnan(y)) { undefinedResult = true; return false; }
        return x < y;
    }

    // BigInt aritmetigi: iki taraf da BigInt olmali (yoksa TypeError)
    Value BigBin(uint8_t op, const Value& a, const Value& b) {
        if (!a.IsBig() || !b.IsBig()) Throw(L"TypeError", L"Cannot mix BigInt and other types, use explicit conversions");
        BigNum x = BigOf(a), y = BigOf(b), q, r;
        switch (op) {
        case O_Add: return BigVal(BigAdd(x, y));
        case O_Sub: return BigVal(BigSub(x, y));
        case O_Mul:
            if (x.m.size() + y.m.size() > 200000) Throw(L"RangeError", L"Maximum BigInt size exceeded");
            return BigVal(BigMul(x, y));
        case O_Div: if (y.IsZero()) Throw(L"RangeError", L"Division by zero"); BigDivMod(x, y, q, r); return BigVal(q);
        case O_Mod: if (y.IsZero()) Throw(L"RangeError", L"Division by zero"); BigDivMod(x, y, q, r); return BigVal(r);
        case O_Exp: {
            if (y.neg) Throw(L"RangeError", L"Exponent must be non-negative");
            if (y.m.size() > 1 || (!y.m.empty() && double(y.m[0]) * double(std::max<size_t>(1, BigBitLen(x))) > 6400000.0))
                if (!(x.IsZero() || (x.m.size() == 1 && x.m[0] == 1))) Throw(L"RangeError", L"Maximum BigInt size exceeded");
            BigNum res = BigNum::FromI64(1), base = x;
            uint64_t e = y.m.empty() ? 0 : y.m[0];
            if (x.m.size() == 1 && x.m[0] == 1) { res = (x.neg && (y.m.empty() ? false : (y.m[0] & 1))) ? BigNum::FromI64(-1) : BigNum::FromI64(1); return BigVal(res); }
            while (e) { if (e & 1) res = BigMul(res, base); e >>= 1; if (e) base = BigMul(base, base); }
            return BigVal(res);
        }
        case O_Shl: case O_Shr: {
            if (y.m.size() > 1) Throw(L"RangeError", L"Maximum BigInt size exceeded");
            int64_t n = y.m.empty() ? 0 : int64_t(y.m[0]);
            if (y.neg) n = -n;
            if (op == O_Shr) n = -n;
            if (n > 1000000) Throw(L"RangeError", L"Maximum BigInt size exceeded");
            return BigVal(n >= 0 ? BigShlMag(x, size_t(n)) : BigShr(x, size_t(-n)));
        }
        case O_UShr: Throw(L"TypeError", L"BigInts have no unsigned right shift, use >> instead");
        case O_BitAnd: return BigVal(BigBitwise(x, y, [](uint32_t p, uint32_t q2) { return p & q2; }));
        case O_BitOr: return BigVal(BigBitwise(x, y, [](uint32_t p, uint32_t q2) { return p | q2; }));
        case O_BitXor: return BigVal(BigBitwise(x, y, [](uint32_t p, uint32_t q2) { return p ^ q2; }));
        }
        Throw(L"TypeError", L"Invalid BigInt operation");
    }

    static bool BigArith(uint8_t op) {
        return op == O_Sub || op == O_Mul || op == O_Div || op == O_Mod || op == O_Exp || op == O_Shl || op == O_Shr ||
               op == O_UShr || op == O_BitAnd || op == O_BitOr || op == O_BitXor;
    }

    Value BinOp(uint8_t op, const Value& a0, const Value& b0) {
        // hizli yol: iki sayi (en sik durum) - ToPrimitive/ToNum yok
        if (a0.t == Value::Num && b0.t == Value::Num) {
            double x = a0.n, y = b0.n;
            switch (op) {
            case O_Add: return Value::N(x + y);
            case O_Sub: return Value::N(x - y);
            case O_Mul: return Value::N(x * y);
            case O_Div: return Value::N(x / y);
            case O_Lt: return Value::B(x < y);
            case O_Gt: return Value::B(x > y);
            case O_Le: return Value::B(x <= y);
            case O_Ge: return Value::B(x >= y);
            case O_SEq: case O_Eq: return Value::B(x == y);
            case O_SNe: case O_Ne: return Value::B(x != y);
            case O_BitAnd: return Value::N(double(ToInt32(x) & ToInt32(y)));
            case O_BitOr: return Value::N(double(ToInt32(x) | ToInt32(y)));
            case O_BitXor: return Value::N(double(ToInt32(x) ^ ToInt32(y)));
            case O_Shl: return Value::N(double(int32_t(uint32_t(ToInt32(x)) << (ToUint32(y) & 31))));
            case O_Shr: return Value::N(double(ToInt32(x) >> (ToUint32(y) & 31)));
            case O_UShr: return Value::N(double(ToUint32(x) >> (ToUint32(y) & 31)));
            case O_Mod:
                if (y == 0 || std::isnan(x) || std::isnan(y) || std::isinf(x)) return Value::N(NAN);
                if (std::isinf(y)) return Value::N(x);
                if (x == 0) return Value::N(x);   // -0 korunur
                if (x > 0 && y > 0 && x <= 9007199254740991.0 && y <= 9007199254740991.0 && x == std::floor(x) && y == std::floor(y))
                    return Value::N(double(int64_t(x) % int64_t(y)));   // tamsayi: fmod'dan hizli, ayni sonuc
                return Value::N(std::fmod(x, y));
            default: break;
            }
        }
        if ((a0.IsBig() || b0.IsBig()) && BigArith(op)) {
            Value a = rt.ToPrimitive(a0, 1), b = rt.ToPrimitive(b0, 1);
            return BigBin(op, a, b);
        }
        const Value& a = a0;
        const Value& b = b0;
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
            if (pa.IsBig() || pb.IsBig()) return BigBin(O_Add, pa, pb);
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
        rt.curLine = e->line;
        switch (e->k) {
        case K::Num:
            if (e->flag2) { BigNum b; BigParse(*e->str, b, false); return BigVal(b); }
            return Value::N(e->num);
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
            // Spesifikasyon: ayni cagri noktasi her seferinde AYNI (dondurulmus) dizgi dizisini alir.
            // lit-html gibi kutuphaneler sablonlari bu kimlige gore onbellege alir.
            Obj* sa;
            auto cached = rt.templateObjects.find(e);
            if (cached != rt.templateObjects.end()) sa = cached->second;
            else {
                std::vector<Value> strs, raws;
                for (auto& s : t->cooked) strs.push_back(Value::S(s));
                for (auto& s : t->raw) raws.push_back(Value::S(s));
                sa = rt.NewArrayFrom(strs);
                Obj* ra = rt.NewArrayFrom(raws);
                ra->frozen = true;
                ra->extensible = false;
                rt.DefineData(sa, L"raw", Value::O(ra), false);
                sa->frozen = true;
                sa->extensible = false;
                rt.templateObjects[e] = sa;
            }
            std::vector<Value> args{Value::O(sa)};
            for (Ast* x : t->list) args.push_back(Eval(x, env));
            Value fn, thisV;
            if (e->a->k == K::Member) { thisV = Eval(e->a->a, env); fn = rt.Get(thisV, *e->a->str); }
            else fn = Eval(e->a, env);
            if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", Describe(e->a) + L" is not a function");
            return rt.Call(fn, thisV, args);
        }
        case K::Ident: return GetVar(NodeAtom(e), env);
        case K::This: return ThisOf(env);
        case K::Super: Throw(L"SyntaxError", L"'super' keyword unexpected here");
        case K::Regex: return Value::O(rt.NewRegExp(*e->str, e->str2));
        case K::Arr: {
            Obj* a = rt.NewArray(0);
            for (Ast* el : e->list) {
                if (el->k == K::Hole) a->elems.push_back(Value::Hole());
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
            Value ov = rt.ToPrimitive(RefGet(r, env), 1);
            if (ov.IsBig()) {
                BigNum o = BigOf(ov), nv = e->op == O_Inc ? BigAdd(o, BigNum::FromI64(1)) : BigSub(o, BigNum::FromI64(1));
                Value nvv = BigVal(nv);
                RefPut(r, nvv, env);
                return e->flag ? nvv : ov;
            }
            double old = rt.ToNum(ov);
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
        case K::Yield:
            // Uretec govdeleri coroutine makinesinde yurutulur; buraya yalnizca makinenin
            // desteklemedigi bir ifade icindeki yield duserse gelinir.
            Throw(L"SyntaxError", L"merJS: bu ifade icinde yield kullanimi desteklenmiyor");
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
                if (!TryGetVar(NodeAtom(e->a), env, v)) return Value::S(L"undefined");
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
        case O_Neg: { Value v = rt.ToPrimitive(Eval(e->a, env), 1); return v.IsBig() ? BigVal(BigNeg(BigOf(v))) : Value::N(-rt.ToNum(v)); }
        case O_Pos: return Value::N(rt.ToNum(Eval(e->a, env)));
        case O_BitNot: { Value v = rt.ToPrimitive(Eval(e->a, env), 1); return v.IsBig() ? BigVal(BigNot(BigOf(v))) : Value::N(double(~ToInt32(rt.ToNum(v)))); }
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
                SetVar(NodeAtom(t), v, env);
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
        // Hizli yol: yerel degiskene dize ekleme (s += x). Dizge yalnizca bu baglamada ise yerinde
        // buyutulur: dongude dize kurmak O(n^2) yerine O(n) olur.
        if (e->op == O_Add && t->k == K::Ident) {
            Atom a = NodeAtom(t);
            Binding* b = FindPlain(a, env);
            if (b && b->v.IsStr()) {
                Value cur = b->v;
                Value rhs = Eval(e->b, env);
                Value prim = rhs.IsStr() ? rhs : rt.ToPrimitive(rhs, 0);
                Str rs = rt.ToStrS(prim);
                Binding* b2 = FindPlain(a, env);
                if (b2 && b2->kind != 2 && b2->v.IsStr() && b2->v.s.get() == cur.s.get()) {
                    if (cur.s->size() + rs->size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
                    cur = Value();   // kendi kopyamizi birak
                    if (b2->v.s.use_count() == 1) {
                        std::wstring& w = const_cast<std::wstring&>(*b2->v.s);
                        if (w.capacity() < w.size() + rs->size()) w.reserve(std::max(w.size() * 2, w.size() + rs->size()));
                        w += *rs;
                        return completion_ ? b2->v : Value();
                    }
                    std::wstring r;
                    r.reserve(b2->v.s->size() + rs->size());
                    r += *b2->v.s;
                    r += *rs;
                    b2->v = Value::S(std::move(r));
                    return completion_ ? b2->v : Value();
                }
                // baglama degisti (rhs degiskeni degistirdi vb.): genel yol, eski deger kullanilir
                std::wstring r = *cur.s + *rs;
                Value v = Value::S(std::move(r));
                SetVar(a, v, env);
                return v;
            }
            // global degisken (ust duzey var): global nesnenin kendi veri ozelligi
            if (!b && !HasWith(env)) {
                Prop* gp = rt.global->props.Find(*a);
                if (gp && !gp->accessor && gp->writable && gp->v.IsStr()) {
                    Value cur = gp->v;
                    Value rhs = Eval(e->b, env);
                    Value prim = rhs.IsStr() ? rhs : rt.ToPrimitive(rhs, 0);
                    Str rs = rt.ToStrS(prim);
                    Prop* gp2 = rt.global->props.Find(*a);
                    if (gp2 && !gp2->accessor && gp2->writable && gp2->v.IsStr() && gp2->v.s.get() == cur.s.get() && !FindPlain(a, env)) {
                        if (cur.s->size() + rs->size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
                        cur = Value();
                        if (gp2->v.s.use_count() == 1) {
                            std::wstring& w = const_cast<std::wstring&>(*gp2->v.s);
                            if (w.capacity() < w.size() + rs->size()) w.reserve(std::max(w.size() * 2, w.size() + rs->size()));
                            w += *rs;
                            return completion_ ? gp2->v : Value();
                        }
                        std::wstring r;
                        r.reserve(gp2->v.s->size() + rs->size());
                        r += *gp2->v.s;
                        r += *rs;
                        gp2->v = Value::S(std::move(r));
                        return completion_ ? gp2->v : Value();
                    }
                    std::wstring r = *cur.s + *rs;
                    Value v = Value::S(std::move(r));
                    SetVar(a, v, env);
                    return v;
                }
            }
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

    // ---------- ES modulleri: bagimliliklari govdeden once yukle ve baglari kur ----------
    void LinkImports(const std::vector<Ast*>& list, Env* env) {
        auto self = rt.currentModule;
        for (Ast* s : list) {
            if (s->k != K::ImportDecl || !s->str) continue;
            auto dep = rt.LoadModuleInternal(*s->str, self->url);
            rt.currentModule = self;
            if (!dep) continue;
            for (Ast* sp : s->list) {
                const std::wstring& imported = *sp->str;
                Value v;
                if (imported == L"*") v = Value::O(dep->namespaceObj);
                else {
                    auto it = dep->exports.find(imported);
                    if (it != dep->exports.end()) v = it->second;
                    else if (dep->evaluated) {
                        if (!rt.HasOwn(dep->namespaceObj, imported))
                            rt.Throw(L"SyntaxError", L"The requested module '" + *s->str + L"' does not provide an export named '" + imported + L"'");
                        v = rt.Get(Value::O(dep->namespaceObj), imported);
                    }
                }
                env->Define(sp->str2, 2).v = v;
            }
        }
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
            if (completion_ && !c.v.IsUndef()) last.v = c.v;
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
        rt.curLine = s->line;
        switch (s->k) {
        case K::ExprStmt: { Comp c; if (completion_) c.v = Eval(s->a, env); else Eval(s->a, env); return c; }
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
                                    rt.currentModule->localExports.emplace_back(name, name);
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
                        if (!depMod) rt.currentModule->localExports.emplace_back(expName, origName);
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
            if (rt.currentModule && !env->isGlobal) LinkImports(s->list, env);
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
        if (s->flag) {
            // 'for await' yedek yolu (coroutine disi baglam): her adimi senkron bekle
            if (it.IsNullish()) Throw(L"TypeError", rt.ToStr(it) + L" is not async iterable");
            Value fn = rt.Get(it, L"\x01Symbol.asyncIterator");
            bool async = fn.IsObj() && fn.o->IsCallable();
            if (!async) fn = rt.Get(it, L"\x01Symbol.iterator");
            if (!fn.IsObj() || !fn.o->IsCallable()) Throw(L"TypeError", Describe(s->b) + L" is not async iterable");
            std::vector<Value> none;
            Value iter = rt.Call(fn, it, none);
            Value next = rt.Get(iter, L"next");
            while (true) {
                std::vector<Value> n2;
                Value r = rt.Call(next, iter, n2);
                if (async) r = Await(r);
                if (!r.IsObj()) Throw(L"TypeError", L"Iterator result is not an object");
                if (rt.Truthy(rt.Get(r, L"done"))) break;
                Value v = rt.Get(r, L"value");
                if (!async) v = Await(v);
                int ctl = body(v, out);
                if (ctl != 0) {
                    Value ret = rt.Get(iter, L"return");
                    if (ret.IsObj() && ret.o->IsCallable()) { std::vector<Value> n3; rt.Call(ret, iter, n3); }
                    if (ctl == 2) return out;
                    break;
                }
            }
            return Comp();
        }
        if (it.IsObj() && it.o->cls == Cls::Array) {
            Obj* a = it.o;
            for (size_t i = 0; i < a->elems.size(); i++) {
                int r = body(Unhole(a->elems[i]), out);
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

// ======================================================================
//  Coroutine makinesi: gercek async/await ve tembel (lazy) ureteçler
// ======================================================================
// async / uretec fonksiyonlarinin govdesi burada, GC'nin izledigi acik bir cerceve
// yigini (Coro::frames) uzerinde yurutulur. Her cerceve bir AST dugumu + adim sayaci
// (pc) + ara degerlerdir. 'await' ya da 'yield' noktasinda makine dondugunde tum durum
// cercevelerde kalir; Promise cozuldugunde (ya da next() cagrildiginda) kaldigi yerden
// surer. Alt agacinda await/yield olmayan dugumler (sus == false) icin olagan hizli
// ozyinelemeli yorumlayici kullanilir; boylece yavaslama yalnizca askiya alma
// noktalarini iceren yollarda olur.

enum : uint8_t { CT_Normal = 0, CT_Return = 1, CT_Break = 2, CT_Continue = 3, CT_Throw = 9 };

Value CoroAwaitResume(Runtime& rt, Obj* self, const Value& thisV, std::vector<Value>& args);

Obj* IterResult(Runtime& rt, const Value& v, bool done) {
    Obj* r = rt.NewObject();
    rt.DefineData(r, L"value", v);
    rt.DefineData(r, L"done", Value::B(done));
    return r;
}

// Makinenin kendi cercevesiyle yurutebildigi ifadeler
bool CoSupE(Ast* n) {
    switch (n->k) {
    case K::Await: case K::Yield: case K::Bin: case K::Logic: case K::Cond: case K::Seq:
    case K::Tpl: case K::Arr: case K::ObjLit: case K::New:
        return true;
    case K::Unary: return n->op != O_Delete;
    case K::Assign: return !n->a->sus;
    case K::Member: case K::Index: return n->a->k != K::Super;
    case K::Call:
        if (n->a->k == K::Super) return false;
        if ((n->a->k == K::Member || n->a->k == K::Index) && n->a->a->k == K::Super) return false;
        return true;
    default: return false;
    }
}

// Makinenin kendi cercevesiyle yurutebildigi deyimler
bool CoSupS(Ast* n) {
    switch (n->k) {
    case K::ExprStmt: case K::VarDecl: case K::Return: case K::Throw: case K::If: case K::Block:
    case K::For: case K::ForIn: case K::ForOf: case K::While: case K::DoWhile: case K::Try:
    case K::Switch: case K::Labeled:
        return true;
    default: return false;
    }
}

struct Machine {
    Runtime& rt;
    In& in;
    Coro* co;
    bool suspended = false;

    CoFrame& Top() { return co->frames.back(); }

    void Push(Ast* n, Env* env, bool stmt, const Labels* labels) {
        if (co->frames.size() > 200000) in.Throw(L"RangeError", L"Maximum call stack size exceeded");
        co->frames.emplace_back();
        CoFrame& f = co->frames.back();
        f.node = n;
        f.env = env;
        f.stmt = stmt;
        if (labels) f.labels = *labels;
    }

    // Ifade: true donerse sonuc co->res'te hazir; false ise alt cerceve eklendi.
    bool E(Ast* n, Env* env) {
        if (!n->sus || !CoSupE(n)) { co->res = in.Eval(n, env); return true; }
        Push(n, env, false, nullptr);
        return false;
    }

    // Deyim: true donerse tamamlanma co->rct/res/rlabel'da hazir.
    bool S(Ast* n, Env* env, const Labels* labels) {
        if (!n->sus || !CoSupS(n)) {
            Comp c = in.Exec(n, env, labels);
            co->rct = uint8_t(c.t);
            co->res = c.v;
            co->rlabel = c.label;
            return true;
        }
        Push(n, env, true, labels);
        return false;
    }

    void Done(const Value& v0) {
        Value v = v0;            // v0 cogu zaman cikarilan cercevenin icini gosterir: once kopyala
        co->frames.pop_back();
        co->res = std::move(v);
        co->rct = CT_Normal;
        co->rlabel = nullptr;
    }
    void DoneC(uint8_t t, const Value& v0, const std::wstring* label) {
        Value v = v0;
        co->frames.pop_back();
        co->res = std::move(v);
        co->rct = t;
        co->rlabel = label;
    }
    void DoneChild() { Value v = co->res; DoneC(co->rct, v, co->rlabel); }
    Comp ChildComp() {
        Comp c;
        c.t = Comp::T(co->rct);
        c.v = co->res;
        c.label = co->rlabel;
        return c;
    }

    void Finish(uint8_t outcome, const Value& v) {
        co->frames.clear();
        co->state = Coro::Done;
        co->outcome = outcome;
        co->outVal = v;
    }

    // Degeri bekle: Promise'e cevir, tepkileri bagla ve makineyi durdur.
    void AwaitValue(const Value& v, int resumePc) {
        Obj* p;
        if (v.IsObj() && v.o->cls == Cls::Promise) p = v.o;
        else {
            p = rt.NewPromise();
            rt.ResolvePromise(p, v);
        }
        Obj* onF = rt.NewFunction(CoroAwaitResume, L"", 1);
        onF->x->slots = {Value::O(co->holder), Value::N(0)};
        Obj* onR = rt.NewFunction(CoroAwaitResume, L"", 1);
        onR->x->slots = {Value::O(co->holder), Value::N(1)};
        rt.PromiseThen(p, Value::O(onF), Value::O(onR), nullptr);
        Top().pc = resumePc;
        co->state = Coro::SuspAwait;
        suspended = true;
    }

    void YieldValue(const Value& v, int resumePc) {
        Top().pc = resumePc;
        co->state = Coro::SuspYield;
        co->outcome = Coro::OutYield;
        co->outVal = v;
        suspended = true;
    }

    void CloseIter(const Value& iter) {
        Value ret = rt.Get(iter, L"return");
        if (ret.IsObj() && ret.o->IsCallable()) {
            std::vector<Value> none;
            rt.Call(ret, iter, none);
        }
    }

    // Istisnayi cerceveler boyunca yay: try/catch/finally yakalar.
    // false: fonksiyon istisnayla bitti.
    bool Unwind(const Value& v) {
        while (!co->frames.empty()) {
            CoFrame& f = Top();
            if (f.stmt && f.node->k == K::Try) {
                if (f.pc == 1 && f.node->c) { f.pc = 2; f.cv = v; return true; }
                if ((f.pc == 1 || f.pc == 2 || f.pc == 3) && f.node->d) {
                    f.ct = CT_Throw;
                    f.cv = v;
                    f.clabel = nullptr;
                    f.pc = 5;
                    return true;
                }
            }
            if (f.stmt && f.node->k == K::ForOf && f.rkind >= 2 && f.pc >= 2 && !f.vals.empty()) {
                Value iter = f.vals[0];
                try { CloseIter(iter); } catch (JSThrow&) {}
            }
            co->frames.pop_back();
        }
        Finish(Coro::OutThrow, v);
        return false;
    }

    // Uretecin return(v) ile kapatilmasi: askidaki ifadeyi birak, icinde bulundugu
    // deyimin yerine 'return v' tamamlanmasi koy (finally bloklari yine calisir).
    bool InjectReturn(const Value& v) {
        while (!co->frames.empty() && !Top().stmt) co->frames.pop_back();
        if (!co->frames.empty() && Top().node->k != K::Func) co->frames.pop_back();
        if (co->frames.empty()) { Finish(Coro::OutReturn, v); return false; }
        co->rct = CT_Return;
        co->res = v;
        co->rlabel = nullptr;
        return true;
    }

    // ?. kisa devresi: zincirin kokune kadar cerceveleri at.
    void ShortCircuit() {
        while (!co->frames.empty()) {
            CoFrame& f = Top();
            if (!f.stmt && f.node->flag2 && (f.node->k == K::Call || f.node->k == K::Member || f.node->k == K::Index)) {
                Done(Value());
                return;
            }
            co->frames.pop_back();
        }
        in.Throw(L"SyntaxError", L"optional chain");
    }

    void Loop() {
        while (!co->frames.empty()) {
            in.Step();
            try {
                Run();
            } catch (JSThrow& t) {
                Value v = t.v;
                if (!Unwind(v)) return;
            } catch (OptShort&) {
                try {
                    ShortCircuit();
                } catch (JSThrow& t) {
                    Value v = t.v;
                    if (!Unwind(v)) return;
                }
            }
            if (suspended) return;
        }
        Value r = co->res;
        Finish(Coro::OutReturn, r);
    }

    // mode: 0 next(v) / await sonucu, 1 throw(v) / await reddi, 2 return(v)
    void Resume(bool fresh, int mode, const Value& v) {
        if (fresh) { Loop(); return; }
        if (co->frames.empty()) { Finish(Coro::OutReturn, Value()); return; }
        CoFrame& f = Top();
        if (!f.stmt && f.node->k == K::Yield && f.node->flag && f.pc == 4) {
            // yield*: istegi ic yineleyiciye ilet
            f.idx = size_t(mode);
            co->res = v;
            Loop();
            return;
        }
        if (mode == 0) {
            co->res = v;
            co->rct = CT_Normal;
            co->rlabel = nullptr;
            Loop();
        } else if (mode == 1) {
            if (Unwind(v)) Loop();
        } else {
            if (InjectReturn(v)) Loop();
        }
    }

    void Run() {
        CoFrame& f = Top();
        Ast* n = f.node;
        rt.curLine = n->line;
        if (n->k == K::Func) RunRoot(f, n);
        else if (f.stmt) RunS(f, n);
        else RunE(f, n);
    }

    // ---------- Fonksiyon govdesi ----------
    void RunRoot(CoFrame& f, Ast* n) {
        FuncInfo* fi = n->fi;
        if (fi->exprBody) {
            if (f.pc == 0) { f.pc = 1; if (!E(fi->body, f.env)) return; }
            Value v = co->res;
            Done(v);
            return;
        }
        const std::vector<Ast*>& list = fi->body->list;
        if (f.pc == 0) { in.Hoist(list, f.env); f.pc = 1; f.idx = 0; }
        while (true) {
            if (f.pc == 2) {
                f.pc = 1;
                if (co->rct == CT_Return) { Value v = co->res; Done(v); return; }
            }
            if (f.idx >= list.size()) { Done(Value()); return; }
            Ast* st = list[f.idx++];
            f.pc = 2;
            if (!S(st, f.env, nullptr)) return;
        }
    }

    // ---------- Deyimler ----------
    void RunS(CoFrame& f, Ast* n) {
        switch (n->k) {
        case K::ExprStmt:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            { Value v = co->res; DoneC(CT_Normal, v, nullptr); }
            return;
        case K::VarDecl: {
            int mode = n->kind == 0 ? B_Var : n->kind == 1 ? B_Let : B_Const;
            while (true) {
                if (f.pc == 1) {
                    Ast* d = n->list[f.idx];
                    Value v = co->res;
                    if (d->a->k == K::Ident) In::NameFn(v, *d->a->str);
                    in.Bind(d->a, v, f.env, mode);
                    f.idx++;
                    f.pc = 0;
                }
                if (f.idx >= n->list.size()) { DoneC(CT_Normal, Value(), nullptr); return; }
                Ast* d = n->list[f.idx];
                if (!d->b) {
                    if (n->kind != 0) in.Bind(d->a, Value(), f.env, mode);
                    f.idx++;
                    continue;
                }
                f.pc = 1;
                if (!E(d->b, f.env)) return;
            }
        }
        case K::Return:
            if (f.pc == 0) {
                if (!n->a) { DoneC(CT_Return, Value(), nullptr); return; }
                f.pc = 1;
                if (!E(n->a, f.env)) return;
            }
            { Value v = co->res; DoneC(CT_Return, v, nullptr); }
            return;
        case K::Throw:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            { Value v = co->res; throw JSThrow{v}; }
        case K::If:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) {
                Ast* br = rt.Truthy(co->res) ? n->b : n->c;
                if (!br) { DoneC(CT_Normal, Value(), nullptr); return; }
                f.pc = 2;
                if (!S(br, f.env, nullptr)) return;
            }
            DoneChild();
            return;
        case K::Block: {
            if (f.pc == 0) {
                f.e1 = n->flag2 ? in.NewEnv(f.env) : f.env;
                in.Hoist(n->list, f.e1);
                f.pc = 1;
                f.idx = 0;
            }
            while (true) {
                if (f.pc == 2) {
                    if (co->rct != CT_Normal) { DoneChild(); return; }
                    f.pc = 1;
                }
                if (f.idx >= n->list.size()) { DoneC(CT_Normal, Value(), nullptr); return; }
                Ast* st = n->list[f.idx++];
                f.pc = 2;
                if (!S(st, f.e1, nullptr)) return;
            }
        }
        case K::Labeled:
            if (f.pc == 0) {
                Labels ls = f.labels;
                ls.push_back(&n->str2);
                f.pc = 1;
                if (!S(n->a, f.env, &ls)) return;
            }
            if (co->rct == CT_Break && co->rlabel && *co->rlabel == n->str2) { DoneC(CT_Normal, Value(), nullptr); return; }
            DoneChild();
            return;
        case K::While:
            while (true) {
                if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
                if (f.pc == 1) {
                    if (!rt.Truthy(co->res)) { DoneC(CT_Normal, Value(), nullptr); return; }
                    f.pc = 2;
                    if (!S(n->b, f.env, nullptr)) return;
                }
                int r = In::LoopCtl(ChildComp(), &f.labels);
                if (r == 1) { DoneC(CT_Normal, Value(), nullptr); return; }
                if (r == 2) { DoneChild(); return; }
                f.pc = 0;
            }
        case K::DoWhile:
            while (true) {
                if (f.pc == 0) { f.pc = 1; if (!S(n->b, f.env, nullptr)) return; }
                if (f.pc == 1) {
                    int r = In::LoopCtl(ChildComp(), &f.labels);
                    if (r == 1) { DoneC(CT_Normal, Value(), nullptr); return; }
                    if (r == 2) { DoneChild(); return; }
                    f.pc = 2;
                    if (!E(n->a, f.env)) return;
                }
                if (!rt.Truthy(co->res)) { DoneC(CT_Normal, Value(), nullptr); return; }
                f.pc = 0;
            }
        case K::For: return RunFor(f, n);
        case K::ForIn: return RunForIn(f, n);
        case K::ForOf: return RunForOf(f, n);
        case K::Try: return RunTry(f, n);
        case K::Switch: return RunSwitch(f, n);
        default:
            in.Throw(L"SyntaxError", L"Unexpected statement");
        }
    }

    void RunFor(CoFrame& f, Ast* n) {
        bool perIter = f.rkind == 1;
        while (true) {
            switch (f.pc) {
            case 0:
                perIter = n->a && n->a->k == K::VarDecl && n->a->kind != 0;
                f.rkind = perIter ? 1 : 0;
                f.e1 = perIter ? in.NewEnv(f.env) : f.env;
                f.pc = 1;
                if (n->a && !S(n->a, f.e1, nullptr)) return;
                if (!n->a) co->rct = CT_Normal;
                // fallthrough
            case 1:
                if (co->rct != CT_Normal) { DoneChild(); return; }
                f.e2 = perIter ? in.CopyEnv(f.e1) : f.e1;
                f.pc = 2;
                // fallthrough
            case 2:
                if (!n->b) { f.pc = 4; continue; }
                f.pc = 3;
                if (!E(n->b, f.e2)) return;
                // fallthrough
            case 3:
                if (!rt.Truthy(co->res)) { DoneC(CT_Normal, Value(), nullptr); return; }
                f.pc = 4;
                // fallthrough
            case 4:
                f.pc = 5;
                if (!S(n->d, f.e2, nullptr)) return;
                // fallthrough
            case 5: {
                int r = In::LoopCtl(ChildComp(), &f.labels);
                if (r == 1) { DoneC(CT_Normal, Value(), nullptr); return; }
                if (r == 2) { DoneChild(); return; }
                if (perIter) f.e2 = in.CopyEnv(f.e2);
                if (!n->c) { f.pc = 2; continue; }
                f.pc = 6;
                if (!E(n->c, f.e2)) return;
            }
                // fallthrough
            case 6:
                f.pc = 2;
                continue;
            }
        }
    }

    void RunForIn(CoFrame& f, Ast* n) {
        while (true) {
            switch (f.pc) {
            case 0:
                f.pc = 1;
                if (!E(n->b, f.env)) return;
                // fallthrough
            case 1: {
                Value o = co->res;
                if (o.IsNullish()) { DoneC(CT_Normal, Value(), nullptr); return; }
                Obj* obj = rt.ToObject(o);
                f.vals.clear();
                f.vals.push_back(Value::O(obj));
                std::set<std::wstring> seen;
                int guard = 0;
                for (Obj* p = obj; p && guard < 1000; p = p->proto, guard++)
                    for (auto& k : rt.OwnKeys(p, true)) if (seen.insert(k).second) f.vals.push_back(Value::S(k));
                f.idx = 1;
                f.pc = 2;
            }
                // fallthrough
            case 2: {
                if (f.idx >= f.vals.size()) { DoneC(CT_Normal, Value(), nullptr); return; }
                Value k = f.vals[f.idx++];
                if (!rt.HasProperty(f.vals[0].o, *k.s)) continue;
                Env* it = nullptr;
                in.BindLoopVar(n->a, k, f.env, it);
                f.e2 = it;
                f.pc = 3;
                if (!S(n->c, f.e2, nullptr)) return;
            }
                // fallthrough
            case 3: {
                int r = In::LoopCtl(ChildComp(), &f.labels);
                if (r == 1) { DoneC(CT_Normal, Value(), nullptr); return; }
                if (r == 2) { DoneChild(); return; }
                f.pc = 2;
                continue;
            }
            }
        }
    }

    // rkind: 1 dizi/anlik liste, 2 yineleyici protokolu, 3 async yineleyici, 4 senkron yineleyici + await
    void RunForOf(CoFrame& f, Ast* n) {
        while (true) {
            switch (f.pc) {
            case 0:
                f.pc = 1;
                if (!E(n->b, f.env)) return;
                // fallthrough
            case 1: {
                Value it = co->res;
                f.vals.clear();
                f.idx = 0;
                if (n->flag) {
                    if (it.IsNullish()) in.Throw(L"TypeError", rt.ToStr(it) + L" is not async iterable");
                    Value fn = rt.Get(it, L"\x01Symbol.asyncIterator");
                    bool async = fn.IsObj() && fn.o->IsCallable();
                    if (!async) fn = rt.Get(it, L"\x01Symbol.iterator");
                    if (!fn.IsObj() || !fn.o->IsCallable()) in.Throw(L"TypeError", in.Describe(n->b) + L" is not async iterable");
                    std::vector<Value> none;
                    Value iter = rt.Call(fn, it, none);
                    f.vals = {iter, rt.Get(iter, L"next")};
                    f.rkind = async ? 3 : 4;
                } else if (it.IsObj() && it.o->cls == Cls::Array) {
                    f.vals = {it};
                    f.rkind = 1;
                } else if (it.IsStr() || (it.IsObj() && (it.o->cls == Cls::Map || it.o->cls == Cls::Set || it.o->cls == Cls::Arguments || it.o->cls == Cls::TypedArray))) {
                    f.vals = {Value::O(rt.NewArrayFrom(rt.IterateToList(it)))};
                    f.rkind = 1;
                } else {
                    if (it.IsNullish()) in.Throw(L"TypeError", rt.ToStr(it) + L" is not iterable");
                    Value fn = rt.Get(it, L"\x01Symbol.iterator");
                    if (!fn.IsObj() || !fn.o->IsCallable()) in.Throw(L"TypeError", in.Describe(n->b) + L" is not iterable");
                    std::vector<Value> none;
                    Value iter = rt.Call(fn, it, none);
                    f.vals = {iter, rt.Get(iter, L"next")};
                    f.rkind = 2;
                }
                f.pc = 2;
            }
                // fallthrough
            case 2: {
                Value v;
                if (f.rkind == 1) {
                    Obj* a = f.vals[0].o;
                    if (f.idx >= a->elems.size()) { DoneC(CT_Normal, Value(), nullptr); return; }
                    v = Unhole(a->elems[f.idx++]);
                } else {
                    std::vector<Value> none;
                    Value r = rt.Call(f.vals[1], f.vals[0], none);
                    if (f.rkind == 3) { AwaitValue(r, 3); return; }
                    if (!r.IsObj()) in.Throw(L"TypeError", L"Iterator result is not an object");
                    if (rt.Truthy(rt.Get(r, L"done"))) { DoneC(CT_Normal, Value(), nullptr); return; }
                    v = rt.Get(r, L"value");
                    if (f.rkind == 4) { AwaitValue(v, 4); return; }
                }
                co->res = v;
                f.pc = 5;
                continue;
            }
            case 3: {
                Value r = co->res;
                if (!r.IsObj()) in.Throw(L"TypeError", L"Iterator result is not an object");
                if (rt.Truthy(rt.Get(r, L"done"))) { DoneC(CT_Normal, Value(), nullptr); return; }
                co->res = rt.Get(r, L"value");
                f.pc = 5;
                continue;
            }
            case 4:
                f.pc = 5;
                continue;
            case 5: {
                Value v = co->res;
                Env* ie = nullptr;
                in.BindLoopVar(n->a, v, f.env, ie);
                f.e2 = ie;
                f.pc = 6;
                if (!S(n->c, f.e2, nullptr)) return;
            }
                // fallthrough
            case 6: {
                Comp c = ChildComp();
                int r = In::LoopCtl(c, &f.labels);
                if (r == 0) { f.pc = 2; continue; }
                if (f.rkind >= 2) {
                    Value iter = f.vals[0];
                    f.rkind = 0;   // kapatma sirasinda hata olursa Unwind tekrar kapatmasin
                    CloseIter(iter);
                }
                if (r == 1) { DoneC(CT_Normal, Value(), nullptr); return; }
                DoneC(uint8_t(c.t), c.v, c.label);
                return;
            }
            default:
                DoneC(CT_Normal, Value(), nullptr);
                return;
            }
        }
    }

    // pc: 0 basla, 1 try blogu calisiyor, 2 catch kur, 3 catch calisiyor, 5 finally'ye gec, 6 finally calisiyor
    void RunTry(CoFrame& f, Ast* n) {
        while (true) {
            switch (f.pc) {
            case 0:
                f.pc = 1;
                if (!S(n->a, f.env, nullptr)) return;
                // fallthrough
            case 1:
                f.ct = co->rct;
                f.cv = co->res;
                f.clabel = co->rlabel;
                f.pc = 5;
                continue;
            case 2: {
                Value ex = f.cv;
                f.cv = Value();
                f.e1 = in.NewEnv(f.env);
                f.pc = 3;
                if (n->b) in.Bind(n->b, ex, f.e1, B_Let);
                if (!S(n->c, f.e1, nullptr)) return;
            }
                // fallthrough
            case 3:
                f.ct = co->rct;
                f.cv = co->res;
                f.clabel = co->rlabel;
                f.pc = 5;
                continue;
            case 5:
                if (n->d) {
                    f.pc = 6;
                    if (!S(n->d, f.env, nullptr)) return;
                    continue;
                }
                if (f.ct == CT_Throw) { Value v = f.cv; throw JSThrow{v}; }
                { uint8_t t = f.ct; Value v = f.cv; const std::wstring* l = f.clabel; DoneC(t, v, l); }
                return;
            case 6:
                if (co->rct != CT_Normal) { DoneChild(); return; }
                if (f.ct == CT_Throw) { Value v = f.cv; throw JSThrow{v}; }
                { uint8_t t = f.ct; Value v = f.cv; const std::wstring* l = f.clabel; DoneC(t, v, l); }
                return;
            default:
                DoneC(CT_Normal, Value(), nullptr);
                return;
            }
        }
    }

    // vals[0] ayirt edici, rkind: calistirilan case, idx: case testi / deyim konumu, cv: default konumu
    void RunSwitch(CoFrame& f, Ast* n) {
        while (true) {
            switch (f.pc) {
            case 0:
                f.pc = 1;
                if (!E(n->a, f.env)) return;
                // fallthrough
            case 1:
                f.vals = {co->res};
                f.e1 = in.NewEnv(f.env);
                for (Ast* c : n->list) in.Hoist(c->list, f.e1);
                f.idx = 0;
                f.cv = Value::N(-1);
                f.pc = 2;
                // fallthrough
            case 2: {
                if (f.idx >= n->list.size()) {
                    int def = int(f.cv.n);
                    if (def < 0) { DoneC(CT_Normal, Value(), nullptr); return; }
                    f.rkind = def;
                    f.idx = 0;
                    f.pc = 4;
                    continue;
                }
                Ast* c = n->list[f.idx];
                if (!c->a) { f.cv = Value::N(double(f.idx)); f.idx++; continue; }
                f.pc = 3;
                if (!E(c->a, f.e1)) return;
            }
                // fallthrough
            case 3:
                if (rt.StrictEq(f.vals[0], co->res)) {
                    f.rkind = int(f.idx);
                    f.idx = 0;
                    f.pc = 4;
                    continue;
                }
                f.idx++;
                f.pc = 2;
                continue;
            case 4: {
                if (size_t(f.rkind) >= n->list.size()) { DoneC(CT_Normal, Value(), nullptr); return; }
                const std::vector<Ast*>& stmts = n->list[size_t(f.rkind)]->list;
                if (f.idx >= stmts.size()) { f.rkind++; f.idx = 0; continue; }
                Ast* st = stmts[f.idx++];
                f.pc = 5;
                if (!S(st, f.e1, nullptr)) return;
            }
                // fallthrough
            case 5:
                if (co->rct == CT_Break && !co->rlabel) { DoneC(CT_Normal, Value(), nullptr); return; }
                if (co->rct != CT_Normal) { DoneChild(); return; }
                f.pc = 4;
                continue;
            default:
                DoneC(CT_Normal, Value(), nullptr);
                return;
            }
        }
    }

    // ---------- Ifadeler ----------
    // Arguman listesi: vals[2..]'ye ekler. true: hepsi hazir.
    bool Args(CoFrame& f, const std::vector<Ast*>& list, int loopPc) {
        while (true) {
            if (f.pc == loopPc + 1) {
                Ast* a = list[f.idx];
                if (a->k == K::Spread) {
                    std::vector<Value> items = rt.IterateToList(co->res);
                    if (f.vals.size() + items.size() > 1000000) in.Throw(L"RangeError", L"Too many arguments");
                    f.vals.insert(f.vals.end(), items.begin(), items.end());
                } else f.vals.push_back(co->res);
                f.idx++;
                f.pc = loopPc;
            }
            if (f.idx >= list.size()) return true;
            Ast* a = list[f.idx];
            f.pc = loopPc + 1;
            if (!E(a->k == K::Spread ? a->a : a, f.env)) return false;
        }
    }

    void RunE(CoFrame& f, Ast* n) {
        switch (n->k) {
        case K::Await:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) { Value v = co->res; AwaitValue(v, 2); return; }
            { Value v = co->res; Done(v); }
            return;
        case K::Yield:
            if (n->flag) return RunYieldStar(f, n);
            if (f.pc == 0) {
                f.pc = 1;
                if (n->a) { if (!E(n->a, f.env)) return; }
                else co->res = Value();
            }
            if (f.pc == 1) {
                Value v = co->res;
                if (co->kind == Coro::AsyncGen) { AwaitValue(v, 2); return; }
                YieldValue(v, 3);
                return;
            }
            if (f.pc == 2) { Value v = co->res; YieldValue(v, 3); return; }
            { Value v = co->res; Done(v); }
            return;
        case K::Bin:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) { f.vals = {co->res}; f.pc = 2; if (!E(n->b, f.env)) return; }
            { Value b = co->res; Value a = f.vals[0]; Done(in.BinOp(n->op, a, b)); }
            return;
        case K::Logic:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) {
                Value a = co->res;
                bool shortc = n->op == O_And ? !rt.Truthy(a) : n->op == O_Or ? rt.Truthy(a) : !a.IsNullish();
                if (shortc) { Done(a); return; }
                f.pc = 2;
                if (!E(n->b, f.env)) return;
            }
            { Value v = co->res; Done(v); }
            return;
        case K::Cond:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) { f.pc = 2; if (!E(rt.Truthy(co->res) ? n->b : n->c, f.env)) return; }
            { Value v = co->res; Done(v); }
            return;
        case K::Seq:
            while (true) {
                if (f.pc == 1 && f.idx >= n->list.size()) { Value v = co->res; Done(v); return; }
                f.pc = 1;
                if (!E(n->list[f.idx++], f.env)) return;
            }
        case K::Unary:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            {
                Value a = co->res;
                switch (n->op) {
                case O_TypeOf: Done(Value::S(rt.TypeOf(a))); return;
                case O_Void: Done(Value()); return;
                case O_Not: Done(Value::B(!rt.Truthy(a))); return;
                case O_Neg: { Value v = rt.ToPrimitive(a, 1); Done(v.IsBig() ? BigVal(BigNeg(BigOf(v))) : Value::N(-rt.ToNum(v))); return; }
                case O_Pos: Done(Value::N(rt.ToNum(a))); return;
                case O_BitNot: { Value v = rt.ToPrimitive(a, 1); Done(v.IsBig() ? BigVal(BigNot(BigOf(v))) : Value::N(double(~ToInt32(rt.ToNum(v))))); return; }
                default: Done(Value()); return;
                }
            }
        case K::Tpl:
            if (f.pc == 0) { f.key = *n->cooked[0]; f.idx = 0; f.pc = 1; }
            while (true) {
                if (f.pc == 2) {
                    Value v = co->res;
                    if (v.t == Value::Symbol) in.Throw(L"TypeError", L"Cannot convert a Symbol value to a string");
                    f.key += rt.ToStr(v);
                    if (f.idx < n->cooked.size()) f.key += *n->cooked[f.idx];
                    f.pc = 1;
                }
                if (f.idx >= n->list.size()) { Done(Value::S(std::move(f.key))); return; }
                Ast* x = n->list[f.idx++];
                f.pc = 2;
                if (!E(x, f.env)) return;
            }
        case K::Arr:
            while (true) {
                if (f.pc == 1) {
                    Ast* el = n->list[f.idx - 1];
                    if (el->k == K::Spread) {
                        std::vector<Value> items = rt.IterateToList(co->res);
                        f.vals.insert(f.vals.end(), items.begin(), items.end());
                    } else f.vals.push_back(co->res);
                    if (f.vals.size() > 50000000) in.Throw(L"RangeError", L"Invalid array length");
                    f.pc = 0;
                }
                if (f.idx >= n->list.size()) { Done(Value::O(rt.NewArrayFrom(f.vals))); return; }
                Ast* el = n->list[f.idx++];
                if (el->k == K::Hole) { f.vals.push_back(Value::Hole()); continue; }
                f.pc = 1;
                if (!E(el->k == K::Spread ? el->a : el, f.env)) return;
            }
        case K::ObjLit: return RunObjLit(f, n);
        case K::Member:
        case K::Index:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) {
                Value o = co->res;
                if (n->flag && o.IsNullish()) throw OptShort{};
                if (n->k == K::Member) { Done(rt.Get(o, *n->str)); return; }
                f.vals = {o};
                f.pc = 2;
                if (!E(n->b, f.env)) return;
            }
            { Value k = co->res; Value o = f.vals[0]; Done(rt.GetV(o, k)); }
            return;
        case K::Call: {
            Ast* c = n->a;
            while (true) {
                switch (f.pc) {
                case 0:
                    if (c->k == K::Member || c->k == K::Index) { f.pc = 1; if (!E(c->a, f.env)) return; continue; }
                    f.pc = 3;
                    if (!E(c, f.env)) return;
                    continue;
                case 1: {
                    Value o = co->res;
                    if (c->flag && o.IsNullish()) throw OptShort{};
                    if (c->k == K::Member) { f.vals = {rt.Get(o, *c->str), o}; f.pc = 4; continue; }
                    f.vals = {Value(), o};
                    f.pc = 2;
                    if (!E(c->b, f.env)) return;
                    continue;
                }
                case 2: f.vals[0] = rt.GetV(f.vals[1], co->res); f.pc = 4; continue;
                case 3: f.vals = {co->res, Value()}; f.pc = 4; continue;
                case 4:
                    if (n->flag && f.vals[0].IsNullish()) throw OptShort{};
                    f.idx = 0;
                    f.pc = 10;
                    continue;
                default: {
                    if (!Args(f, n->list, 10)) return;
                    Value fn = f.vals[0], thisV = f.vals[1];
                    if (!fn.IsObj() || !fn.o->IsCallable()) in.Throw(L"TypeError", in.Describe(c) + L" is not a function");
                    std::vector<Value> args(f.vals.begin() + 2, f.vals.end());
                    Done(rt.Call(fn, thisV, args));
                    return;
                }
                }
            }
        }
        case K::New:
            if (f.pc == 0) { f.pc = 1; if (!E(n->a, f.env)) return; }
            if (f.pc == 1) { f.vals = {co->res, Value()}; f.idx = 0; f.pc = 10; }
            {
                if (!Args(f, n->list, 10)) return;
                Value fn = f.vals[0];
                if (!fn.IsObj() || !fn.o->IsCallable()) in.Throw(L"TypeError", in.Describe(n->a) + L" is not a constructor");
                std::vector<Value> args(f.vals.begin() + 2, f.vals.end());
                Done(rt.Construct(fn, args));
            }
            return;
        case K::Assign: return RunAssign(f, n);
        default:
            in.Throw(L"SyntaxError", L"Unexpected expression");
        }
    }

    void RunYieldStar(CoFrame& f, Ast* n) {
        // vals: [0] ic yineleyici, [1] next; rkind 1: async ic yineleyici; idx: istek turu
        while (true) {
            switch (f.pc) {
            case 0:
                f.pc = 1;
                if (!E(n->a, f.env)) return;
                // fallthrough
            case 1: {
                Value it = co->res;
                if (it.IsNullish()) in.Throw(L"TypeError", rt.ToStr(it) + L" is not iterable");
                Value fn;
                f.rkind = 0;
                if (co->kind == Coro::AsyncGen) {
                    fn = rt.Get(it, L"\x01Symbol.asyncIterator");
                    if (fn.IsObj() && fn.o->IsCallable()) f.rkind = 1;
                }
                if (!f.rkind) fn = rt.Get(it, L"\x01Symbol.iterator");
                if (!fn.IsObj() || !fn.o->IsCallable()) in.Throw(L"TypeError", in.Describe(n->a) + L" is not iterable");
                std::vector<Value> none;
                Value iter = rt.Call(fn, it, none);
                f.vals = {iter, rt.Get(iter, L"next")};
                f.idx = 0;
                co->res = Value();
                f.pc = 2;
            }
                // fallthrough
            case 2: {
                Value received = co->res;
                Value iter = f.vals[0];
                Value r;
                std::vector<Value> a{received};
                if (f.idx == 0) r = rt.Call(f.vals[1], iter, a);
                else if (f.idx == 1) {
                    Value t = rt.Get(iter, L"throw");
                    if (!t.IsObj() || !t.o->IsCallable()) {
                        try { CloseIter(iter); } catch (JSThrow&) {}
                        in.Throw(L"TypeError", L"The iterator does not provide a 'throw' method");
                    }
                    r = rt.Call(t, iter, a);
                } else {
                    Value rf = rt.Get(iter, L"return");
                    if (!rf.IsObj() || !rf.o->IsCallable()) { if (InjectReturn(received)) return; return; }
                    r = rt.Call(rf, iter, a);
                }
                if (f.rkind == 1) { AwaitValue(r, 3); return; }
                co->res = r;
                f.pc = 3;
            }
                // fallthrough
            case 3: {
                Value r = co->res;
                if (!r.IsObj()) in.Throw(L"TypeError", L"Iterator result is not an object");
                if (rt.Truthy(rt.Get(r, L"done"))) {
                    Value v = rt.Get(r, L"value");
                    if (f.idx == 2) { InjectReturn(v); return; }
                    Done(v);
                    return;
                }
                YieldValue(rt.Get(r, L"value"), 4);
                return;
            }
            case 4:
                // Resume() istek turunu f.idx'e, degeri co->res'e koydu
                f.pc = 2;
                continue;
            default:
                Done(Value());
                return;
            }
        }
    }

    void RunObjLit(CoFrame& f, Ast* n) {
        if (f.pc == 0) { f.vals = {Value::O(rt.NewObject())}; f.idx = 0; f.pc = 1; }
        Obj* o = f.vals[0].o;
        while (true) {
            if (f.pc == 2) {
                Ast* p = n->list[f.idx - 1];
                Value v = co->res;
                if (p->k == K::Spread) {
                    if (v.IsObj()) for (auto& k : rt.OwnKeys(v.o, true)) rt.DefineData(o, k, rt.Get(v, k));
                    else if (v.IsStr()) for (size_t i = 0; i < v.s->size(); i++) rt.DefineData(o, std::to_wstring(i), Value::S(std::wstring(1, (*v.s)[i])));
                } else if (!p->flag && !p->isStatic && f.key == L"__proto__") {
                    if (v.IsObj()) o->proto = v.o;
                    else if (v.t == Value::Null) o->proto = nullptr;
                } else {
                    In::NameFn(v, f.key);
                    rt.DefineData(o, f.key, v);
                }
                f.pc = 1;
            }
            if (f.idx >= n->list.size()) { Done(f.vals[0]); return; }
            Ast* p = n->list[f.idx++];
            if (p->k == K::Spread) {
                f.pc = 2;
                if (!E(p->a, f.env)) return;
                continue;
            }
            std::wstring key = p->flag ? rt.KeyOf(in.Eval(p->a, f.env)) : *p->a->str;
            if (p->kind == 1 || p->kind == 2) {
                Obj* fn = in.MakeClosure(p->b, f.env);
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
                Obj* fn = in.MakeClosure(p->b, f.env);
                fn->x->home = o;
                fn->x->name = key;
                rt.DefineData(o, key, Value::O(fn));
                continue;
            }
            f.key = key;
            f.pc = 2;
            if (!E(p->b, f.env)) return;
        }
    }

    void RunAssign(CoFrame& f, Ast* n) {
        Ast* t = n->a;
        if (n->op == O_None) {
            if (f.pc == 0) {
                if (t->k == K::Member) f.vals = {t->a->k == K::Super ? in.ThisOf(f.env) : in.Eval(t->a, f.env)};
                else if (t->k == K::Index) { Value o = in.Eval(t->a, f.env); Value k = in.Eval(t->b, f.env); f.vals = {o, k}; }
                f.pc = 1;
                if (!E(n->b, f.env)) return;
            }
            Value v = co->res;
            switch (t->k) {
            case K::ObjPat: case K::ArrPat: in.Bind(t, v, f.env, B_Assign); break;
            case K::Ident: In::NameFn(v, *t->str); in.SetVar(NodeAtom(t), v, f.env); break;
            case K::Member: rt.Put(f.vals[0], *t->str, v); break;
            case K::Index: rt.PutV(f.vals[0], f.vals[1], v); break;
            default: in.Throw(L"SyntaxError", L"Invalid assignment target");
            }
            Done(v);
            return;
        }
        if (f.pc == 0) {
            In::Ref r = in.MakeRef(t, f.env);
            Value cur = in.RefGet(r, f.env);
            f.rkind = r.kind;
            f.rname = r.name;
            f.key = r.key;
            f.vals = {r.base, r.keyV, cur};
            if (n->op == O_And || n->op == O_Or || n->op == O_Nullish) {
                bool assign = n->op == O_And ? rt.Truthy(cur) : n->op == O_Or ? !rt.Truthy(cur) : cur.IsNullish();
                if (!assign) { Done(cur); return; }
            }
            f.pc = 1;
            if (!E(n->b, f.env)) return;
        }
        In::Ref r;
        r.kind = f.rkind;
        r.name = f.rname;
        r.key = f.key;
        r.base = f.vals[0];
        r.keyV = f.vals[1];
        Value v = (n->op == O_And || n->op == O_Or || n->op == O_Nullish) ? co->res : in.BinOp(n->op, f.vals[2], co->res);
        in.RefPut(r, v, f.env);
        Done(v);
    }
};

// ---------- Surucu: coroutine'i calistir ve sonucu turune gore isle ----------
struct CoDriver {
    Runtime& rt;
    In& in;

    void Drive(Coro* co, int mode, const Value& v) {
        bool fresh = co->state == Coro::Start;
        rt.CheckStack();
        if (rt.depth + 1 > rt.maxDepth) in.Throw(L"RangeError", L"Maximum call stack size exceeded");
        ++rt.depth;
        co->state = Coro::Running;
        co->outcome = Coro::OutNone;
        struct DG { int& d; int& c; ~DG() { d--; c--; } } dg{rt.depth, rt.coroDepth};
        rt.coroDepth++;
        CallFrameGuard cfg(rt, co->fn && co->fn->x && co->fn->x->fn ? co->fn->x->fn->fi : nullptr);
        Machine m{rt, in, co};
        try {
            m.Resume(fresh, mode, v);
        } catch (...) {
            // JSAbort (sinir asimi) vb.: coroutine kullanilamaz hale gelir
            co->frames.clear();
            co->state = Coro::Done;
            throw;
        }
    }

    // Async uretec: kuyruktaki en ondeki istegi sonuca gore kapat
    void SettleFront(Coro* co) {
        if (co->queue.empty()) return;
        CoReq q = co->queue.front();
        co->queue.erase(co->queue.begin());
        if (co->outcome == Coro::OutYield) rt.ResolvePromise(q.promise, Value::O(IterResult(rt, co->outVal, false)));
        else if (co->outcome == Coro::OutReturn) rt.ResolvePromise(q.promise, Value::O(IterResult(rt, co->outVal, true)));
        else if (co->outcome == Coro::OutThrow) rt.RejectPromise(q.promise, co->outVal);
    }

    void DrainAsyncGen(Coro* co) {
        while (!co->queue.empty()) {
            if (co->state == Coro::Running || co->state == Coro::SuspAwait) return;
            CoReq& r = co->queue.front();
            if (co->state == Coro::Start && r.mode != 0) { co->state = Coro::Done; co->frames.clear(); }
            if (co->state == Coro::Done) {
                CoReq q = r;
                co->queue.erase(co->queue.begin());
                if (q.mode == 1) rt.RejectPromise(q.promise, q.v);
                else rt.ResolvePromise(q.promise, Value::O(IterResult(rt, q.mode == 2 ? q.v : Value(), true)));
                continue;
            }
            int mode = r.mode;
            Value v = r.v;
            Drive(co, mode, v);
            if (co->state == Coro::SuspAwait) return;
            SettleFront(co);
        }
    }

    // await sonrasi (ya da ilk calismadan sonra) sonucu isle
    void After(Coro* co) {
        if (co->state == Coro::SuspAwait) return;
        if (co->kind == Coro::Async) {
            if (co->outcome == Coro::OutReturn) rt.ResolvePromise(co->promise, co->outVal);
            else if (co->outcome == Coro::OutThrow) rt.RejectPromise(co->promise, co->outVal);
            co->outVal = Value();
        } else if (co->kind == Coro::AsyncGen) {
            SettleFront(co);
            DrainAsyncGen(co);
        }
    }
};

Value CoroAwaitResume(Runtime& rt, Obj* self, const Value&, std::vector<Value>& args) {
    Obj* holder = self->x->slots[0].o;
    int mode = int(self->x->slots[1].n);
    Coro* co = holder && holder->x ? holder->x->coro : nullptr;
    if (!co || co->state != Coro::SuspAwait) return Value();
    In in{rt};
    CoDriver d{rt, in};
    d.Drive(co, mode, args.empty() ? Value() : args[0]);
    d.After(co);
    return Value();
}

Value In::StartCoroutine(Obj* f, Env* fenv) {
    FuncInfo* fi = f->x->fn->fi;
    Coro* co = rt.Alloc<Coro>();
    co->kind = fi->isGen ? (fi->isAsync ? Coro::AsyncGen : Coro::Gen) : Coro::Async;
    co->fenv = fenv;
    co->fn = f;
    co->frames.emplace_back();
    CoFrame& root = co->frames.back();
    root.node = f->x->fn;
    root.env = fenv;
    root.stmt = true;
    Obj* holder;
    if (co->kind == Coro::Gen) holder = rt.NewObject(rt.generatorProto);
    else if (co->kind == Coro::AsyncGen) holder = rt.NewObject(rt.asyncGeneratorProto);
    else holder = rt.NewObject(rt.objectProto);
    // Uretec fonksiyonunun kendi .prototype'i varsa (kullanici eklediyse) onu kullan
    if (co->kind != Coro::Async) {
        Prop* pp = f->props.Find(L"prototype");
        if (pp && !pp->accessor && pp->v.IsObj()) holder->proto = pp->v.o;
    }
    holder->X().coro = co;
    co->holder = holder;
    if (co->kind != Coro::Async) return Value::O(holder);
    co->promise = rt.NewPromise();
    Obj* p = co->promise;
    CoDriver d{rt, *this};
    d.Drive(co, 0, Value());
    d.After(co);
    return Value::O(p);
}

Coro* GenCoro(Runtime& rt, const Value& thisV, uint8_t kind, const wchar_t* what) {
    Coro* co = thisV.IsObj() && thisV.o->x ? thisV.o->x->coro : nullptr;
    if (!co || co->kind != kind) rt.Throw(L"TypeError", std::wstring(what) + L" method called on incompatible receiver");
    return co;
}

#define CO_NATIVE(name) Value name(Runtime& rt, Obj*, const Value& thisV, std::vector<Value>& args)
CO_NATIVE(GenP_next) { return rt.GeneratorResume(GenCoro(rt, thisV, Coro::Gen, L"Generator.prototype.next")->holder, 0, args.empty() ? Value() : args[0]); }
CO_NATIVE(GenP_throw) { return rt.GeneratorResume(GenCoro(rt, thisV, Coro::Gen, L"Generator.prototype.throw")->holder, 1, args.empty() ? Value() : args[0]); }
CO_NATIVE(GenP_return) { return rt.GeneratorResume(GenCoro(rt, thisV, Coro::Gen, L"Generator.prototype.return")->holder, 2, args.empty() ? Value() : args[0]); }
CO_NATIVE(AGenP_next) { return rt.AsyncGeneratorEnqueue(thisV.IsObj() ? thisV.o : nullptr, 0, args.empty() ? Value() : args[0]); }
CO_NATIVE(AGenP_throw) { return rt.AsyncGeneratorEnqueue(thisV.IsObj() ? thisV.o : nullptr, 1, args.empty() ? Value() : args[0]); }
CO_NATIVE(AGenP_return) { return rt.AsyncGeneratorEnqueue(thisV.IsObj() ? thisV.o : nullptr, 2, args.empty() ? Value() : args[0]); }
CO_NATIVE(AGenP_self) { (void)rt; (void)args; return thisV; }
#undef CO_NATIVE

} // namespace

// ---------------- Runtime koprusu ----------------

Value Runtime::EvalAst(Ast* e, Env* env) {
    In in{*this};
    struct FG { Runtime& r; const std::wstring* f; ~FG() { r.curFile = f; } } fg{*this, curFile};
    if (e->fi && e->fi->file) curFile = e->fi->file;
    if (e->k == K::Program) return in.Exec(e, env, nullptr).v;
    return in.Eval(e, env);
}

Value Runtime::GeneratorResume(Obj* gen, int mode, const Value& v) {
    Coro* co = gen && gen->x ? gen->x->coro : nullptr;
    if (!co || co->kind != Coro::Gen) Throw(L"TypeError", L"not a generator");
    if (co->state == Coro::Running) Throw(L"TypeError", L"Generator is already running");
    if (co->state == Coro::Start && mode != 0) { co->state = Coro::Done; co->frames.clear(); }
    if (co->state == Coro::Done) {
        if (mode == 1) throw JSThrow{v};
        return Value::O(IterResult(*this, mode == 2 ? v : Value(), true));
    }
    In in{*this};
    CoDriver d{*this, in};
    d.Drive(co, mode, v);
    Value out = co->outVal;
    co->outVal = Value();
    if (co->outcome == Coro::OutYield) return Value::O(IterResult(*this, out, false));
    if (co->outcome == Coro::OutThrow) throw JSThrow{out};
    return Value::O(IterResult(*this, out, true));
}

Value Runtime::AsyncGeneratorEnqueue(Obj* gen, int mode, const Value& v) {
    Obj* p = NewPromise();
    Coro* co = gen && gen->x ? gen->x->coro : nullptr;
    if (!co || co->kind != Coro::AsyncGen) {
        RejectPromise(p, Value::O(NewError(L"TypeError", L"not an async generator")));
        return Value::O(p);
    }
    CoReq r;
    r.mode = mode;
    r.v = v;
    r.promise = p;
    co->queue.push_back(r);
    In in{*this};
    CoDriver d{*this, in};
    d.DrainAsyncGen(co);
    return Value::O(p);
}

void InstallCoroutines(Runtime& rt) {
    Obj* gp = rt.NewObject(rt.iteratorProto);
    rt.DefineMethod(gp, L"next", GenP_next, 1);
    rt.DefineMethod(gp, L"return", GenP_return, 1);
    rt.DefineMethod(gp, L"throw", GenP_throw, 1);
    rt.DefineData(gp, L"\x01Symbol.toStringTag", Value::S(L"Generator"), false);
    rt.generatorProto = gp;
    Obj* ap = rt.NewObject(rt.objectProto);
    rt.DefineMethod(ap, L"next", AGenP_next, 1);
    rt.DefineMethod(ap, L"return", AGenP_return, 1);
    rt.DefineMethod(ap, L"throw", AGenP_throw, 1);
    rt.DefineData(ap, L"\x01Symbol.asyncIterator", Value::O(rt.NewFunction(AGenP_self, L"[Symbol.asyncIterator]", 0)), false);
    rt.DefineData(ap, L"\x01Symbol.toStringTag", Value::S(L"AsyncGenerator"), false);
    rt.asyncGeneratorProto = ap;
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
    CountStep();
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
