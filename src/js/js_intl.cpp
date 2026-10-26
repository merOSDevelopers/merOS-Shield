// merOS Shield - merJS : ECMAScript Internationalization API (ECMA-402 / Intl)
#include "js_intl.h"
#include <windows.h>
#include <winnls.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace mer {
namespace js {

namespace {

#define A(i) (args.size() > (i) ? args[i] : Value())
#define NATIVE(name) Value name(Runtime& rt, Obj* self, const Value& thisV, std::vector<Value>& args)
#define UNUSED (void)self; (void)thisV; (void)args; (void)rt

static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    return s;
}

static std::wstring ToUpper(std::wstring s) {
    for (auto& c : s) if (c >= L'a' && c <= L'z') c = c - L'a' + L'A';
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
    size_t end = s.size();
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) end--;
    return s.substr(start, end - start);
}

// BCP 47 / RFC 5646 Canonicalizer
static bool CanonicalizeLocaleTag(Runtime& rt, const std::wstring& inTag, std::wstring& outTag, bool throwOnErr = true) {
    std::wstring tag = Trim(inTag);
    if (tag.empty()) {
        if (throwOnErr) rt.Throw(L"RangeError", L"Invalid language tag: empty tag");
        return false;
    }
    std::replace(tag.begin(), tag.end(), L'_', L'-');

    std::vector<std::wstring> subtags;
    size_t pos = 0;
    while (pos < tag.size()) {
        size_t next = tag.find(L'-', pos);
        if (next == std::wstring::npos) next = tag.size();
        std::wstring sub = tag.substr(pos, next - pos);
        if (sub.empty() || sub.size() > 8) {
            if (throwOnErr) rt.Throw(L"RangeError", L"Invalid language tag: " + inTag);
            return false;
        }
        for (wchar_t ch : sub) {
            if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))) {
                if (throwOnErr) rt.Throw(L"RangeError", L"Invalid language tag: " + inTag);
                return false;
            }
        }
        subtags.push_back(sub);
        pos = next + 1;
    }

    if (subtags.empty()) {
        if (throwOnErr) rt.Throw(L"RangeError", L"Invalid language tag: " + inTag);
        return false;
    }

    // Subtag 0: Language (2-3 alpha)
    std::wstring lang = subtags[0];
    if (lang.size() != 2 && lang.size() != 3 && lang != L"und") {
        if (throwOnErr) rt.Throw(L"RangeError", L"Invalid language subtag in: " + inTag);
        return false;
    }
    for (wchar_t ch : lang) {
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))) {
            if (throwOnErr) rt.Throw(L"RangeError", L"Invalid language subtag in: " + inTag);
            return false;
        }
    }
    outTag = ToLower(lang);

    for (size_t i = 1; i < subtags.size(); i++) {
        std::wstring sub = subtags[i];
        if (sub.size() == 4) {
            // Script: Titlecase (e.g. Latn, Hans)
            sub[0] = (sub[0] >= L'a' && sub[0] <= L'z') ? wchar_t(sub[0] - L'a' + L'A') : sub[0];
            for (size_t k = 1; k < sub.size(); k++) {
                if (sub[k] >= L'A' && sub[k] <= L'Z') sub[k] = wchar_t(sub[k] - L'A' + L'a');
            }
            outTag += L"-" + sub;
        } else if (sub.size() == 2 || (sub.size() == 3 && sub[0] >= L'0' && sub[0] <= L'9')) {
            // Region: Uppercase (e.g. TR, US, 419)
            outTag += L"-" + ToUpper(sub);
        } else {
            outTag += L"-" + ToLower(sub);
        }
    }
    return true;
}

static std::wstring GetSystemDefaultLocale() {
    WCHAR buf[LOCALE_NAME_MAX_LENGTH] = { 0 };
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0) {
        return buf;
    }
    return L"en-US";
}

static std::wstring ResolveSingleLocale(Runtime& rt, const Value& v) {
    if (v.IsNullish()) return GetSystemDefaultLocale();
    if (v.IsStr()) {
        std::wstring res;
        CanonicalizeLocaleTag(rt, *v.s, res, true);
        return res;
    }
    if (v.IsObj()) {
        Value lenVal = rt.Get(v, L"length");
        if (lenVal.IsNum() && lenVal.n > 0) {
            Value first = rt.Get(v, L"0");
            if (first.IsStr()) {
                std::wstring res;
                CanonicalizeLocaleTag(rt, *first.s, res, true);
                return res;
            }
        }
    }
    return GetSystemDefaultLocale();
}

