// merOS Shield - Uluslararasılaştırma ve Yerelleştirme (i18n) Katmanı
#include "i18n.h"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdio>

namespace mer {
namespace i18n {

namespace {

std::atomic<Language> g_currentLanguage{Language::English};

// Dahili sayfa CSS stilleri
const wchar_t* kPageCss =
    L"<style>"
    L"body{margin:0;background:#f8fafc;color:#0f172a;font-family:'Segoe UI',sans-serif}"
    L".w{max-width:840px;margin:36px auto;padding:0 24px}"
    L".brand{display:flex;align-items:center;gap:14px;margin:0 0 16px}"
    L".brand-icon{width:44px;height:44px;background:#1e40af;border-radius:10px;display:flex;align-items:center;justify-content:center;color:#fff;font-size:22px;font-family:'Segoe Fluent Icons','Segoe MDL2 Assets'}"
    L".brand-text h1{font-size:30px;font-weight:700;margin:0;line-height:1.2;color:#0f172a}"
    L".brand-pill{display:inline-block;font-size:11px;font-weight:700;color:#2563eb;background:#eff6ff;border:1px solid #bfdbfe;border-radius:10px;padding:1px 8px;margin-top:2px}"
    L".alt{color:#475569;margin:0 0 20px;font-size:14px;line-height:1.5}"
    L".ara{display:flex;gap:10px;margin:0 0 22px}"
    L".ara input[name=q]{flex:1;height:44px;padding:0 16px;border:1px solid #cbd5e1;border-radius:10px;font-size:15px;background:#fff}"
    L".ara input[type=submit]{height:44px;padding:0 22px;background:#2563eb;color:#fff;border:none;border-radius:10px;font-weight:600;font-size:14px;cursor:pointer}"
    L".izgara{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:0 0 22px}"
    L".card{display:flex;align-items:center;gap:12px;background:#fff;border:1px solid #e2e8f0;border-radius:10px;padding:12px 14px;text-decoration:none;color:inherit}"
    L".card:hover{border-color:#93c5fd;background:#f8fafc;text-decoration:none}"
    L".badge{width:38px;height:38px;border-radius:8px;display:flex;align-items:center;justify-content:center;font-weight:700;font-size:14px;color:#fff}"
    L".badge-wiki{background:#202122;font-family:Georgia,serif;font-size:18px}"
    L".badge-hn{background:#ff6600;font-family:Arial,sans-serif;font-size:16px}"
    L".badge-cnn{background:#cc0000;font-size:11px;font-weight:800}"
    L".badge-npr{background:#1e3a8a;font-size:12px;font-weight:700}"
    L".badge-gh{background:#181717;font-size:13px;font-weight:700}"
    L".badge-bbc{background:#991b1b;font-size:11px;font-weight:800}"
    L".info{display:flex;flex-direction:column}"
    L".title{font-size:14px;font-weight:600;color:#1e293b;line-height:1.3}"
    L".host{font-size:12px;color:#64748b;margin-top:2px}"
    L".k{background:#fff;border:1px solid #e2e8f0;border-radius:12px;padding:20px 24px;margin:0 0 18px}"
    L".k h3{margin:0 0 14px;font-size:16px;color:#1e293b;font-weight:700}"
    L".sec-list{display:grid;grid-template-columns:repeat(2,1fr);gap:12px;margin:0;padding:0;list-style:none}"
    L".sec-item{display:flex;align-items:flex-start;gap:8px;font-size:13px;color:#334155;line-height:1.45}"
    L".ic{font-family:'Segoe Fluent Icons','Segoe MDL2 Assets';font-size:15px;margin-top:2px}"
    L".ic-lock{color:#16a34a}.ic-block{color:#dc2626}.ic-code{color:#ea580c}.ic-dl{color:#2563eb}"
    L".kucuk{color:#64748b;font-size:13px;line-height:1.5;margin:0 0 12px}"
    L".lang-bar{padding-top:12px;border-top:1px solid #e2e8f0;font-size:13px;color:#64748b}"
    L".lang-bar a{font-weight:600;color:#2563eb;margin:0 4px;text-decoration:none}"
    L"code{background:#f1f5f9;border:1px solid #e2e8f0;border-radius:4px;padding:2px 6px;font-family:Consolas,monospace;font-size:13px}"
    L"</style>";

// Metin Tablosu - İngilizce (Varsayılan)
const wchar_t* const kStringsEn[] = {
    // UI & Durum çubuğu
    /* AppName */               L"merOS Shield",
    /* SearchOrAddress */       L"Search or enter web address",
    /* NewTab */               L"New Tab",
    /* Loading */               L"Loading: ",
    /* LoadingImages */         L"Loading images (%d)   ",
    /* BlockedTrackers */       L"Blocked trackers: %d   ",
    /* JsErrors */              L"JS: %d error(s) (F12)",
    /* JsOk */                  L"JS \x2714",
    /* JsDisabled */            L"JS disabled",
    /* SnapshotSaved */         L"Page snapshot saved: ",
    /* SnapshotFailed */        L"Failed to save page snapshot.",
    /* ScriptLoadFailed */      L"Failed to load script: ",
    /* NetInitFailed */         L"Failed to initialize network layer.",
    /* PageTruncated */         L"<p><i>[Page truncated at 24 MB limit]</i></p>",
    /* InlineScript */          L"inline",

    // Dahili Sayfalar - Giriş (Home)
    /* HomeTitle */             L"New Tab",
    /* HomeTagline */           L"A fast, security-focused browser powered by custom engines <b>MerEngine</b> and <b>merJS</b>.",
    /* HomeSearchPlaceholder */ L"Search with DuckDuckGo",
    /* HomeSearchButton */      L"Search",
    /* HomeSecurityTitle */     L"Security",
    /* HomeSecItem1 */          L"HTTPS only (TLS 1.2 / 1.3), certificate errors are never bypassed",
    /* HomeSecItem2 */          L"Tracker and ad servers blocked, no third-party cookies, no Referer",
    /* HomeSecItem3 */          L"Sandboxed JavaScript: step, memory, and recursion limits, no popups",
    /* HomeSecItem4 */          L"No automatic downloads, mixed content blocked",
    /* HomeShortcuts */         L"Shortcuts: Ctrl+L address, F5 reload, F12 console, Alt+\x2190/\x2192 back/forward, Ctrl +/- zoom. Engine info: <a href='about:engine'>about:engine</a>",

    // Dahili Sayfalar - Motor (Engine)
    /* EngineTitle */           L"MerEngine 0.2 + merJS 0.1",
    /* EngineSubtitle */        L"Engines built from scratch for merOS Shield.",
    /* EnginePipelineTitle */   L"Pipeline",
    /* EnginePipeline1 */       L"Network: WinHTTP + SChannel (HTTP/2, gzip, TLS 1.3), parallel subresource download",
    /* EnginePipeline2 */       L"HTML parser \x2192 DOM (node and depth bounded)",
    /* EnginePipeline3 */       L"CSS: selectors (attribute, :not, :first-child, +, ~), <code>var()</code>, <code>@media</code>, flexbox, grid",
    /* EnginePipeline4 */       L"merJS: ES2020+, classes, Promise, async/await, Map/Set, RegExp, JSON, DOM API, TypedArrays, TextEncoder/Decoder, MSE, Intl",
    /* EnginePipeline5 */       L"Layout: block, inline, list, table, flex, grid, images, form fields",
    /* EnginePipeline6 */       L"Rendering: Direct2D + DirectWrite (GPU), WIC images, Windows Media Foundation Video Decoder",
    /* EngineTestTitle */       L"JS Test",
    /* EngineTestFail */        L"JavaScript is disabled or failed to run.",
    /* EngineTestOk */          L"merJS is running \x2714  ",

    // Uyarı & Hata Sayfaları
    /* InsecureWarningTitle */   L"No secure connection",
    /* InsecureWarningBody */    L"This site does not seem to support HTTPS. Everything sent via HTTP (search queries, login credentials) can be intercepted and modified by others on the network.",
    /* InsecureWarningAction */  L"I understand the risk, open this site with insecure HTTP",
    /* InvalidAddressTitle */    L"Invalid address",
    /* InvalidAddressBody */     L"Could not understand this address.",
    /* BlockedSchemeTitle */     L"Blocked",
    /* BlockedSchemeBody */      L"\"%s:\" addresses are not opened for security reasons.",
    /* SuspiciousAddressTitle */ L"Suspicious address",
    /* SuspiciousAddressBody */  L"This address contains the \"user@server\" deception used to hide the actual host. Destination host: %s",
    /* SuspiciousAddressAction */L"Continue to %s anyway",
    /* PageNotFoundTitle */      L"Page not found",
    /* PageNotFoundBody */       L"This internal page does not exist.",
    /* UnsupportedFileTitle */   L"File not opened",
    /* UnsupportedFileBody */    L"This content type (%s) cannot be displayed by the engine. Automatic downloads are disabled for your safety.",
    /* PageLoadFailedTitle */    L"Could not load page",

    // Ağ Hataları
    /* NetErrTlsFailure */       L"Could not establish a secure connection. The site's certificate is invalid, expired, or uses an unsupported TLS version. merOS Shield blocked this page for your security.",
    /* NetErrNameNotResolved */  L"Server not found. The address may be misspelled, or you may not have an internet connection.",
    /* NetErrCannotConnect */    L"Could not connect to the server.",
    /* NetErrTimeout */          L"The server did not respond in time.",
    /* NetErrRedirectFailed */   L"The site tried to redirect from a secure (HTTPS) connection to an insecure (HTTP) address. This was blocked.",
    /* NetErrUnrecognizedScheme*/L"This address type is not supported.",
    /* NetErrOperationCancelled*/L"The request was cancelled.",
    /* NetErrGeneric */          L"Network error (code %u)."
};

// Metin Tablosu - Türkçe (Alternatif)
const wchar_t* const kStringsTr[] = {
    // UI & Durum çubuğu
    /* AppName */               L"merOS Shield",
    /* SearchOrAddress */       L"Adres yaz veya ara",
    /* NewTab */               L"Yeni Sekme",
    /* Loading */               L"Y\u00FCkleniyor: ",
    /* LoadingImages */         L"Resimler y\u00FCkleniyor (%d)   ",
    /* BlockedTrackers */       L"Engellenen izleyici: %d   ",
    /* JsErrors */              L"JS: %d hata (F12)",
    /* JsOk */                  L"JS \x2714",
    /* JsDisabled */            L"JS kapal\u0131",
    /* SnapshotSaved */         L"Sayfa g\u00F6r\u00FCnt\u00FCs\u00FC kaydedildi: ",
    /* SnapshotFailed */        L"Sayfa g\u00F6r\u00FCnt\u00FCs\u00FC kaydedilemedi.",
    /* ScriptLoadFailed */      L"Betik y\u00FCklenemedi: ",
    /* NetInitFailed */         L"A\u011F katman\u0131 ba\u015Flat\u0131lamad\u0131.",
    /* PageTruncated */         L"<p><i>[Sayfa 24 MB s\u0131n\u0131r\u0131nda kesildi]</i></p>",
    /* InlineScript */          L"sat\u0131r i\u00E7i",

    // Dahili Sayfalar - Giriş (Home)
    /* HomeTitle */             L"Yeni Sekme",
    /* HomeTagline */           L"Kendi motorumuz <b>MerEngine</b> ve JavaScript motorumuz <b>merJS</b> ile \u00E7al\u0131\u015Fan, g\u00FCvenlik ve h\u0131z odakl\u0131 taray\u0131c\u0131.",
    /* HomeSearchPlaceholder */ L"DuckDuckGo ile ara",
    /* HomeSearchButton */      L"Ara",
    /* HomeSecurityTitle */     L"G\u00FCvenlik",
    /* HomeSecItem1 */          L"Yaln\u0131zca HTTPS (TLS 1.2 / 1.3), sertifika hatalar\u0131 asla atlanmaz",
    /* HomeSecItem2 */          L"\u0130zleyici ve reklam sunucular\u0131 engellenir, \u00FC\u00E7\u00FCnc\u00FC taraf \u00E7erez yok, Referer yok",
    /* HomeSecItem3 */          L"JavaScript korumal\u0131 \u00E7al\u0131\u015F\u0131r: ad\u0131m, bellek ve \u00F6zyineleme s\u0131n\u0131rlar\u0131, a\u00E7\u0131l\u0131r pencere yok",
    /* HomeSecItem4 */          L"Otomatik indirme yok, kar\u0131\u015F\u0131k i\u00E7erik engelli",
    /* HomeShortcuts */         L"K\u0131sayollar: Ctrl+L adres, F5 yenile, F12 konsol, Alt+\x2190/\x2192 geri/ileri, Ctrl +/- yak\u0131nla\u015Ft\u0131r. Motor bilgisi: <a href='about:motor'>about:motor</a>",

    // Dahili Sayfalar - Motor (Engine)
    /* EngineTitle */           L"MerEngine 0.2 + merJS 0.1",
    /* EngineSubtitle */        L"merOS Shield'\u0131n s\u0131f\u0131rdan yaz\u0131lm\u0131\u015F motorlar\u0131.",
    /* EnginePipelineTitle */   L"Boru hatt\u0131",
    /* EnginePipeline1 */       L"A\u011F: WinHTTP + SChannel (HTTP/2, gzip, TLS 1.3), paralel alt kaynak indirme",
    /* EnginePipeline2 */       L"HTML ayr\u0131\u015Ft\u0131r\u0131c\u0131 \x2192 DOM (d\u00FC\u011F\u00FCm ve derinlik s\u0131n\u0131rl\u0131)",
    /* EnginePipeline3 */       L"CSS: se\u00E7iciler (\u00F6znitelik, :not, :first-child, +, ~), <code>var()</code>, <code>@media</code>, flexbox, grid",
    /* EnginePipeline4 */       L"merJS: ES2020+, s\u0131n\u0131flar, Promise, async/await, Map/Set, RegExp, JSON, DOM API, TypedArrays, TextEncoder/Decoder, MSE, Intl",
    /* EnginePipeline5 */       L"Yerle\u015Fim: blok, sat\u0131r i\u00E7i, liste, tablo, flex, grid, resim, form alanlar\u0131",
    /* EnginePipeline6 */       L"\u00C7izim: Direct2D + DirectWrite (GPU), resimler WIC ile, Windows Media Foundation Video Decoder",
    /* EngineTestTitle */       L"JS testi",
    /* EngineTestFail */        L"JavaScript kapal\u0131 ya da \u00E7al\u0131\u015Fmad\u0131.",
    /* EngineTestOk */          L"merJS \u00E7al\u0131\u015F\u0131yor \x2714  ",

    // Uyarı & Hata Sayfaları
    /* InsecureWarningTitle */   L"G\u00FCvenli ba\u011Flant\u0131 yok",
    /* InsecureWarningBody */    L"Bu site HTTPS desteklemiyor gibi g\u00F6r\u00FCn\u00FCyor. HTTP ile g\u00F6nderilen her \u015Fey (aranan kelimeler, girilen bilgiler) a\u011Fdaki ba\u015Fkalar\u0131 taraf\u0131ndan okunabilir ve de\u011Fi\u015Ftirilebilir.",
    /* InsecureWarningAction */  L"Riski anl\u0131yorum, bu siteyi g\u00FCvensiz HTTP ile a\u00E7",
    /* InvalidAddressTitle */    L"Ge\u00E7ersiz adres",
    /* InvalidAddressBody */     L"Bu adres anla\u015F\u0131lamad\u0131.",
    /* BlockedSchemeTitle */     L"Engellendi",
    /* BlockedSchemeBody */      L"\"%s:\" adresleri g\u00FCvenlik nedeniyle a\u00E7\u0131lmaz.",
    /* SuspiciousAddressTitle */ L"\u015E\u00FCpheli adres",
    /* SuspiciousAddressBody */  L"Bu adres, ger\u00E7ek sunucu ad\u0131n\u0131 gizlemek i\u00E7in kullan\u0131lan \"kullan\u0131c\u0131@sunucu\" hilesini i\u00E7eriyor. As\u0131l gidilecek sunucu: %s",
    /* SuspiciousAddressAction */L"Yine de %s adresine git",
    /* PageNotFoundTitle */      L"Sayfa bulunamad\u0131",
    /* PageNotFoundBody */       L"Bu dahili sayfa yok.",
    /* UnsupportedFileTitle */   L"Dosya a\u00E7\u0131lmad\u0131",
    /* UnsupportedFileBody */    L"Bu i\u00E7erik t\u00FCr\u00FC (%s) motor taraf\u0131ndan g\u00F6sterilemiyor. G\u00FCvenli\u011Fin i\u00E7in otomatik indirme yap\u0131lmaz.",
    /* PageLoadFailedTitle */    L"Sayfa a\u00E7\u0131lamad\u0131",

    // Ağ Hataları
    /* NetErrTlsFailure */       L"G\u00FCvenli ba\u011Flant\u0131 kurulamad\u0131. Sitenin sertifikas\u0131 ge\u00E7ersiz, s\u00FCresi dolmu\u015F veya bu site eski bir TLS s\u00FCr\u00FCm\u00FC kullan\u0131yor. merOS Shield bu sayfay\u0131 g\u00FCvenli\u011Fin i\u00E7in a\u00E7mad\u0131.",
    /* NetErrNameNotResolved */  L"Sunucu bulunamad\u0131. Adres yanl\u0131\u015F yaz\u0131lm\u0131\u015F olabilir ya da internet ba\u011Flant\u0131n yok.",
    /* NetErrCannotConnect */    L"Sunucuya ba\u011Flan\u0131lamad\u0131.",
    /* NetErrTimeout */          L"Sunucu zaman\u0131nda yan\u0131t vermedi.",
    /* NetErrRedirectFailed */   L"Site, g\u00FCvenli (HTTPS) ba\u011Flant\u0131dan g\u00FCvensiz (HTTP) adrese y\u00F6nlendirmeye \u00E7al\u0131\u015Ft\u0131. Bu engellendi.",
    /* NetErrUnrecognizedScheme*/L"Bu adres t\u00FCr\u00FC desteklenmiyor.",
    /* NetErrOperationCancelled*/L"\u0130stek iptal edildi.",
    /* NetErrGeneric */          L"A\u011F hatas\u0131 (kod %u)."
};

} // namespace

Language GetCurrentLanguage() {
    return g_currentLanguage.load();
}

void SetCurrentLanguage(Language lang) {
    g_currentLanguage.store(lang);
}

const wchar_t* GetLanguageCode() {
    return (GetCurrentLanguage() == Language::English) ? L"en-US" : L"tr-TR";
}

const wchar_t* GetLanguageCodeShort() {
    return (GetCurrentLanguage() == Language::English) ? L"en" : L"tr";
}

std::vector<std::wstring> GetLanguageList() {
    if (GetCurrentLanguage() == Language::English) {
        return { L"en-US", L"en", L"tr-TR", L"tr" };
    } else {
        return { L"tr-TR", L"tr", L"en-US", L"en" };
    }
}

std::wstring GetAcceptLanguageHeader() {
    if (GetCurrentLanguage() == Language::English) {
        return L"Accept-Language: en-US,en;q=0.9,tr-TR;q=0.8,tr;q=0.7\r\n";
    } else {
        return L"Accept-Language: tr-TR,tr;q=0.9,en-US;q=0.8,en;q=0.7\r\n";
    }
}

const wchar_t* Get(StringId id) {
    return Get(id, GetCurrentLanguage());
}

const wchar_t* Get(StringId id, Language lang) {
    size_t idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(StringId::Count_)) return L"";
    if (lang == Language::English) {
        return kStringsEn[idx];
    } else {
        return kStringsTr[idx];
    }
}

std::wstring NetErrorText(DWORD e) {
    Language lang = GetCurrentLanguage();
    switch (e) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        return Get(StringId::NetErrTlsFailure, lang);
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return Get(StringId::NetErrNameNotResolved, lang);
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return Get(StringId::NetErrCannotConnect, lang);
    case ERROR_WINHTTP_TIMEOUT:
        return Get(StringId::NetErrTimeout, lang);
    case ERROR_WINHTTP_REDIRECT_FAILED:
        return Get(StringId::NetErrRedirectFailed, lang);
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return Get(StringId::NetErrUnrecognizedScheme, lang);
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return Get(StringId::NetErrOperationCancelled, lang);
    default: {
        wchar_t buf[128];
        const wchar_t* fmt = Get(StringId::NetErrGeneric, lang);
        swprintf(buf, 128, fmt, e);
        return buf;
    }
    }
}

