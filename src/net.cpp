// merOS Shield - ağ katmanı (WinHTTP + Windows SChannel TLS)
// Sifreleme Windows'un kendi TLS yigini (SChannel) ile yapilir: TLS 1.2/1.3,
// sertifika dogrulamasi asla atlanmaz. Cerez, otomatik NTLM kimlik dogrulamasi
// ve Referer yalnizca Chrome'un varsayilan politikasiyla (baska kokene sadece koken) gonderilir.
#include "net.h"
#include "url.h"
#include "util.h"
#include "i18n.h"
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
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
// Uyumluluk kimligi: Edge, Opera, Vivaldi gibi Chromium uyumlu kimlik + kendi adimiz (tum sitelerde ayni)
const wchar_t* kUA = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 merOSShield/0.3";

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

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::wstring ContentTypeForPath(const std::wstring& path) {
    std::wstring l = ToLower(path);
    size_t dot = l.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : l.substr(dot + 1);
    if (ext == L"html" || ext == L"htm" || ext == L"xhtml") return L"text/html";
    if (ext == L"css") return L"text/css";
    if (ext == L"js" || ext == L"mjs") return L"application/javascript";
    if (ext == L"json") return L"application/json";
    if (ext == L"svg") return L"image/svg+xml";
    if (ext == L"png") return L"image/png";
    if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
    if (ext == L"gif") return L"image/gif";
    if (ext == L"webp") return L"image/webp";
    if (ext == L"ico") return L"image/x-icon";
    if (ext == L"bmp") return L"image/bmp";
    if (ext == L"txt" || ext == L"md" || ext == L"log") return L"text/plain";
    if (ext == L"xml") return L"text/xml";
    return L"application/octet-stream";
}

bool IsReservedDosDevicePath(const std::wstring& wp) {
    // wp: "C:\klasor\dosya.ext"
    size_t i = 3;
    while (i <= wp.size()) {
        size_t j = wp.find(L'\\', i);
        if (j == std::wstring::npos) j = wp.size();
        std::wstring seg = wp.substr(i, j - i);
        i = j + 1;
        if (seg.empty()) continue;
        if (seg == L"." || seg == L"..") return true;
        // Uzanti ve bosluklari kirp ("CON.txt" veya "NUL " cihaz adi sayilir)
        size_t dot = seg.find(L'.');
        std::wstring stem = ToLower(Trim(dot == std::wstring::npos ? seg : seg.substr(0, dot)));
        if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
            stem == L"conin$" || stem == L"conout$") return true;
        if ((StartsWith(stem, L"com") || StartsWith(stem, L"lpt")) &&
            stem.size() == 4 && stem[3] >= L'1' && stem[3] <= L'9') return true;
    }
    return false;
}

// file:///C:/a/b.html -> yerel dosyayi okur (yalnizca okuma, klasor listeleme ve cihaz dosyalari yasak)
FetchResult FetchFile(const Url& u, size_t maxBytes) {
    FetchResult r;
    std::wstring path = u.path;
    size_t q = path.find(L'?');
    if (q != std::wstring::npos) path = path.substr(0, q);
    // %XX (UTF-8) cozumu
    std::string bytes;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == L'%' && i + 2 < path.size() && HexDigit(path[i + 1]) >= 0 && HexDigit(path[i + 2]) >= 0) {
            char decoded = char(HexDigit(path[i + 1]) * 16 + HexDigit(path[i + 2]));
            if (decoded == '\0') { r.error = ERROR_FILE_NOT_FOUND; return r; }
            bytes += decoded;
            i += 2;
        } else {
            bytes += ToUtf8(std::wstring(1, path[i]));
        }
    }
    if (bytes.find('\0') != std::string::npos) { r.error = ERROR_FILE_NOT_FOUND; return r; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0 && !bytes.empty()) { r.error = ERROR_FILE_NOT_FOUND; return r; }
    std::wstring wp(size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), &wp[0], n);
    if (!wp.empty() && wp[0] == L'/') wp.erase(0, 1);
    for (auto& c : wp) if (c == L'/') c = L'\\';
    // Surucu harfi zorunlu ("C:\..."), UNC ("\\..."), NTFS ADS (ikinci ':') veya '\0' yasak
    if (wp.size() < 3 || !iswalpha(wp[0]) || wp[1] != L':' || wp[2] != L'\\' ||
        wp.find(L'\0') != std::wstring::npos || wp.find(L':', 2) != std::wstring::npos ||
        wp.find(L"\\\\") != std::wstring::npos || IsReservedDosDevicePath(wp)) {
        r.error = ERROR_FILE_NOT_FOUND;
        return r;
    }
    wchar_t fullBuf[MAX_PATH * 4];
    DWORD fullLen = GetFullPathNameW(wp.c_str(), MAX_PATH * 4, fullBuf, nullptr);
    if (fullLen == 0 || fullLen >= MAX_PATH * 4) { r.error = ERROR_FILE_NOT_FOUND; return r; }
    std::wstring fullPath(fullBuf, fullLen);
    if (fullPath.size() < 3 || towlower(fullPath[0]) != towlower(wp[0]) || fullPath[1] != L':' || fullPath[2] != L'\\' ||
        IsReservedDosDevicePath(fullPath)) {
        r.error = ERROR_FILE_NOT_FOUND;
        return r;
    }
    DWORD attr = GetFileAttributesW(fullPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        r.error = ERROR_FILE_NOT_FOUND;
        return r;
    }
    HANDLE h = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { r.error = GetLastError(); return r; }
    if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);
        r.error = ERROR_ACCESS_DENIED;
        return r;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(h);
        r.error = ERROR_ACCESS_DENIED;
        return r;
    }
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr)) { r.error = GetLastError(); CloseHandle(h); return r; }
        if (got == 0) break;
        if (r.body.size() + got > maxBytes) { r.body.append(buf.data(), maxBytes - r.body.size()); r.truncated = true; break; }
        r.body.append(buf.data(), got);
    }
    CloseHandle(h);
    r.ok = true;
    r.status = 200;
    r.contentType = ContentTypeForPath(fullPath);
    r.finalUrl = u.ToString();
    return r;
}