static std::wstring GetOptStr(Runtime& rt, Obj* opts, const wchar_t* name, const std::wstring& defVal = L"") {
    if (!opts) return defVal;
    Value v = rt.Get(Value::O(opts), name);
    if (v.IsUndef()) return defVal;
    return rt.ToStr(v);
}

static int GetOptInt(Runtime& rt, Obj* opts, const wchar_t* name, int defVal) {
    if (!opts) return defVal;
    Value v = rt.Get(Value::O(opts), name);
    if (v.IsUndef()) return defVal;
    return int(rt.ToNum(v));
}

static bool GetOptBool(Runtime& rt, Obj* opts, const wchar_t* name, bool defVal) {
    if (!opts) return defVal;
    Value v = rt.Get(Value::O(opts), name);
    if (v.IsUndef()) return defVal;
    return rt.Truthy(v);
}

// ================= Intl.getCanonicalLocales =================

NATIVE(Intl_getCanonicalLocales) {
    UNUSED;
    Obj* arr = rt.NewArray(0);
    Value locs = A(0);
    if (locs.IsUndef()) return Value::O(arr);

    std::vector<std::wstring> rawTags;
    if (locs.IsStr()) {
        rawTags.push_back(*locs.s);
    } else if (locs.IsObj()) {
        Value lenVal = rt.Get(locs, L"length");
        if (lenVal.IsNum()) {
            size_t len = size_t(std::max(0.0, lenVal.n));
            for (size_t i = 0; i < len; i++) {
                Value item = rt.Get(locs, std::to_wstring(i));
                rawTags.push_back(rt.ToStr(item));
            }
        }
    } else {
        rt.Throw(L"TypeError", L"The locales argument must be a string or an array of strings");
    }

    std::vector<std::wstring> seen;
    for (const auto& raw : rawTags) {
        std::wstring can;
        CanonicalizeLocaleTag(rt, raw, can, true);
        if (std::find(seen.begin(), seen.end(), can) == seen.end()) {
            seen.push_back(can);
            arr->elems.push_back(Value::S(can));
        }
    }
    return Value::O(arr);
}

// ================= Intl.DateTimeFormat =================

static SYSTEMTIME MsToSystemTime(double ms, bool utc) {
    int64_t totalSec = int64_t(ms / 1000.0);
    int remMs = int(std::fmod(ms, 1000.0));
    if (remMs < 0) remMs += 1000;

    time_t raw = time_t(totalSec);
    struct tm ti;
    if (utc) {
#if defined(_WIN32)
        gmtime_s(&ti, &raw);
#else
        gmtime_r(&raw, &ti);
#endif
    } else {
#if defined(_WIN32)
        localtime_s(&ti, &raw);
#else
        localtime_r(&raw, &ti);
#endif
    }

    SYSTEMTIME st;
    st.wYear = WORD(ti.tm_year + 1900);
    st.wMonth = WORD(ti.tm_mon + 1);
    st.wDay = WORD(ti.tm_mday);
    st.wDayOfWeek = WORD(ti.tm_wday);
    st.wHour = WORD(ti.tm_hour);
    st.wMinute = WORD(ti.tm_min);
    st.wSecond = WORD(ti.tm_sec);
    st.wMilliseconds = WORD(remMs);
    return st;
}

static Obj* CreateIntlInstance(Runtime& rt, const wchar_t* ctorName) {
    Obj* proto = nullptr;
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    if (!proto && ctorName) {
        auto it = rt.ctors.find(ctorName);
        if (it != rt.ctors.end()) {
            Value p = rt.Get(Value::O(it->second), L"prototype");
            if (p.IsObj()) proto = p.o;
        }
    }
    return rt.NewObject(proto ? proto : rt.objectProto);
}

NATIVE(DateTimeFormat_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"DateTimeFormat");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);

    Obj* optsObj = A(1).IsObj() ? A(1).o : nullptr;
    std::wstring dateStyle = GetOptStr(rt, optsObj, L"dateStyle");
    std::wstring timeStyle = GetOptStr(rt, optsObj, L"timeStyle");
    std::wstring timeZone = GetOptStr(rt, optsObj, L"timeZone", L"");
    std::wstring year = GetOptStr(rt, optsObj, L"year", (dateStyle.empty() && timeStyle.empty()) ? L"numeric" : L"");
    std::wstring month = GetOptStr(rt, optsObj, L"month", (dateStyle.empty() && timeStyle.empty()) ? L"numeric" : L"");
    std::wstring day = GetOptStr(rt, optsObj, L"day", (dateStyle.empty() && timeStyle.empty()) ? L"numeric" : L"");
    std::wstring hour = GetOptStr(rt, optsObj, L"hour");
    std::wstring minute = GetOptStr(rt, optsObj, L"minute");
    std::wstring second = GetOptStr(rt, optsObj, L"second");
    std::wstring weekday = GetOptStr(rt, optsObj, L"weekday");

    rt.DefineData(o, L"\x02dateStyle", Value::S(dateStyle), false);
    rt.DefineData(o, L"\x02timeStyle", Value::S(timeStyle), false);
    rt.DefineData(o, L"\x02timeZone", Value::S(timeZone), false);
    rt.DefineData(o, L"\x02year", Value::S(year), false);
    rt.DefineData(o, L"\x02month", Value::S(month), false);
    rt.DefineData(o, L"\x02day", Value::S(day), false);
    rt.DefineData(o, L"\x02hour", Value::S(hour), false);
    rt.DefineData(o, L"\x02minute", Value::S(minute), false);
    rt.DefineData(o, L"\x02second", Value::S(second), false);
    rt.DefineData(o, L"\x02weekday", Value::S(weekday), false);

    return Value::O(o);
}

