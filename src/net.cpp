// merOS Shield - ağ katmanı (WinHTTP + Windows SChannel TLS)
// Sifreleme Windows'un kendi TLS yigini (SChannel) ile yapilir: TLS 1.2/1.3,
// sertifika dogrulamasi asla atlanmaz. Cerez, otomatik NTLM kimlik dogrulamasi
// ve Referer gonderilmez (gizlilik).
#include "net.h"
#include "url.h"
#include "util.h"
#include "i18n.h"
#include <winhttp.h>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif

namespace mer {

namespace {

HINTERNET g_session = nullptr;
const wchar_t* kUA = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) merOSShield/0.2 MerEngine/0.2 merJS/0.1";

struct H {
    HINTERNET h = nullptr;
    explicit H(HINTERNET x) : h(x) {}
    ~H() { if (h) WinHttpCloseHandle(h); }
    H(const H&) = delete;
    H& operator=(const H&) = delete;
};

UINT CodePageFor(const std::wstring& name) {
    std::wstring n = ToLower(Trim(name));
    while (!n.empty() && (n.back() == L'"' || n.back() == L'\'' || n.back() == L';')) n.pop_back();
    while (!n.empty() && (n[0] == L'"' || n[0] == L'\'')) n.erase(0, 1);
    if (n == L"utf-8" || n == L"utf8") return CP_UTF8;
    if (n == L"windows-1254" || n == L"iso-8859-9" || n == L"latin5" || n == L"cp1254") return 1254;
    if (n == L"windows-1252" || n == L"iso-8859-1" || n == L"latin1" || n == L"us-ascii" || n == L"ascii") return 1252;
    if (n == L"windows-1251" || n == L"koi8-r") return n == L"koi8-r" ? 20866 : 1251;
    if (n == L"windows-1250" || n == L"iso-8859-2") return 1250;
    if (n == L"shift_jis" || n == L"shift-jis") return 932;
    if (n == L"gb2312" || n == L"gbk") return 936;
    if (n == L"big5") return 950;
    if (n == L"euc-kr") return 949;
    return 0;
}

std::wstring FindCharset(const std::wstring& s) {
    size_t p = ToLower(s).find(L"charset=");
    if (p == std::wstring::npos) return L"";
    size_t st = p + 8;
    size_t e = st;
    while (e < s.size() && s[e] != L';' && s[e] != L'"' && s[e] != L'\'' && s[e] != L'>' && s[e] != L' ' && s[e] != L'/') e++;
    if (st < s.size() && (s[st] == L'"' || s[st] == L'\'')) { st++; e = st; while (e < s.size() && s[e] != L'"' && s[e] != L'\'') e++; }
    return s.substr(st, e - st);
}

std::wstring Convert(const std::string& b, size_t off, UINT cp, bool strict) {
    if (b.size() <= off) return L"";
    int len = int(b.size() - off);
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int n = MultiByteToWideChar(cp, flags, b.data() + off, len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(cp, flags, b.data() + off, len, &w[0], n);
    return w;
}

} // namespace

bool NetInit() {
    g_session = WinHttpOpen(kUA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session)
        g_session = WinHttpOpen(kUA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) return false;

    // Yalnizca modern TLS: 1.3 (Windows 11) + 1.2. Eski SSL/TLS 1.0/1.1 kapali.
    DWORD prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot))) {
        prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot));
    }
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL; // gzip/deflate: daha az veri = daha hizli
    WinHttpSetOption(g_session, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
    DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP; // HTTPS'ten HTTP'ye dusurme yasak
    WinHttpSetOption(g_session, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof(pol));
    DWORD maxr = 10;
    WinHttpSetOption(g_session, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxr, sizeof(maxr));
    WinHttpSetTimeouts(g_session, 10000, 10000, 15000, 30000);
    return true;
}

void NetShutdown() {
    if (g_session) { WinHttpCloseHandle(g_session); g_session = nullptr; }
}

#pragma comment(lib, "winhttp.lib")

