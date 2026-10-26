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

// merOS Shield Özgün Koyu Tema (Cyber-Slate Dark) - Yeni Sekme ve Dahili Sayfa CSS Stilleri
const wchar_t* kPageCss =
    L"<style>"
    L"body{margin:0;background:#111621;color:#f1f5f9;font-family:'Segoe UI',sans-serif}"
    L".w{max-width:820px;margin:0 auto;padding:34px 28px 24px}"
    L".top-bar{display:flex;justify-content:space-between;align-items:center;margin:0 0 28px}"
    L".brand{display:flex;align-items:center;gap:14px}"
    L".brand-orb{width:48px;height:48px;border-radius:14px;background:linear-gradient(135deg,#0ea5e9,#2563eb,#10b981);display:flex;align-items:center;justify-content:center;color:#fff;font-size:24px;font-family:'Segoe Fluent Icons','Segoe MDL2 Assets';box-shadow:0 6px 20px rgba(14,165,233,0.28)}"
    L".brand-title{font-size:24px;font-weight:700;color:#f8fafc;margin:0;line-height:1.15}"
    L".brand-sub{font-size:12px;color:#38bdf8;font-weight:600;margin-top:2px}"
    L".top-actions{display:flex;align-items:center;gap:8px;font-size:12px;color:#94a3b8;background:#182030;border:1px solid #263248;border-radius:18px;padding:6px 14px}"
    L".top-actions a{color:#38bdf8;text-decoration:none;font-weight:600}"
    L".search-wrap{max-width:580px;margin:0 auto 34px}"
    L".ara{display:flex;align-items:center;gap:12px;background:#182030;border:1px solid #2b3954;border-radius:26px;padding:6px 18px;box-shadow:0 8px 24px rgba(0,0,0,0.34)}"
    L".search-ic{width:28px;height:28px;border-radius:14px;background:linear-gradient(135deg,#0284c7,#2563eb);display:flex;align-items:center;justify-content:center;color:#fff;font-family:'Segoe Fluent Icons','Segoe MDL2 Assets';font-size:13px}"
    L".ara input[name=q]{flex:1;height:38px;padding:0 6px;border:none;border-radius:18px;font-size:15px;background:#182030;color:#f8fafc}"
    L".tiles{display:grid;grid-template-columns:repeat(6,1fr);gap:14px;max-width:680px;margin:0 auto 34px}"
    L".tile{display:flex;flex-direction:column;align-items:center;text-decoration:none;color:#e2e8f0}"
    L".tile-box{width:82px;height:82px;background:#182030;border:1px solid #263248;border-radius:16px;display:flex;align-items:center;justify-content:center;margin-bottom:8px;box-shadow:0 6px 16px rgba(0,0,0,0.25)}"
    L".tile:hover .tile-box{background:#1f2a3f;border-color:#38bdf8}"
    L".badge{width:44px;height:44px;border-radius:12px;display:flex;align-items:center;justify-content:center;font-weight:700;font-size:16px;color:#fff}"
    L".badge-yt{background:#e62117;color:#fff;font-size:17px}"
    L".badge-wiki{background:#2d3748;font-family:Georgia,serif;font-size:20px}"
    L".badge-gh{background:#0f172a;border:1px solid #334155;font-size:15px}"
    L".badge-hn{background:#ff6600;font-family:Arial,sans-serif;font-size:18px}"
    L".badge-cnn{background:#cc0000;font-size:12px;font-weight:800}"
    L".badge-bbc{background:#991b1b;font-size:12px;font-weight:800}"
    L".tile-lbl{font-size:12px;color:#cbd5e1;text-align:center}"
    L".shield-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px;max-width:680px;margin:0 auto 18px}"
    L".scard{display:flex;align-items:flex-start;gap:12px;background:#161e2e;border:1px solid #243046;border-radius:14px;padding:14px 16px}"
    L".sic{width:34px;height:34px;border-radius:10px;display:flex;align-items:center;justify-content:center;font-family:'Segoe Fluent Icons','Segoe MDL2 Assets';font-size:16px}"
    L".sic-tls{background:#064e3b;color:#34d399}"
    L".sic-ad{background:#1e3a8a;color:#60a5fa}"
    L".sic-js{background:#3b0764;color:#c084fc}"
    L".sic-dl{background:#0c4a6e;color:#38bdf8}"
    L".stxt{flex:1}"
    L".stitle{font-size:13px;font-weight:700;color:#f1f5f9;margin:0 0 3px}"
    L".sdesc{font-size:12px;color:#94a3b8;line-height:1.4}"
    L".k{background:#161e2e;border:1px solid #243046;border-radius:14px;padding:20px 24px;margin:0 0 18px}"
    L".k h3{margin:0 0 14px;font-size:16px;color:#f8fafc;font-weight:700}"
    L".alt{color:#94a3b8;margin:0 0 20px;font-size:14px;line-height:1.5}"
    L"a{color:#38bdf8;text-decoration:none}"
    L"code{background:#0f172a;border:1px solid #263248;border-radius:4px;padding:2px 6px;font-family:Consolas,monospace;font-size:13px;color:#e2e8f0}"
    L"</style>";