static std::wstring GetFieldStr(Runtime& rt, const Value& thisV, const wchar_t* key, const std::wstring& def = L"") {
    Value v = rt.Get(thisV, key);
    return v.IsStr() ? *v.s : def;
}

static std::wstring FormatDateTimeInternal(Runtime& rt, const Value& thisV, double t) {
    if (std::isnan(t) || std::isinf(t)) {
        rt.Throw(L"RangeError", L"Invalid time value");
    }

    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring tz = GetFieldStr(rt, thisV, L"\x02timeZone");
    std::wstring dateStyle = GetFieldStr(rt, thisV, L"\x02dateStyle");
    std::wstring timeStyle = GetFieldStr(rt, thisV, L"\x02timeStyle");
    std::wstring yearOpt = GetFieldStr(rt, thisV, L"\x02year");
    std::wstring monthOpt = GetFieldStr(rt, thisV, L"\x02month");
    std::wstring dayOpt = GetFieldStr(rt, thisV, L"\x02day");
    std::wstring hourOpt = GetFieldStr(rt, thisV, L"\x02hour");
    std::wstring minuteOpt = GetFieldStr(rt, thisV, L"\x02minute");
    std::wstring secondOpt = GetFieldStr(rt, thisV, L"\x02second");

    bool isUtc = (tz == L"UTC" || tz == L"Etc/UTC");
    SYSTEMTIME st = MsToSystemTime(t, isUtc);

    DWORD dFlags = DATE_SHORTDATE;
    if (dateStyle == L"long" || dateStyle == L"full") dFlags = DATE_LONGDATE;

    WCHAR dateBuf[128] = { 0 };
    WCHAR timeBuf[128] = { 0 };

    bool hasDate = !dateStyle.empty() || !yearOpt.empty() || !monthOpt.empty() || !dayOpt.empty();
    bool hasTime = !timeStyle.empty() || !hourOpt.empty() || !minuteOpt.empty() || !secondOpt.empty();

    if (hasDate) {
        if (!GetDateFormatEx(loc.c_str(), dFlags, &st, NULL, dateBuf, 128, NULL)) {
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, dFlags, &st, NULL, dateBuf, 128, NULL);
        }
    }
    if (hasTime) {
        if (!GetTimeFormatEx(loc.c_str(), 0, &st, NULL, timeBuf, 128)) {
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, NULL, timeBuf, 128);
        }
    }

    if (hasDate && hasTime) {
        return std::wstring(dateBuf) + L" " + timeBuf;
    }
    if (hasDate) return dateBuf;
    if (hasTime) return timeBuf;
    return dateBuf;
}

NATIVE(DateTimeFormatP_format) {
    UNUSED;
    double t = 0;
    if (args.empty() || A(0).IsUndef()) {
        t = double(time(nullptr)) * 1000.0;
    } else if (A(0).IsObj() && A(0).o->cls == Cls::Date) {
        t = A(0).o->x ? A(0).o->x->num : 0.0;
    } else {
        t = rt.ToNum(A(0));
    }
    return Value::S(FormatDateTimeInternal(rt, thisV, t));
}

NATIVE(DateTimeFormatP_formatToParts) {
    UNUSED;
    double t = args.empty() || A(0).IsUndef() ? double(time(nullptr)) * 1000.0 : rt.ToNum(A(0));
    std::wstring formatted = FormatDateTimeInternal(rt, thisV, t);
    Obj* arr = rt.NewArray(0);

    // Basit ve deterministik part tokenizer
    std::wstring cur;
    auto PushPart = [&](const std::wstring& type, const std::wstring& val) {
        if (val.empty()) return;
        Obj* p = rt.NewObject();
        rt.DefineData(p, L"type", Value::S(type));
        rt.DefineData(p, L"value", Value::S(val));
        arr->elems.push_back(Value::O(p));
    };

    for (size_t i = 0; i < formatted.size(); i++) {
        wchar_t ch = formatted[i];
        if (ch >= L'0' && ch <= L'9') {
            cur += ch;
        } else {
            if (!cur.empty()) {
                if (cur.size() == 4) PushPart(L"year", cur);
                else PushPart(L"numeric", cur);
                cur.clear();
            }
            PushPart(L"literal", std::wstring(1, ch));
        }
    }
    if (!cur.empty()) {
        if (cur.size() == 4) PushPart(L"year", cur);
        else PushPart(L"numeric", cur);
    }
    return Value::O(arr);
}

