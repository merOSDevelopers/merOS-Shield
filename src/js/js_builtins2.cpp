// merOS Shield - merJS : RegExp, String'in regex metotları, Date, URI kodlama, base64
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <list>
#include <regex>
#include "js_ast.h"
#include "../util.h"

namespace mer {
namespace js {

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

// ================= RegExp =================

struct Compiled {
    std::wregex re;
    std::vector<std::wstring> names; // grup indeksi -> ad (bos olabilir)
    size_t groups = 0;
};

constexpr size_t kMaxRegexInput = 4 * 1024 * 1024;

static std::wstring ExpandUnicodeProperty(const std::wstring& prop, bool negate) {
    std::wstring p = prop;
    if (p.rfind(L"General_Category=", 0) == 0) p = p.substr(17);
    else if (p.rfind(L"gc=", 0) == 0) p = p.substr(3);

    std::wstring body;
    if (p == L"L" || p == L"Letter") {
        body = L"a-zA-Z\u00C0-\u024F\u0370-\u03FF\u0400-\u04FF\u0590-\u05FF\u0600-\u06FF\u0900-\u097F\u4E00-\u9FFF\uAC00-\uD7AF";
    } else if (p == L"N" || p == L"Number") {
        body = L"0-9\u0660-\u0669\u06F0-\u06F9\u0966-\u096F\uFF10-\uFF19";
    } else if (p == L"P" || p == L"Punctuation") {
        body = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\u00A1\u00BF\u2010-\u2027\u2030-\u205E";
    } else if (p == L"S" || p == L"Symbol") {
        body = L"\\$\\+<=>^`|~¢£¤¥§©¬®°±µ¶·¸¹º»¼½¾¿×÷\u20AC-\u20CF";
    } else if (p == L"Z" || p == L"Separator" || p == L"White_Space" || p == L"space") {
        body = L" \\t\\r\\n\\v\\f\u00A0\u1680\u2000-\u200A\u2028\u2029\u202F\u205F\u3000";
    } else if (p == L"ASCII") {
        body = L"\\x00-\\x7F";
    } else if (p == L"Any") {
        body = L"\\s\\S";
    } else if (p == L"Emoji") {
        body = L"\u231A\u231B\u23E9-\u23EC\u23F0\u23F3\u25FD\u25FE\u2614\u2615\u2648-\u2653\u267F\u2693\u26A1\u26AA\u26AB\u26BD\u26BE\u26C4\u26C5\u26CE\u26D4\u26EA\u26F2\u26F3\u26F5\u26FA\u26FD\u2705\u270A\u270B\u2728\u274C\u274E\u2753-\u2755\u2757\u2795-\u2797\u27B0\u27BF\u2B1B\u2B1C\u2B50\u2B55";
    } else if (p == L"sc=Latin" || p == L"Script=Latin") {
        body = L"a-zA-Z\u00C0-\u024F";
    } else if (p == L"sc=Greek" || p == L"Script=Greek") {
        body = L"\u0370-\u03FF";
    } else if (p == L"sc=Cyrillic" || p == L"Script=Cyrillic") {
        body = L"\u0400-\u04FF";
    } else if (p == L"sc=Arabic" || p == L"Script=Arabic") {
        body = L"\u0600-\u06FF";
    } else if (p == L"sc=Hebrew" || p == L"Script=Hebrew") {
        body = L"\u0590-\u05FF";
    } else if (p == L"sc=Han" || p == L"Script=Han") {
        body = L"\u4E00-\u9FFF";
    } else if (p == L"sc=Hiragana" || p == L"Script=Hiragana") {
        body = L"\u3040-\u309F";
    } else if (p == L"sc=Katakana" || p == L"Script=Katakana") {
        body = L"\u30A0-\u30FF";
    } else if (p == L"sc=Hangul" || p == L"Script=Hangul") {
        body = L"\uAC00-\uD7AF";
    } else {
        body = L"a-zA-Z0-9";
    }
    if (negate) return L"[^" + body + L"]";
    return L"[" + body + L"]";
}

std::shared_ptr<Compiled> CompileRegex(Runtime& rt, const std::wstring& src, const std::wstring& flags) {
    static thread_local std::list<std::pair<std::wstring, std::shared_ptr<Compiled>>> cache;
    std::wstring key = flags + L"/" + src;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->first == key) {
            auto c = it->second;
            cache.splice(cache.begin(), cache, it);
            return c;
        }
    }
    for (wchar_t f : flags)
        if (std::wstring(L"dgimsuyv").find(f) == std::wstring::npos) rt.Throw(L"SyntaxError", L"Invalid regular expression flags");
    bool dotAll = flags.find(L's') != std::wstring::npos;
    bool unicode = flags.find(L'u') != std::wstring::npos || flags.find(L'v') != std::wstring::npos;
    auto c = std::make_shared<Compiled>();
    std::vector<std::wstring> names{L""};
    std::wstring out;
    bool inClass = false;
    for (size_t i = 0; i < src.size(); i++) {
        wchar_t ch = src[i];
        if (ch == L'\\' && i + 1 < src.size()) {
            wchar_t n = src[i + 1];
            if (n == L'k' && i + 2 < src.size() && src[i + 2] == L'<') {
                size_t e = src.find(L'>', i + 3);
                if (e == std::wstring::npos) rt.Throw(L"SyntaxError", L"Invalid named reference");
                std::wstring nm = src.substr(i + 3, e - i - 3);
                size_t idx = 0;
                for (size_t k = 1; k < names.size(); k++) if (names[k] == nm) idx = k;
                if (!idx) rt.Throw(L"SyntaxError", L"Invalid named capture referenced");
                out += L"\\" + std::to_wstring(idx);
                i = e;
                continue;
            }
            if (n == L'u' && i + 2 < src.size() && src[i + 2] == L'{' && unicode) {
                size_t e = src.find(L'}', i + 3);
                if (e == std::wstring::npos) rt.Throw(L"SyntaxError", L"Invalid Unicode escape");
                unsigned long cp = wcstoul(src.substr(i + 3, e - i - 3).c_str(), nullptr, 16);
                wchar_t buf[16];
                if (cp < 0x10000) { swprintf(buf, 16, L"\\u%04lx", cp); out += buf; }
                else if (sizeof(wchar_t) == 2) {
                    cp -= 0x10000;
                    swprintf(buf, 16, L"\\u%04lx", 0xD800 + (cp >> 10)); out += buf;
                    swprintf(buf, 16, L"\\u%04lx", 0xDC00 + (cp & 0x3FF)); out += buf;
                } else out += wchar_t(cp);
                i = e;
                continue;
            }
            if ((n == L'p' || n == L'P') && i + 2 < src.size() && src[i + 2] == L'{') {
                size_t e = src.find(L'}', i + 3);
                if (e == std::wstring::npos) rt.Throw(L"SyntaxError", L"Invalid Unicode property escape");
                std::wstring prop = src.substr(i + 3, e - i - 3);
                bool neg = (n == L'P');
                std::wstring exp = ExpandUnicodeProperty(prop, neg);
                if (inClass) {
                    if (!neg && exp.size() >= 2 && exp.front() == L'[' && exp.back() == L']') {
                        out += exp.substr(1, exp.size() - 2);
                    } else {
                        out += exp;
                    }
                } else {
                    out += exp;
                }
                i = e;
                continue;
            }
            if (n == L'0' && (i + 2 >= src.size() || src[i + 2] < L'0' || src[i + 2] > L'9')) {
                out += L"\\x00";
                i++;
                continue;
            }
            if (n == L'/') { out += L'/'; i++; continue; }
            if (n == L'd' && inClass) { out += L"0-9"; i++; continue; }
            out += ch;
            out += n;
            i++;
            continue;
        }
        if (inClass) {
            if (ch == L']') inClass = false;
            out += ch;
            continue;
        }
        if (ch == L'[') {
            if (i + 2 < src.size() && src[i + 1] == L'^' && src[i + 2] == L']') { out += L"[\\s\\S]"; i += 2; continue; }
            if (i + 1 < src.size() && src[i + 1] == L']') { out += L"(?!)"; i += 1; continue; }
            inClass = true;
            out += ch;
            continue;
        }
        if (ch == L'(' && i + 1 < src.size() && src[i + 1] == L'?') {
            if (i + 2 < src.size() && src[i + 2] == L'<') {
                if (i + 3 < src.size() && (src[i + 3] == L'=' || src[i + 3] == L'!')) rt.Throw(L"SyntaxError", L"Lookbehind assertions are not supported");
                size_t e = src.find(L'>', i + 3);
                if (e == std::wstring::npos) rt.Throw(L"SyntaxError", L"Invalid capture group name");
                names.push_back(src.substr(i + 3, e - i - 3));
                out += L'(';
                i = e;
                continue;
            }
            out += ch;
            continue;
        }
        if (ch == L'(') names.push_back(L"");
        if (ch == L'.' && dotAll) { out += L"[\\s\\S]"; continue; }
        out += ch;
    }
    auto fl = std::regex_constants::ECMAScript;
    if (flags.find(L'i') != std::wstring::npos) fl |= std::regex_constants::icase;
    try {
        c->re.assign(out, fl);
    } catch (std::regex_error&) {
        rt.Throw(L"SyntaxError", L"Invalid regular expression: /" + src + L"/");
    }
    c->groups = c->re.mark_count();
    c->names = names;
    c->names.resize(c->groups + 1);
    cache.emplace_front(key, c);
    if (cache.size() > 256) cache.pop_back();
    return c;
}

Obj* ThisRe(Runtime& rt, const Value& v) {
    if (v.IsObj() && v.o->cls == Cls::RegExp) return v.o;
    rt.Throw(L"TypeError", L"RegExp method called on incompatible receiver");
}

Compiled& Comp(Obj* re) { return *static_cast<Compiled*>(re->x->re.get()); }

// Tek eslesme: basariyla esleserse dizi, yoksa null
Value RegExec(Runtime& rt, Obj* re, const std::wstring& s) {
    const std::wstring& fl = re->x->flags;
    bool global = fl.find(L'g') != std::wstring::npos, sticky = fl.find(L'y') != std::wstring::npos;
    if (s.size() > kMaxRegexInput) rt.Throw(L"RangeError", L"merJS: regex girdisi çok uzun");
    double last = (global || sticky) ? ToInteger(rt, rt.Get(Value::O(re), L"lastIndex")) : 0;
    if (last < 0) last = 0;
    if (last > double(s.size())) { if (global || sticky) rt.Put(Value::O(re), L"lastIndex", Value::N(0)); return Value::Null_(); }
    Compiled& c = Comp(re);
    std::wsmatch m;
    auto start = s.begin() + long(last);
    auto mf = std::regex_constants::match_default;
    if (last > 0) mf |= std::regex_constants::match_prev_avail;
    if (sticky) mf |= std::regex_constants::match_continuous;
    bool found;
    try {
        found = std::regex_search(start, s.end(), m, c.re, mf);
    } catch (std::regex_error&) {
        rt.Throw(L"RangeError", L"merJS: regex çok karmaşık");
    }
    rt.steps += s.size() / 64 + 1;
    if (!found) {
        if (global || sticky) rt.Put(Value::O(re), L"lastIndex", Value::N(0));
        return Value::Null_();
    }
    double index = last + double(m.position(0));
    Obj* arr = rt.NewArray(0);
    for (size_t g = 0; g < m.size(); g++) arr->elems.push_back(m[g].matched ? Value::S(m[g].str()) : Value());
    rt.DefineData(arr, L"index", Value::N(index));
    rt.DefineData(arr, L"input", Value::S(s));
    bool anyName = false;
    for (auto& n : c.names) if (!n.empty()) anyName = true;
    if (anyName) {
        Obj* gr = rt.NewObject();
        for (size_t g = 1; g < c.names.size() && g < m.size(); g++)
            if (!c.names[g].empty()) rt.DefineData(gr, c.names[g], m[g].matched ? Value::S(m[g].str()) : Value());
        rt.DefineData(arr, L"groups", Value::O(gr));
    } else rt.DefineData(arr, L"groups", Value());
    if (global || sticky) rt.Put(Value::O(re), L"lastIndex", Value::N(index + double(m.length(0))));
    return Value::O(arr);
}

