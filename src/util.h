// merOS Shield - MerEngine
// util.h : platformdan bagimsiz kucuk yardimcilar (motor cekirdegi Windows'a bagli degil)
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cstdlib>
inline int _wtoi(const wchar_t* s) { return int(wcstol(s, nullptr, 10)); }
inline double _wtof(const wchar_t* s) { return wcstod(s, nullptr); }
#endif

namespace mer {

inline bool IsWS(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f'; }

// Yalnizca ASCII kucultme: Turkce locale'de "I" -> "ı" sorununu onler.
inline wchar_t LowerC(wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c + 32) : c; }
inline std::wstring ToLower(std::wstring s) { for (auto& c : s) c = LowerC(c); return s; }
inline wchar_t UpperC(wchar_t c) { return (c >= L'a' && c <= L'z') ? wchar_t(c - 32) : c; }
inline std::wstring ToUpper(std::wstring s) { for (auto& c : s) c = UpperC(c); return s; }

inline std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsWS(s[a])) a++;
    while (b > a && IsWS(s[b - 1])) b--;
    return s.substr(a, b - a);
}

inline bool StartsWith(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
inline bool EndsWith(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

inline std::vector<std::wstring> SplitWS(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    int paren = 0;
    for (wchar_t c : s) {
        if (c == L'(') paren++;
        if (c == L')' && paren > 0) paren--;
        if (IsWS(c) && paren == 0) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

inline void AppendCodepoint(std::wstring& o, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
        cp -= 0x10000;
        o += wchar_t(0xD800 + (cp >> 10));
        o += wchar_t(0xDC00 + (cp & 0x3FF));
    } else {
        o += wchar_t(cp);
    }
}

inline std::string ToUtf8(const std::wstring& w) {
    std::string o;
    o.reserve(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        uint32_t c = uint32_t(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
            uint32_t d = uint32_t(w[i + 1]);
            if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
        }
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    }
    return o;
}

inline std::wstring FromUtf8(const uint8_t* data, size_t len, bool ignoreBOM = false, bool fatal = false, bool* hasError = nullptr) {
    if (hasError) *hasError = false;
    std::wstring out;
    out.reserve(len);
    size_t i = 0;
    if (!ignoreBOM && len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        i = 3;
    }
    while (i < len) {
        uint8_t b0 = data[i];
        if (b0 < 0x80) {
            out += wchar_t(b0);
            i++;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80 || b0 < 0xC2) {
                if (fatal) { if (hasError) *hasError = true; return out; }
                out += wchar_t(0xFFFD);
                i++;
            } else {
                uint32_t cp = ((b0 & 0x1F) << 6) | (data[i + 1] & 0x3F);
                out += wchar_t(cp);
                i += 2;
            }
        } else if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= len) {
                if (fatal) { if (hasError) *hasError = true; return out; }
                out += wchar_t(0xFFFD);
                i++;
            } else {
                uint8_t b1 = data[i + 1];
                uint8_t b2 = data[i + 2];
                bool ok = ((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80);
                if (b0 == 0xE0 && b1 < 0xA0) ok = false; // overlong
                if (b0 == 0xED && b1 >= 0xA0) ok = false; // UTF-16 surrogates
                if (!ok) {
                    if (fatal) { if (hasError) *hasError = true; return out; }
                    out += wchar_t(0xFFFD);
                    i++;
                } else {
                    uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                    out += wchar_t(cp);
                    i += 3;
                }
            }
        } else if ((b0 & 0xF8) == 0xF0) {
            if (i + 3 >= len) {
                if (fatal) { if (hasError) *hasError = true; return out; }
                out += wchar_t(0xFFFD);
                i++;
            } else {
                uint8_t b1 = data[i + 1];
                uint8_t b2 = data[i + 2];
                uint8_t b3 = data[i + 3];
                bool ok = ((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80) && ((b3 & 0xC0) == 0x80);
                if (b0 == 0xF0 && b1 < 0x90) ok = false; // overlong
                if (b0 == 0xF4 && b1 > 0x8F) ok = false; // > U+10FFFF
                if (b0 > 0xF4) ok = false;
                if (!ok) {
                    if (fatal) { if (hasError) *hasError = true; return out; }
                    out += wchar_t(0xFFFD);
                    i++;
                } else {
                    uint32_t cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
                        cp -= 0x10000;
                        out += wchar_t(0xD800 + (cp >> 10));
                        out += wchar_t(0xDC00 + (cp & 0x3FF));
                    } else {
                        out += wchar_t(cp);
                    }
                    i += 4;
                }
            }
        } else {
            if (fatal) { if (hasError) *hasError = true; return out; }
            out += wchar_t(0xFFFD);
            i++;
        }
    }
    return out;
}

inline std::wstring HtmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'&': o += L"&amp;"; break;
        case L'"': o += L"&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

} // namespace mer
