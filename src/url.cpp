// merOS Shield - MerEngine : URL ayrıştırma ve çözümleme
#include "url.h"
#include "util.h"
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mer {

namespace {

bool IsSchemeChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
           c == L'+' || c == L'-' || c == L'.';
}

std::wstring IdnToAsciiHost(const std::wstring& host) {
#ifdef _WIN32
    wchar_t buf[512];
    int n = IdnToAscii(0, host.c_str(), int(host.size()), buf, 512);
    if (n <= 0) return L"";
    return ToLower(std::wstring(buf, n));
#else
    (void)host;
    return L"";
#endif
}

// Yol/sorgu icindeki ASCII disi ve guvensiz karakterleri UTF-8 %XX olarak kodlar.
std::wstring EncodeUnsafe(const std::wstring& s) {
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::wstring o;
    bool need = false;
    for (wchar_t c : s) if (c <= 0x20 || c > 0x7E || c == L'"' || c == L'<' || c == L'>' || c == L'`') { need = true; break; }
    if (!need) return s;
    std::wstring run;
    auto flushRun = [&]() {
        if (run.empty()) return;
        std::string u = ToUtf8(run);
        for (unsigned char b : u) { o += L'%'; o += hex[b >> 4]; o += hex[b & 15]; }
        run.clear();
    };
    for (wchar_t c : s) {
        if (c <= 0x20 || c > 0x7E || c == L'"' || c == L'<' || c == L'>' || c == L'`') run += c;
        else { flushRun(); o += c; }
    }
    flushRun();
    return o;
}

std::wstring NormalizePath(const std::wstring& p) {
    // p: "/a/./b/../c" (sorgu yok)
    std::vector<std::wstring> segs;
    size_t i = 1;
    bool trailing = false;
    while (i <= p.size()) {
        size_t j = p.find(L'/', i);
        if (j == std::wstring::npos) j = p.size();
        std::wstring seg = p.substr(i, j - i);
        bool last = (j == p.size());
        if (seg == L"." || seg == L"%2e" || seg == L"%2E") { if (last) trailing = true; }
        else if (seg == L".." || seg == L"%2e%2e" || seg == L"%2E%2E") { if (!segs.empty()) segs.pop_back(); if (last) trailing = true; }
        else { segs.push_back(seg); trailing = false; }
        i = j + 1;
    }
    std::wstring o;
    for (auto& s : segs) { o += L'/'; o += s; }
    if (o.empty() || trailing) o += L'/';
    return o;
}

} // namespace

bool HasScheme(const std::wstring& s, std::wstring* scheme) {
    if (s.empty() || !((s[0] >= L'a' && s[0] <= L'z') || (s[0] >= L'A' && s[0] <= L'Z'))) return false;
    size_t i = 0;
    while (i < s.size() && IsSchemeChar(s[i])) i++;
    if (i < s.size() && s[i] == L':') {
        if (scheme) *scheme = ToLower(s.substr(0, i));
        return true;
    }
    return false;
}

std::wstring Url::Origin() const {
    if (scheme == L"about") return L"about:";
    std::wstring o = scheme + L"://" + host;
    if (!((scheme == L"https" && port == 443) || (scheme == L"http" && port == 80)))
        o += L":" + std::to_wstring(port);
    return o;
}

std::wstring Url::ToString() const {
    if (!ok) return L"";
    if (scheme == L"about") return L"about:" + path;
    return Origin() + path;
}