NATIVE(RegExp_ctor) {
    UNUSED;
    std::wstring src, flags;
    if (A(0).IsObj() && A(0).o->cls == Cls::RegExp) {
        src = A(0).o->x->str;
        flags = A(1).IsUndef() ? A(0).o->x->flags : rt.ToStr(A(1));
        if (!rt.nativeNewTarget && A(1).IsUndef()) return A(0);
    } else {
        src = A(0).IsUndef() ? L"(?:)" : rt.ToStr(A(0));
        flags = A(1).IsUndef() ? L"" : rt.ToStr(A(1));
    }
    return Value::O(rt.NewRegExp(src, flags));
}
NATIVE(RegExpP_exec) { UNUSED; return RegExec(rt, ThisRe(rt, thisV), rt.ToStr(A(0))); }
NATIVE(RegExpP_test) { UNUSED; return Value::B(!RegExec(rt, ThisRe(rt, thisV), rt.ToStr(A(0))).IsNullish()); }
NATIVE(RegExpP_toString) { UNUSED; Obj* r = ThisRe(rt, thisV); return Value::S(L"/" + r->x->str + L"/" + r->x->flags); }
NATIVE(RegExpP_source) { UNUSED; if (!thisV.IsObj() || thisV.o->cls != Cls::RegExp) return Value::S(L"(?:)"); return Value::S(thisV.o->x->str); }
NATIVE(RegExpP_flags) { UNUSED; if (!thisV.IsObj() || thisV.o->cls != Cls::RegExp) return Value::S(L""); return Value::S(thisV.o->x->flags); }
Value FlagGet(Runtime& rt, const Value& thisV, wchar_t f) { (void)rt; return Value::B(thisV.IsObj() && thisV.o->cls == Cls::RegExp && thisV.o->x->flags.find(f) != std::wstring::npos); }
NATIVE(RegExpP_global) { UNUSED; return FlagGet(rt, thisV, L'g'); }
NATIVE(RegExpP_ignoreCase) { UNUSED; return FlagGet(rt, thisV, L'i'); }
NATIVE(RegExpP_multiline) { UNUSED; return FlagGet(rt, thisV, L'm'); }
NATIVE(RegExpP_sticky) { UNUSED; return FlagGet(rt, thisV, L'y'); }
NATIVE(RegExpP_unicode) { UNUSED; return FlagGet(rt, thisV, L'u'); }
NATIVE(RegExpP_dotAll) { UNUSED; return FlagGet(rt, thisV, L's'); }

std::wstring StrThis(Runtime& rt, const Value& thisV) {
    if (thisV.IsNullish()) rt.Throw(L"TypeError", L"String.prototype method called on null or undefined");
    return rt.ToStr(thisV);
}
bool IsRe(const Value& v) { return v.IsObj() && v.o->cls == Cls::RegExp; }
bool HasG(Obj* re) { return re->x->flags.find(L'g') != std::wstring::npos; }

void AdvanceEmpty(Runtime& rt, Obj* re, const Value& m, const std::wstring& s) {
    if (rt.ToStr(rt.GetV(m, Value::N(0))).empty()) {
        double li = ToInteger(rt, rt.Get(Value::O(re), L"lastIndex"));
        rt.Put(Value::O(re), L"lastIndex", Value::N(li + 1));
    }
    (void)s;
}

NATIVE(StringP_match) {
    UNUSED;
    std::wstring s = StrThis(rt, thisV);
    Obj* re = IsRe(A(0)) ? A(0).o : rt.NewRegExp(A(0).IsUndef() ? L"(?:)" : rt.ToStr(A(0)), L"");
    if (!HasG(re)) return RegExec(rt, re, s);
    rt.Put(Value::O(re), L"lastIndex", Value::N(0));
    std::vector<Value> all;
    while (all.size() < 10000000) {
        Value m = RegExec(rt, re, s);
        if (m.IsNullish()) break;
        all.push_back(rt.GetV(m, Value::N(0)));
        AdvanceEmpty(rt, re, m, s);
    }
    if (all.empty()) return Value::Null_();
    return Value::O(rt.NewArrayFrom(all));
}
NATIVE(StringP_matchAll) {
    UNUSED;
    std::wstring s = StrThis(rt, thisV);
    Obj* re = nullptr;
    if (IsRe(A(0))) {
        if (!HasG(A(0).o)) rt.Throw(L"TypeError", L"String.prototype.matchAll called with a non-global RegExp argument");
        re = rt.NewRegExp(A(0).o->x->str, A(0).o->x->flags);
    } else re = rt.NewRegExp(rt.ToStr(A(0)), L"g");
    std::vector<Value> all;
    while (all.size() < 10000000) {
        Value m = RegExec(rt, re, s);
        if (m.IsNullish()) break;
        all.push_back(m);
        AdvanceEmpty(rt, re, m, s);
    }
    return Value::O(rt.MakeIteratorFromList(all));
}
NATIVE(StringP_search) {
    UNUSED;
    std::wstring s = StrThis(rt, thisV);
    Obj* re = IsRe(A(0)) ? rt.NewRegExp(A(0).o->x->str, L"") : rt.NewRegExp(rt.ToStr(A(0)), L"");
    Value m = RegExec(rt, re, s);
    return m.IsNullish() ? Value::N(-1) : rt.Get(m, L"index");
}

std::wstring ExpandReplacement(Runtime& rt, const std::wstring& rep, const std::wstring& s, size_t pos, const std::wstring& matched,
                               const std::vector<Value>& caps, const Value& groups) {
    std::wstring o;
    for (size_t i = 0; i < rep.size(); i++) {
        wchar_t c = rep[i];
        if (c != L'$' || i + 1 >= rep.size()) { o += c; continue; }
        wchar_t n = rep[i + 1];
        if (n == L'$') { o += L'$'; i++; }
        else if (n == L'&') { o += matched; i++; }
        else if (n == L'`') { o += s.substr(0, pos); i++; }
        else if (n == L'\'') { o += s.substr(std::min(s.size(), pos + matched.size())); i++; }
        else if (n >= L'0' && n <= L'9') {
            size_t idx = size_t(n - L'0');
            size_t used = 1;
            if (i + 2 < rep.size() && rep[i + 2] >= L'0' && rep[i + 2] <= L'9') {
                size_t two = idx * 10 + size_t(rep[i + 2] - L'0');
                if (two >= 1 && two <= caps.size()) { idx = two; used = 2; }
            }
            if (idx >= 1 && idx <= caps.size()) {
                if (!caps[idx - 1].IsUndef()) o += rt.ToStr(caps[idx - 1]);
                i += used;
            } else o += c;
        } else if (n == L'<' && !groups.IsNullish()) {
            size_t e = rep.find(L'>', i + 2);
            if (e == std::wstring::npos) { o += c; continue; }
            Value g = rt.Get(groups, rep.substr(i + 2, e - i - 2));
            if (!g.IsUndef()) o += rt.ToStr(g);
            i = e;
        } else o += c;
    }
    return o;
}