NATIVE(DateTimeFormatP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"calendar", Value::S(L"gregory"));
    rt.DefineData(o, L"numberingSystem", Value::S(L"latn"));
    rt.DefineData(o, L"timeZone", rt.Get(thisV, L"\x02timeZone"));
    rt.DefineData(o, L"year", rt.Get(thisV, L"\x02year"));
    rt.DefineData(o, L"month", rt.Get(thisV, L"\x02month"));
    rt.DefineData(o, L"day", rt.Get(thisV, L"\x02day"));
    return Value::O(o);
}

NATIVE(DateTimeFormat_supportedLocalesOf) {
    UNUSED;
    return Intl_getCanonicalLocales(rt, self, thisV, args);
}

// ================= Intl.NumberFormat =================

static std::wstring FormatNumberInternal(Runtime& rt, const Value& thisV, double num) {
    if (std::isnan(num)) return L"NaN";
    if (std::isinf(num)) return num > 0 ? L"Infinity" : L"-Infinity";

    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring style = GetFieldStr(rt, thisV, L"\x02style", L"decimal");
    std::wstring currency = GetFieldStr(rt, thisV, L"\x02currency");
    std::wstring currDisp = GetFieldStr(rt, thisV, L"\x02currencyDisplay", L"symbol");
    Value minFracV = rt.Get(thisV, L"\x02minFrac");
    int minFrac = minFracV.IsNum() ? int(minFracV.n) : 0;
    Value maxFracV = rt.Get(thisV, L"\x02maxFrac");
    int maxFrac = maxFracV.IsNum() ? int(maxFracV.n) : 3;
    bool useGrouping = rt.Truthy(rt.Get(thisV, L"\x02useGrouping"));

    if (style == L"percent") num *= 100.0;

    std::wostringstream ss;
    ss << std::fixed << std::setprecision(maxFrac) << num;
    std::wstring valStr = ss.str();

    NUMBERFMTW nf = { 0 };
    nf.NumDigits = UINT(maxFrac);
    nf.LeadingZero = 1;
    nf.Grouping = useGrouping ? 3 : 0;

    WCHAR decimalSep[4] = { L'.' };
    WCHAR thousandSep[4] = { L',' };
    GetLocaleInfoEx(loc.c_str(), LOCALE_SDECIMAL, decimalSep, 4);
    GetLocaleInfoEx(loc.c_str(), LOCALE_STHOUSAND, thousandSep, 4);

    nf.lpDecimalSep = decimalSep;
    nf.lpThousandSep = thousandSep;
    nf.NegativeOrder = 1;

    WCHAR outBuf[128] = { 0 };
    if (!GetNumberFormatEx(loc.c_str(), 0, valStr.c_str(), &nf, outBuf, 128)) {
        GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, valStr.c_str(), &nf, outBuf, 128);
    }
    std::wstring res = outBuf;

    // Minimum fraction trailing zero handling
    size_t dotPos = res.rfind(decimalSep);
    if (dotPos != std::wstring::npos) {
        size_t curFrac = res.size() - dotPos - 1;
        while (int(curFrac) > minFrac && res.back() == L'0') {
            res.pop_back();
            curFrac--;
        }
        if (curFrac == 0 && res.back() == decimalSep[0]) {
            res.pop_back();
        }
    }

    if (style == L"percent") {
        res += L"%";
    } else if (style == L"currency") {
        std::wstring sym = currency;
        if (currDisp == L"symbol") {
            if (currency == L"USD") sym = L"$";
            else if (currency == L"EUR") sym = L"€";
            else if (currency == L"TRY") sym = L"₺";
            else if (currency == L"GBP") sym = L"£";
            else if (currency == L"JPY") sym = L"¥";
        }
        res = sym + L" " + res;
    }
    return res;
}