std::wstring GetHomePageHtml() {
    Language lang = GetCurrentLanguage();
    bool isEn = (lang == Language::English);

    std::wstring html;
    html += L"<!doctype html><html><head><title>";
    html += Get(StringId::HomeTitle, lang);
    html += L"</title>";
    html += kPageCss;
    html += L"</head><body><div class=w>";

    // Brand Header with native vector shield icon
    html += L"<div class=brand>";
    html += L"<div class=brand-icon>&#xEA18;</div>";
    html += L"<div class=brand-text>";
    html += L"<h1>merOS Shield</h1>";
    html += L"<span class=brand-pill>SECURE BROWSER</span>";
    html += L"</div></div>";

    html += L"<p class=alt>";
    html += Get(StringId::HomeTagline, lang);
    html += L"</p>";

    // Search bar
    html += L"<form class=ara action='https://html.duckduckgo.com/html/' method=get>";
    html += L"<input name=q placeholder='";
    html += Get(StringId::HomeSearchPlaceholder, lang);
    html += L"'><input type=submit value='";
    html += Get(StringId::HomeSearchButton, lang);
    html += L"'></form>";

    // Speed Dial / Quick Access Cards with clean branded badges
    html += L"<div class=izgara>";
    if (isEn) {
        html += L"<a class=card href='https://en.wikipedia.org/'><div class='badge badge-wiki'>W</div><div class=info><span class=title>Wikipedia</span><span class=host>en.wikipedia.org</span></div></a>";
        html += L"<a class=card href='https://news.ycombinator.com/'><div class='badge badge-hn'>Y</div><div class=info><span class=title>Hacker News</span><span class=host>news.ycombinator.com</span></div></a>";
        html += L"<a class=card href='https://lite.cnn.com/'><div class='badge badge-cnn'>CNN</div><div class=info><span class=title>CNN Lite</span><span class=host>lite.cnn.com</span></div></a>";
        html += L"<a class=card href='https://text.npr.org/'><div class='badge badge-npr'>npr</div><div class=info><span class=title>NPR Text</span><span class=host>text.npr.org</span></div></a>";
        html += L"<a class=card href='https://github.com/'><div class='badge badge-gh'>GH</div><div class=info><span class=title>GitHub</span><span class=host>github.com</span></div></a>";
        html += L"<a class=card href='https://www.bbc.com/news'><div class='badge badge-bbc'>BBC</div><div class=info><span class=title>BBC News</span><span class=host>bbc.com/news</span></div></a>";
    } else {
        html += L"<a class=card href='https://tr.wikipedia.org/'><div class='badge badge-wiki'>W</div><div class=info><span class=title>Vikipedi</span><span class=host>tr.wikipedia.org</span></div></a>";
        html += L"<a class=card href='https://news.ycombinator.com/'><div class='badge badge-hn'>Y</div><div class=info><span class=title>Hacker News</span><span class=host>news.ycombinator.com</span></div></a>";
        html += L"<a class=card href='https://lite.cnn.com/'><div class='badge badge-cnn'>CNN</div><div class=info><span class=title>CNN Lite</span><span class=host>lite.cnn.com</span></div></a>";
        html += L"<a class=card href='https://text.npr.org/'><div class='badge badge-npr'>npr</div><div class=info><span class=title>NPR Text</span><span class=host>text.npr.org</span></div></a>";
        html += L"<a class=card href='https://github.com/'><div class='badge badge-gh'>GH</div><div class=info><span class=title>GitHub</span><span class=host>github.com</span></div></a>";
        html += L"<a class=card href='https://www.bbc.com/turkce'><div class='badge badge-bbc'>BBC</div><div class=info><span class=title>BBC T\u00FCrk\u00E7e</span><span class=host>bbc.com/turkce</span></div></a>";
    }
    html += L"</div>";

    // Security Box with vector icons
    html += L"<div class=k><h3><span class='ic ic-lock'>&#xE72E;</span> ";
    html += Get(StringId::HomeSecurityTitle, lang);
    html += L"</h3><ul class=sec-list>";
    html += L"<li class=sec-item><span class='ic ic-lock'>&#xE72E;</span><div>"; html += Get(StringId::HomeSecItem1, lang); html += L"</div></li>";
    html += L"<li class=sec-item><span class='ic ic-block'>&#xE71A;</span><div>"; html += Get(StringId::HomeSecItem2, lang); html += L"</div></li>";
    html += L"<li class=sec-item><span class='ic ic-code'>&#xE943;</span><div>"; html += Get(StringId::HomeSecItem3, lang); html += L"</div></li>";
    html += L"<li class=sec-item><span class='ic ic-dl'>&#xE896;</span><div>"; html += Get(StringId::HomeSecItem4, lang); html += L"</div></li>";
    html += L"</ul></div>";

    // Shortcuts
    html += L"<p class=kucuk><span class=ic>&#xE765;</span> ";
    html += Get(StringId::HomeShortcuts, lang);
    html += L"</p>";

    // Language switcher
    html += L"<div class='lang-bar'><span class=ic>&#xE774;</span> Language / Dil: ";
    if (isEn) {
        html += L"<b>English (Default)</b> | <a href='about:lang?tr'>T\u00FCrk\u00E7e</a>";
    } else {
        html += L"<a href='about:lang?en'>English</a> | <b>T\u00FCrk\u00E7e</b>";
    }
    html += L"</div>";

    html += L"</div></body></html>";
    return html;
}