Value DoReplace(Runtime& rt, const Value& thisV, std::vector<Value>& args, bool all) {
    std::wstring s = StrThis(rt, thisV);
    Value pat = A(0), rep = A(1);
    bool fn = IsCallable(rep);
    std::wstring repStr = fn ? L"" : rt.ToStr(rep);
    std::wstring out;
    if (IsRe(pat)) {
        Obj* re = pat.o;
        bool global = HasG(re);
        if (all && !global) rt.Throw(L"TypeError", L"replaceAll must be called with a global RegExp");
        if (global) rt.Put(Value::O(re), L"lastIndex", Value::N(0));
        size_t last = 0;
        int guard = 0;
        while (guard++ < 10000000) {
            Value m = RegExec(rt, re, s);
            if (m.IsNullish()) break;
            size_t pos = size_t(rt.ToNum(rt.Get(m, L"index")));
            std::wstring matched = rt.ToStr(rt.GetV(m, Value::N(0)));
            std::vector<Value> caps(m.o->elems.begin() + 1, m.o->elems.end());
            Value groups = rt.Get(m, L"groups");
            if (pos < last) break;
            out += s.substr(last, pos - last);
            if (fn) {
                std::vector<Value> a{Value::S(matched)};
                a.insert(a.end(), caps.begin(), caps.end());
                a.push_back(Value::N(double(pos)));
                a.push_back(Value::S(s));
                if (!groups.IsUndef()) a.push_back(groups);
                out += rt.ToStr(rt.Call(rep, Value(), a));
            } else out += ExpandReplacement(rt, repStr, s, pos, matched, caps, groups);
            last = pos + matched.size();
            if (out.size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
            if (!global) break;
            if (matched.empty()) {
                double li = ToInteger(rt, rt.Get(Value::O(re), L"lastIndex"));
                rt.Put(Value::O(re), L"lastIndex", Value::N(li + 1));
            }
        }
        out += s.substr(std::min(last, s.size()));
        return Value::S(std::move(out));
    }
    std::wstring p = rt.ToStr(pat);
    size_t last = 0, pos = s.find(p);
    while (pos != std::wstring::npos) {
        out += s.substr(last, pos - last);
        if (fn) {
            std::vector<Value> a{Value::S(p), Value::N(double(pos)), Value::S(s)};
            out += rt.ToStr(rt.Call(rep, Value(), a));
        } else out += ExpandReplacement(rt, repStr, s, pos, p, {}, Value());
        last = pos + p.size();
        if (!all) break;
        pos = s.find(p, p.empty() ? last + 1 : last);
        if (p.empty() && last >= s.size()) break;
        if (out.size() > 256u * 1024 * 1024) throw JSAbort{L"dize çok büyük"};
    }
    out += s.substr(std::min(last, s.size()));
    return Value::S(std::move(out));
}
NATIVE(StringP_replace) { UNUSED; return DoReplace(rt, thisV, args, false); }
NATIVE(StringP_replaceAll) { UNUSED; return DoReplace(rt, thisV, args, true); }

NATIVE(StringP_split) {
    UNUSED;
    std::wstring s = StrThis(rt, thisV);
    double lim = A(1).IsUndef() ? 4294967295.0 : double(uint32_t(int64_t(rt.ToNum(A(1)))));
    std::vector<Value> out;
    if (lim == 0) return Value::O(rt.NewArray(0));
    if (A(0).IsUndef()) { out.push_back(Value::S(s)); return Value::O(rt.NewArrayFrom(out)); }
    if (IsRe(A(0))) {
        Obj* re = rt.NewRegExp(A(0).o->x->str, A(0).o->x->flags + (HasG(A(0).o) ? L"" : L"g"));
        if (s.empty()) {
            Value m = RegExec(rt, re, s);
            if (m.IsNullish()) out.push_back(Value::S(s));
            return Value::O(rt.NewArrayFrom(out));
        }
        size_t last = 0;
        rt.Put(Value::O(re), L"lastIndex", Value::N(0));
        while (out.size() < lim) {
            Value m = RegExec(rt, re, s);
            if (m.IsNullish()) break;
            size_t pos = size_t(rt.ToNum(rt.Get(m, L"index")));
            std::wstring matched = rt.ToStr(rt.GetV(m, Value::N(0)));
            if (matched.empty()) {
                if (pos >= s.size()) break;
                if (pos == last) { rt.Put(Value::O(re), L"lastIndex", Value::N(double(pos + 1))); if (pos == 0) continue; }
            }
            if (pos + matched.size() > s.size()) break;
            if (matched.empty() && pos == last && pos != 0) { }
            out.push_back(Value::S(s.substr(last, pos - last)));
            for (size_t g = 1; g < m.o->elems.size() && out.size() < lim; g++) out.push_back(m.o->elems[g]);
            last = pos + matched.size();
            if (matched.empty()) rt.Put(Value::O(re), L"lastIndex", Value::N(double(pos + 1)));
        }
        if (out.size() < lim) out.push_back(Value::S(s.substr(std::min(last, s.size()))));
        return Value::O(rt.NewArrayFrom(out));
    }
    std::wstring sep = rt.ToStr(A(0));
    if (sep.empty()) {
        for (size_t i = 0; i < s.size() && out.size() < lim; i++) out.push_back(Value::S(std::wstring(1, s[i])));
        return Value::O(rt.NewArrayFrom(out));
    }
    size_t last = 0, pos;
    while ((pos = s.find(sep, last)) != std::wstring::npos && out.size() < lim) {
        out.push_back(Value::S(s.substr(last, pos - last)));
        last = pos + sep.size();
    }
    if (out.size() < lim) out.push_back(Value::S(s.substr(last)));
    return Value::O(rt.NewArrayFrom(out));
}

// ================= Date =================

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
double MakeTime(double y, double mon, double d, double h, double mi, double s, double ms) {
    if (!std::isfinite(y) || !std::isfinite(mon) || !std::isfinite(d) || !std::isfinite(h) || !std::isfinite(mi) || !std::isfinite(s) || !std::isfinite(ms)) return NAN;
    double ym = y + std::floor(mon / 12);
    double mn = std::fmod(mon, 12);
    if (mn < 0) mn += 12;
    if (std::fabs(ym) > 400000) return NAN;
    double days = double(DaysFromCivil(int64_t(ym), unsigned(mn) + 1, 1)) + d - 1;
    double t = days * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
    if (std::fabs(t) > 8.64e15) return NAN;
    return t;
}
struct Parts { double y, mon, d, h, mi, s, ms, wd; };
Parts Split(double t) {
    Parts p;
    double days = std::floor(t / 86400000.0);
    double rem = t - days * 86400000.0;
    int64_t y;
    unsigned m, d;
    CivilFromDays(int64_t(days), y, m, d);
    p.y = double(y); p.mon = m - 1; p.d = d;
    p.h = std::floor(rem / 3600000.0);
    rem -= p.h * 3600000.0;
    p.mi = std::floor(rem / 60000.0);
    rem -= p.mi * 60000.0;
    p.s = std::floor(rem / 1000.0);
    p.ms = rem - p.s * 1000.0;
    double wd = std::fmod(days + 4, 7);
    if (wd < 0) wd += 7;
    p.wd = wd;
    return p;
}
double LocalOffset(double t) {
    if (!std::isfinite(t)) return 0;
    double secs = std::floor(t / 1000.0);
    if (secs < -62135596800.0) secs = -62135596800.0;
    if (secs > 253402300799.0) secs = 253402300799.0;
    time_t tt = time_t(secs);
    struct tm lt {};
#ifdef _WIN32
    if (localtime_s(&lt, &tt) != 0) return 0;
#else
    if (!localtime_r(&tt, &lt)) return 0;
#endif
    double local = MakeTime(lt.tm_year + 1900, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, 0);
    return local - secs * 1000.0;
}
double ToLocal(double t) { return t + LocalOffset(t); }
double FromLocal(double lt) {
    double guess = lt - LocalOffset(lt);
    return lt - LocalOffset(guess);
}
double Clip(double t) { if (!std::isfinite(t) || std::fabs(t) > 8.64e15) return NAN; return std::trunc(t) + 0.0; }

double NowMs() {
    using namespace std::chrono;
    return double(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

double ParseDateInternal(const std::wstring& in) {
    std::wstring s = in;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(0, 1);
    // ISO 8601
    int Y = 0, M = 1, D = 1, h = 0, mi = 0, sec = 0, ms = 0;
    wchar_t tz[16] = {0};
    if (s.size() >= 4 && iswdigit(s[0]) && iswdigit(s[1]) && iswdigit(s[2]) && iswdigit(s[3]) && (s.size() == 4 || s[4] == L'-')) {
        size_t i = 0;
        auto num = [&](int digits, int& out) -> bool {
            int v = 0;
            for (int k = 0; k < digits; k++) { if (i >= s.size() || !iswdigit(s[i])) return false; v = v * 10 + (s[i++] - L'0'); }
            out = v;
            return true;
        };
        if (!num(4, Y)) return NAN;
        bool dateOnly = true;
        if (i < s.size() && s[i] == L'-') { i++; if (!num(2, M)) return NAN; }
        if (i < s.size() && s[i] == L'-') { i++; if (!num(2, D)) return NAN; }
        if (i < s.size() && (s[i] == L'T' || s[i] == L't' || s[i] == L' ')) {
            dateOnly = false;
            i++;
            if (!num(2, h)) return NAN;
            if (i < s.size() && s[i] == L':') { i++; if (!num(2, mi)) return NAN; }
            if (i < s.size() && s[i] == L':') { i++; if (!num(2, sec)) return NAN; }
            if (i < s.size() && (s[i] == L'.' || s[i] == L',')) {
                i++;
                int digits = 0, v = 0;
                while (i < s.size() && iswdigit(s[i])) { if (digits < 3) { v = v * 10 + (s[i] - L'0'); digits++; } i++; }
                while (digits < 3) { v *= 10; digits++; }
                ms = v;
            }
        }
        double t = MakeTime(Y, M - 1, D, h, mi, sec, ms);
        if (i < s.size()) {
            if (s[i] == L'Z' || s[i] == L'z') return t;
            if (s[i] == L'+' || s[i] == L'-') {
                int sign = s[i] == L'-' ? -1 : 1;
                i++;
                int oh = 0, om = 0;
                if (!num(2, oh)) return NAN;
                if (i < s.size() && s[i] == L':') i++;
                num(2, om);
                return t - sign * (oh * 3600000.0 + om * 60000.0);
            }
            return NAN;
        }
        return dateOnly ? t : FromLocal(t);
    }
    (void)tz;
    // "Thu, 15 Oct 2026 12:00:00 GMT" / "October 15, 2026 10:00" / "15 Oct 2026"
    static const wchar_t* mons[] = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun", L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    std::vector<std::wstring> toks;
    std::wstring cur;
    for (wchar_t c : s) {
        if (iswalnum(c) || c == L':' || c == L'+' || c == L'-') cur += c;
        else { if (!cur.empty()) toks.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) toks.push_back(cur);
    int month = -1, day = -1, year = -1;
    bool utc = false;
    double offset = 0;
    for (auto& t : toks) {
        std::wstring l = t;
        for (auto& c : l) c = wchar_t(towlower(c));
        if (l.find(L':') != std::wstring::npos) {
            {
                // HH:MM[:SS] (swscanf yerine elle: MSVC /sdl uyumlu)
                int parts[3] = {0, 0, 0}, pi = 0;
                for (wchar_t c : l) {
                    if (c == L':') { if (++pi > 2) break; }
                    else if (c >= L'0' && c <= L'9') parts[pi] = parts[pi] * 10 + (c - L'0');
                }
                h = parts[0]; mi = parts[1]; sec = parts[2];
            }
            continue;
        }
        if (l == L"gmt" || l == L"utc" || l == L"z") { utc = true; continue; }
        if ((l[0] == L'+' || l[0] == L'-') && l.size() == 5) {
            int v = _wtoi(l.c_str() + 1);
            offset = (l[0] == L'-' ? -1 : 1) * ((v / 100) * 3600000.0 + (v % 100) * 60000.0);
            utc = true;
            continue;
        }
        if (l.size() > 3 && (l.compare(0, 3, L"gmt") == 0 || l.compare(0, 3, L"utc") == 0)) {
            int v = _wtoi(l.c_str() + 4);
            offset = (l[3] == L'-' ? -1 : 1) * ((v / 100) * 3600000.0 + (v % 100) * 60000.0);
            utc = true;
            continue;
        }
        bool matched = false;
        for (int k = 0; k < 12; k++) if (l.compare(0, 3, mons[k]) == 0 && iswalpha(l[0])) { month = k; matched = true; break; }
        if (matched || iswalpha(l[0])) continue;
        int v = _wtoi(l.c_str());
        if (l.size() >= 3 || v > 31) year = v;
        else if (day < 0) day = v;
        else if (year < 0) year = v + (v < 50 ? 2000 : 1900);
    }
    if (month < 0 || day < 0 || year < 0) return NAN;
    double t = MakeTime(year, month, day, h, mi, sec, 0);
    return utc ? t - offset : FromLocal(t);
}

double ThisTime(Runtime& rt, const Value& thisV) {
    if (thisV.IsObj() && thisV.o->cls == Cls::Date) return thisV.o->x->num;
    rt.Throw(L"TypeError", L"this is not a Date object.");
}

const wchar_t* kDays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};

std::wstring TimeStr(double t) {
    Parts p = Split(ToLocal(t));
    double off = LocalOffset(t) / 60000.0;
    wchar_t buf[96];
    int oa = int(std::fabs(off));
    swprintf(buf, 96, L"%02d:%02d:%02d GMT%c%02d%02d", int(p.h), int(p.mi), int(p.s), off < 0 ? L'-' : L'+', oa / 60, oa % 60);
    return buf;
}
std::wstring DateStr(double t) {
    Parts p = Split(ToLocal(t));
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls %ls %02d %04d", kDays[int(p.wd)], kMonths[int(p.mon)], int(p.d), int(p.y));
    return buf;
}

NATIVE(Date_ctor) {
    UNUSED;
    if (!rt.nativeNewTarget) {
        double t = NowMs();
        return Value::S(DateStr(t) + L" " + TimeStr(t));
    }
    double t;
    if (args.empty()) t = NowMs();
    else if (args.size() == 1) {
        Value v = A(0);
        if (v.IsObj() && v.o->cls == Cls::Date) t = v.o->x->num;
        else {
            v = rt.ToPrimitive(v, 0);
            t = v.IsStr() ? ParseDateInternal(*v.s) : rt.ToNum(v);
        }
    } else {
        double c[7] = {0, 0, 1, 0, 0, 0, 0};
        for (size_t i = 0; i < 7 && i < args.size(); i++) c[i] = rt.ToNum(args[i]);
        if (c[0] >= 0 && c[0] <= 99 && c[0] == std::floor(c[0])) c[0] += 1900;
        t = FromLocal(MakeTime(c[0], c[1], c[2], c[3], c[4], c[5], c[6]));
    }
    Obj* proto = rt.dateProto;
    Value np = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
    if (np.IsObj()) proto = np.o;
    Obj* d = rt.NewObject(proto);
    d->cls = Cls::Date;
    d->X().num = Clip(t);
    return Value::O(d);
}
NATIVE(Date_now) { UNUSED; return Value::N(NowMs()); }
NATIVE(Date_parse) { UNUSED; return Value::N(ParseDateInternal(rt.ToStr(A(0)))); }
NATIVE(Date_UTC) {
    UNUSED;
    double c[7] = {NAN, 0, 1, 0, 0, 0, 0};
    for (size_t i = 0; i < 7 && i < args.size(); i++) c[i] = rt.ToNum(args[i]);
    if (c[0] >= 0 && c[0] <= 99 && c[0] == std::floor(c[0])) c[0] += 1900;
    return Value::N(Clip(MakeTime(c[0], c[1], c[2], c[3], c[4], c[5], c[6])));
}
NATIVE(DateP_getTime) { UNUSED; return Value::N(ThisTime(rt, thisV)); }

Value Component(Runtime& rt, const Value& thisV, int which, bool utc) {
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::N(NAN);
    Parts p = Split(utc ? t : ToLocal(t));
    double v[] = {p.y, p.mon, p.d, p.h, p.mi, p.s, p.ms, p.wd};
    return Value::N(v[which]);
}
#define GETTER(nm, idx, utc) NATIVE(nm) { UNUSED; return Component(rt, thisV, idx, utc); }
GETTER(DateP_getFullYear, 0, false) GETTER(DateP_getMonth, 1, false) GETTER(DateP_getDate, 2, false)
GETTER(DateP_getHours, 3, false) GETTER(DateP_getMinutes, 4, false) GETTER(DateP_getSeconds, 5, false)
GETTER(DateP_getMilliseconds, 6, false) GETTER(DateP_getDay, 7, false)
GETTER(DateP_getUTCFullYear, 0, true) GETTER(DateP_getUTCMonth, 1, true) GETTER(DateP_getUTCDate, 2, true)
GETTER(DateP_getUTCHours, 3, true) GETTER(DateP_getUTCMinutes, 4, true) GETTER(DateP_getUTCSeconds, 5, true)
GETTER(DateP_getUTCMilliseconds, 6, true) GETTER(DateP_getUTCDay, 7, true)

NATIVE(DateP_getYear) { UNUSED; Value v = Component(rt, thisV, 0, false); return Value::N(v.n - 1900); }
NATIVE(DateP_getTimezoneOffset) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::N(NAN);
    return Value::N(-LocalOffset(t) / 60000.0);
}

Value SetComp(Runtime& rt, const Value& thisV, std::vector<Value>& args, int first, int maxN, bool utc) {
    double t = ThisTime(rt, thisV);
    Parts p = Split(std::isnan(t) ? 0 : (utc ? t : ToLocal(t)));
    double v[] = {p.y, p.mon, p.d, p.h, p.mi, p.s, p.ms};
    for (int k = 0; k < maxN && size_t(k) < args.size(); k++) v[first + k] = rt.ToNum(args[size_t(k)]);
    if (std::isnan(t) && first != 0) return Value::N(NAN);
    double nt = MakeTime(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    if (!utc) nt = FromLocal(nt);
    nt = Clip(nt);
    thisV.o->x->num = nt;
    return Value::N(nt);
}
#define SETTER(nm, first, maxN, utc) NATIVE(nm) { UNUSED; return SetComp(rt, thisV, args, first, maxN, utc); }
SETTER(DateP_setFullYear, 0, 3, false) SETTER(DateP_setMonth, 1, 2, false) SETTER(DateP_setDate, 2, 1, false)
SETTER(DateP_setHours, 3, 4, false) SETTER(DateP_setMinutes, 4, 3, false) SETTER(DateP_setSeconds, 5, 2, false)
SETTER(DateP_setMilliseconds, 6, 1, false)
SETTER(DateP_setUTCFullYear, 0, 3, true) SETTER(DateP_setUTCMonth, 1, 2, true) SETTER(DateP_setUTCDate, 2, 1, true)
SETTER(DateP_setUTCHours, 3, 4, true) SETTER(DateP_setUTCMinutes, 4, 3, true) SETTER(DateP_setUTCSeconds, 5, 2, true)
SETTER(DateP_setUTCMilliseconds, 6, 1, true)
NATIVE(DateP_setTime) { UNUSED; ThisTime(rt, thisV); thisV.o->x->num = Clip(rt.ToNum(A(0))); return Value::N(thisV.o->x->num); }

NATIVE(DateP_toISOString) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) rt.Throw(L"RangeError", L"Invalid time value");
    Parts p = Split(t);
    wchar_t buf[64];
    if (p.y >= 0 && p.y <= 9999) swprintf(buf, 64, L"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", int(p.y), int(p.mon) + 1, int(p.d), int(p.h), int(p.mi), int(p.s), int(p.ms));
    else swprintf(buf, 64, L"%+07d-%02d-%02dT%02d:%02d:%02d.%03dZ", int(p.y), int(p.mon) + 1, int(p.d), int(p.h), int(p.mi), int(p.s), int(p.ms));
    return Value::S(buf);
}
NATIVE(DateP_toJSON) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::Null_();
    return DateP_toISOString(rt, self, thisV, args);
}
NATIVE(DateP_toString) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::S(L"Invalid Date");
    return Value::S(DateStr(t) + L" " + TimeStr(t));
}
NATIVE(DateP_toDateString) { UNUSED; double t = ThisTime(rt, thisV); return Value::S(std::isnan(t) ? L"Invalid Date" : DateStr(t)); }
NATIVE(DateP_toTimeString) { UNUSED; double t = ThisTime(rt, thisV); return Value::S(std::isnan(t) ? L"Invalid Date" : TimeStr(t)); }
NATIVE(DateP_toUTCString) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::S(L"Invalid Date");
    Parts p = Split(t);
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls, %02d %ls %04d %02d:%02d:%02d GMT", kDays[int(p.wd)], int(p.d), kMonths[int(p.mon)], int(p.y), int(p.h), int(p.mi), int(p.s));
    return Value::S(buf);
}
NATIVE(DateP_toLocaleDateString) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::S(L"Invalid Date");
    if (!args.empty()) {
        Value intl = rt.Get(Value::O(rt.global), L"Intl");
        if (intl.IsObj()) {
            Value dtfCtor = rt.Get(intl, L"DateTimeFormat");
            if (dtfCtor.IsObj() && dtfCtor.o->IsCallable()) {
                std::vector<Value> dtfArgs;
                dtfArgs.push_back(args[0]);
                Obj* opt = (args.size() > 1 && args[1].IsObj()) ? args[1].o : rt.NewObject();
                dtfArgs.push_back(Value::O(opt));
                Value dtf = rt.Call(dtfCtor, Value::O(rt.global), dtfArgs);
                if (dtf.IsObj()) {
                    Value fmt = rt.Get(dtf, L"format");
                    if (fmt.IsObj() && fmt.o->IsCallable()) {
                        std::vector<Value> fa{Value::N(t)};
                        return rt.Call(fmt, dtf, fa);
                    }
                }
            }
        }
    }
    Parts p = Split(ToLocal(t));
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d.%02d.%04d", int(p.d), int(p.mon) + 1, int(p.y));
    return Value::S(buf);
}
NATIVE(DateP_toLocaleTimeString) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::S(L"Invalid Date");
    if (!args.empty()) {
        Value intl = rt.Get(Value::O(rt.global), L"Intl");
        if (intl.IsObj()) {
            Value dtfCtor = rt.Get(intl, L"DateTimeFormat");
            if (dtfCtor.IsObj() && dtfCtor.o->IsCallable()) {
                std::vector<Value> dtfArgs;
                dtfArgs.push_back(args[0]);
                Obj* opt = (args.size() > 1 && args[1].IsObj()) ? args[1].o : rt.NewObject();
                dtfArgs.push_back(Value::O(opt));
                Value dtf = rt.Call(dtfCtor, Value::O(rt.global), dtfArgs);
                if (dtf.IsObj()) {
                    Value fmt = rt.Get(dtf, L"format");
                    if (fmt.IsObj() && fmt.o->IsCallable()) {
                        std::vector<Value> fa{Value::N(t)};
                        return rt.Call(fmt, dtf, fa);
                    }
                }
            }
        }
    }
    Parts p = Split(ToLocal(t));
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d:%02d:%02d", int(p.h), int(p.mi), int(p.s));
    return Value::S(buf);
}
NATIVE(DateP_toLocaleString) {
    UNUSED;
    double t = ThisTime(rt, thisV);
    if (std::isnan(t)) return Value::S(L"Invalid Date");
    if (!args.empty()) {
        Value intl = rt.Get(Value::O(rt.global), L"Intl");
        if (intl.IsObj()) {
            Value dtfCtor = rt.Get(intl, L"DateTimeFormat");
            if (dtfCtor.IsObj() && dtfCtor.o->IsCallable()) {
                std::vector<Value> dtfArgs;
                dtfArgs.push_back(args[0]);
                Obj* opt = (args.size() > 1 && args[1].IsObj()) ? args[1].o : rt.NewObject();
                dtfArgs.push_back(Value::O(opt));
                Value dtf = rt.Call(dtfCtor, Value::O(rt.global), dtfArgs);
                if (dtf.IsObj()) {
                    Value fmt = rt.Get(dtf, L"format");
                    if (fmt.IsObj() && fmt.o->IsCallable()) {
                        std::vector<Value> fa{Value::N(t)};
                        return rt.Call(fmt, dtf, fa);
                    }
                }
            }
        }
    }
    return Value::S(*DateP_toLocaleDateString(rt, self, thisV, args).s + L" " + *DateP_toLocaleTimeString(rt, self, thisV, args).s);
}