NATIVE(NumberFormat_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"NumberFormat");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);

    Obj* opts = A(1).IsObj() ? A(1).o : nullptr;
    std::wstring style = GetOptStr(rt, opts, L"style", L"decimal");
    std::wstring currency = GetOptStr(rt, opts, L"currency", L"");
    std::wstring currDisp = GetOptStr(rt, opts, L"currencyDisplay", L"symbol");
    bool useGrouping = GetOptBool(rt, opts, L"useGrouping", true);
    int minInt = GetOptInt(rt, opts, L"minimumIntegerDigits", 1);
    int minFrac = GetOptInt(rt, opts, L"minimumFractionDigits", (style == L"currency") ? 2 : 0);
    int maxFrac = GetOptInt(rt, opts, L"maximumFractionDigits", (style == L"currency") ? 2 : (style == L"percent" ? 0 : 3));

    if (style == L"currency" && currency.empty()) {
        rt.Throw(L"TypeError", L"Currency code is required with currency style");
    }

    rt.DefineData(o, L"\x02style", Value::S(style), false);
    rt.DefineData(o, L"\x02currency", Value::S(ToUpper(currency)), false);
    rt.DefineData(o, L"\x02currencyDisplay", Value::S(currDisp), false);
    rt.DefineData(o, L"\x02useGrouping", Value::B(useGrouping), false);
    rt.DefineData(o, L"\x02minInt", Value::N(minInt), false);
    rt.DefineData(o, L"\x02minFrac", Value::N(minFrac), false);
    rt.DefineData(o, L"\x02maxFrac", Value::N(maxFrac), false);

    return Value::O(o);
}

NATIVE(NumberFormatP_format) {
    UNUSED;
    double n = rt.ToNum(A(0));
    return Value::S(FormatNumberInternal(rt, thisV, n));
}

NATIVE(NumberFormatP_formatToParts) {
    UNUSED;
    double n = rt.ToNum(A(0));
    std::wstring formatted = FormatNumberInternal(rt, thisV, n);
    Obj* arr = rt.NewArray(0);

    Obj* p1 = rt.NewObject();
    rt.DefineData(p1, L"type", Value::S(L"integer"));
    rt.DefineData(p1, L"value", Value::S(formatted));
    arr->elems.push_back(Value::O(p1));
    return Value::O(arr);
}

NATIVE(NumberFormatP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"numberingSystem", Value::S(L"latn"));
    rt.DefineData(o, L"style", rt.Get(thisV, L"\x02style"));
    rt.DefineData(o, L"currency", rt.Get(thisV, L"\x02currency"));
    rt.DefineData(o, L"minimumFractionDigits", rt.Get(thisV, L"\x02minFrac"));
    rt.DefineData(o, L"maximumFractionDigits", rt.Get(thisV, L"\x02maxFrac"));
    rt.DefineData(o, L"useGrouping", rt.Get(thisV, L"\x02useGrouping"));
    return Value::O(o);
}

NATIVE(NumberFormat_supportedLocalesOf) {
    UNUSED;
    return Intl_getCanonicalLocales(rt, self, thisV, args);
}

// ================= Intl.Collator =================

NATIVE(Collator_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"Collator");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);

    Obj* opts = A(1).IsObj() ? A(1).o : nullptr;
    std::wstring sens = GetOptStr(rt, opts, L"sensitivity", L"variant");
    std::wstring usage = GetOptStr(rt, opts, L"usage", L"sort");
    bool numeric = GetOptBool(rt, opts, L"numeric", false);
    bool ignorePunc = GetOptBool(rt, opts, L"ignorePunctuation", false);

    rt.DefineData(o, L"\x02sensitivity", Value::S(sens), false);
    rt.DefineData(o, L"\x02usage", Value::S(usage), false);
    rt.DefineData(o, L"\x02numeric", Value::B(numeric), false);
    rt.DefineData(o, L"\x02ignorePunctuation", Value::B(ignorePunc), false);

    return Value::O(o);
}

NATIVE(CollatorP_compare) {
    UNUSED;
    std::wstring s1 = rt.ToStr(A(0));
    std::wstring s2 = rt.ToStr(A(1));
    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring sens = GetFieldStr(rt, thisV, L"\x02sensitivity", L"variant");
    bool num = rt.Truthy(rt.Get(thisV, L"\x02numeric"));
    bool punc = rt.Truthy(rt.Get(thisV, L"\x02ignorePunctuation"));

    DWORD flags = LINGUISTIC_IGNORECASE;
    if (sens == L"base") flags |= (NORM_IGNORECASE | NORM_IGNORENONSPACE);
    else if (sens == L"accent") flags |= NORM_IGNORECASE;
    else if (sens == L"case") flags |= NORM_IGNORENONSPACE;
    else if (sens == L"variant") flags = 0;

    if (num) flags |= SORT_DIGITSASNUMBERS;
    if (punc) flags |= NORM_IGNORESYMBOLS;

    int r = CompareStringEx(loc.c_str(), flags, s1.c_str(), int(s1.size()), s2.c_str(), int(s2.size()), NULL, NULL, 0);
    if (r == 0) {
        r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, flags, s1.c_str(), int(s1.size()), s2.c_str(), int(s2.size()), NULL, NULL, 0);
    }
    int cmp = 0;
    if (r == CSTR_LESS_THAN) cmp = -1;
    else if (r == CSTR_GREATER_THAN) cmp = 1;
    return Value::N(cmp);
}

