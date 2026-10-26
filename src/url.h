// merOS Shield - MerEngine : URL ayrıştırma ve çözümleme
#pragma once
#include <string>

namespace mer {

struct Url {
    bool ok = false;
    std::wstring scheme;   // "https", "http", "about", ...
    std::wstring host;     // her zaman ASCII (IDN -> punycode: homograf saldirilarina karsi)
    int port = 0;
    std::wstring path;     // yol + sorgu, yuzde-kodlanmis ("/a/b?x=1")
    std::wstring fragment;
    bool hadUserInfo = false; // "https://banka.com@kotu.site" hilesi tespit edildi mi

    std::wstring Origin() const;
    std::wstring ToString() const; // fragment haric
    bool IsSecure() const { return scheme == L"https"; }
};

Url ParseUrl(const std::wstring& s);
std::wstring ResolveUrl(const std::wstring& base, const std::wstring& rel);
bool HasScheme(const std::wstring& s, std::wstring* scheme);
std::wstring UrlEncodeComponent(const std::wstring& s);
bool IsSameOrigin(const Url& a, const Url& b);
bool IsPrivateOrLocalHost(const std::wstring& host);

} // namespace mer