// ================= URI ve base64 =================

std::wstring Encode(Runtime& rt, const std::wstring& s, const wchar_t* keep) {
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = uint32_t(s[i]);
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || (c < 128 && wcschr(keep, wchar_t(c)) && c)) {
            o += wchar_t(c);
            continue;
        }
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size()) rt.Throw(L"URIError", L"URI malformed");
            uint32_t d = uint32_t(s[i + 1]);
            if (d < 0xDC00 || d > 0xDFFF) rt.Throw(L"URIError", L"URI malformed");
            c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            i++;
        } else if (c >= 0xDC00 && c <= 0xDFFF) rt.Throw(L"URIError", L"URI malformed");
        unsigned char b[4];
        int n;
        if (c < 0x80) { b[0] = uint8_t(c); n = 1; }
        else if (c < 0x800) { b[0] = uint8_t(0xC0 | (c >> 6)); b[1] = uint8_t(0x80 | (c & 0x3F)); n = 2; }
        else if (c < 0x10000) { b[0] = uint8_t(0xE0 | (c >> 12)); b[1] = uint8_t(0x80 | ((c >> 6) & 0x3F)); b[2] = uint8_t(0x80 | (c & 0x3F)); n = 3; }
        else { b[0] = uint8_t(0xF0 | (c >> 18)); b[1] = uint8_t(0x80 | ((c >> 12) & 0x3F)); b[2] = uint8_t(0x80 | ((c >> 6) & 0x3F)); b[3] = uint8_t(0x80 | (c & 0x3F)); n = 4; }
        for (int k = 0; k < n; k++) { o += L'%'; o += hex[b[k] >> 4]; o += hex[b[k] & 15]; }
    }
    return o;
}
int HexD(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
std::wstring Decode(Runtime& rt, const std::wstring& s, const wchar_t* reserved) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'%') { o += s[i]; continue; }
        auto byteAt = [&](size_t p) -> int {
            if (p + 2 >= s.size()) rt.Throw(L"URIError", L"URI malformed");
            if (s[p] != L'%') rt.Throw(L"URIError", L"URI malformed");
            int a = HexD(s[p + 1]), b = HexD(s[p + 2]);
            if (a < 0 || b < 0) rt.Throw(L"URIError", L"URI malformed");
            return a * 16 + b;
        };
        if (i + 2 >= s.size()) rt.Throw(L"URIError", L"URI malformed");
        int b0 = byteAt(i);
        if (b0 < 0x80) {
            if (reserved && wcschr(reserved, wchar_t(b0)) && b0) o += s.substr(i, 3);
            else o += wchar_t(b0);
            i += 2;
            continue;
        }
        int n = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) rt.Throw(L"URIError", L"URI malformed");
        uint32_t cp = uint32_t(b0 & (0xFF >> (n + 1)));
        for (int k = 1; k < n; k++) {
            size_t p = i + size_t(3 * k);
            if (p + 2 >= s.size()) rt.Throw(L"URIError", L"URI malformed");
            int bk = byteAt(p);
            if ((bk & 0xC0) != 0x80) rt.Throw(L"URIError", L"URI malformed");
            cp = (cp << 6) | uint32_t(bk & 0x3F);
        }
        i += size_t(3 * n) - 1;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) rt.Throw(L"URIError", L"URI malformed");
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) { cp -= 0x10000; o += wchar_t(0xD800 + (cp >> 10)); o += wchar_t(0xDC00 + (cp & 0x3FF)); }
        else o += wchar_t(cp);
    }
    return o;
}
NATIVE(Global_encodeURIComponent) { UNUSED; return Value::S(Encode(rt, rt.ToStr(A(0)), L"-_.!~*'()")); }
NATIVE(Global_encodeURI) { UNUSED; return Value::S(Encode(rt, rt.ToStr(A(0)), L"-_.!~*'();/?:@&=+$,#")); }
NATIVE(Global_decodeURIComponent) { UNUSED; return Value::S(Decode(rt, rt.ToStr(A(0)), nullptr)); }
NATIVE(Global_decodeURI) { UNUSED; return Value::S(Decode(rt, rt.ToStr(A(0)), L";/?:@&=+$,#")); }
NATIVE(Global_escape) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0)), o;
    for (wchar_t c : s) {
        if (iswalnum(c) && c < 128) o += c;
        else if (wcschr(L"@*_+-./", c) && c) o += c;
        else { wchar_t b[8]; if (c < 256) swprintf(b, 8, L"%%%02X", unsigned(c)); else swprintf(b, 8, L"%%u%04X", unsigned(c)); o += b; }
    }
    return Value::S(o);
}
NATIVE(Global_unescape) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0)), o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 5 < s.size() + 0 && s[i + 1] == L'u') {
            int v = 0; bool ok = true;
            for (int k = 2; k < 6; k++) { int h = HexD(s[i + size_t(k)]); if (h < 0) ok = false; v = v * 16 + h; }
            if (ok) { o += wchar_t(v); i += 5; continue; }
        }
        if (s[i] == L'%' && i + 2 < s.size()) {
            int a = HexD(s[i + 1]), b = HexD(s[i + 2]);
            if (a >= 0 && b >= 0) { o += wchar_t(a * 16 + b); i += 2; continue; }
        }
        o += s[i];
    }
    return Value::S(o);
}
NATIVE(Global_btoa) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0));
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring o;
    for (wchar_t c : s) if (uint32_t(c) > 255) rt.Throw(L"Error", L"InvalidCharacterError: The string to be encoded contains characters outside of the Latin1 range.");
    for (size_t i = 0; i < s.size(); i += 3) {
        uint32_t n = uint32_t(s[i]) << 16;
        if (i + 1 < s.size()) n |= uint32_t(s[i + 1]) << 8;
        if (i + 2 < s.size()) n |= uint32_t(s[i + 2]);
        o += wchar_t(t[(n >> 18) & 63]);
        o += wchar_t(t[(n >> 12) & 63]);
        o += i + 1 < s.size() ? wchar_t(t[(n >> 6) & 63]) : L'=';
        o += i + 2 < s.size() ? wchar_t(t[n & 63]) : L'=';
    }
    return Value::S(o);
}
NATIVE(Global_atob) {
    UNUSED;
    std::wstring s = rt.ToStr(A(0)), o;
    uint32_t buf = 0;
    int bits = 0;
    for (wchar_t c : s) {
        int v;
        if (c >= L'A' && c <= L'Z') v = c - L'A';
        else if (c >= L'a' && c <= L'z') v = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') v = c - L'0' + 52;
        else if (c == L'+' || c == L'-') v = 62;
        else if (c == L'/' || c == L'_') v = 63;
        else if (c == L'=' || iswspace(c)) continue;
        else rt.Throw(L"Error", L"InvalidCharacterError: The string to be decoded is not correctly encoded.");
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; o += wchar_t((buf >> bits) & 0xFF); }
    }
    return Value::S(o);
}

} // namespace