NATIVE(CollatorP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"usage", rt.Get(thisV, L"\x02usage"));
    rt.DefineData(o, L"sensitivity", rt.Get(thisV, L"\x02sensitivity"));
    rt.DefineData(o, L"numeric", rt.Get(thisV, L"\x02numeric"));
    rt.DefineData(o, L"ignorePunctuation", rt.Get(thisV, L"\x02ignorePunctuation"));
    rt.DefineData(o, L"collation", Value::S(L"default"));
    return Value::O(o);
}

NATIVE(Collator_supportedLocalesOf) {
    UNUSED;
    return Intl_getCanonicalLocales(rt, self, thisV, args);
}

// ================= Intl.PluralRules =================

NATIVE(PluralRules_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"PluralRules");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);
    Obj* opts = A(1).IsObj() ? A(1).o : nullptr;
    std::wstring type = GetOptStr(rt, opts, L"type", L"cardinal");
    rt.DefineData(o, L"\x02type", Value::S(type), false);
    return Value::O(o);
}

NATIVE(PluralRulesP_select) {
    UNUSED;
    double n = std::fabs(rt.ToNum(A(0)));
    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring lang = loc.substr(0, loc.find(L'-'));

    if (lang == L"tr" || lang == L"ja" || lang == L"zh" || lang == L"ko") {
        return Value::S(L"other");
    }
    if (lang == L"en" || lang == L"de" || lang == L"nl" || lang == L"it") {
        if (n == 1.0) return Value::S(L"one");
        return Value::S(L"other");
    }
    if (lang == L"fr" || lang == L"pt") {
        if (n >= 0.0 && n < 2.0) return Value::S(L"one");
        return Value::S(L"other");
    }
    if (lang == L"ru" || lang == L"uk") {
        int64_t i = int64_t(n);
        int64_t mod10 = i % 10;
        int64_t mod100 = i % 100;
        if (mod10 == 1 && mod100 != 11) return Value::S(L"one");
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 10 || mod100 >= 20)) return Value::S(L"few");
        return Value::S(L"many");
    }
    if (n == 1.0) return Value::S(L"one");
    return Value::S(L"other");
}

NATIVE(PluralRulesP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"type", rt.Get(thisV, L"\x02type"));
    return Value::O(o);
}

NATIVE(PluralRules_supportedLocalesOf) {
    UNUSED;
    return Intl_getCanonicalLocales(rt, self, thisV, args);
}

// ================= Intl.RelativeTimeFormat =================

NATIVE(RelativeTimeFormat_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"RelativeTimeFormat");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);
    Obj* opts = A(1).IsObj() ? A(1).o : nullptr;
    std::wstring style = GetOptStr(rt, opts, L"style", L"long");
    std::wstring numOpt = GetOptStr(rt, opts, L"numeric", L"always");
    rt.DefineData(o, L"\x02style", Value::S(style), false);
    rt.DefineData(o, L"\x02numeric", Value::S(numOpt), false);
    return Value::O(o);
}

NATIVE(RelativeTimeFormatP_format) {
    UNUSED;
    double val = rt.ToNum(A(0));
    std::wstring unit = ToLower(rt.ToStr(A(1)));
    if (!unit.empty() && unit.back() == L's') unit.pop_back();

    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring numOpt = GetFieldStr(rt, thisV, L"\x02numeric", L"always");
    std::wstring lang = loc.substr(0, loc.find(L'-'));

    // Otomatik goreceli isimlendirme (auto)
    if (numOpt == L"auto") {
        if (unit == L"day") {
            if (val == 0) return Value::S(lang == L"tr" ? L"bugün" : L"today");
            if (val == -1) return Value::S(lang == L"tr" ? L"dün" : L"yesterday");
            if (val == 1) return Value::S(lang == L"tr" ? L"yarın" : L"tomorrow");
        }
    }

    std::wstring absValStr = std::to_wstring(int64_t(std::fabs(val)));
    if (lang == L"tr") {
        std::wstring trUnit = unit == L"second" ? L"saniye" :
                              unit == L"minute" ? L"dakika" :
                              unit == L"hour" ? L"saat" :
                              unit == L"day" ? L"gün" :
                              unit == L"week" ? L"hafta" :
                              unit == L"month" ? L"ay" :
                              unit == L"year" ? L"yıl" : unit;
        if (val < 0) return Value::S(absValStr + L" " + trUnit + L" önce");
        return Value::S(absValStr + L" " + trUnit + L" sonra");
    }

    // Default: English
    std::wstring enUnit = unit;
    if (std::fabs(val) != 1.0) enUnit += L"s";
    if (val < 0) return Value::S(absValStr + L" " + enUnit + L" ago");
    return Value::S(L"in " + absValStr + L" " + enUnit);
}