bool IsValidTokenChar(wchar_t c) {
    if (c <= 0x20 || c >= 0x7F) return false;
    static const wchar_t* sep = L"()<>@,;:\\\"/[]?={}";
    for (const wchar_t* p = sep; *p; ++p) if (c == *p) return false;
    return true;
}

std::wstring SanitizeHttpMethod(const std::wstring& mIn) {
    std::wstring m = Trim(mIn);
    if (m.empty()) return L"GET";
    for (wchar_t c : m) if (!IsValidTokenChar(c)) return L"GET";
    std::wstring u = ToUpper(m);
    if (u == L"CONNECT" || u == L"TRACE" || u == L"TRACK") return L"GET";
    return m;
}

std::wstring StripHeaderCtl(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\0') continue;
        if (c < 0x20 && c != L'\t') continue;
        o += c;
    }
    return o;
}

std::wstring SanitizeExtraHeaders(const std::wstring& raw, bool stripCredentials) {
    std::wstring out;
    size_t st = 0;
    while (st < raw.size()) {
        size_t e = raw.find(L"\r\n", st);
        if (e == std::wstring::npos) e = raw.find(L'\n', st);
        std::wstring line = (e == std::wstring::npos) ? raw.substr(st) : raw.substr(st, e - st);
        st = (e == std::wstring::npos) ? raw.size() : (raw[e] == L'\r' && e + 1 < raw.size() && raw[e + 1] == L'\n' ? e + 2 : e + 1);
        size_t colon = line.find(L':');
        if (colon == std::wstring::npos || colon == 0) continue;
        std::wstring name = Trim(line.substr(0, colon));
        std::wstring val = StripHeaderCtl(Trim(line.substr(colon + 1)));
        if (name.empty()) continue;
        bool validName = true;
        for (wchar_t c : name) if (!IsValidTokenChar(c)) { validName = false; break; }
        if (!validName) continue;
        std::wstring low = ToLower(name);
        if (low == L"host" || low == L"content-length" || low == L"transfer-encoding" ||
            low == L"connection" || low == L"keep-alive" || low == L"upgrade" || low == L"te" || low == L"trailer")
            continue;
        if (stripCredentials && (low == L"cookie" || low == L"cookie2" || low == L"authorization" || low == L"proxy-authorization"))
            continue;
        out += name + L": " + val + L"\r\n";
    }
    return out;
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

std::wstring ReferrerFor(const std::wstring& docUrl, const std::wstring& target) {
    Url d = ParseUrl(docUrl), t = ParseUrl(target);
    if (!d.ok || !t.ok) return L"";
    if (d.scheme != L"https" && d.scheme != L"http") return L"";
    if (t.scheme != L"https" && t.scheme != L"http") return L"";
    if (d.scheme == L"https" && t.scheme != L"https") return L"";   // guvenlik dususu: gonderme
    if (IsSameOrigin(d, t)) return d.ToString();
    return d.Origin() + L"/";
}

FetchOptions SubresourceOpts(const std::wstring& docUrl, const std::wstring& target) {
    FetchOptions o;
    o.referer = ReferrerFor(docUrl, target);
    return o;
}

FetchResult Fetch(const std::wstring& url, size_t maxBytes, const std::atomic<int>* cancel, int expected, const FetchOptions* opt) {
    FetchOptions defOpt;
    if (!opt) opt = &defOpt;
    FetchResult r;
    if (!g_session) NetInit();
    Url u = ParseUrl(url);
    if (u.ok && u.scheme == L"file") return FetchFile(u, maxBytes);
    if (!u.ok || (u.scheme != L"http" && u.scheme != L"https") || !g_session) {
        r.error = ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
        return r;
    }

    Url firstUrl = u;
    std::wstring curMethod = SanitizeHttpMethod(opt->method);
    std::string curBody = opt->body;
    std::wstring curContentType = StripHeaderCtl(opt->contentType);
    std::wstring curReferer = StripHeaderCtl(opt->referer);
    std::wstring curUA = StripHeaderCtl(opt->userAgent);
    bool crossOriginRedirect = false;

    for (int hop = 0; hop <= 10; hop++) {
        if (cancel && cancel->load() != expected) { r.error = ERROR_WINHTTP_OPERATION_CANCELLED; return r; }

        H conn(WinHttpConnect(g_session, u.host.c_str(), INTERNET_PORT(u.port), 0));
        if (!conn.h) { r.error = GetLastError(); return r; }
        static const wchar_t* accept[] = {L"text/html", L"application/xhtml+xml", L"text/css", L"text/plain", L"*/*", nullptr};
        H req(WinHttpOpenRequest(conn.h, curMethod.c_str(), u.path.c_str(), nullptr, WINHTTP_NO_REFERER, accept,
                                 u.IsSecure() ? WINHTTP_FLAG_SECURE : 0));
        if (!req.h) { r.error = GetLastError(); return r; }

        // Yonlendirmeleri elle denetle: SSRF (127.0.0.1 / yerel ag), sema degisimi ve capraz kokene Cookie sizmasini onle
        DWORD redirPol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirPol, sizeof(redirPol));

        if (!curUA.empty()) {
            std::wstring ua = L"User-Agent: " + curUA;
            WinHttpAddRequestHeaders(req.h, ua.c_str(), DWORD(-1L), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        DWORD feat = WINHTTP_DISABLE_AUTHENTICATION | ((opt->cookies && !crossOriginRedirect) ? 0 : WINHTTP_DISABLE_COOKIES);
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
        if (!curContentType.empty()) hdrs += L"Content-Type: " + curContentType + L"\r\n";
        if (!curReferer.empty()) hdrs += L"Referer: " + curReferer + L"\r\n";
        if (!opt->extraHeaders.empty()) hdrs += SanitizeExtraHeaders(opt->extraHeaders, crossOriginRedirect);

        void* body = curBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(curBody.data());
        DWORD blen = DWORD(curBody.size());
        if (!WinHttpSendRequest(req.h, hdrs.c_str(), DWORD(-1L), body, blen, blen, 0) ||
            !WinHttpReceiveResponse(req.h, nullptr)) {
            r.error = GetLastError();
            return r;
        }

        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = status;

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (hop == 10) {
                r.error = ERROR_WINHTTP_REDIRECT_FAILED;
                return r;
            }
            DWORD locSz = 0;
            WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locSz, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || locSz == 0 || locSz > 65536) {
                r.error = ERROR_WINHTTP_REDIRECT_FAILED;
                return r;
            }
            std::wstring loc(locSz / sizeof(wchar_t) + 1, L'\0');
            if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, &loc[0], &locSz, WINHTTP_NO_HEADER_INDEX)) {
                r.error = ERROR_WINHTTP_REDIRECT_FAILED;
                return r;
            }
            loc = std::wstring(loc.c_str());
            Url next = ParseUrl(ResolveUrl(u.ToString(), loc));
            // Yalnizca http/https ve HTTPS -> HTTP dusurme yasak
            if (!next.ok || (next.scheme != L"http" && next.scheme != L"https") || (u.IsSecure() && !next.IsSecure())) {
                r.error = ERROR_WINHTTP_REDIRECT_FAILED;
                return r;
            }
            // Genelden yerel/ozel aga (127.0.0.1, 192.168.x.x, 169.254.169.254 vb.) SSRF yonlendirmesi yasak
            if (!IsPrivateOrLocalHost(firstUrl.host) && IsPrivateOrLocalHost(next.host)) {
                r.error = ERROR_WINHTTP_REDIRECT_FAILED;
                return r;
            }
            if (!IsSameOrigin(firstUrl, next)) crossOriginRedirect = true;
            curReferer = ReferrerFor(u.ToString(), next.ToString());
            if (status == 303 || ((status == 301 || status == 302) && ToUpper(curMethod) == L"POST")) {
                curMethod = L"GET";
                curBody.clear();
                curContentType.clear();
            }
            u = std::move(next);
            continue;
        }

        DWORD stSz = 0;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &stSz, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && stSz > 0 && stSz < 4096) {
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
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && hdrSz > 0 && hdrSz < 262144) {
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

        r.finalUrl = u.ToString();

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

    r.error = ERROR_WINHTTP_REDIRECT_FAILED;
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
                    if (q != std::wstring::npos) {
                        size_t endPos = tag.find_first_of(L" />", q + 2);
                        cp = CodePageFor(tag.substr(q + 1, endPos == std::wstring::npos ? std::wstring::npos : endPos - q - 1));
                    }
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