double ParseDate(const std::wstring& in) {
    return ParseDateInternal(in);
}

Obj* Runtime::NewRegExp(const std::wstring& src, const std::wstring& flags) {
    auto c = CompileRegex(*this, src, flags);
    Obj* r = NewObject(regexpProto);
    r->cls = Cls::RegExp;
    r->X().str = src;
    r->x->flags = flags;
    r->x->re = c;
    Prop& p = r->props.Put(L"lastIndex");
    p.v = Value::N(0);
    p.enumerable = false;
    return r;
}

// ---------------- ArrayBuffer, DataView & TypedArrays ----------------

NATIVE(ArrayBuffer_ctor) {
    UNUSED;
    double n = A(0).IsUndef() ? 0 : rt.ToNum(A(0));
    if (std::isnan(n) || n < 0 || n != std::floor(n) || n > 2147483648.0) {
        rt.Throw(L"RangeError", L"Invalid array buffer length");
    }
    Obj* ab = rt.NewArrayBuffer(size_t(n));
    return Value::O(ab);
}

NATIVE(ArrayBufferP_byteLength) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::ArrayBuffer || !thisV.o->x) {
        rt.Throw(L"TypeError", L"Method get ArrayBuffer.prototype.byteLength called on incompatible receiver");
    }
    return Value::N(double(thisV.o->x->byteLength));
}

NATIVE(ArrayBufferP_slice) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::ArrayBuffer || !thisV.o->x) {
        rt.Throw(L"TypeError", L"Method called on incompatible receiver");
    }
    ObjExtra& x = *thisV.o->x;
    int64_t len = int64_t(x.byteLength);
    int64_t b = A(0).IsUndef() ? 0 : int64_t(rt.ToNum(A(0)));
    if (b < 0) b = std::max<int64_t>(0, len + b); else b = std::min<int64_t>(len, b);
    int64_t e = A(1).IsUndef() ? len : int64_t(rt.ToNum(A(1)));
    if (e < 0) e = std::max<int64_t>(0, len + e); else e = std::min<int64_t>(len, e);
    size_t count = b < e ? size_t(e - b) : 0;
    Obj* res = rt.NewArrayBuffer(count);
    if (count > 0 && x.bufferData && res->x->bufferData) {
        memcpy(res->x->bufferData->data(), x.bufferData->data() + b, count);
    }
    return Value::O(res);
}

NATIVE(ArrayBuffer_isView) {
    UNUSED;
    return Value::B(A(0).IsObj() && (A(0).o->cls == Cls::TypedArray || A(0).o->cls == Cls::DataView));
}

// DataView helpers
static uint8_t* GetDataViewPtr(Runtime& rt, const Value& thisV, const Value& offsetVal, size_t needBytes) {
    if (!thisV.IsObj() || thisV.o->cls != Cls::DataView || !thisV.o->x) {
        rt.Throw(L"TypeError", L"Method called on incompatible receiver");
    }
    ObjExtra& x = *thisV.o->x;
    double offNum = rt.ToNum(offsetVal);
    if (std::isnan(offNum) || offNum < 0 || offNum != std::floor(offNum)) {
        rt.Throw(L"RangeError", L"Offset is outside the bounds of the DataView");
    }
    size_t off = size_t(offNum);
    if (off + needBytes > x.byteLength || !x.bufferData || x.byteOffset + off + needBytes > x.bufferData->size()) {
        rt.Throw(L"RangeError", L"Offset is outside the bounds of the DataView");
    }
    return x.bufferData->data() + x.byteOffset + off;
}

NATIVE(DataView_ctor) {
    UNUSED;
    if (!A(0).IsObj() || A(0).o->cls != Cls::ArrayBuffer || !A(0).o->x) {
        rt.Throw(L"TypeError", L"First argument to DataView constructor must be an ArrayBuffer");
    }
    Obj* buf = A(0).o;
    double offNum = A(1).IsUndef() ? 0 : rt.ToNum(A(1));
    if (std::isnan(offNum) || offNum < 0 || offNum != std::floor(offNum)) {
        rt.Throw(L"RangeError", L"Start offset is outside the bounds of the buffer");
    }
    size_t byteOffset = size_t(offNum);
    if (byteOffset > buf->x->byteLength) {
        rt.Throw(L"RangeError", L"Start offset is outside the bounds of the buffer");
    }
    size_t byteLength = 0;
    if (!A(2).IsUndef()) {
        double lenNum = rt.ToNum(A(2));
        if (std::isnan(lenNum) || lenNum < 0 || lenNum != std::floor(lenNum)) {
            rt.Throw(L"RangeError", L"Invalid DataView length");
        }
        byteLength = size_t(lenNum);
        if (byteOffset + byteLength > buf->x->byteLength) {
            rt.Throw(L"RangeError", L"Invalid DataView length");
        }
    } else {
        byteLength = buf->x->byteLength - byteOffset;
    }
    Obj* dv = rt.NewObject(rt.dataViewProto);
    dv->cls = Cls::DataView;
    dv->X().bufferData = buf->x->bufferData;
    dv->x->arrayBufferObj = buf;
    dv->x->byteOffset = byteOffset;
    dv->x->byteLength = byteLength;
    return Value::O(dv);
}

NATIVE(DataViewP_buffer) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::DataView || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    return thisV.o->x->arrayBufferObj ? Value::O(thisV.o->x->arrayBufferObj) : Value();
}
NATIVE(DataViewP_byteLength) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::DataView || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    return Value::N(double(thisV.o->x->byteLength));
}
NATIVE(DataViewP_byteOffset) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::DataView || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    return Value::N(double(thisV.o->x->byteOffset));
}
NATIVE(DataViewP_getInt8) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 1);
    return Value::N(double(*(int8_t*)p));
}
NATIVE(DataViewP_getUint8) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 1);
    return Value::N(double(*p));
}
NATIVE(DataViewP_setInt8) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 1);
    *p = uint8_t(int8_t(int32_t(rt.ToNum(A(1)))));
    return Value();
}
NATIVE(DataViewP_setUint8) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 1);
    *p = uint8_t(uint32_t(rt.ToNum(A(1))));
    return Value();
}
NATIVE(DataViewP_getInt16) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 2);
    bool le = rt.Truthy(A(1));
    int16_t v = le ? int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8))
                   : int16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
    return Value::N(double(v));
}
NATIVE(DataViewP_getUint16) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 2);
    bool le = rt.Truthy(A(1));
    uint16_t v = le ? (uint16_t(p[0]) | (uint16_t(p[1]) << 8))
                    : ((uint16_t(p[0]) << 8) | uint16_t(p[1]));
    return Value::N(double(v));
}
NATIVE(DataViewP_setInt16) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 2);
    uint16_t v = uint16_t(uint32_t(rt.ToNum(A(1))));
    bool le = rt.Truthy(A(2));
    if (le) { p[0] = uint8_t(v & 0xFF); p[1] = uint8_t(v >> 8); }
    else    { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v & 0xFF); }
    return Value();
}
NATIVE(DataViewP_setUint16) { return DataViewP_setInt16(rt, self, thisV, args); }
NATIVE(DataViewP_getInt32) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 4);
    bool le = rt.Truthy(A(1));
    uint32_t u = le ? (uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24))
                    : ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
    return Value::N(double(int32_t(u)));
}
NATIVE(DataViewP_getUint32) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 4);
    bool le = rt.Truthy(A(1));
    uint32_t u = le ? (uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24))
                    : ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
    return Value::N(double(u));
}
NATIVE(DataViewP_setInt32) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 4);
    uint32_t u = uint32_t(rt.ToNum(A(1)));
    bool le = rt.Truthy(A(2));
    if (le) {
        p[0] = uint8_t(u & 0xFF); p[1] = uint8_t((u >> 8) & 0xFF);
        p[2] = uint8_t((u >> 16) & 0xFF); p[3] = uint8_t(u >> 24);
    } else {
        p[0] = uint8_t(u >> 24); p[1] = uint8_t((u >> 16) & 0xFF);
        p[2] = uint8_t((u >> 8) & 0xFF); p[3] = uint8_t(u & 0xFF);
    }
    return Value();
}
NATIVE(DataViewP_setUint32) { return DataViewP_setInt32(rt, self, thisV, args); }
NATIVE(DataViewP_getFloat32) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 4);
    bool le = rt.Truthy(A(1));
    uint32_t u = le ? (uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24))
                    : ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
    float f; memcpy(&f, &u, 4);
    return Value::N(double(f));
}
NATIVE(DataViewP_setFloat32) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 4);
    float f = float(rt.ToNum(A(1)));
    uint32_t u; memcpy(&u, &f, 4);
    bool le = rt.Truthy(A(2));
    if (le) {
        p[0] = uint8_t(u & 0xFF); p[1] = uint8_t((u >> 8) & 0xFF);
        p[2] = uint8_t((u >> 16) & 0xFF); p[3] = uint8_t(u >> 24);
    } else {
        p[0] = uint8_t(u >> 24); p[1] = uint8_t((u >> 16) & 0xFF);
        p[2] = uint8_t((u >> 8) & 0xFF); p[3] = uint8_t(u & 0xFF);
    }
    return Value();
}
NATIVE(DataViewP_getFloat64) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 8);
    bool le = rt.Truthy(A(1));
    uint64_t u = 0;
    if (le) {
        for (int i = 0; i < 8; i++) u |= (uint64_t(p[i]) << (i * 8));
    } else {
        for (int i = 0; i < 8; i++) u = (u << 8) | uint64_t(p[i]);
    }
    double d; memcpy(&d, &u, 8);
    return Value::N(d);
}
NATIVE(DataViewP_setFloat64) {
    UNUSED;
    uint8_t* p = GetDataViewPtr(rt, thisV, A(0), 8);
    double d = rt.ToNum(A(1));
    uint64_t u; memcpy(&u, &d, 8);
    bool le = rt.Truthy(A(2));
    if (le) {
        for (int i = 0; i < 8; i++) p[i] = uint8_t((u >> (i * 8)) & 0xFF);
    } else {
        for (int i = 0; i < 8; i++) p[7 - i] = uint8_t((u >> (i * 8)) & 0xFF);
    }
    return Value();
}