std::wstring GetEnginePageHtml() {
    Language lang = GetCurrentLanguage();

    std::wstring html;
    html += L"<!doctype html><html><head><title>MerEngine</title>";
    html += kPageCss;
    html += L"</head><body><div class=w><h1>";
    html += Get(StringId::EngineTitle, lang);
    html += L"</h1><p class=alt>";
    html += Get(StringId::EngineSubtitle, lang);
    html += L"</p>";

    html += L"<div class=k><h3>";
    html += Get(StringId::EnginePipelineTitle, lang);
    html += L"</h3><ol>";
    html += L"<li>"; html += Get(StringId::EnginePipeline1, lang); html += L"</li>";
    html += L"<li>"; html += Get(StringId::EnginePipeline2, lang); html += L"</li>";
    html += L"<li>"; html += Get(StringId::EnginePipeline3, lang); html += L"</li>";
    html += L"<li>"; html += Get(StringId::EnginePipeline4, lang); html += L"</li>";
    html += L"<li>"; html += Get(StringId::EnginePipeline5, lang); html += L"</li>";
    html += L"<li>"; html += Get(StringId::EnginePipeline6, lang); html += L"</li>";
    html += L"</ol></div>";

    html += L"<div class=k><h3>";
    html += Get(StringId::EngineTestTitle, lang);
    html += L"</h3><p id=jt>";
    html += Get(StringId::EngineTestFail, lang);
    html += L"</p>";
    html += L"<script>document.getElementById('jt').textContent = '";
    html += Get(StringId::EngineTestOk, lang);
    html += L"' + [1,2,3].map(x => x * x).join(' \x00B7 ') + '  \x00B7  ' + new Date().toLocaleDateString();</script></div>";

    html += L"</div></body></html>";
    return html;
}

} // namespace i18n
} // namespace mer