NATIVE(RelativeTimeFormatP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"style", rt.Get(thisV, L"\x02style"));
    rt.DefineData(o, L"numeric", rt.Get(thisV, L"\x02numeric"));
    return Value::O(o);
}

// ================= Intl.ListFormat =================

NATIVE(ListFormat_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"ListFormat");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);
    return Value::O(o);
}

NATIVE(ListFormatP_format) {
    UNUSED;
    Value list = A(0);
    std::vector<std::wstring> items;
    if (list.IsObj()) {
        Value lenVal = rt.Get(list, L"length");
        if (lenVal.IsNum()) {
            size_t len = size_t(std::max(0.0, lenVal.n));
            for (size_t i = 0; i < len; i++) {
                items.push_back(rt.ToStr(rt.Get(list, std::to_wstring(i))));
            }
        }
    }
    if (items.empty()) return Value::S(L"");
    if (items.size() == 1) return Value::S(items[0]);
    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring lang = loc.substr(0, loc.find(L'-'));
    std::wstring conj = (lang == L"tr") ? L" ve " : L", and ";

    if (items.size() == 2) {
        return Value::S(items[0] + ((lang == L"tr") ? L" ve " : L" and ") + items[1]);
    }
    std::wstring res;
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) {
            if (i == items.size() - 1) res += conj;
            else res += L", ";
        }
        res += items[i];
    }
    return Value::S(res);
}

NATIVE(ListFormatP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"type", Value::S(L"conjunction"));
    return Value::O(o);
}

// ================= Intl.DisplayNames =================

NATIVE(DisplayNames_ctor) {
    UNUSED;
    Obj* o = (thisV.IsObj() && thisV.o && thisV.o != rt.global) ? thisV.o : CreateIntlInstance(rt, L"DisplayNames");
    std::wstring loc = ResolveSingleLocale(rt, A(0));
    rt.DefineData(o, L"\x02locale", Value::S(loc), false);
    Obj* opts = A(1).IsObj() ? A(1).o : nullptr;
    std::wstring type = GetOptStr(rt, opts, L"type", L"language");
    rt.DefineData(o, L"\x02type", Value::S(type), false);
    return Value::O(o);
}

NATIVE(DisplayNamesP_of) {
    UNUSED;
    std::wstring code = rt.ToStr(A(0));
    std::wstring loc = GetFieldStr(rt, thisV, L"\x02locale", GetSystemDefaultLocale());
    std::wstring type = GetFieldStr(rt, thisV, L"\x02type", L"language");

    // Basit dil isim haritalamasi
    if (type == L"language") {
        if (code == L"en") return Value::S(loc.rfind(L"tr", 0) == 0 ? L"İngilizce" : L"English");
        if (code == L"tr") return Value::S(loc.rfind(L"tr", 0) == 0 ? L"Türkçe" : L"Turkish");
        if (code == L"de") return Value::S(loc.rfind(L"tr", 0) == 0 ? L"Almanca" : L"German");
        if (code == L"fr") return Value::S(loc.rfind(L"tr", 0) == 0 ? L"Fransızca" : L"French");
    }
    return Value::S(code);
}

NATIVE(DisplayNamesP_resolvedOptions) {
    UNUSED;
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"locale", rt.Get(thisV, L"\x02locale"));
    rt.DefineData(o, L"type", rt.Get(thisV, L"\x02type"));
    return Value::O(o);
}

} // namespace