// TypedArray shared implementation
static Obj* CreateTypedArray(Runtime& rt, TypedKind kind, std::vector<Value>& args) {
    size_t elSize = ElementSize(kind);
    Value a0 = args.empty() ? Value() : args[0];
    if (a0.IsObj() && a0.o->cls == Cls::ArrayBuffer && a0.o->x) {
        Obj* buf = a0.o;
        double byteOffsetNum = args.size() > 1 && !args[1].IsUndef() ? rt.ToNum(args[1]) : 0;
        if (byteOffsetNum < 0 || byteOffsetNum != std::floor(byteOffsetNum) || fmod(byteOffsetNum, double(elSize)) != 0) {
            rt.Throw(L"RangeError", L"byteOffset should be a multiple of BYTES_PER_ELEMENT");
        }
        size_t byteOffset = size_t(byteOffsetNum);
        if (byteOffset > buf->x->byteLength) {
            rt.Throw(L"RangeError", L"Start offset is outside the bounds of the buffer");
        }
        size_t count = 0;
        if (args.size() > 2 && !args[2].IsUndef()) {
            double lenNum = rt.ToNum(args[2]);
            if (lenNum < 0 || lenNum != std::floor(lenNum)) rt.Throw(L"RangeError", L"Invalid typed array length");
            count = size_t(lenNum);
            if (byteOffset + count * elSize > buf->x->byteLength) rt.Throw(L"RangeError", L"Invalid typed array length");
        } else {
            size_t rem = buf->x->byteLength - byteOffset;
            if (rem % elSize != 0) rt.Throw(L"RangeError", L"byteLength should be a multiple of BYTES_PER_ELEMENT");
            count = rem / elSize;
        }
        return rt.NewTypedArrayFromBuffer(kind, buf, byteOffset, count);
    }
    if (a0.IsObj() && a0.o->cls == Cls::TypedArray && a0.o->x) {
        Obj* src = a0.o;
        size_t count = src->x->typedLength;
        Obj* ta = rt.NewTypedArray(kind, count);
        for (size_t i = 0; i < count; i++) {
            double num = GetTypedElementNum(*src->x, i);
            SetTypedElement(rt, *ta->x, i, Value::N(num));
        }
        return ta;
    }
    if (a0.IsObj()) {
        if (a0.o->cls == Cls::Array) {
            size_t count = a0.o->elems.size();
            Obj* ta = rt.NewTypedArray(kind, count);
            for (size_t i = 0; i < count; i++) {
                SetTypedElement(rt, *ta->x, i, a0.o->elems[i]);
            }
            return ta;
        }
        std::vector<Value> items = rt.IterateToList(a0);
        Obj* ta = rt.NewTypedArray(kind, items.size());
        for (size_t i = 0; i < items.size(); i++) {
            SetTypedElement(rt, *ta->x, i, items[i]);
        }
        return ta;
    }
    double n = a0.IsUndef() ? 0 : rt.ToNum(a0);
    if (std::isnan(n) || n < 0 || n != std::floor(n) || n > 2147483648.0 / elSize) {
        rt.Throw(L"RangeError", L"Invalid typed array length");
    }
    size_t count = size_t(n);
    return rt.NewTypedArray(kind, count);
}

#define DEF_TYPED_CTOR(NAME, KIND) \
    NATIVE(NAME##_ctor) { \
        UNUSED; \
        Obj* ta = CreateTypedArray(rt, KIND, args); \
        return Value::O(ta); \
    }

DEF_TYPED_CTOR(Int8Array, TypedKind::Int8)
DEF_TYPED_CTOR(Uint8Array, TypedKind::Uint8)
DEF_TYPED_CTOR(Uint8ClampedArray, TypedKind::Uint8Clamped)
DEF_TYPED_CTOR(Int16Array, TypedKind::Int16)
DEF_TYPED_CTOR(Uint16Array, TypedKind::Uint16)
DEF_TYPED_CTOR(Int32Array, TypedKind::Int32)
DEF_TYPED_CTOR(Uint32Array, TypedKind::Uint32)
DEF_TYPED_CTOR(Float32Array, TypedKind::Float32)
DEF_TYPED_CTOR(Float64Array, TypedKind::Float64)

NATIVE(TypedArrayP_buffer) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    if (!x.arrayBufferObj) {
        Obj* ab = rt.NewObject(rt.arrayBufferProto);
        ab->cls = Cls::ArrayBuffer;
        ab->X().bufferData = x.bufferData;
        ab->x->byteLength = x.bufferData ? x.bufferData->size() : 0;
        x.arrayBufferObj = ab;
    }
    return Value::O(x.arrayBufferObj);
}
NATIVE(TypedArrayP_byteLength) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    return Value::N(double(thisV.o->x->byteLength));
}
NATIVE(TypedArrayP_byteOffset) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    return Value::N(double(thisV.o->x->byteOffset));
}
NATIVE(TypedArrayP_length) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    return Value::N(double(thisV.o->x->typedLength));
}

NATIVE(TypedArrayP_subarray) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    int64_t len = int64_t(x.typedLength);
    int64_t b = A(0).IsUndef() ? 0 : int64_t(rt.ToNum(A(0)));
    if (b < 0) b = std::max<int64_t>(0, len + b); else b = std::min<int64_t>(len, b);
    int64_t e = A(1).IsUndef() ? len : int64_t(rt.ToNum(A(1)));
    if (e < 0) e = std::max<int64_t>(0, len + e); else e = std::min<int64_t>(len, e);
    size_t count = b < e ? size_t(e - b) : 0;
    size_t elSize = ElementSize(x.typedKind);
    size_t newOffset = x.byteOffset + size_t(b) * elSize;
    Obj* sub = rt.NewTypedArrayFromBuffer(x.typedKind, x.arrayBufferObj, newOffset, count);
    if (!x.arrayBufferObj) {
        sub->x->bufferData = x.bufferData;
    }
    return Value::O(sub);
}

NATIVE(TypedArrayP_slice) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    int64_t len = int64_t(x.typedLength);
    int64_t b = A(0).IsUndef() ? 0 : int64_t(rt.ToNum(A(0)));
    if (b < 0) b = std::max<int64_t>(0, len + b); else b = std::min<int64_t>(len, b);
    int64_t e = A(1).IsUndef() ? len : int64_t(rt.ToNum(A(1)));
    if (e < 0) e = std::max<int64_t>(0, len + e); else e = std::min<int64_t>(len, e);
    size_t count = b < e ? size_t(e - b) : 0;
    Obj* res = rt.NewTypedArray(x.typedKind, count);
    size_t elSize = ElementSize(x.typedKind);
    if (count > 0 && x.bufferData && res->x->bufferData) {
        memcpy(res->x->bufferData->data(), x.bufferData->data() + x.byteOffset + size_t(b) * elSize, count * elSize);
    }
    return Value::O(res);
}

NATIVE(TypedArrayP_set) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& destX = *thisV.o->x;
    double offNum = A(1).IsUndef() ? 0 : rt.ToNum(A(1));
    if (std::isnan(offNum) || offNum < 0 || offNum != std::floor(offNum)) {
        rt.Throw(L"RangeError", L"Offset is out of bounds");
    }
    size_t targetOffset = size_t(offNum);
    if (A(0).IsObj() && A(0).o->cls == Cls::TypedArray && A(0).o->x) {
        ObjExtra& srcX = *A(0).o->x;
        size_t count = srcX.typedLength;
        if (targetOffset + count > destX.typedLength) rt.Throw(L"RangeError", L"Offset is out of bounds");
        size_t elSize = ElementSize(destX.typedKind);
        if (srcX.typedKind == destX.typedKind && destX.bufferData && srcX.bufferData) {
            memmove(destX.bufferData->data() + destX.byteOffset + targetOffset * elSize,
                    srcX.bufferData->data() + srcX.byteOffset, count * elSize);
        } else {
            for (size_t i = 0; i < count; i++) {
                double val = GetTypedElementNum(srcX, i);
                SetTypedElement(rt, destX, targetOffset + i, Value::N(val));
            }
        }
    } else if (A(0).IsObj()) {
        std::vector<Value> items = rt.IterateToList(A(0));
        if (targetOffset + items.size() > destX.typedLength) rt.Throw(L"RangeError", L"Offset is out of bounds");
        for (size_t i = 0; i < items.size(); i++) {
            SetTypedElement(rt, destX, targetOffset + i, items[i]);
        }
    } else {
        rt.Throw(L"TypeError", L"First argument to set must be an array or typed array");
    }
    return Value();
}

NATIVE(TypedArrayP_fill) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    int64_t len = int64_t(x.typedLength);
    int64_t b = A(1).IsUndef() ? 0 : int64_t(rt.ToNum(A(1)));
    if (b < 0) b = std::max<int64_t>(0, len + b); else b = std::min<int64_t>(len, b);
    int64_t e = A(2).IsUndef() ? len : int64_t(rt.ToNum(A(2)));
    if (e < 0) e = std::max<int64_t>(0, len + e); else e = std::min<int64_t>(len, e);
    for (int64_t i = b; i < e; i++) {
        SetTypedElement(rt, x, size_t(i), A(0));
    }
    return thisV;
}

NATIVE(TypedArrayP_join) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    std::wstring sep = A(0).IsUndef() ? L"," : rt.ToStr(A(0));
    std::wstring out;
    for (size_t i = 0; i < x.typedLength; i++) {
        if (i > 0) out += sep;
        out += Runtime::NumToStr(GetTypedElementNum(x, i));
    }
    return Value::S(out);
}

NATIVE(TypedArrayP_toString) {
    UNUSED;
    return TypedArrayP_join(rt, self, thisV, args);
}

NATIVE(TypedArrayP_reverse) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    size_t n = x.typedLength;
    for (size_t i = 0; i < n / 2; i++) {
        double a = GetTypedElementNum(x, i);
        double b = GetTypedElementNum(x, n - 1 - i);
        SetTypedElement(rt, x, i, Value::N(b));
        SetTypedElement(rt, x, n - 1 - i, Value::N(a));
    }
    return thisV;
}

NATIVE(TypedArrayP_indexOf) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    double searchNum = rt.ToNum(A(0));
    int64_t len = int64_t(x.typedLength);
    int64_t from = A(1).IsUndef() ? 0 : int64_t(rt.ToNum(A(1)));
    if (from < 0) from = std::max<int64_t>(0, len + from);
    for (int64_t i = from; i < len; i++) {
        double v = GetTypedElementNum(x, size_t(i));
        if (v == searchNum && !std::isnan(searchNum)) return Value::N(double(i));
    }
    return Value::N(-1);
}

NATIVE(TypedArrayP_includes) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    ObjExtra& x = *thisV.o->x;
    double searchNum = rt.ToNum(A(0));
    int64_t len = int64_t(x.typedLength);
    int64_t from = A(1).IsUndef() ? 0 : int64_t(rt.ToNum(A(1)));
    if (from < 0) from = std::max<int64_t>(0, len + from);
    for (int64_t i = from; i < len; i++) {
        double v = GetTypedElementNum(x, size_t(i));
        if (v == searchNum || (std::isnan(v) && std::isnan(searchNum))) return Value::B(true);
    }
    return Value::B(false);
}

NATIVE(TypedArrayP_forEach) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) {
        std::vector<Value> cbArgs{GetTypedElement(x, i), Value::N(double(i)), thisV};
        rt.Call(cb, recv, cbArgs);
    }
    return Value();
}

NATIVE(TypedArrayP_map) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    Obj* res = rt.NewTypedArray(x.typedKind, x.typedLength);
    for (size_t i = 0; i < x.typedLength; i++) {
        std::vector<Value> cbArgs{GetTypedElement(x, i), Value::N(double(i)), thisV};
        Value r = rt.Call(cb, recv, cbArgs);
        SetTypedElement(rt, *res->x, i, r);
    }
    return Value::O(res);
}

NATIVE(TypedArrayP_filter) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    std::vector<Value> matched;
    for (size_t i = 0; i < x.typedLength; i++) {
        Value val = GetTypedElement(x, i);
        std::vector<Value> cbArgs{val, Value::N(double(i)), thisV};
        if (rt.Truthy(rt.Call(cb, recv, cbArgs))) matched.push_back(val);
    }
    Obj* res = rt.NewTypedArray(x.typedKind, matched.size());
    for (size_t i = 0; i < matched.size(); i++) SetTypedElement(rt, *res->x, i, matched[i]);
    return Value::O(res);
}