// Metin Tablosu - İngilizce (Varsayılan)
const wchar_t* const kStringsEn[] = {
    // UI & Durum çubuğu
    /* AppName */               L"merOS Shield",
    /* SearchOrAddress */       L"Search with DuckDuckGo or enter web address",
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
    /* HomeSearchPlaceholder */ L"Search with DuckDuckGo or enter web address",
    /* HomeSearchButton */      L"Search",
    /* HomeSecurityTitle */     L"Security",
    /* HomeSecItem1 */          L"HTTPS only (TLS 1.2 / 1.3), certificate errors are never bypassed",
    /* HomeSecItem2 */          L"Tracker and ad servers blocked, no third-party cookies, Referer trimmed to origin",
    /* HomeSecItem3 */          L"Sandboxed JavaScript: step, memory, and recursion limits, no popups",
    /* HomeSecItem4 */          L"No automatic downloads, mixed content blocked",
    /* HomeShortcuts */         L"Shortcuts: Ctrl+L address, F5 reload, F12 console, Alt+\x2190/\x2192 back/forward, Ctrl+T new tab, Ctrl+W close, Ctrl+Tab switch, Ctrl+F find, Ctrl+U source, Ctrl +/- zoom. Engine info: <a href='about:engine'>about:engine</a>",

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
    /* SearchOrAddress */       L"DuckDuckGo ile ara veya web adresi yaz",
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
    /* HomeSearchPlaceholder */ L"DuckDuckGo ile ara veya web adresi yaz",
    /* HomeSearchButton */      L"Ara",
    /* HomeSecurityTitle */     L"G\u00FCvenlik",
    /* HomeSecItem1 */          L"Yaln\u0131zca HTTPS (TLS 1.2 / 1.3), sertifika hatalar\u0131 asla atlanmaz",
    /* HomeSecItem2 */          L"\u0130zleyici ve reklam sunucular\u0131 engellenir, \u00FC\u00E7\u00FCnc\u00FC taraf \u00E7erez yok, Referer yaln\u0131zca k\u00F6ken",
    /* HomeSecItem3 */          L"JavaScript korumal\u0131 \u00E7al\u0131\u015F\u0131r: ad\u0131m, bellek ve \u00F6zyineleme s\u0131n\u0131rlar\u0131, a\u00E7\u0131l\u0131r pencere yok",
    /* HomeSecItem4 */          L"Otomatik indirme yok, kar\u0131\u015F\u0131k i\u00E7erik engelli",
    /* HomeShortcuts */         L"K\u0131sayollar: Ctrl+L adres, F5 yenile, F12 konsol, Alt+\x2190/\x2192 geri/ileri, Ctrl+T yeni sekme, Ctrl+W kapat, Ctrl+Tab sekme de\u011Fi\u015Ftir, Ctrl+F bul, Ctrl+U kaynak, Ctrl +/- yak\u0131nla\u015Ft\u0131r. Motor bilgisi: <a href='about:motor'>about:motor</a>",

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
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return lang == Language::Turkish ? L"Dosya bulunamad\u0131. Ta\u015F\u0131nm\u0131\u015F, silinmi\u015F ya da ad\u0131 de\u011Fi\u015Fmi\u015F olabilir."
                                         : L"File not found. It may have been moved, deleted or renamed.";
    case ERROR_ACCESS_DENIED:
        return lang == Language::Turkish ? L"Bu dosyay\u0131 okuma izni yok." : L"Access to this file was denied.";
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

    // Ust Baslik: merOS Shield Ozgun Kalkan Kimligi + Motor/Dil Rozeti
    html += L"<div class=top-bar>";
    html += L"<div class=brand>";
    html += L"<div class=brand-orb>&#xEA18;</div>";
    html += L"<div><div class=brand-title>merOS Shield</div>";
    html += L"<div class=brand-sub>MerEngine 0.2 &nbsp;&#x00B7;&nbsp; merJS Sandbox</div></div>";
    html += L"</div>";
    html += L"<div class=top-actions><a href='about:engine'>MerEngine</a> &nbsp;&#x00B7;&nbsp; ";
    if (isEn) {
        html += L"<b>EN</b> | <a href='about:lang?tr'>TR</a>";
    } else {
        html += L"<a href='about:lang?en'>EN</a> | <b>TR</b>";
    }
    html += L"</div></div>";

    // Ortalanmis Koyu Arama Kapsulu
    html += L"<div class=search-wrap>";
    html += L"<form class=ara action='https://html.duckduckgo.com/html/' method=get>";
    html += L"<div class=search-ic>&#xE721;</div>";
    html += L"<input name=q placeholder='";
    html += Get(StringId::HomeSearchPlaceholder, lang);
    html += L"'></form></div>";

    // 6'li Hizli Erisim Kutucuklari
    html += L"<div class=tiles>";
    html += L"<a class=tile href='https://www.youtube.com/'><div class=tile-box><div class='badge badge-yt'>&#x25B6;</div></div><span class=tile-lbl>YouTube</span></a>";
    if (isEn) {
        html += L"<a class=tile href='https://en.wikipedia.org/'><div class=tile-box><div class='badge badge-wiki'>W</div></div><span class=tile-lbl>Wikipedia</span></a>";
    } else {
        html += L"<a class=tile href='https://tr.wikipedia.org/'><div class=tile-box><div class='badge badge-wiki'>W</div></div><span class=tile-lbl>Vikipedi</span></a>";
    }
    html += L"<a class=tile href='https://github.com/'><div class=tile-box><div class='badge badge-gh'>GH</div></div><span class=tile-lbl>GitHub</span></a>";
    html += L"<a class=tile href='https://news.ycombinator.com/'><div class=tile-box><div class='badge badge-hn'>Y</div></div><span class=tile-lbl>Hacker News</span></a>";
    html += L"<a class=tile href='https://lite.cnn.com/'><div class=tile-box><div class='badge badge-cnn'>CNN</div></div><span class=tile-lbl>CNN Lite</span></a>";
    if (isEn) {
        html += L"<a class=tile href='https://www.bbc.com/news'><div class=tile-box><div class='badge badge-bbc'>BBC</div></div><span class=tile-lbl>BBC News</span></a>";
    } else {
        html += L"<a class=tile href='https://www.bbc.com/turkce'><div class=tile-box><div class='badge badge-bbc'>BBC</div></div><span class=tile-lbl>BBC T\u00FCrk\u00E7e</span></a>";
    }
    html += L"</div>";

    // merOS Shield Aktif Guvenlik & Motor Kalkanlari (2x2 Kompakt Grid)
    html += L"<div class=shield-grid>";
    if (isEn) {
        html += L"<div class=scard><div class='sic sic-tls'>&#xE72E;</div><div class=stxt><div class=stitle>Strict HTTPS (TLS 1.3)</div><div class=sdesc>Certificate errors are never bypassed; insecure traffic is blocked.</div></div></div>";
        html += L"<div class=scard><div class='sic sic-ad'>&#xE71A;</div><div class=stxt><div class=stitle>Tracker &amp; Ad Shield</div><div class=sdesc>Ad servers blocked, zero third-party cookies, origin-only Referer.</div></div></div>";
        html += L"<div class=scard><div class='sic sic-js'>&#xE943;</div><div class=stxt><div class=stitle>Sandboxed merJS Engine</div><div class=sdesc>Isolated per-tab execution with strict step, memory &amp; recursion limits.</div></div></div>";
        html += L"<div class=scard><div class='sic sic-dl'>&#xEA18;</div><div class=stxt><div class=stitle>Zero Auto-Download</div><div class=sdesc>Mixed content and unsolicited binary downloads are blocked by default.</div></div></div>";
    } else {
        html += L"<div class=scard><div class='sic sic-tls'>&#xE72E;</div><div class=stxt><div class=stitle>Zorunlu HTTPS (TLS 1.3)</div><div class=sdesc>Sertifika hatalar\u0131 asla atlanmaz; g\u00FCvensiz a\u011F trafi\u011Fi engellenir.</div></div></div>";
        html += L"<div class=scard><div class='sic sic-ad'>&#xE71A;</div><div class=stxt><div class=stitle>\u0130zleyici ve Reklam Kalkan\u0131</div><div class=sdesc>Takip\u00E7i sunucular\u0131 ve 3. taraf \u00E7erezler engellenir, Referer k\u0131rp\u0131l\u0131r.</div></div></div>";
        html += L"<div class=scard><div class='sic sic-js'>&#xE943;</div><div class=stxt><div class=stitle>Korumal\u0131 merJS Motoru</div><div class=sdesc>Her sekme kendi i\u015Flem hatt\u0131nda bellek ve ad\u0131m s\u0131n\u0131rlar\u0131yla izole \u00E7al\u0131\u015F\u0131r.</div></div></div>";
        html += L"<div class=scard><div class='sic sic-dl'>&#xEA18;</div><div class=stxt><div class=stitle>Otomatik \u0130ndirme Korumas\u0131</div><div class=sdesc>Kar\u0131\u015F\u0131k i\u00E7erik ve izinsiz dosya indirmeleri varsay\u0131lan olarak kapal\u0131d\u0131r.</div></div></div>";
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

    html += L"<p class=alt><a href='about:home'>&#x2190; ";
    html += (lang == Language::English) ? L"Back to New Tab" : L"Yeni Sekmeye D\u00F6n";
    html += L"</a></p>";

    html += L"</div></body></html>";
    return html;
}

} // namespace i18n
} // namespace mer