Url ParseUrl(const std::wstring& in) {
    Url u;
    std::wstring s;
    for (wchar_t c : Trim(in)) if (c != L'\t' && c != L'\n' && c != L'\r') s += c;
    std::wstring sch;
    if (!HasScheme(s, &sch)) return u;
    u.scheme = sch;
    size_t p = sch.size() + 1;

    if (sch == L"about") {
        u.path = ToLower(s.substr(p));
        size_t h = u.path.find(L'#');
        if (h != std::wstring::npos) u.path = u.path.substr(0, h);
        u.ok = true;
        return u;
    }
    if (sch != L"http" && sch != L"https") {
        // Diger semalar (javascript:, file:, data:, ...) motor tarafindan acilmaz.
        u.path = s.substr(p);
        u.ok = true;
        return u;
    }

    while (p < s.size() && (s[p] == L'/' || s[p] == L'\\')) p++;
    size_t e = p;
    while (e < s.size() && s[e] != L'/' && s[e] != L'?' && s[e] != L'#' && s[e] != L'\\') e++;
    std::wstring auth = s.substr(p, e - p);
    size_t at = auth.rfind(L'@');
    if (at != std::wstring::npos) { auth = auth.substr(at + 1); u.hadUserInfo = true; }

    std::wstring host = auth, port;
    if (!host.empty() && host[0] == L'[') {
        size_t rb = host.find(L']');
        if (rb == std::wstring::npos) return u;
        if (rb + 1 < host.size() && host[rb + 1] == L':') port = host.substr(rb + 2);
        host = host.substr(0, rb + 1);
    } else {
        size_t c = host.rfind(L':');
        if (c != std::wstring::npos) { port = host.substr(c + 1); host = host.substr(0, c); }
    }
    host = ToLower(host);
    while (!host.empty() && host.back() == L'.') host.pop_back();
    if (host.empty()) return u;

    bool ascii = true;
    for (wchar_t c : host) if (c > 127) { ascii = false; break; }
    if (!ascii) {
        host = IdnToAsciiHost(host);
        if (host.empty()) return u;
    }
    for (wchar_t c : host) {
        bool okc = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.' ||
                   c == L'_' || c == L'[' || c == L']' || c == L':';
        if (!okc) return u;
    }
    u.host = host;
    u.port = (sch == L"https") ? 443 : 80;
    if (!port.empty()) {
        int v = 0;
        for (wchar_t c : port) {
            if (c < L'0' || c > L'9') return u;
            v = v * 10 + (c - L'0');
            if (v > 65535) return u;
        }
        if (v == 0) return u;
        u.port = v;
    }

    std::wstring rest = s.substr(e);
    size_t h = rest.find(L'#');
    if (h != std::wstring::npos) { u.fragment = rest.substr(h + 1); rest = rest.substr(0, h); }
    std::wstring path = rest, query;
    size_t q = rest.find(L'?');
    if (q != std::wstring::npos) { path = rest.substr(0, q); query = rest.substr(q); }
    for (auto& c : path) if (c == L'\\') c = L'/';
    if (path.empty()) path = L"/";
    u.path = EncodeUnsafe(NormalizePath(path) + query);
    u.ok = true;
    return u;
}

std::wstring ResolveUrl(const std::wstring& baseStr, const std::wstring& relIn) {
    std::wstring rel;
    for (wchar_t c : Trim(relIn)) if (c != L'\t' && c != L'\n' && c != L'\r') rel += c;
    Url b = ParseUrl(baseStr);
    if (rel.empty()) return b.ok ? b.ToString() : L"";
    if (HasScheme(rel, nullptr)) return rel;
    if (!b.ok || b.scheme == L"about") return rel;
    if (rel.size() >= 2 && (rel[0] == L'/' || rel[0] == L'\\') && (rel[1] == L'/' || rel[1] == L'\\'))
        return b.scheme + L":" + rel;
    if (rel[0] == L'#') return b.ToString() + rel;
    if (rel[0] == L'/') return b.Origin() + rel;
    std::wstring pathOnly = b.path;
    size_t q = pathOnly.find(L'?');
    if (q != std::wstring::npos) pathOnly = pathOnly.substr(0, q);
    if (rel[0] == L'?') return b.Origin() + pathOnly + rel;
    size_t slash = pathOnly.rfind(L'/');
    std::wstring dir = slash == std::wstring::npos ? L"/" : pathOnly.substr(0, slash + 1);
    return b.Origin() + dir + rel;
}

std::wstring UrlEncodeComponent(const std::wstring& s) {
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::string u = ToUtf8(s);
    std::wstring o;
    for (unsigned char c : u) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            o += wchar_t(c);
        else if (c == ' ') o += L'+';
        else { o += L'%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

} // namespace mer