NATIVE(TypedArrayP_reduce) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    ObjExtra& x = *thisV.o->x;
    if (x.typedLength == 0 && A(1).IsUndef()) rt.Throw(L"TypeError", L"Reduce of empty array with no initial value");
    size_t k = 0;
    Value accum;
    if (!A(1).IsUndef()) {
        accum = A(1);
    } else {
        accum = GetTypedElement(x, 0);
        k = 1;
    }
    for (; k < x.typedLength; k++) {
        std::vector<Value> cbArgs{accum, GetTypedElement(x, k), Value::N(double(k)), thisV};
        accum = rt.Call(cb, Value(), cbArgs);
    }
    return accum;
}

NATIVE(TypedArrayP_some) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) {
        std::vector<Value> cbArgs{GetTypedElement(x, i), Value::N(double(i)), thisV};
        if (rt.Truthy(rt.Call(cb, recv, cbArgs))) return Value::B(true);
    }
    return Value::B(false);
}

NATIVE(TypedArrayP_every) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) {
        std::vector<Value> cbArgs{GetTypedElement(x, i), Value::N(double(i)), thisV};
        if (!rt.Truthy(rt.Call(cb, recv, cbArgs))) return Value::B(false);
    }
    return Value::B(true);
}

NATIVE(TypedArrayP_find) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) {
        Value val = GetTypedElement(x, i);
        std::vector<Value> cbArgs{val, Value::N(double(i)), thisV};
        if (rt.Truthy(rt.Call(cb, recv, cbArgs))) return val;
    }
    return Value();
}

NATIVE(TypedArrayP_findIndex) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    Value cb = A(0);
    if (!cb.IsObj() || !cb.o->IsCallable()) rt.Throw(L"TypeError", L"Callback is not a function");
    Value recv = A(1).IsUndef() ? Value() : A(1);
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) {
        std::vector<Value> cbArgs{GetTypedElement(x, i), Value::N(double(i)), thisV};
        if (rt.Truthy(rt.Call(cb, recv, cbArgs))) return Value::N(double(i));
    }
    return Value::N(-1);
}

NATIVE(TypedArrayP_values) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    std::vector<Value> list;
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) list.push_back(GetTypedElement(x, i));
    return Value::O(rt.MakeIteratorFromList(std::move(list)));
}

NATIVE(TypedArrayP_keys) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    std::vector<Value> list;
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) list.push_back(Value::N(double(i)));
    return Value::O(rt.MakeIteratorFromList(std::move(list)));
}

NATIVE(TypedArrayP_entries) {
    UNUSED;
    if (!thisV.IsObj() || thisV.o->cls != Cls::TypedArray || !thisV.o->x) rt.Throw(L"TypeError", L"Incompatible receiver");
    std::vector<Value> list;
    ObjExtra& x = *thisV.o->x;
    for (size_t i = 0; i < x.typedLength; i++) {
        list.push_back(Value::O(rt.NewArrayFrom({Value::N(double(i)), GetTypedElement(x, i)})));
    }
    return Value::O(rt.MakeIteratorFromList(std::move(list)));
}

NATIVE(TypedArray_from) {
    UNUSED;
    Value source = A(0);
    Value mapFn = A(1);
    Value thisArg = A(2);
    bool hasMap = mapFn.IsObj() && mapFn.o->IsCallable();
    std::vector<Value> items = rt.IterateToList(source);
    Obj* ctor = thisV.IsObj() && thisV.o->IsCallable() ? thisV.o : rt.ctors[L"Uint8Array"];
    std::vector<Value> cArgs{Value::N(double(items.size()))};
    Obj* ta = rt.Construct(Value::O(ctor), cArgs).o;
    for (size_t i = 0; i < items.size(); i++) {
        Value val = items[i];
        if (hasMap) {
            std::vector<Value> mArgs{val, Value::N(double(i))};
            val = rt.Call(mapFn, thisArg, mArgs);
        }
        if (ta && ta->x) SetTypedElement(rt, *ta->x, i, val);
    }
    return Value::O(ta);
}

NATIVE(TypedArray_of) {
    UNUSED;
    Obj* ctor = thisV.IsObj() && thisV.o->IsCallable() ? thisV.o : rt.ctors[L"Uint8Array"];
    std::vector<Value> cArgs{Value::N(double(args.size()))};
    Obj* ta = rt.Construct(Value::O(ctor), cArgs).o;
    for (size_t i = 0; i < args.size(); i++) {
        if (ta && ta->x) SetTypedElement(rt, *ta->x, i, args[i]);
    }
    return Value::O(ta);
}

static void InstallTypedArrays(Runtime& rt) {
    Obj* g = rt.global;
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Acc = [&](Obj* o, const wchar_t* n, NativeFn get, NativeFn set) { rt.DefineAccessor(o, n, get, set); };
    auto Ctor = [&](const wchar_t* name, NativeFn fn, int len, Obj* proto) {
        Obj* c = rt.NewFunction(fn, name, len, true);
        Prop& p = c->props.Put(L"prototype");
        p.v = Value::O(proto);
        p.enumerable = false;
        rt.DefineData(proto, L"constructor", Value::O(c), false);
        rt.DefineData(g, name, Value::O(c), false);
        rt.ctors[name] = c;
        return c;
    };

    // 1. ArrayBuffer
    rt.arrayBufferProto = rt.NewObject();
    rt.arrayBufferProto->cls = Cls::ArrayBuffer;
    Acc(rt.arrayBufferProto, L"byteLength", ArrayBufferP_byteLength, nullptr);
    M(rt.arrayBufferProto, L"slice", ArrayBufferP_slice, 2);
    rt.DefineData(rt.arrayBufferProto, L"\x01Symbol.toStringTag", Value::S(L"ArrayBuffer"), false);
    Obj* abCtor = Ctor(L"ArrayBuffer", ArrayBuffer_ctor, 1, rt.arrayBufferProto);
    M(abCtor, L"isView", ArrayBuffer_isView, 1);

    // 2. DataView
    rt.dataViewProto = rt.NewObject();
    rt.dataViewProto->cls = Cls::DataView;
    Acc(rt.dataViewProto, L"buffer", DataViewP_buffer, nullptr);
    Acc(rt.dataViewProto, L"byteLength", DataViewP_byteLength, nullptr);
    Acc(rt.dataViewProto, L"byteOffset", DataViewP_byteOffset, nullptr);
    M(rt.dataViewProto, L"getInt8", DataViewP_getInt8, 1);
    M(rt.dataViewProto, L"getUint8", DataViewP_getUint8, 1);
    M(rt.dataViewProto, L"setInt8", DataViewP_setInt8, 2);
    M(rt.dataViewProto, L"setUint8", DataViewP_setUint8, 2);
    M(rt.dataViewProto, L"getInt16", DataViewP_getInt16, 1);
    M(rt.dataViewProto, L"getUint16", DataViewP_getUint16, 1);
    M(rt.dataViewProto, L"setInt16", DataViewP_setInt16, 2);
    M(rt.dataViewProto, L"setUint16", DataViewP_setUint16, 2);
    M(rt.dataViewProto, L"getInt32", DataViewP_getInt32, 1);
    M(rt.dataViewProto, L"getUint32", DataViewP_getUint32, 1);
    M(rt.dataViewProto, L"setInt32", DataViewP_setInt32, 2);
    M(rt.dataViewProto, L"setUint32", DataViewP_setUint32, 2);
    M(rt.dataViewProto, L"getFloat32", DataViewP_getFloat32, 1);
    M(rt.dataViewProto, L"setFloat32", DataViewP_setFloat32, 2);
    M(rt.dataViewProto, L"getFloat64", DataViewP_getFloat64, 1);
    M(rt.dataViewProto, L"setFloat64", DataViewP_setFloat64, 2);
    rt.DefineData(rt.dataViewProto, L"\x01Symbol.toStringTag", Value::S(L"DataView"), false);
    Ctor(L"DataView", DataView_ctor, 1, rt.dataViewProto);

    // 3. Shared %TypedArray%.prototype
    rt.typedArrayProto = rt.NewObject();
    rt.typedArrayProto->cls = Cls::TypedArray;
    Obj* TP = rt.typedArrayProto;
    Acc(TP, L"buffer", TypedArrayP_buffer, nullptr);
    Acc(TP, L"byteLength", TypedArrayP_byteLength, nullptr);
    Acc(TP, L"byteOffset", TypedArrayP_byteOffset, nullptr);
    Acc(TP, L"length", TypedArrayP_length, nullptr);
    M(TP, L"set", TypedArrayP_set, 1);
    M(TP, L"subarray", TypedArrayP_subarray, 2);
    M(TP, L"slice", TypedArrayP_slice, 2);
    M(TP, L"fill", TypedArrayP_fill, 1);
    M(TP, L"indexOf", TypedArrayP_indexOf, 1);
    M(TP, L"includes", TypedArrayP_includes, 1);
    M(TP, L"join", TypedArrayP_join, 1);
    M(TP, L"toString", TypedArrayP_toString, 0);
    M(TP, L"reverse", TypedArrayP_reverse, 0);
    M(TP, L"forEach", TypedArrayP_forEach, 1);
    M(TP, L"map", TypedArrayP_map, 1);
    M(TP, L"filter", TypedArrayP_filter, 1);
    M(TP, L"reduce", TypedArrayP_reduce, 1);
    M(TP, L"some", TypedArrayP_some, 1);
    M(TP, L"every", TypedArrayP_every, 1);
    M(TP, L"find", TypedArrayP_find, 1);
    M(TP, L"findIndex", TypedArrayP_findIndex, 1);
    M(TP, L"values", TypedArrayP_values, 0);
    M(TP, L"keys", TypedArrayP_keys, 0);
    M(TP, L"entries", TypedArrayP_entries, 0);
    rt.DefineData(TP, L"\x01Symbol.iterator", Value::O(rt.NewFunction(TypedArrayP_values, L"[Symbol.iterator]", 0)), false);

    // 4. Concrete TypedArray constructors
    struct TypeDef { const wchar_t* name; NativeFn fn; TypedKind kind; };
    TypeDef types[] = {
        {L"Int8Array", Int8Array_ctor, TypedKind::Int8},
        {L"Uint8Array", Uint8Array_ctor, TypedKind::Uint8},
        {L"Uint8ClampedArray", Uint8ClampedArray_ctor, TypedKind::Uint8Clamped},
        {L"Int16Array", Int16Array_ctor, TypedKind::Int16},
        {L"Uint16Array", Uint16Array_ctor, TypedKind::Uint16},
        {L"Int32Array", Int32Array_ctor, TypedKind::Int32},
        {L"Uint32Array", Uint32Array_ctor, TypedKind::Uint32},
        {L"Float32Array", Float32Array_ctor, TypedKind::Float32},
        {L"Float64Array", Float64Array_ctor, TypedKind::Float64},
    };

    for (auto& td : types) {
        Obj* proto = rt.NewObject(rt.typedArrayProto);
        proto->cls = Cls::TypedArray;
        double bpe = double(ElementSize(td.kind));
        rt.DefineData(proto, L"BYTES_PER_ELEMENT", Value::N(bpe), false);
        rt.DefineData(proto, L"\x01Symbol.toStringTag", Value::S(td.name), false);
        Obj* c = Ctor(td.name, td.fn, 3, proto);
        rt.DefineData(c, L"BYTES_PER_ELEMENT", Value::N(bpe), false);
        M(c, L"from", TypedArray_from, 1);
        M(c, L"of", TypedArray_of, 0);
    }
}

// ================= TextEncoder / TextDecoder =================