void InstallIntl(Runtime& rt) {
    Obj* g = rt.global;
    auto M = [&](Obj* o, const wchar_t* n, NativeFn f, int len) { rt.DefineMethod(o, n, f, len); };
    auto Ctor = [&](const wchar_t* name, NativeFn fn, int len, Obj* proto) {
        Obj* c = rt.NewFunction(fn, name, len, true);
        Prop& p = c->props.Put(L"prototype");
        p.v = Value::O(proto);
        p.enumerable = false;
        rt.DefineData(proto, L"constructor", Value::O(c), false);
        rt.ctors[name] = c;
        return c;
    };

    // Intl Global Object
    Obj* intl = rt.NewObject(rt.objectProto);
    rt.DefineData(intl, L"\x01Symbol.toStringTag", Value::S(L"Intl"), false);
    M(intl, L"getCanonicalLocales", Intl_getCanonicalLocales, 1);

    // DateTimeFormat
    Obj* dtfProto = rt.NewObject(rt.objectProto);
    rt.DefineData(dtfProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.DateTimeFormat"), false);
    M(dtfProto, L"format", DateTimeFormatP_format, 1);
    M(dtfProto, L"formatToParts", DateTimeFormatP_formatToParts, 1);
    M(dtfProto, L"resolvedOptions", DateTimeFormatP_resolvedOptions, 0);
    Obj* dtfCtor = Ctor(L"DateTimeFormat", DateTimeFormat_ctor, 0, dtfProto);
    M(dtfCtor, L"supportedLocalesOf", DateTimeFormat_supportedLocalesOf, 1);
    rt.DefineData(intl, L"DateTimeFormat", Value::O(dtfCtor));

    // NumberFormat
    Obj* nfProto = rt.NewObject(rt.objectProto);
    rt.DefineData(nfProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.NumberFormat"), false);
    M(nfProto, L"format", NumberFormatP_format, 1);
    M(nfProto, L"formatToParts", NumberFormatP_formatToParts, 1);
    M(nfProto, L"resolvedOptions", NumberFormatP_resolvedOptions, 0);
    Obj* nfCtor = Ctor(L"NumberFormat", NumberFormat_ctor, 0, nfProto);
    M(nfCtor, L"supportedLocalesOf", NumberFormat_supportedLocalesOf, 1);
    rt.DefineData(intl, L"NumberFormat", Value::O(nfCtor));

    // Collator
    Obj* colProto = rt.NewObject(rt.objectProto);
    rt.DefineData(colProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.Collator"), false);
    M(colProto, L"compare", CollatorP_compare, 2);
    M(colProto, L"resolvedOptions", CollatorP_resolvedOptions, 0);
    Obj* colCtor = Ctor(L"Collator", Collator_ctor, 0, colProto);
    M(colCtor, L"supportedLocalesOf", Collator_supportedLocalesOf, 1);
    rt.DefineData(intl, L"Collator", Value::O(colCtor));

    // PluralRules
    Obj* prProto = rt.NewObject(rt.objectProto);
    rt.DefineData(prProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.PluralRules"), false);
    M(prProto, L"select", PluralRulesP_select, 1);
    M(prProto, L"resolvedOptions", PluralRulesP_resolvedOptions, 0);
    Obj* prCtor = Ctor(L"PluralRules", PluralRules_ctor, 0, prProto);
    M(prCtor, L"supportedLocalesOf", PluralRules_supportedLocalesOf, 1);
    rt.DefineData(intl, L"PluralRules", Value::O(prCtor));

    // RelativeTimeFormat
    Obj* rtfProto = rt.NewObject(rt.objectProto);
    rt.DefineData(rtfProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.RelativeTimeFormat"), false);
    M(rtfProto, L"format", RelativeTimeFormatP_format, 2);
    M(rtfProto, L"resolvedOptions", RelativeTimeFormatP_resolvedOptions, 0);
    Obj* rtfCtor = Ctor(L"RelativeTimeFormat", RelativeTimeFormat_ctor, 0, rtfProto);
    rt.DefineData(intl, L"RelativeTimeFormat", Value::O(rtfCtor));

    // ListFormat
    Obj* lfProto = rt.NewObject(rt.objectProto);
    rt.DefineData(lfProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.ListFormat"), false);
    M(lfProto, L"format", ListFormatP_format, 1);
    M(lfProto, L"resolvedOptions", ListFormatP_resolvedOptions, 0);
    Obj* lfCtor = Ctor(L"ListFormat", ListFormat_ctor, 0, lfProto);
    rt.DefineData(intl, L"ListFormat", Value::O(lfCtor));

    // DisplayNames
    Obj* dnProto = rt.NewObject(rt.objectProto);
    rt.DefineData(dnProto, L"\x01Symbol.toStringTag", Value::S(L"Intl.DisplayNames"), false);
    M(dnProto, L"of", DisplayNamesP_of, 1);
    M(dnProto, L"resolvedOptions", DisplayNamesP_resolvedOptions, 0);
    Obj* dnCtor = Ctor(L"DisplayNames", DisplayNames_ctor, 0, dnProto);
    rt.DefineData(intl, L"DisplayNames", Value::O(dnCtor));

    // window.Intl ve global.Intl
    rt.DefineData(g, L"Intl", Value::O(intl));
}

} // namespace js
} // namespace mer