FetchResult Fetch(const std::wstring& url, size_t maxBytes, const std::atomic<int>* cancel, int expected, const FetchOptions* opt) {
    FetchOptions defOpt;
    if (!opt) opt = &defOpt;
    FetchResult r;
    if (!g_session) NetInit();
    Url u = ParseUrl(url);
    if (!u.ok || (u.scheme != L"http" && u.scheme != L"https") || !g_session) {
        r.error = ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
        return r;
    }
    H conn(WinHttpConnect(g_session, u.host.c_str(), INTERNET_PORT(u.port), 0));
    if (!conn.h) { r.error = GetLastError(); return r; }
    static const wchar_t* accept[] = {L"text/html", L"application/xhtml+xml", L"text/css", L"text/plain", L"*/*", nullptr};
    H req(WinHttpOpenRequest(conn.h, opt->method.c_str(), u.path.c_str(), nullptr, WINHTTP_NO_REFERER, accept,
                             u.IsSecure() ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) { r.error = GetLastError(); return r; }

    DWORD feat = WINHTTP_DISABLE_AUTHENTICATION | (opt->cookies ? 0 : WINHTTP_DISABLE_COOKIES);
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof(feat));
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH; // Windows kimlik bilgisi sizdirma yok
    WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
    DWORD proto = WINHTTP_PROTOCOL_FLAG_HTTP2; // HTTP/2: tek baglanti, daha hizli
    WinHttpSetOption(req.h, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &proto, sizeof(proto));

    std::wstring hdrs =
        i18n::GetAcceptLanguageHeader() +
        L"Sec-GPC: 1\r\n"
        L"DNT: 1\r\n"
        L"Upgrade-Insecure-Requests: 1\r\n";
    if (!opt->contentType.empty()) hdrs += L"Content-Type: " + opt->contentType + L"\r\n";
    if (!opt->referer.empty()) hdrs += L"Referer: " + opt->referer + L"\r\n";
    if (!opt->extraHeaders.empty()) hdrs += opt->extraHeaders;
    void* body = opt->body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(opt->body.data());
    DWORD blen = DWORD(opt->body.size());
    if (!WinHttpSendRequest(req.h, hdrs.c_str(), DWORD(-1L), body, blen, blen, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = GetLastError();
        return r;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = status;

    DWORD stSz = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &stSz, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && stSz > 0) {
        std::wstring stTxt(stSz / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, &stTxt[0], &stSz, WINHTTP_NO_HEADER_INDEX)) {
            r.statusText = std::wstring(stTxt.c_str());
        }
    }
    if (r.statusText.empty()) {
        r.statusText = (status == 200) ? L"OK" : (status == 404) ? L"Not Found" : (status == 500) ? L"Internal Server Error" : L"";
    }

    DWORD hdrSz = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hdrSz, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && hdrSz > 0) {
        std::wstring raw(hdrSz / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &hdrSz, WINHTTP_NO_HEADER_INDEX)) {
            r.rawHeaders = std::wstring(raw.c_str());
        }
    }

    sz = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && sz > 0 && sz < 4096) {
        std::wstring ct(sz / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, &ct[0], &sz, WINHTTP_NO_HEADER_INDEX))
            r.contentType = std::wstring(ct.c_str());
    }

    DWORD ulen = 0;
    WinHttpQueryOption(req.h, WINHTTP_OPTION_URL, nullptr, &ulen);
    if (ulen > 0 && ulen < 65536) {
        std::wstring fu(ulen / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryOption(req.h, WINHTTP_OPTION_URL, &fu[0], &ulen)) r.finalUrl = std::wstring(fu.c_str());
    }
    if (r.finalUrl.empty()) r.finalUrl = u.ToString();

    std::vector<char> buf(64 * 1024);
    for (;;) {
        if (cancel && cancel->load() != expected) { r.error = ERROR_WINHTTP_OPERATION_CANCELLED; return r; }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), DWORD(buf.size()), &got)) { r.error = GetLastError(); return r; }
        if (got == 0) break;
        if (r.body.size() + got > maxBytes) {
            r.body.append(buf.data(), maxBytes - r.body.size());
            r.truncated = true;
            break;
        }
        r.body.append(buf.data(), got);
    }
    r.ok = true;
    return r;
}

std::wstring DecodeBody(const std::string& b, const std::wstring& contentType) {
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF)
        return Convert(b, 3, CP_UTF8, false);
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);

    UINT cp = CodePageFor(FindCharset(contentType));
    if (!cp) {
        // <meta charset=...> ilk 4 KB icinde aranir
        std::string head = b.substr(0, 4096);
        std::wstring wh;
        for (char c : head) wh += wchar_t((unsigned char)c);
        std::wstring lh = ToLower(wh);
        size_t m = lh.find(L"<meta");
        while (m != std::wstring::npos && !cp) {
            size_t e = lh.find(L'>', m);
            std::wstring tag = lh.substr(m, e == std::wstring::npos ? std::wstring::npos : e - m);
            cp = CodePageFor(FindCharset(tag));
            if (!cp) {
                size_t c = tag.find(L"charset");
                if (c != std::wstring::npos) {
                    size_t q = tag.find_first_of(L"=", c);
                    if (q != std::wstring::npos) cp = CodePageFor(tag.substr(q + 1, tag.find_first_of(L" />", q + 2) - q - 1));
                }
            }
            m = lh.find(L"<meta", m + 5);
        }
    }
    if (cp && cp != CP_UTF8) return Convert(b, 0, cp, false);
    std::wstring w = Convert(b, 0, CP_UTF8, true);
    if (!w.empty() || b.empty()) return w;
    return Convert(b, 0, 1254, false); // gecersiz UTF-8 ise Turkce Windows kod sayfasi
}

std::wstring NetErrorText(DWORD e) {
    return i18n::NetErrorText(e);
}

} // namespace mer