NATIVE(TextEncoder_ctor) {
    UNUSED;
    Obj* proto = nullptr;
    auto it = rt.ctors.find(L"TextEncoder");
    if (it != rt.ctors.end()) {
        Value p = rt.Get(Value::O(it->second), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* obj = rt.NewObject(proto ? proto : rt.objectProto);
    return Value::O(obj);
}

NATIVE(TextEncoderP_encoding) {
    UNUSED;
    return Value::S(L"utf-8");
}

NATIVE(TextEncoderP_encode) {
    UNUSED;
    std::wstring str = args.empty() || args[0].IsUndef() ? L"" : rt.ToStr(A(0));
    std::string u8 = ToUtf8(str);
    Obj* ta = rt.NewTypedArray(TypedKind::Uint8, u8.size());
    if (!u8.empty() && ta->x && ta->x->bufferData) {
        memcpy(ta->x->bufferData->data(), u8.data(), u8.size());
    }
    return Value::O(ta);
}

NATIVE(TextEncoderP_encodeInto) {
    UNUSED;
    std::wstring str = args.empty() || args[0].IsUndef() ? L"" : rt.ToStr(A(0));
    Value destV = A(1);
    if (!destV.IsObj() || destV.o->cls != Cls::TypedArray || !destV.o->x || destV.o->x->typedKind != TypedKind::Uint8) {
        rt.Throw(L"TypeError", L"The second argument to encodeInto must be an instance of Uint8Array");
    }
    ObjExtra& destX = *destV.o->x;
    uint8_t* destPtr = destX.bufferData ? destX.bufferData->data() + destX.byteOffset : nullptr;
    size_t destLen = destX.typedLength;

    size_t read = 0;
    size_t written = 0;
    for (size_t i = 0; i < str.size(); i++) {
        uint32_t c = uint32_t(str[i]);
        size_t codeUnits = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.size()) {
            uint32_t d = uint32_t(str[i + 1]);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                codeUnits = 2;
            }
        }
        uint8_t buf[4];
        size_t bCount = 0;
        if (c < 0x80) {
            buf[0] = uint8_t(c);
            bCount = 1;
        } else if (c < 0x800) {
            buf[0] = uint8_t(0xC0 | (c >> 6));
            buf[1] = uint8_t(0x80 | (c & 0x3F));
            bCount = 2;
        } else if (c < 0x10000) {
            buf[0] = uint8_t(0xE0 | (c >> 12));
            buf[1] = uint8_t(0x80 | ((c >> 6) & 0x3F));
            buf[2] = uint8_t(0x80 | (c & 0x3F));
            bCount = 3;
        } else {
            buf[0] = uint8_t(0xF0 | (c >> 18));
            buf[1] = uint8_t(0x80 | ((c >> 12) & 0x3F));
            buf[2] = uint8_t(0x80 | ((c >> 6) & 0x3F));
            buf[3] = uint8_t(0x80 | (c & 0x3F));
            bCount = 4;
        }

        if (written + bCount > destLen) {
            break;
        }
        if (destPtr) {
            memcpy(destPtr + written, buf, bCount);
        }
        written += bCount;
        read += codeUnits;
        if (codeUnits == 2) i++;
    }

    Obj* res = rt.NewObject();
    rt.DefineData(res, L"read", Value::N(double(read)));
    rt.DefineData(res, L"written", Value::N(double(written)));
    return Value::O(res);
}

NATIVE(TextDecoder_ctor) {
    UNUSED;
    std::wstring label = args.empty() || args[0].IsUndef() ? L"utf-8" : rt.ToStr(args[0]);
    label = ToLower(Trim(label));
    if (label != L"utf-8" && label != L"utf8" && label != L"unicode-1-1-utf-8") {
        rt.Throw(L"RangeError", L"The encoding label provided is not supported");
    }
    bool fatal = false;
    bool ignoreBOM = false;
    if (args.size() > 1 && args[1].IsObj()) {
        Value fv = rt.Get(args[1], L"fatal");
        fatal = rt.Truthy(fv);
        Value bv = rt.Get(args[1], L"ignoreBOM");
        ignoreBOM = rt.Truthy(bv);
    }
    Obj* proto = nullptr;
    auto it = rt.ctors.find(L"TextDecoder");
    if (it != rt.ctors.end()) {
        Value p = rt.Get(Value::O(it->second), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* obj = rt.NewObject(proto ? proto : rt.objectProto);
    rt.DefineData(obj, L"\x02td_fatal", Value::B(fatal), false);
    rt.DefineData(obj, L"\x02td_ignoreBOM", Value::B(ignoreBOM), false);
    return Value::O(obj);
}

NATIVE(TextDecoderP_encoding) {
    UNUSED;
    return Value::S(L"utf-8");
}
NATIVE(TextDecoderP_fatal) {
    UNUSED;
    Value v = rt.Get(thisV, L"\x02td_fatal");
    return Value::B(rt.Truthy(v));
}
NATIVE(TextDecoderP_ignoreBOM) {
    UNUSED;
    Value v = rt.Get(thisV, L"\x02td_ignoreBOM");
    return Value::B(rt.Truthy(v));
}
NATIVE(TextDecoderP_decode) {
    UNUSED;
    if (args.empty() || A(0).IsUndef()) {
        return Value::S(L"");
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
        rt.Throw(L"TypeError", L"The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
    }

    bool fatal = rt.Truthy(rt.Get(thisV, L"\x02td_fatal"));
    bool ignoreBOM = rt.Truthy(rt.Get(thisV, L"\x02td_ignoreBOM"));
    bool hasErr = false;
    std::wstring res = (ptr && len > 0) ? FromUtf8(ptr, len, ignoreBOM, fatal, &hasErr) : L"";
    if (hasErr) {
        rt.Throw(L"TypeError", L"The encoded data was not valid utf-8");
    }
    return Value::S(res);
}

static void InstallEncoding(Runtime& rt) {
    Obj* g = rt.global;
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Ctor = [&](const wchar_t* name, NativeFn fn, int len, Obj* proto) {
        Obj* c = rt.NewFunction(fn, name, len, true);
        Prop& p = c->props.Put(L"prototype");
        p.v = Value::O(proto);
        p.enumerable = false;
        rt.DefineData(proto, L"constructor", Value::O(c), false);
        rt.DefineData(g, name, Value::O(c), false);
        rt.ctors[name] = c;
        return c;
    };

    // TextEncoder
    Obj* textEncoderProto = rt.NewObject(rt.objectProto);
    rt.DefineData(textEncoderProto, L"\x01Symbol.toStringTag", Value::S(L"TextEncoder"), false);
    rt.DefineAccessor(textEncoderProto, L"encoding", TextEncoderP_encoding, nullptr);
    M(textEncoderProto, L"encode", TextEncoderP_encode, 1);
    M(textEncoderProto, L"encodeInto", TextEncoderP_encodeInto, 2);
    Ctor(L"TextEncoder", TextEncoder_ctor, 0, textEncoderProto);

    // TextDecoder
    Obj* textDecoderProto = rt.NewObject(rt.objectProto);
    rt.DefineData(textDecoderProto, L"\x01Symbol.toStringTag", Value::S(L"TextDecoder"), false);
    rt.DefineAccessor(textDecoderProto, L"encoding", TextDecoderP_encoding, nullptr);
    rt.DefineAccessor(textDecoderProto, L"fatal", TextDecoderP_fatal, nullptr);
    rt.DefineAccessor(textDecoderProto, L"ignoreBOM", TextDecoderP_ignoreBOM, nullptr);
    M(textDecoderProto, L"decode", TextDecoderP_decode, 1);
    Ctor(L"TextDecoder", TextDecoder_ctor, 0, textDecoderProto);
}

void InstallBuiltins2(Runtime& rt) {
    Obj* g = rt.global;
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Ctor = [&](const wchar_t* name, NativeFn fn, int len, Obj* proto) {
        Obj* c = rt.NewFunction(fn, name, len, true);
        Prop& p = c->props.Put(L"prototype");
        p.v = Value::O(proto);
        p.enumerable = false;
        rt.DefineData(proto, L"constructor", Value::O(c), false);
        rt.DefineData(g, name, Value::O(c), false);
        rt.ctors[name] = c;
        return c;
    };

    Ctor(L"RegExp", RegExp_ctor, 2, rt.regexpProto);
    Obj* RP = rt.regexpProto;
    M(RP, L"exec", RegExpP_exec, 1); M(RP, L"test", RegExpP_test, 1); M(RP, L"toString", RegExpP_toString, 0);
    rt.DefineAccessor(RP, L"source", RegExpP_source, nullptr);
    rt.DefineAccessor(RP, L"flags", RegExpP_flags, nullptr);
    rt.DefineAccessor(RP, L"global", RegExpP_global, nullptr);
    rt.DefineAccessor(RP, L"ignoreCase", RegExpP_ignoreCase, nullptr);
    rt.DefineAccessor(RP, L"multiline", RegExpP_multiline, nullptr);
    rt.DefineAccessor(RP, L"sticky", RegExpP_sticky, nullptr);
    rt.DefineAccessor(RP, L"unicode", RegExpP_unicode, nullptr);
    rt.DefineAccessor(RP, L"dotAll", RegExpP_dotAll, nullptr);

    Obj* SP = rt.stringProto;
    M(SP, L"match", StringP_match, 1); M(SP, L"matchAll", StringP_matchAll, 1); M(SP, L"search", StringP_search, 1);
    M(SP, L"replace", StringP_replace, 2); M(SP, L"replaceAll", StringP_replaceAll, 2); M(SP, L"split", StringP_split, 2);

    Obj* DC = Ctor(L"Date", Date_ctor, 7, rt.dateProto);
    M(DC, L"now", Date_now, 0); M(DC, L"parse", Date_parse, 1); M(DC, L"UTC", Date_UTC, 7);
    Obj* DP = rt.dateProto;
    struct { const wchar_t* n; NativeFn f; int l; } dm[] = {
        {L"getTime", DateP_getTime, 0}, {L"valueOf", DateP_getTime, 0},
        {L"getFullYear", DateP_getFullYear, 0}, {L"getMonth", DateP_getMonth, 0}, {L"getDate", DateP_getDate, 0},
        {L"getDay", DateP_getDay, 0}, {L"getHours", DateP_getHours, 0}, {L"getMinutes", DateP_getMinutes, 0},
        {L"getSeconds", DateP_getSeconds, 0}, {L"getMilliseconds", DateP_getMilliseconds, 0}, {L"getYear", DateP_getYear, 0},
        {L"getUTCFullYear", DateP_getUTCFullYear, 0}, {L"getUTCMonth", DateP_getUTCMonth, 0}, {L"getUTCDate", DateP_getUTCDate, 0},
        {L"getUTCDay", DateP_getUTCDay, 0}, {L"getUTCHours", DateP_getUTCHours, 0}, {L"getUTCMinutes", DateP_getUTCMinutes, 0},
        {L"getUTCSeconds", DateP_getUTCSeconds, 0}, {L"getUTCMilliseconds", DateP_getUTCMilliseconds, 0},
        {L"getTimezoneOffset", DateP_getTimezoneOffset, 0}, {L"setTime", DateP_setTime, 1},
        {L"setFullYear", DateP_setFullYear, 3}, {L"setMonth", DateP_setMonth, 2}, {L"setDate", DateP_setDate, 1},
        {L"setHours", DateP_setHours, 4}, {L"setMinutes", DateP_setMinutes, 3}, {L"setSeconds", DateP_setSeconds, 2},
        {L"setMilliseconds", DateP_setMilliseconds, 1}, {L"setUTCFullYear", DateP_setUTCFullYear, 3},
        {L"setUTCMonth", DateP_setUTCMonth, 2}, {L"setUTCDate", DateP_setUTCDate, 1}, {L"setUTCHours", DateP_setUTCHours, 4},
        {L"setUTCMinutes", DateP_setUTCMinutes, 3}, {L"setUTCSeconds", DateP_setUTCSeconds, 2},
        {L"setUTCMilliseconds", DateP_setUTCMilliseconds, 1}, {L"toISOString", DateP_toISOString, 0},
        {L"toJSON", DateP_toJSON, 1}, {L"toString", DateP_toString, 0}, {L"toDateString", DateP_toDateString, 0},
        {L"toTimeString", DateP_toTimeString, 0}, {L"toUTCString", DateP_toUTCString, 0}, {L"toGMTString", DateP_toUTCString, 0},
        {L"toLocaleDateString", DateP_toLocaleDateString, 0}, {L"toLocaleTimeString", DateP_toLocaleTimeString, 0},
        {L"toLocaleString", DateP_toLocaleString, 0}};
    for (auto& d : dm) M(DP, d.n, d.f, d.l);

    M(g, L"encodeURIComponent", Global_encodeURIComponent, 1);
    M(g, L"encodeURI", Global_encodeURI, 1);
    M(g, L"decodeURIComponent", Global_decodeURIComponent, 1);
    M(g, L"decodeURI", Global_decodeURI, 1);
    M(g, L"escape", Global_escape, 1);
    M(g, L"unescape", Global_unescape, 1);
    M(g, L"btoa", Global_btoa, 1);
    M(g, L"atob", Global_atob, 1);

    InstallTypedArrays(rt);
    InstallEncoding(rt);
}

} // namespace js
} // namespace mer
