// merOS Shield - merJS : BigInt (keyfi hassasiyetli tamsayi)
// Isaret + 32 bitlik basamaklar (kucukten buyuge). Deger Value::s icinde paketlenir:
// [0] isaret (0/1), ardindan her basamak icin iki 16 bitlik karakter. Boylece Value buyumez, GC degismez.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "js.h"

namespace mer {
namespace js {

struct BigNum {
    bool neg = false;
    std::vector<uint32_t> m;   // buyukluk, kucuk basamak once; sifir = bos

    bool IsZero() const { return m.empty(); }
    void Norm() {
        while (!m.empty() && m.back() == 0) m.pop_back();
        if (m.empty()) neg = false;
    }
    static BigNum FromU64(uint64_t v) {
        BigNum b;
        while (v) { b.m.push_back(uint32_t(v)); v >>= 32; }
        return b;
    }
    static BigNum FromI64(int64_t v) {
        BigNum b = FromU64(v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v));
        b.neg = v < 0;
        b.Norm();
        return b;
    }
    // d tamsayi ve sonlu olmali
    static BigNum FromDouble(double d) {
        BigNum b;
        bool n = d < 0;
        d = std::fabs(std::trunc(d));
        while (d >= 1) {
            double q = std::floor(d / 4294967296.0);
            b.m.push_back(uint32_t(d - q * 4294967296.0));
            d = q;
        }
        b.neg = n;
        b.Norm();
        return b;
    }
    double ToDouble() const {
        double r = 0;
        for (size_t i = m.size(); i-- > 0;) r = r * 4294967296.0 + double(m[i]);
        return neg ? -r : r;
    }
    uint64_t LowU64() const {   // mod 2^64 (iki tumleyen, isaret dahil)
        uint64_t v = (m.size() > 0 ? m[0] : 0) | (uint64_t(m.size() > 1 ? m[1] : 0) << 32);
        return neg ? uint64_t(0) - v : v;
    }
    std::wstring Encode() const {
        std::wstring s;
        s.reserve(1 + m.size() * 2);
        s += wchar_t(neg ? 1 : 0);
        for (uint32_t d : m) { s += wchar_t(d & 0xFFFF); s += wchar_t(d >> 16); }
        return s;
    }
    static BigNum Decode(const std::wstring& s) {
        BigNum b;
        if (s.empty()) return b;
        b.neg = s[0] != 0;
        for (size_t i = 1; i + 1 < s.size(); i += 2) b.m.push_back(uint32_t(uint16_t(s[i])) | (uint32_t(uint16_t(s[i + 1])) << 16));
        b.Norm();
        return b;
    }
};

inline int BigCmpMag(const BigNum& a, const BigNum& b) {
    if (a.m.size() != b.m.size()) return a.m.size() < b.m.size() ? -1 : 1;
    for (size_t i = a.m.size(); i-- > 0;) if (a.m[i] != b.m[i]) return a.m[i] < b.m[i] ? -1 : 1;
    return 0;
}
inline int BigCmp(const BigNum& a, const BigNum& b) {
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    int c = BigCmpMag(a, b);
    return a.neg ? -c : c;
}
inline std::vector<uint32_t> BigAddMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> r(std::max(a.size(), b.size()) + 1, 0);
    uint64_t c = 0;
    for (size_t i = 0; i < r.size(); i++) {
        uint64_t s = c + (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        r[i] = uint32_t(s);
        c = s >> 32;
    }
    return r;
}
// |a| >= |b|
inline std::vector<uint32_t> BigSubMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> r(a.size(), 0);
    int64_t br = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int64_t d = int64_t(a[i]) - (i < b.size() ? int64_t(b[i]) : 0) - br;
        br = d < 0 ? 1 : 0;
        if (d < 0) d += 4294967296LL;
        r[i] = uint32_t(d);
    }
    return r;
}
inline BigNum BigAdd(const BigNum& a, const BigNum& b) {
    BigNum r;
    if (a.neg == b.neg) { r.m = BigAddMag(a.m, b.m); r.neg = a.neg; }
    else if (BigCmpMag(a, b) >= 0) { r.m = BigSubMag(a.m, b.m); r.neg = a.neg; }
    else { r.m = BigSubMag(b.m, a.m); r.neg = b.neg; }
    r.Norm();
    return r;
}
inline BigNum BigNeg(BigNum a) { if (!a.IsZero()) a.neg = !a.neg; return a; }
inline BigNum BigSub(const BigNum& a, const BigNum& b) { return BigAdd(a, BigNeg(b)); }
inline BigNum BigMul(const BigNum& a, const BigNum& b) {
    BigNum r;
    if (a.IsZero() || b.IsZero()) return r;
    r.m.assign(a.m.size() + b.m.size(), 0);
    for (size_t i = 0; i < a.m.size(); i++) {
        uint64_t c = 0;
        for (size_t j = 0; j < b.m.size(); j++) {
            uint64_t t = uint64_t(a.m[i]) * b.m[j] + r.m[i + j] + c;
            r.m[i + j] = uint32_t(t);
            c = t >> 32;
        }
        size_t k = i + b.m.size();
        while (c) { uint64_t t = uint64_t(r.m[k]) + c; r.m[k] = uint32_t(t); c = t >> 32; k++; }
    }
    r.neg = a.neg != b.neg;
    r.Norm();
    return r;
}
inline size_t BigBitLen(const BigNum& a) {
    if (a.m.empty()) return 0;
    uint32_t top = a.m.back();
    size_t n = 0;
    while (top) { n++; top >>= 1; }
    return (a.m.size() - 1) * 32 + n;
}
// Sifira dogru bolme; kalan bolunenin isaretini alir (JS / ve %)
inline void BigDivMod(const BigNum& a, const BigNum& b, BigNum& q, BigNum& r) {
    q = BigNum();
    r = BigNum();
    if (BigCmpMag(a, b) < 0) { r = a; return; }
    if (b.m.size() == 1) {   // tek basamakli bolen: hizli yol
        uint64_t d = b.m[0], rem = 0;
        q.m.assign(a.m.size(), 0);
        for (size_t i = a.m.size(); i-- > 0;) {
            uint64_t cur = (rem << 32) | a.m[i];
            q.m[i] = uint32_t(cur / d);
            rem = cur % d;
        }
        q.neg = a.neg != b.neg;
        q.Norm();
        r = BigNum::FromU64(rem);
        r.neg = a.neg;
        r.Norm();
        return;
    }
    BigNum bm = b;
    bm.neg = false;
    size_t bits = BigBitLen(a);
    q.m.assign(a.m.size(), 0);
    BigNum rem;
    for (size_t i = bits; i-- > 0;) {
        // rem = rem*2 + bit
        uint32_t carry = (a.m[i / 32] >> (i % 32)) & 1;
        for (size_t k = 0; k < rem.m.size(); k++) {
            uint32_t nc = rem.m[k] >> 31;
            rem.m[k] = (rem.m[k] << 1) | carry;
            carry = nc;
        }
        if (carry) rem.m.push_back(carry);
        if (BigCmpMag(rem, bm) >= 0) {
            rem.m = BigSubMag(rem.m, bm.m);
            rem.Norm();
            q.m[i / 32] |= 1u << (i % 32);
        }
    }
    q.neg = a.neg != b.neg;
    q.Norm();
    r = rem;
    r.neg = a.neg && !r.IsZero();
    r.Norm();
}
inline BigNum BigShlMag(const BigNum& a, size_t n) {
    BigNum r;
    if (a.IsZero()) return r;
    size_t w = n / 32, b = n % 32;
    r.m.assign(a.m.size() + w + 1, 0);
    for (size_t i = 0; i < a.m.size(); i++) {
        uint64_t v = uint64_t(a.m[i]) << b;
        r.m[i + w] |= uint32_t(v);
        r.m[i + w + 1] |= uint32_t(v >> 32);
    }
    r.neg = a.neg;
    r.Norm();
    return r;
}
inline BigNum BigShrMagTrunc(const BigNum& a, size_t n) {
    BigNum r;
    size_t w = n / 32, b = n % 32;
    if (w >= a.m.size()) return r;
    r.m.assign(a.m.size() - w, 0);
    for (size_t i = w; i < a.m.size(); i++) {
        uint64_t v = a.m[i] >> b;
        if (b && i + 1 < a.m.size()) v |= uint64_t(a.m[i + 1]) << (32 - b);
        r.m[i - w] = uint32_t(v);
    }
    r.neg = a.neg;
    r.Norm();
    return r;
}
// Aritmetik saga kaydirma (eksi sonsuza yuvarlar)
inline BigNum BigShr(const BigNum& a, size_t n) {
    BigNum t = BigShrMagTrunc(a, n);
    if (a.neg) {
        // atilan bitlerden biri 1 ise bir azalt
        bool lost = false;
        for (size_t i = 0; i < n && i / 32 < a.m.size() && !lost; i++) if ((a.m[i / 32] >> (i % 32)) & 1) lost = true;
        if (lost) t = BigSub(t, BigNum::FromI64(1));
        if (t.IsZero() && (lost || !a.IsZero())) t = lost ? BigNum::FromI64(-1) : t;
    }
    return t;
}
// Iki tumleyen gosterime cevir (n basamak)
inline std::vector<uint32_t> BigToTwos(const BigNum& a, size_t n) {
    std::vector<uint32_t> r(n, 0);
    for (size_t i = 0; i < a.m.size() && i < n; i++) r[i] = a.m[i];
    if (a.neg) {
        uint64_t c = 1;
        for (size_t i = 0; i < n; i++) { uint64_t v = uint64_t(uint32_t(~r[i])) + c; r[i] = uint32_t(v); c = v >> 32; }
    }
    return r;
}
inline BigNum BigFromTwos(std::vector<uint32_t> r) {
    BigNum b;
    bool neg = !r.empty() && (r.back() >> 31);
    if (neg) {
        uint64_t c = 1;
        for (auto& x : r) { uint64_t v = uint64_t(uint32_t(~x)) + c; x = uint32_t(v); c = v >> 32; }
    }
    b.m = std::move(r);
    b.neg = neg;
    b.Norm();
    return b;
}
template <class F>
inline BigNum BigBitwise(const BigNum& a, const BigNum& b, F f) {
    size_t n = std::max(a.m.size(), b.m.size()) + 1;
    auto x = BigToTwos(a, n), y = BigToTwos(b, n);
    for (size_t i = 0; i < n; i++) x[i] = f(x[i], y[i]);
    return BigFromTwos(x);
}
inline BigNum BigNot(const BigNum& a) { return BigSub(BigNeg(a), BigNum::FromI64(1)); }   // ~a = -a - 1
// a mod 2^bits (0 <= sonuc < 2^bits)
inline BigNum BigAsUintN(const BigNum& a, size_t bits) {
    size_t n = bits / 32 + 1;
    auto t = BigToTwos(a, std::max(n, a.m.size() + 1));
    t.resize(n);
    if (bits % 32) t[bits / 32] &= (1u << (bits % 32)) - 1; else t[bits / 32] = 0;
    BigNum r;
    r.m = t;
    r.Norm();
    return r;
}
inline BigNum BigAsIntN(const BigNum& a, size_t bits) {
    if (bits == 0) return BigNum();
    BigNum u = BigAsUintN(a, bits);
    size_t top = bits - 1;
    bool set = top / 32 < u.m.size() && ((u.m[top / 32] >> (top % 32)) & 1);
    if (!set) return u;
    return BigSub(u, BigShlMag(BigNum::FromI64(1), bits));
}
inline std::wstring BigToString(BigNum a, int radix) {
    if (a.IsZero()) return L"0";
    static const wchar_t* dig = L"0123456789abcdefghijklmnopqrstuvwxyz";
    bool neg = a.neg;
    a.neg = false;
    std::wstring s;
    // radix^k tek basamaga sigacak sekilde parcala (hiz)
    uint32_t chunk = uint32_t(radix);
    int per = 1;
    while (uint64_t(chunk) * uint32_t(radix) <= 0xFFFFFFFFu) { chunk *= uint32_t(radix); per++; }
    BigNum d = BigNum::FromU64(chunk), q, r;
    while (!a.IsZero()) {
        BigDivMod(a, d, q, r);
        uint32_t v = r.m.empty() ? 0 : r.m[0];
        for (int k = 0; k < per; k++) {
            s += dig[v % uint32_t(radix)];
            v /= uint32_t(radix);
            if (q.IsZero() && v == 0) break;
        }
        a = q;
    }
    while (s.size() > 1 && s.back() == L'0') s.pop_back();
    if (neg) s += L'-';
    std::reverse(s.begin(), s.end());
    return s;
}
// BigInt("...") / literal: bosluklar kirpilir, 0x/0o/0b, isaret (yalnizca ondalikta); gecersizse false
inline bool BigParse(const std::wstring& in, BigNum& out, bool allowSign = true) {
    size_t a = 0, b = in.size();
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\v' || c == L'\f' || c == 0xA0 || c == 0xFEFF || c == 0x2028 || c == 0x2029; };
    while (a < b && ws(in[a])) a++;
    while (b > a && ws(in[b - 1])) b--;
    out = BigNum();
    if (a == b) return true;   // "" -> 0n
    bool neg = false;
    int radix = 10;
    if (allowSign && (in[a] == L'+' || in[a] == L'-')) { neg = in[a] == L'-'; a++; }
    else if (b - a > 2 && in[a] == L'0' && (in[a + 1] == L'x' || in[a + 1] == L'X' || in[a + 1] == L'o' || in[a + 1] == L'O' || in[a + 1] == L'b' || in[a + 1] == L'B')) {
        wchar_t k = in[a + 1] | 0x20;
        radix = k == L'x' ? 16 : k == L'o' ? 8 : 2;
        a += 2;
    }
    if (a == b) return false;
    BigNum rad = BigNum::FromU64(uint64_t(radix));
    for (size_t i = a; i < b; i++) {
        wchar_t c = in[i];
        if (c == L'_') continue;
        int v = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'z') ? c - L'a' + 10 : (c >= L'A' && c <= L'Z') ? c - L'A' + 10 : 99;
        if (v >= radix) return false;
        out = BigAdd(BigMul(out, rad), BigNum::FromU64(uint64_t(v)));
    }
    out.neg = neg && !out.IsZero();
    return true;
}

inline Value BigVal(const BigNum& b) {
    Value v;
    v.t = Value::BigInt;
    v.s = std::make_shared<const std::wstring>(b.Encode());
    return v;
}
inline BigNum BigOf(const Value& v) { return BigNum::Decode(*v.s); }

} // namespace js
} // namespace mer
