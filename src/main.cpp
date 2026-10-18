// merOS Shield Browser - kendi motoru (MerEngine + merJS) ile güvenlik ve hız odaklı tarayıcı
// Pencere, adres cubugu, gecmis, gezinme, JS olay dongusu, formlar ve guvenlik politikalari burada.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "blocklist.h"
#include "css.h"
#include "dom.h"
#include "images.h"
#include "js/js.h"
#include "net.h"
#include "style.h"
#include "url.h"
#include "util.h"
#include "view.h"
#include "i18n.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace mer;

namespace {

const wchar_t* kAppName = L"merOS Shield";
const UINT WM_APP_PAGE = WM_APP + 1;
const UINT WM_APP_IMAGE = WM_APP + 2;
const UINT WM_APP_SCRIPT = WM_APP + 3;
const UINT WM_APP_CSS = WM_APP + 4;
enum { ID_BACK = 101, ID_FWD, ID_RELOAD, ID_HOME, ID_ADDR, ID_SEC, ID_STATUS, ID_JS, ID_CONSOLE };
enum { T_JS = 1, T_RESTYLE = 2, T_REFRESH = 3, T_SNAPSHOT = 4 };

const size_t kMaxPage = 24u * 1024 * 1024, kMaxCss = 4u * 1024 * 1024, kMaxScript = 6u * 1024 * 1024, kMaxImage = 12u * 1024 * 1024;

HINSTANCE g_inst;
HWND g_frame, g_back, g_fwd, g_reload, g_home, g_addr, g_sec, g_status, g_js, g_console;
HFONT g_uiFont, g_symFont, g_monoFont;
PageView g_view;

std::atomic<int> g_navId{0};
std::vector<std::wstring> g_history;
int g_histIdx = -1;
std::set<std::wstring> g_httpAllowed;   // kullanicinin bu oturumda HTTP'ye izin verdigi sunucular
bool g_jsEnabled = true;
bool g_consoleVisible = false;
std::wstring g_hoverText;
std::wstring g_snapshotPath;   // --snapshot ile: sayfa yuklenince PNG kaydet ve cik

enum NavMode { NavPush, NavHistory, NavReload };

// Buyuk yiginla is parcacigi (JS ozyinelemesi icin 64 MB ayrilmis yigin)
void RunThread(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(nullptr, 64u * 1024 * 1024, [](LPVOID p) -> DWORD {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
        try { (*f)(); } catch (...) {}
        CoUninitialize();
        return 0;
    }, heap, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) CloseHandle(h);
    else delete heap;
}

bool SubresourceAllowed(const std::wstring& url, bool pageSecure, bool& blocked) {
    blocked = false;
    Url u = ParseUrl(url);
    if (!u.ok || (u.scheme != L"https" && u.scheme != L"http")) return false;
    if (pageSecure && u.scheme == L"http") return false;      // karisik icerik engeli
    if (IsBlockedHost(u.host)) { blocked = true; return false; } // izleyici / reklam
    return true;
}

// ---------------- Sayfa durumu ve JS ana bilgisayari ----------------

struct PageState;
void PageLog(PageState* p, int level, const std::wstring& msg);
bool PageNodeRect(PageState* p, const Node* n, double& x, double& y, double& w, double& h);
void PageRequestScript(PageState* p, Node* node, const std::wstring& url);
void PageSubmit(PageState* p, Node* form, Node* submitter, bool fromScript);
void PageSetUrl(PageState* p, const std::wstring& url);
void GoBack();
void GoForward();

struct PageHost : js::Host {
    PageState* page = nullptr;
    void Log(int level, const std::wstring& msg) override { PageLog(page, level, msg); }
    void Navigate(const std::wstring& url) override;
    std::wstring PageUrl() override;
    std::wstring Resolve(const std::wstring& rel) override;
    void ViewportSize(double& w, double& h) override;
    bool NodeRect(const Node* n, double& x, double& y, double& w, double& h) override { return PageNodeRect(page, n, x, y, w, h); }
    void SetUrlNoNav(const std::wstring& url) override { PageSetUrl(page, url); }
    void HistoryGo(int d) override;
    void RequestScript(Node* n, const std::wstring& url) override { PageRequestScript(page, n, url); }
    void SubmitForm(Node* f, Node* s) override { PageSubmit(page, f, s, true); }
    void UpdateVideoFrame(const std::wstring& key, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgra) override;
};

struct PageState {
    int id = 0;
    std::wstring url, base, title;
    bool secure = false, internal = false, js = false;
    NavMode mode = NavPush;
    std::unique_ptr<Document> doc;
    std::vector<Style> styles;
    std::map<std::wstring, std::wstring> cssCache;   // harici CSS adres -> metin
    std::set<std::wstring> cssPending;
    std::unique_ptr<js::Runtime> rt;
    PageHost host;
    ImageStore images;
    bool ui = false;                                 // arayuz is parcacigina baglandi mi
    std::vector<std::pair<Node*, std::wstring>> pendingScripts; // yukleme sirasinda istenen dinamik betikler
    std::vector<std::wstring> log;
    int jsErrors = 0, blocked = 0, imagesInFlight = 0;
    std::vector<std::wstring> imageQueue;
    std::wstring refreshUrl;
    double refreshDelay = -1;
    Node* focused = nullptr;
    std::vector<Node*> byId;
    double viewW = 1280, viewH = 800;
};

std::unique_ptr<PageState> g_page;

std::wstring PageHost::PageUrl() { return page->url; }
std::wstring PageHost::Resolve(const std::wstring& rel) { return ResolveUrl(page->base, rel); }
void PageHost::ViewportSize(double& w, double& h) {
    if (page->ui) { w = g_view.ViewW(); h = g_view.ViewH(); }
    else { w = page->viewW; h = page->viewH; }
}
void PageHost::Navigate(const std::wstring& url) { if (page->rt) page->rt->pendingNav = url; }
void PageHost::HistoryGo(int d) {
    if (!page->ui) return;
    if (d < 0) PostMessageW(g_frame, WM_COMMAND, ID_BACK, 0);
    else if (d > 0) PostMessageW(g_frame, WM_COMMAND, ID_FWD, 0);
}
void PageHost::UpdateVideoFrame(const std::wstring& key, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgra) {
    if (!page) return;
    ImageEntry e;
    e.state = 1;
    e.w = w;
    e.h = h;
    e.px = bgra;
    page->images.map[key] = std::move(e);
    if (page->ui) {
        g_view.Relayout(true);
        InvalidateRect(g_view.Hwnd(), nullptr, FALSE);
    }
}

struct Job {
    int id = 0;
    std::wstring url;
    std::wstring inlineHtml;     // dahili sayfalar icin hazir HTML
    bool upgradedFromHttp = false;
    std::wstring originalHttp;
    NavMode mode = NavPush;
    FetchOptions opt;
    bool js = true;
    double viewW = 1280, viewH = 800;
};

// ---------------- Dahili sayfalar ----------------

const wchar_t* kPageCss =
    L"<style>body{margin:0;background:#f3f5fa;color:#1c2333;font-family:'Segoe UI',sans-serif}"
    L".w{max-width:780px;margin:52px auto;padding:0 28px}h1{font-size:38px;font-weight:600;margin:0 0 6px}"
    L".alt{color:#5c6577;margin:0 0 26px;font-size:15px}.k{background:#fff;border:1px solid #dde2ec;padding:16px 22px;margin:0 0 14px}"
    L".k h3{margin:0 0 8px;font-size:17px}a{color:#2453cf;text-decoration:none}.u{color:#b42318}"
    L"code{background:#e8ecf4;padding:1px 4px}.kucuk{color:#7a8294;font-size:13px}"
    L".ara{display:flex;gap:8px;margin:0 0 22px}.ara input[name=q]{flex:1}"
    L".izgara{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.izgara a{display:block;background:#fff;border:1px solid #dde2ec;padding:10px 12px}</style>";

std::wstring HomePage() {
    return i18n::GetHomePageHtml();
}

std::wstring EnginePage() {
    return i18n::GetEnginePageHtml();
}

std::wstring MessagePage(const std::wstring& title, const std::wstring& msg, const std::wstring& url, const std::wstring& extra = L"") {
    return std::wstring(L"<html><head><title>") + HtmlEscape(title) + L"</title>" + kPageCss +
           L"</head><body><div class=w><h1 class=u>" + HtmlEscape(title) + L"</h1><p class=alt>" + HtmlEscape(msg) +
           L"</p><div class=k><p class=kucuk>" + HtmlEscape(url) + L"</p>" + extra + L"</div></div></body></html>";
}

std::wstring InternalPage(const std::wstring& url) {
    std::wstring p = ToLower(url);
    if (StartsWith(p, L"about:lang")) {
        if (p.find(L"tr") != std::wstring::npos) {
            i18n::SetCurrentLanguage(i18n::Language::Turkish);
        } else if (p.find(L"en") != std::wstring::npos) {
            i18n::SetCurrentLanguage(i18n::Language::English);
        }
        return HomePage();
    }
    if (p == L"about:home" || p == L"about:newtab") return HomePage();
    if (p == L"about:blank") return L"<html><head><title>about:blank</title></head><body></body></html>";
    if (p == L"about:motor" || p == L"about:engine") return EnginePage();
    return MessagePage(i18n::Get(i18n::StringId::PageNotFoundTitle), i18n::Get(i18n::StringId::PageNotFoundBody), url);
}

std::wstring BodyToHtml(const FetchResult& f, const std::wstring& url) {
    std::wstring ct = ToLower(f.contentType);
    if (ct.empty() || ct.find(L"text/html") != std::wstring::npos || ct.find(L"xhtml") != std::wstring::npos)
        return DecodeBody(f.body, f.contentType);
    if (StartsWith(ct, L"text/") || ct.find(L"json") != std::wstring::npos || ct.find(L"xml") != std::wstring::npos || ct.find(L"javascript") != std::wstring::npos)
        return L"<html><head><title>" + HtmlEscape(url) + L"</title></head><body><pre>" + HtmlEscape(DecodeBody(f.body, f.contentType)) + L"</pre></body></html>";
    if (StartsWith(ct, L"image/"))
        return L"<html><head><title>" + HtmlEscape(url) + L"</title></head><body style='margin:0;background:#1f2430;text-align:center'><img src=\"" + HtmlEscape(url) + L"\" style='max-width:100%'></body></html>";
    wchar_t fbuf[512];
    swprintf(fbuf, 512, i18n::Get(i18n::StringId::UnsupportedFileBody), f.contentType.c_str());
    return MessagePage(i18n::Get(i18n::StringId::UnsupportedFileTitle), fbuf, url);
}

// ---------------- Stil yeniden hesaplama ----------------

// Belgedeki <style> ve <link rel=stylesheet> sirasina gore stil sayfasi kurar.
// Henuz indirilmemis harici CSS adreslerini 'missing'e ekler.
void BuildSheet(PageState& p, StyleSheet& sheet, std::vector<std::wstring>* missing) {
    std::vector<Node*> st{p.doc->root.get()};
    int linkCount = 0;
    while (!st.empty()) {
        Node* n = st.back();
        st.pop_back();
        if (n->kind == Node::Elem) {
            if (n->name == L"style") {
                bool screen = true;
                if (auto* m = n->Attr(L"media")) { std::wstring ml = ToLower(*m); if (ml.find(L"print") != std::wstring::npos && ml.find(L"screen") == std::wstring::npos) screen = false; }
                if (screen) sheet.Add(TextContent(n));
                continue;
            }
            if (n->name == L"link") {
                auto* rel = n->Attr(L"rel");
                auto* href = n->Attr(L"href");
                if (rel && href && ToLower(*rel).find(L"stylesheet") != std::wstring::npos && ToLower(*rel).find(L"alternate") == std::wstring::npos) {
                    bool screen = true;
                    if (auto* m = n->Attr(L"media")) { std::wstring ml = ToLower(*m); if (ml.find(L"print") != std::wstring::npos && ml.find(L"screen") == std::wstring::npos) screen = false; }
                    if (screen && ++linkCount <= 24) {
                        std::wstring abs = ResolveUrl(p.base, *href);
                        auto it = p.cssCache.find(abs);
                        if (it != p.cssCache.end()) sheet.Add(it->second);
                        else if (missing) missing->push_back(abs);
                    }
                }
                continue;
            }
            if (n->name == L"script" || n->name == L"svg" || n->name == L"template") continue;
        }
        for (size_t i = n->kids.size(); i-- > 0;) st.push_back(n->kids[i].get());
    }
    sheet.BuildIndex();
}

// ---------------- Arka plan yukleyici ----------------

void PageLog(PageState* p, int level, const std::wstring& msg) {
    if (!p) return;
    if (level >= 2) p->jsErrors++;
    if (p->log.size() < 4000) p->log.push_back((level >= 2 ? L"✖ " : level == 1 ? L"⚠ " : L"") + msg);
    if (p->ui && g_consoleVisible && g_page.get() == p) {
        std::wstring line = p->log.back() + L"\r\n";
        int len = GetWindowTextLengthW(g_console);
        SendMessageW(g_console, EM_SETSEL, len, len);
        SendMessageW(g_console, EM_REPLACESEL, FALSE, LPARAM(line.c_str()));
    }
}

void LoadJob(Job job) {
    std::unique_ptr<PageState> p(new PageState());
    p->id = job.id;
    p->mode = job.mode;
    p->url = job.url;
    p->base = job.url;
    p->host.page = p.get();
    p->viewW = job.viewW;
    p->viewH = job.viewH;
    std::wstring html;
    Url u = ParseUrl(job.url);

    if (!job.inlineHtml.empty()) {
        html = job.inlineHtml;
        p->internal = true;
    } else if (u.ok && u.scheme == L"about") {
        html = InternalPage(job.url);
        p->internal = true;
    } else {
        FetchOptions opt = job.opt;
        opt.cookies = true; // ust duzey belge: birinci taraf oturum cerezleri
        FetchResult f = Fetch(job.url, kMaxPage, &g_navId, job.id, &opt);
        if (g_navId.load() != job.id) return;
        if (!f.ok) {
            p->internal = true;
            if (job.upgradedFromHttp && f.error != ERROR_WINHTTP_NAME_NOT_RESOLVED) {
                std::wstring link = L"<p><a href=\"meros-allow-http:" + HtmlEscape(job.originalHttp) +
                                    L"\">" + i18n::Get(i18n::StringId::InsecureWarningAction) + L"</a></p>";
                html = MessagePage(i18n::Get(i18n::StringId::InsecureWarningTitle),
                                   i18n::Get(i18n::StringId::InsecureWarningBody),
                                   job.originalHttp, link);
                p->url = job.originalHttp;
            } else {
                html = MessagePage(i18n::Get(i18n::StringId::PageLoadFailedTitle), NetErrorText(f.error), job.url);
            }
        } else {
            html = BodyToHtml(f, f.finalUrl);
            p->url = f.finalUrl;
            p->base = f.finalUrl;
            if (f.truncated) html += i18n::Get(i18n::StringId::PageTruncated);
        }
    }

    p->doc = ParseHTML(html);
    Document& doc = *p->doc;
    if (!doc.baseHref.empty() && !p->internal) {
        Url bu = ParseUrl(ResolveUrl(p->base, doc.baseHref));
        if (bu.ok && (bu.scheme == L"https" || bu.scheme == L"http")) p->base = bu.ToString();
    }
    Url pageUrl = ParseUrl(p->url);
    p->secure = pageUrl.ok && pageUrl.scheme == L"https";
    p->images.base = p->base;
    // dahili sayfalarda yalnizca kendi betiklerimiz calisir (about:motor)
    p->js = job.js && (!p->internal || ToLower(p->url) == L"about:motor");

    // Betikleri ve CSS'i topla (belge sirasiyla)
    std::vector<Node*> scripts;
    std::vector<std::wstring> scriptUrls;
    {
        std::vector<Node*> st{doc.root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->kind == Node::Elem && n->name == L"script") {
                std::wstring type = n->Attr(L"type") ? ToLower(Trim(*n->Attr(L"type"))) : L"";
                bool isMod = (type == L"module");
                bool classic = type.empty() || type == L"text/javascript" || type == L"application/javascript" || type == L"text/ecmascript" || type == L"application/x-javascript";
                if (classic || isMod) {
                    scripts.push_back(n);
                    scriptUrls.push_back(n->Attr(L"src") ? ResolveUrl(p->base, *n->Attr(L"src")) : L"");
                }
                continue;
            }
            for (size_t i = n->kids.size(); i-- > 0;) st.push_back(n->kids[i].get());
        }
    }
    std::vector<std::wstring> scriptText(scripts.size());
    std::vector<char> scriptOk(scripts.size(), 0);
    std::vector<std::wstring> cssMissing;
    {
        StyleSheet tmp;
        BuildSheet(*p, tmp, &cssMissing);
    }
    if (!p->internal) {
        // CSS ve betikler paralel indirilir (hiz)
        std::vector<std::thread> th;
        std::vector<std::wstring> cssText(cssMissing.size());
        for (size_t i = 0; i < cssMissing.size(); i++) {
            bool blocked;
            if (!SubresourceAllowed(cssMissing[i], p->secure, blocked)) { if (blocked) p->blocked++; continue; }
            int id = job.id;
            th.emplace_back([&cssText, &cssMissing, i, id]() {
                FetchResult cf = Fetch(cssMissing[i], kMaxCss, &g_navId, id);
                if (cf.ok && cf.status < 400) cssText[i] = DecodeBody(cf.body, cf.contentType);
            });
        }
        if (p->js) {
            size_t ext = 0;
            for (size_t i = 0; i < scripts.size(); i++) {
                if (scriptUrls[i].empty()) continue;
                bool blocked;
                if (!SubresourceAllowed(scriptUrls[i], p->secure, blocked) || ++ext > 48) { if (blocked) p->blocked++; continue; }
                int id = job.id;
                th.emplace_back([&scriptText, &scriptOk, &scriptUrls, i, id]() {
                    FetchResult sf = Fetch(scriptUrls[i], kMaxScript, &g_navId, id);
                    if (sf.ok && sf.status < 400) { scriptText[i] = DecodeBody(sf.body, sf.contentType); scriptOk[i] = 1; }
                });
            }
        }
        for (auto& t : th) t.join();
        if (g_navId.load() != job.id) return;
        for (size_t i = 0; i < cssMissing.size(); i++) p->cssCache[cssMissing[i]] = cssText[i];
    }

    // JavaScript: sayfa betiklerini belge sirasiyla calistir (bu is parcaciginda; arayuz donmaz)
    if (p->js) {
        p->rt.reset(new js::Runtime());
        p->rt->stackLimit = 40u * 1024 * 1024;
        p->rt->AttachDocument(p->doc.get(), &p->host);
        for (size_t i = 0; i < scripts.size(); i++) {
            Node* s = scripts[i];
            s->attrs.emplace_back(L"data-merjs-ran", L"1");
            std::wstring type = s->Attr(L"type") ? ToLower(Trim(*s->Attr(L"type"))) : L"";
            bool isMod = (type == L"module");
            std::wstring name = scriptUrls[i].empty() ? (isMod ? p->base : i18n::Get(i18n::StringId::InlineScript)) : scriptUrls[i];
            if (isMod) {
                if (!scriptUrls[i].empty()) {
                    if (!scriptOk[i]) continue;
                    p->rt->RunModule(scriptText[i], scriptUrls[i], s);
                } else {
                    p->rt->RunModule(TextContent(s), p->base, s);
                }
            } else {
                if (!scriptUrls[i].empty()) {
                    if (!scriptOk[i]) continue;
                    p->rt->RunScript(scriptText[i], name, s);
                } else {
                    p->rt->RunScript(TextContent(s), name, s);
                }
            }
            if (g_navId.load() != job.id) return;
            if (!p->rt->pendingNav.empty()) break;
        }
    }

    // <meta http-equiv=refresh>
    {
        std::vector<Node*> st{doc.root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->kind == Node::Elem && n->name == L"meta") {
                auto* he = n->Attr(L"http-equiv");
                auto* c = n->Attr(L"content");
                if (he && c && ToLower(*he) == L"refresh") {
                    double d = _wtof(c->c_str());
                    std::wstring lc = ToLower(*c);
                    size_t up = lc.find(L"url=");
                    if (up != std::wstring::npos && d >= 0 && d <= 30) {
                        std::wstring t = Trim(c->substr(up + 4));
                        while (!t.empty() && (t[0] == L'\'' || t[0] == L'"')) t.erase(0, 1);
                        while (!t.empty() && (t.back() == L'\'' || t.back() == L'"')) t.pop_back();
                        p->refreshUrl = ResolveUrl(p->base, t);
                        p->refreshDelay = d;
                    }
                }
            }
            for (auto& k : n->kids) st.push_back(k.get());
        }
    }

    StyleSheet sheet;
    BuildSheet(*p, sheet, nullptr);
    p->styles = ComputeStyles(doc, sheet, p->js);
    p->title = doc.title;

    if (g_navId.load() != job.id) return;
    PageState* raw = p.release();
    if (!PostMessageW(g_frame, WM_APP_PAGE, 0, LPARAM(raw))) delete raw;
}

// ---------------- Gezinme ----------------

std::wstring NormalizeInput(const std::wstring& in) {
    std::wstring t = Trim(in);
    if (t.empty()) return L"";
    std::wstring lower = ToLower(t);
    if (StartsWith(lower, L"about:")) return lower;
    if (lower.find(L"://") != std::wstring::npos) return t;
    bool looksLikeHost = t.find(L' ') == std::wstring::npos &&
                         (t.find(L'.') != std::wstring::npos || StartsWith(lower, L"localhost"));
    if (!looksLikeHost) return L"https://html.duckduckgo.com/html/?q=" + UrlEncodeComponent(t);
    return L"https://" + t;
}

void UpdateStatus() {
    std::wstring s = g_hoverText;
    if (s.empty() && g_page) {
        std::wstring right;
        if (g_page->blocked) {
            wchar_t trk[64];
            swprintf(trk, 64, i18n::Get(i18n::StringId::BlockedTrackers), g_page->blocked);
            right += trk;
        }
        if (g_page->js) {
            if (g_page->jsErrors) {
                wchar_t errs[64];
                swprintf(errs, 64, i18n::Get(i18n::StringId::JsErrors), g_page->jsErrors);
                right += errs;
            } else {
                right += i18n::Get(i18n::StringId::JsOk);
            }
        } else {
            right += i18n::Get(i18n::StringId::JsDisabled);
        }
        if (g_page->imagesInFlight > 0) {
            wchar_t imgb[64];
            swprintf(imgb, 64, i18n::Get(i18n::StringId::LoadingImages), g_page->imagesInFlight);
            right = imgb + right;
        }
        s = right;
    }
    SetWindowTextW(g_status, s.c_str());
}

void SetStatus(const std::wstring& s) { g_hoverText = s; UpdateStatus(); }

void UpdateButtons() {
    EnableWindow(g_back, g_histIdx > 0);
    EnableWindow(g_fwd, g_histIdx >= 0 && g_histIdx + 1 < int(g_history.size()));
}

void StartJob(Job job) {
    job.id = ++g_navId;
    job.js = g_jsEnabled;
    job.viewW = g_view.ViewW();
    job.viewH = g_view.ViewH();
    KillTimer(g_frame, T_REFRESH);
    SetStatus(std::wstring(i18n::Get(i18n::StringId::Loading)) + job.url);
    RunThread([job]() { LoadJob(job); });
}

void Navigate(const std::wstring& input, NavMode mode, const FetchOptions* opt = nullptr) {
    std::wstring target = NormalizeInput(input);
    if (target.empty()) return;
    Job job;
    job.mode = mode;
    if (opt) job.opt = *opt;
    Url u = ParseUrl(target);
    if (!u.ok) {
        job.url = L"about:hata";
        job.inlineHtml = MessagePage(i18n::Get(i18n::StringId::InvalidAddressTitle), i18n::Get(i18n::StringId::InvalidAddressBody), target);
    } else if (u.scheme != L"http" && u.scheme != L"https" && u.scheme != L"about") {
        job.url = L"about:engellendi";
        wchar_t bbuf[256];
        swprintf(bbuf, 256, i18n::Get(i18n::StringId::BlockedSchemeBody), u.scheme.c_str());
        job.inlineHtml = MessagePage(i18n::Get(i18n::StringId::BlockedSchemeTitle), bbuf, target);
    } else if (u.scheme == L"http" && !g_httpAllowed.count(u.host)) {
        // HTTPS-Oncelikli mod: once guvenli surum denenir
        Url s = u;
        s.scheme = L"https";
        if (s.port == 80) s.port = 443;
        job.url = s.ToString();
        job.upgradedFromHttp = true;
        job.originalHttp = u.ToString();
    } else {
        job.url = u.ToString();
    }
    if (u.ok && u.hadUserInfo) {
        job.url = L"about:uyari";
        wchar_t sbuf[512];
        swprintf(sbuf, 512, i18n::Get(i18n::StringId::SuspiciousAddressBody), u.host.c_str());
        wchar_t abuf[256];
        swprintf(abuf, 256, i18n::Get(i18n::StringId::SuspiciousAddressAction), u.host.c_str());
        std::wstring link = L"<p><a href=\"https://" + HtmlEscape(u.host) + L"/\">" + abuf + L"</a></p>";
        job.inlineHtml = MessagePage(i18n::Get(i18n::StringId::SuspiciousAddressTitle), sbuf, target, link);
    }
    StartJob(std::move(job));
}

void GoBack() { if (g_histIdx > 0) { g_histIdx--; UpdateButtons(); Navigate(g_history[size_t(g_histIdx)], NavHistory); } }
void GoForward() { if (g_histIdx + 1 < int(g_history.size())) { g_histIdx++; UpdateButtons(); Navigate(g_history[size_t(g_histIdx)], NavHistory); } }
void Reload() { if (g_page) Navigate(g_page->url, NavReload); }

void PageSetUrl(PageState* p, const std::wstring& url) {
    if (!p || !p->ui || g_page.get() != p) return;
    Url u = ParseUrl(url);
    Url cur = ParseUrl(p->url);
    if (!u.ok || u.Origin() != cur.Origin()) return; // pushState yalnizca ayni koken
    p->url = url;
    if (g_histIdx >= 0 && size_t(g_histIdx) < g_history.size()) g_history[size_t(g_histIdx)] = url;
    SetWindowTextW(g_addr, url.c_str());
}

// ---------------- JS sonrasi isler ----------------

void Restyle(bool fetchMissing);
void ScanImages();
void ScheduleTimers();

Node* NodeById(int id) {
    if (!g_page || id < 0) return nullptr;
    PageState& p = *g_page;
    if (size_t(id) >= p.byId.size() || !p.byId[size_t(id)] || p.byId[size_t(id)]->id != id) {
        p.byId.assign(size_t(p.doc->count) + 1, nullptr);
        std::vector<Node*> st{p.doc->root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->id >= 0 && size_t(n->id) < p.byId.size()) p.byId[size_t(n->id)] = n;
            for (auto& k : n->kids) st.push_back(k.get());
        }
    }
    return size_t(id) < p.byId.size() ? p.byId[size_t(id)] : nullptr;
}

bool g_restylePending = false;
void RequestRestyle(UINT ms) {
    if (g_restylePending) return;
    g_restylePending = true;
    SetTimer(g_frame, T_RESTYLE, ms, nullptr);
}

// Her JS gorevinden sonra: DOM degistiyse yeniden stil (en fazla ~20/sn), gezinme istekleri, zamanlayicilar
void AfterJs() {
    if (!g_page) return;
    PageState& p = *g_page;
    if (p.rt) {
        if (!p.rt->pendingNav.empty()) {
            std::wstring nav = p.rt->pendingNav;
            p.rt->pendingNav.clear();
            Navigate(nav, NavPush);
            return;
        }
        if (p.rt->dirty) RequestRestyle(50);
        if (p.doc->title != p.title) {
            p.title = p.doc->title;
            SetWindowTextW(g_frame, ((p.title.empty() ? p.url : p.title) + L" — " + kAppName).c_str());
        }
    }
    ScheduleTimers();
    UpdateStatus();
}

void ScheduleTimers() {
    KillTimer(g_frame, T_JS);
    if (!g_page || !g_page->rt) return;
    double due = g_page->rt->NextTimerDue();
    if (due < 0) return;
    double delay = std::max(4.0, due - g_page->rt->Now());
    SetTimer(g_frame, T_JS, UINT(std::min(delay, 3600000.0)), nullptr);
}

void Restyle(bool fetchMissing) {
    if (!g_page) return;
    PageState& p = *g_page;
    StyleSheet sheet;
    std::vector<std::wstring> missing;
    BuildSheet(p, sheet, &missing);
    p.styles = ComputeStyles(*p.doc, sheet, p.js);
    if (p.rt) p.rt->dirty = false;
    p.byId.clear();
    g_view.Relayout(true);
    if (fetchMissing) {
        for (auto& u : missing) {
            if (p.cssPending.count(u)) continue;
            bool blocked;
            if (!SubresourceAllowed(u, p.secure, blocked)) { if (blocked) p.blocked++; p.cssCache[u] = L""; continue; }
            p.cssPending.insert(u);
            int id = p.id;
            RunThread([u, id]() {
                FetchResult cf = Fetch(u, kMaxCss, &g_navId, id);
                auto* res = new std::pair<std::wstring, std::wstring>(u, cf.ok && cf.status < 400 ? DecodeBody(cf.body, cf.contentType) : L"");
                if (!PostMessageW(g_frame, WM_APP_CSS, WPARAM(id), LPARAM(res))) delete res;
            });
        }
    }
    ScanImages();
}

// ---------------- Resimler ----------------

struct ImageResult { std::wstring url; UINT w = 0, h = 0; std::vector<uint8_t> px; bool ok = false, blocked = false; };

void PumpImages() {
    if (!g_page) return;
    PageState& p = *g_page;
    while (p.imagesInFlight < 6 && !p.imageQueue.empty()) {
        std::wstring u = p.imageQueue.front();
        p.imageQueue.erase(p.imageQueue.begin());
        p.imagesInFlight++;
        int id = p.id;
        bool secure = p.secure;
        RunThread([u, id, secure]() {
            std::unique_ptr<ImageResult> r(new ImageResult());
            r->url = u;
            std::string bytes;
            if (StartsWith(ToLower(u.substr(0, 5)), L"data:")) {
                DecodeDataUrl(u, bytes);
            } else {
                bool blocked;
                if (SubresourceAllowed(u, secure, blocked)) {
                    FetchResult f = Fetch(u, kMaxImage, &g_navId, id);
                    if (f.ok && f.status < 400) bytes.swap(f.body);
                } else r->blocked = blocked;
            }
            if (!bytes.empty()) r->ok = DecodeImage(bytes, r->w, r->h, r->px);
            ImageResult* raw = r.release();
            if (!PostMessageW(g_frame, WM_APP_IMAGE, WPARAM(id), LPARAM(raw))) delete raw;
        });
    }
}

void ScanImages() {
    if (!g_page) return;
    PageState& p = *g_page;
    std::vector<Node*> st{p.doc->root.get()};
    while (!st.empty()) {
        Node* n = st.back();
        st.pop_back();
        if (n->kind == Node::Elem) {
            if (n->name == L"img" || (n->name == L"input" && n->Attr(L"type") && ToLower(*n->Attr(L"type")) == L"image")) {
                if (n->id >= 0 && size_t(n->id) < p.styles.size() && p.styles[size_t(n->id)].display != Style::None) {
                    std::wstring u = p.images.UrlOf(n);
                    if (!u.empty() && !p.images.map.count(u) && p.images.map.size() < 600) {
                        p.images.map[u].state = 0;
                        p.imageQueue.push_back(u);
                    }
                }
            }
            if (n->id >= 0 && size_t(n->id) < p.styles.size() && p.styles[size_t(n->id)].display == Style::None) continue;
        }
        for (auto& k : n->kids) st.push_back(k.get());
    }
    PumpImages();
}

// ---------------- Dinamik betikler ----------------

struct ScriptResult { Node* node; std::wstring url, text; bool ok; };

void FetchScript(PageState* p, Node* node, const std::wstring& url) {
    bool blocked;
    if (!SubresourceAllowed(url, p->secure, blocked)) {
        if (blocked) { p->blocked++; PageLog(p, 0, L"[engellendi] " + url); }
        auto* res = new ScriptResult{node, url, L"", false};
        if (!PostMessageW(g_frame, WM_APP_SCRIPT, WPARAM(p->id), LPARAM(res))) delete res;
        return;
    }
    int id = p->id;
    RunThread([node, url, id]() {
        FetchResult f = Fetch(url, kMaxScript, &g_navId, id);
        auto* res = new ScriptResult{node, url, f.ok && f.status < 400 ? DecodeBody(f.body, f.contentType) : L"", f.ok && f.status < 400};
        if (!PostMessageW(g_frame, WM_APP_SCRIPT, WPARAM(id), LPARAM(res))) delete res;
    });
}

void PageRequestScript(PageState* p, Node* node, const std::wstring& url) {
    if (!p) return;
    if (!p->ui) { p->pendingScripts.emplace_back(node, url); return; }
    FetchScript(p, node, url);
}

// ---------------- Formlar ----------------

std::wstring FieldValue(Node* n) {
    if (n->dirtyValue) return n->value;
    if (n->name == L"textarea") return TextContent(n);
    if (n->name == L"select") {
        std::wstring v;
        bool first = true;
        std::vector<Node*> st{n};
        while (!st.empty()) {
            Node* c = st.back();
            st.pop_back();
            if (c->kind == Node::Elem && c->name == L"option") {
                std::wstring ov = c->Attr(L"value") ? *c->Attr(L"value") : Trim(TextContent(c));
                if (first) { v = ov; first = false; }
                if (c->Attr(L"selected")) return ov;
            }
            for (size_t i = c->kids.size(); i-- > 0;) st.push_back(c->kids[i].get());
        }
        return v;
    }
    return n->Attr(L"value") ? *n->Attr(L"value") : L"";
}

bool IsChecked(Node* n) { return n->dirtyChecked ? n->checked : n->Attr(L"checked") != nullptr; }

void PageSubmit(PageState* p, Node* form, Node* submitter, bool fromScript) {
    if (!p || !p->ui || g_page.get() != p || !form) return;
    if (!fromScript && p->rt) {
        if (!p->rt->DispatchSimple(form, L"submit", true)) { AfterJs(); return; }
        if (!p->rt->pendingNav.empty()) { AfterJs(); return; }
    }
    std::wstring action = form->Attr(L"action") && !Trim(*form->Attr(L"action")).empty() ? ResolveUrl(p->base, *form->Attr(L"action")) : p->url;
    if (submitter && submitter->Attr(L"formaction")) action = ResolveUrl(p->base, *submitter->Attr(L"formaction"));
    bool post = form->Attr(L"method") && ToLower(Trim(*form->Attr(L"method"))) == L"post";
    std::wstring q;
    auto add = [&](const std::wstring& k, const std::wstring& v) {
        if (!q.empty()) q += L'&';
        q += UrlEncodeComponent(k) + L"=" + UrlEncodeComponent(v);
    };
    std::vector<Node*> st{form};
    while (!st.empty()) {
        Node* n = st.back();
        st.pop_back();
        if (n->kind == Node::Elem && n != form && !n->Attr(L"disabled")) {
            auto* name = n->Attr(L"name");
            if (name && !name->empty()) {
                if (n->name == L"input") {
                    std::wstring t = n->Attr(L"type") ? ToLower(*n->Attr(L"type")) : L"text";
                    if (t == L"checkbox" || t == L"radio") { if (IsChecked(n)) add(*name, n->Attr(L"value") ? *n->Attr(L"value") : L"on"); }
                    else if (t == L"submit" || t == L"button" || t == L"reset" || t == L"image") { if (n == submitter) add(*name, FieldValue(n)); }
                    else if (t != L"file") add(*name, FieldValue(n));
                } else if (n->name == L"select" || n->name == L"textarea") add(*name, FieldValue(n));
                else if (n->name == L"button" && n == submitter) add(*name, FieldValue(n));
            }
        }
        for (size_t i = n->kids.size(); i-- > 0;) st.push_back(n->kids[i].get());
    }
    if (post) {
        FetchOptions opt;
        opt.method = L"POST";
        opt.body = ToUtf8(q);
        opt.contentType = L"application/x-www-form-urlencoded";
        Url cur = ParseUrl(p->url), dst = ParseUrl(action);
        if (cur.ok && dst.ok && cur.Origin() == dst.Origin()) opt.referer = cur.Origin() + L"/"; // bazi siteler CSRF icin ister (yalnizca koken)
        Navigate(action, NavPush, &opt);
    } else {
        size_t qm = action.find(L'?');
        if (qm != std::wstring::npos) action = action.substr(0, qm);
        size_t hs = action.find(L'#');
        if (hs != std::wstring::npos) action = action.substr(0, hs);
        Navigate(action + L"?" + q, NavPush);
    }
}

Node* FormOf(Node* n) {
    for (Node* p = n; p; p = p->parent) if (p->kind == Node::Elem && p->name == L"form") return p;
    return nullptr;
}

// ---------------- Etkilesim ----------------

void Unfocus() {
    if (g_page) g_page->focused = nullptr;
    g_view.SetFocusNode(-1);
}

void OnLinkHref(const std::wstring& href, bool ctrl) {
    (void)ctrl;
    std::wstring h = Trim(href);
    if (StartsWith(ToLower(h), L"meros-allow-http:")) {
        if (!g_page || !g_page->internal) return; // yalnizca kendi uyari sayfamiz bu izni verebilir
        Url u = ParseUrl(h.substr(17));
        if (u.ok && u.scheme == L"http") {
            g_httpAllowed.insert(u.host);
            Navigate(u.ToString(), NavPush);
        }
        return;
    }
    if (h.empty() || h[0] == L'#') return;
    if (StartsWith(ToLower(h), L"javascript:")) return;
    Navigate(ResolveUrl(g_page ? g_page->base : L"", h), NavPush);
}

void OnViewClick(int nodeId, int link, bool ctrl) {
    if (!g_page) return;
    PageState& p = *g_page;
    Node* n = NodeById(nodeId);
    if (!n) {
        Unfocus();
        if (link >= 0 && size_t(link) < g_view.Layout().links.size()) OnLinkHref(g_view.Layout().links[size_t(link)], ctrl);
        return;
    }
    bool allowDefault = true;
    if (p.rt) {
        allowDefault = p.rt->DispatchClick(n);
        if (!p.rt->pendingNav.empty()) { AfterJs(); return; }
    }
    if (g_page.get() != &p) return;
    if (allowDefault) {
        bool handled = false;
        for (Node* c = n; c && !handled; c = c->parent) {
            if (c->kind != Node::Elem) continue;
            const std::wstring& t = c->name;
            if (t == L"a" && c->Attr(L"href")) { Unfocus(); OnLinkHref(*c->Attr(L"href"), ctrl); return; }
            if (t == L"input") {
                std::wstring type = c->Attr(L"type") ? ToLower(*c->Attr(L"type")) : L"text";
                if (type == L"checkbox") { c->checked = !IsChecked(c); c->dirtyChecked = true; handled = true; if (p.rt) p.rt->DispatchSimple(c, L"change", true); }
                else if (type == L"radio") {
                    Node* f = FormOf(c);
                    std::wstring nm = c->Attr(L"name") ? *c->Attr(L"name") : L"";
                    std::vector<Node*> st{f ? f : p.doc->root.get()};
                    while (!st.empty()) {
                        Node* x = st.back();
                        st.pop_back();
                        if (x->name == L"input" && x->Attr(L"name") && *x->Attr(L"name") == nm) { x->checked = false; x->dirtyChecked = true; }
                        for (auto& k : x->kids) st.push_back(k.get());
                    }
                    c->checked = true;
                    c->dirtyChecked = true;
                    handled = true;
                    if (p.rt) p.rt->DispatchSimple(c, L"change", true);
                } else if (type == L"submit" || type == L"image") { Unfocus(); PageSubmit(&p, FormOf(c), c, false); return; }
                else if (type == L"button" || type == L"reset" || type == L"hidden") handled = true;
                else {
                    p.focused = c;
                    g_view.SetFocusNode(c->id);
                    if (p.rt) p.rt->DispatchSimple(c, L"focus", false);
                    AfterJs();
                    return;
                }
            } else if (t == L"textarea") {
                p.focused = c;
                g_view.SetFocusNode(c->id);
                AfterJs();
                return;
            } else if (t == L"button") {
                std::wstring type = c->Attr(L"type") ? ToLower(*c->Attr(L"type")) : L"submit";
                if (type == L"submit" && FormOf(c)) { Unfocus(); PageSubmit(&p, FormOf(c), c, false); return; }
                handled = true;
            } else if (t == L"select") {
                // sonraki secenege gec (basit acilir liste)
                std::vector<Node*> opts;
                std::vector<Node*> st{c};
                while (!st.empty()) {
                    Node* x = st.back();
                    st.pop_back();
                    if (x->name == L"option") opts.push_back(x);
                    for (size_t i = x->kids.size(); i-- > 0;) st.push_back(x->kids[i].get());
                }
                if (!opts.empty()) {
                    size_t cur = 0;
                    for (size_t i = 0; i < opts.size(); i++) if (opts[i]->Attr(L"selected")) cur = i;
                    for (auto* o : opts) for (size_t i = 0; i < o->attrs.size(); i++) if (o->attrs[i].first == L"selected") { o->attrs.erase(o->attrs.begin() + long(i)); break; }
                    opts[(cur + 1) % opts.size()]->attrs.emplace_back(L"selected", L"");
                    c->dirtyValue = false;
                    if (p.rt) p.rt->DispatchSimple(c, L"change", true);
                }
                handled = true;
            } else if (t == L"label") {
                Node* target = nullptr;
                if (auto* f = c->Attr(L"for")) {
                    std::vector<Node*> st{p.doc->root.get()};
                    while (!st.empty() && !target) {
                        Node* x = st.back();
                        st.pop_back();
                        if (x->idAttr == *f) target = x;
                        for (auto& k : x->kids) st.push_back(k.get());
                    }
                }
                if (target && target != n) { OnViewClick(target->id, -1, ctrl); return; }
            }
        }
    }
    if (link >= 0 && allowDefault && size_t(link) < g_view.Layout().links.size()) { OnLinkHref(g_view.Layout().links[size_t(link)], ctrl); return; }
    Unfocus();
    if (g_page && g_page->rt) g_page->rt->dirty = true;
    AfterJs();
    g_view.Relayout(true);
}

void EditFocused(const std::wstring& insert, bool backspace) {
    if (!g_page || !g_page->focused) return;
    Node* n = g_page->focused;
    if (!n->dirtyValue) { n->value = FieldValue(n); n->dirtyValue = true; }
    if (backspace) {
        if (!n->value.empty()) {
            size_t cut = 1;
            if (n->value.size() >= 2 && n->value.back() >= 0xDC00 && n->value.back() <= 0xDFFF) cut = 2;
            n->value.erase(n->value.size() - cut);
        }
    } else {
        if (n->value.size() + insert.size() > 100000) return;
        n->value += insert;
    }
    g_view.Relayout(true);
    if (g_page->rt) { g_page->rt->DispatchSimple(n, L"input", true); AfterJs(); }
}

void OnViewChar(wchar_t ch) {
    if (!g_page || !g_page->focused) return;
    if (ch == 8) { EditFocused(L"", true); return; }
    if (ch == L'\r') {
        if (g_page->focused->name == L"textarea") EditFocused(L"\n", false);
        return;
    }
    if (ch < 32) return;
    EditFocused(std::wstring(1, ch), false);
}

bool OnViewKey(UINT vk) {
    if (!g_page || !g_page->focused) return false;
    Node* n = g_page->focused;
    if (vk == VK_ESCAPE || vk == VK_TAB) { Unfocus(); return true; }
    if (vk == VK_RETURN && n->name != L"textarea") {
        if (g_page->rt) {
            bool ok = g_page->rt->DispatchKey(n, L"keydown", L"Enter");
            g_page->rt->DispatchSimple(n, L"change", true);
            if (!ok || !g_page->rt->pendingNav.empty()) { AfterJs(); return true; }
        }
        if (Node* f = FormOf(n)) { Unfocus(); PageSubmit(g_page.get(), f, nullptr, false); }
        else AfterJs();
        return true;
    }
    if (vk == 'V' && GetKeyState(VK_CONTROL) < 0) {
        if (OpenClipboard(g_frame)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                    std::wstring s(t);
                    GlobalUnlock(h);
                    if (n->name != L"textarea") for (auto& c : s) if (c == L'\r' || c == L'\n') c = L' ';
                    EditFocused(s.substr(0, 20000), false);
                }
            }
            CloseClipboard();
        }
        return true;
    }
    return false;
}

bool PageNodeRect(PageState* p, const Node* n, double& x, double& y, double& w, double& h) {
    if (!p || !p->ui || g_page.get() != p || !n) return false;
    std::unordered_set<int> ids;
    std::vector<const Node*> st{n};
    while (!st.empty() && ids.size() < 200000) {
        const Node* c = st.back();
        st.pop_back();
        ids.insert(c->id);
        for (auto& k : c->kids) st.push_back(k.get());
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& d : g_view.Layout().items) {
        if (d.node < 0 || !ids.count(d.node)) continue;
        x0 = std::min(x0, d.x); y0 = std::min(y0, d.y);
        x1 = std::max(x1, d.x + d.w); y1 = std::max(y1, d.y + d.h);
    }
    if (x1 < x0) return false;
    x = x0;
    y = y0 - g_view.ScrollY();
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

// ---------------- Sayfa etkinlestirme ----------------

void OnPageReady(PageState* raw) {
    std::unique_ptr<PageState> p(raw);
    if (p->id != g_navId.load()) return;
    if (p->mode == NavPush) {
        if (g_histIdx + 1 < int(g_history.size())) g_history.resize(size_t(g_histIdx + 1));
        if (g_history.empty() || g_history.back() != p->url) g_history.push_back(p->url);
        g_histIdx = int(g_history.size()) - 1;
    } else if (p->mode == NavHistory && g_histIdx >= 0) {
        g_history[size_t(g_histIdx)] = p->url;
    }
    KillTimer(g_frame, T_JS);
    KillTimer(g_frame, T_RESTYLE);
    g_restylePending = false;
    g_view.SetPage(ViewPage());
    g_page = std::move(p);   // eski sayfa (DOM, JS, resimler) burada serbest birakilir
    PageState& pg = *g_page;
    pg.ui = true;
    UpdateButtons();
    SetWindowTextW(g_addr, pg.url.c_str());
    SetWindowTextW(g_frame, ((pg.title.empty() ? pg.url : pg.title) + L" — " + kAppName).c_str());
    Url u = ParseUrl(pg.url);
    if (u.ok && u.scheme == L"about") SetWindowTextW(g_sec, L"⌂ merOS");
    else if (pg.secure) SetWindowTextW(g_sec, L"✔ HTTPS");
    else SetWindowTextW(g_sec, L"⚠ HTTP");
    ViewPage vp;
    vp.doc = pg.doc.get();
    vp.styles = &pg.styles;
    vp.images = &pg.images;
    g_view.SetPage(vp);
    if (g_consoleVisible) {
        std::wstring all;
        for (auto& l : pg.log) all += l + L"\r\n";
        SetWindowTextW(g_console, all.c_str());
    }
    g_hoverText.clear();
    ScanImages();
    if (pg.rt) {
        if (!pg.rt->pendingNav.empty()) { AfterJs(); return; }
        for (auto& ps : pg.pendingScripts) FetchScript(&pg, ps.first, ps.second);
        pg.pendingScripts.clear();
        pg.rt->FireLoadEvents();
        if (pg.rt->dirty) Restyle(true);
        AfterJs();
    } else {
        UpdateStatus();
    }
    if (pg.refreshDelay >= 0 && !pg.refreshUrl.empty()) SetTimer(g_frame, T_REFRESH, UINT(pg.refreshDelay * 1000) + 1, nullptr);
    if (!g_snapshotPath.empty()) SetTimer(g_frame, T_SNAPSHOT, 4000, nullptr);
}

void SaveScreenshot() {
    if (!g_page) return;
    wchar_t dir[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH);
    std::wstring folder = (n > 0 && n < MAX_PATH) ? std::wstring(dir) + L"\\Pictures" : L".";
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, 96, L"\\merOS-Shield-%04d%02d%02d-%02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring path = folder + name;
    if (g_view.SaveSnapshot(path)) SetStatus(std::wstring(i18n::Get(i18n::StringId::SnapshotSaved)) + path);
    else SetStatus(i18n::Get(i18n::StringId::SnapshotFailed));
}

// ---------------- Pencere ----------------

float Scale() { return float(GetDpiForWindow(g_frame)) / 96.f; }

void MakeFonts() {
    for (HFONT f : {g_uiFont, g_symFont, g_monoFont}) if (f) DeleteObject(f);
    int dpi = int(GetDpiForWindow(g_frame));
    g_uiFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_symFont = CreateFontW(-MulDiv(12, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    g_monoFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    for (HWND h : {g_addr, g_sec, g_status, g_js}) SendMessageW(h, WM_SETFONT, WPARAM(g_uiFont), TRUE);
    for (HWND h : {g_back, g_fwd, g_reload, g_home}) SendMessageW(h, WM_SETFONT, WPARAM(g_symFont), TRUE);
    SendMessageW(g_console, WM_SETFONT, WPARAM(g_monoFont), TRUE);
}

void LayoutFrame() {
    RECT rc;
    GetClientRect(g_frame, &rc);
    float s = Scale();
    int pad = int(6 * s), bh = int(30 * s), bw = int(36 * s), top = int(42 * s), sb = int(22 * s);
    int con = g_consoleVisible ? int(170 * s) : 0;
    int x = pad;
    for (HWND b : {g_back, g_fwd, g_reload, g_home}) { MoveWindow(b, x, pad, bw, bh, TRUE); x += bw + int(2 * s); }
    x += pad;
    int secW = int(86 * s), jsW = int(44 * s);
    MoveWindow(g_sec, x, pad + int(6 * s), secW, bh - int(8 * s), TRUE);
    x += secW;
    MoveWindow(g_addr, x, pad + int(2 * s), std::max(50, int(rc.right) - x - pad - jsW - pad), bh - int(4 * s), TRUE);
    MoveWindow(g_js, int(rc.right) - pad - jsW, pad, jsW, bh, TRUE);
    MoveWindow(g_view.Hwnd(), 0, top, rc.right, std::max(0, int(rc.bottom) - top - sb - con), TRUE);
    MoveWindow(g_console, 0, int(rc.bottom) - sb - con, rc.right, con, TRUE);
    ShowWindow(g_console, g_consoleVisible ? SW_SHOW : SW_HIDE);
    MoveWindow(g_status, pad, rc.bottom - sb + int(3 * s), rc.right - 2 * pad, sb - int(4 * s), TRUE);
}

void ToggleConsole() {
    g_consoleVisible = !g_consoleVisible;
    if (g_consoleVisible && g_page) {
        std::wstring all;
        for (auto& l : g_page->log) all += l + L"\r\n";
        SetWindowTextW(g_console, all.c_str());
    }
    LayoutFrame();
}

LRESULT CALLBACK AddrProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        int n = GetWindowTextLengthW(h);
        std::wstring t(size_t(n) + 1, L'\0');
        GetWindowTextW(h, &t[0], n + 1);
        t.resize(size_t(n));
        Navigate(t, NavPush);
        SetFocus(g_view.Hwnd());
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { SetWindowTextW(h, g_page ? g_page->url.c_str() : L""); SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == L'\n' || w == 0x1B)) return 0;
    if (m == WM_CHAR && w == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_SETFOCUS) PostMessageW(h, EM_SETSEL, 0, -1);
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK FrameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_frame = h;
        auto btn = [&](const wchar_t* t, int id, DWORD extra = BS_PUSHBUTTON) {
            return CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | extra, 0, 0, 10, 10, h, HMENU(INT_PTR(id)), g_inst, nullptr);
        };
        g_back = btn(L"←", ID_BACK);
        g_fwd = btn(L"→", ID_FWD);
        g_reload = btn(L"↻", ID_RELOAD);
        g_home = btn(L"⌂", ID_HOME);
        g_js = btn(L"JS", ID_JS, BS_AUTOCHECKBOX | BS_PUSHLIKE);
        SendMessageW(g_js, BM_SETCHECK, g_jsEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        g_sec = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_SEC)), g_inst, nullptr);
        g_addr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_ADDR)), g_inst, nullptr);
        SendMessageW(g_addr, EM_SETCUEBANNER, TRUE, LPARAM(i18n::Get(i18n::StringId::SearchOrAddress)));
        SendMessageW(g_addr, EM_SETLIMITTEXT, 8192, 0);
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_STATUS)), g_inst, nullptr);
        g_console = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_CONSOLE)), g_inst, nullptr);
        SendMessageW(g_console, EM_SETLIMITTEXT, 0, 0);
        SetWindowSubclass(g_addr, AddrProc, 1, 0);
        g_view.Create(h, g_inst);
        g_view.onClick = OnViewClick;
        g_view.onChar = OnViewChar;
        g_view.onKey = OnViewKey;
        g_view.onHover = [](const std::wstring& href) {
            SetStatus(href.empty() ? L"" : ResolveUrl(g_page ? g_page->base : L"", href));
        };
        MakeFonts();
        UpdateButtons();
        return 0;
    }
    case WM_SIZE:
        LayoutFrame();
        return 0;
    case WM_DPICHANGED: {
        RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        MakeFonts();
        LayoutFrame();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = HDC(w);
        if (HWND(l) == g_console) {
            SetTextColor(dc, RGB(30, 34, 44));
            SetBkColor(dc, RGB(250, 250, 252));
            return LRESULT(GetStockObject(WHITE_BRUSH));
        }
        if (HWND(l) == g_sec) {
            std::wstring t(32, L'\0');
            t.resize(size_t(GetWindowTextW(g_sec, &t[0], 32)));
            SetTextColor(dc, t.find(L"HTTPS") != std::wstring::npos ? RGB(22, 128, 61) : t.find(L"HTTP") != std::wstring::npos ? RGB(185, 28, 28) : RGB(70, 80, 100));
        } else {
            SetTextColor(dc, RGB(90, 98, 112));
        }
        SetBkMode(dc, TRANSPARENT);
        return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BACK: GoBack(); return 0;
        case ID_FWD: GoForward(); return 0;
        case ID_RELOAD: Reload(); return 0;
        case ID_HOME: Navigate(L"about:home", NavPush); return 0;
        case ID_JS:
            g_jsEnabled = SendMessageW(g_js, BM_GETCHECK, 0, 0) == BST_CHECKED;
            Reload();
            SetFocus(g_view.Hwnd());
            return 0;
        }
        break;
    case WM_TIMER:
        KillTimer(h, w);
        if (w == T_JS && g_page && g_page->rt) {
            g_page->rt->RunDueTimers(g_page->rt->Now());
            AfterJs();
        } else if (w == T_RESTYLE) {
            g_restylePending = false;
            if (g_page) Restyle(true);
        } else if (w == T_SNAPSHOT) {
            g_view.SaveSnapshot(g_snapshotPath);
            DestroyWindow(h);
        } else if (w == T_REFRESH && g_page && !g_page->refreshUrl.empty()) {
            Navigate(g_page->refreshUrl, NavPush);
        }
        return 0;
    case WM_APP_PAGE:
        OnPageReady(reinterpret_cast<PageState*>(l));
        return 0;
    case WM_APP_IMAGE: {
        std::unique_ptr<ImageResult> r(reinterpret_cast<ImageResult*>(l));
        if (!g_page || g_page->id != int(w)) return 0;
        PageState& p = *g_page;
        p.imagesInFlight = std::max(0, p.imagesInFlight - 1);
        if (r->blocked) p.blocked++;
        ImageEntry& e = p.images.map[r->url];
        if (r->ok) { e.state = 1; e.w = r->w; e.h = r->h; e.px.swap(r->px); }
        else e.state = 2;
        PumpImages();
        if (r->ok) RequestRestyle(p.imagesInFlight ? 120 : 30);
        UpdateStatus();
        return 0;
    }
    case WM_APP_SCRIPT: {
        std::unique_ptr<ScriptResult> r(reinterpret_cast<ScriptResult*>(l));
        if (!g_page || g_page->id != int(w) || !g_page->rt) return 0;
        if (r->ok) g_page->rt->RunScript(r->text, r->url, r->node);
        else PageLog(g_page.get(), 1, std::wstring(i18n::Get(i18n::StringId::ScriptLoadFailed)) + r->url);
        if (g_page && g_page->id == int(w)) {
            g_page->rt->ScriptLoaded(r->node, r->ok);
            AfterJs();
        }
        return 0;
    }
    case WM_APP_CSS: {
        std::unique_ptr<std::pair<std::wstring, std::wstring>> r(reinterpret_cast<std::pair<std::wstring, std::wstring>*>(l));
        if (!g_page || g_page->id != int(w)) return 0;
        g_page->cssCache[r->first] = r->second;
        g_page->cssPending.erase(r->first);
        RequestRestyle(30);
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(g_view.Hwnd());
        return 0;
    case WM_DESTROY:
        g_navId++; // bekleyen yuklemeleri iptal et
        g_view.SetPage(ViewPage());
        g_page.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Isletim sistemi seviyesinde ek korumalar
void HardenProcess() {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32); // DLL ele gecirme (hijacking) onlemi

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY il{};
    il.NoRemoteImages = 1;
    il.NoLowMandatoryLabelImages = 1;
    il.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &il, sizeof(il));

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep{};
    ep.DisableExtensionPoints = 1; // eski AppInit/hook DLL enjeksiyonlarini kapat
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ep, sizeof(ep));

    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY hc{};
    hc.RaiseExceptionOnInvalidHandleReference = 1;
    hc.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &hc, sizeof(hc));

    PROCESS_MITIGATION_FONT_DISABLE_POLICY fp{};
    fp.DisableNonSystemFonts = 1; // yalnizca sistem fontlari (font ayristirici saldirilarina karsi)
    SetProcessMitigationPolicy(ProcessFontDisablePolicy, &fp, sizeof(fp));
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    HardenProcess();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_inst = inst;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (!NetInit()) {
        MessageBoxW(nullptr, i18n::Get(i18n::StringId::NetInitFailed), kAppName, MB_ICONERROR);
        return 1;
    }
    PageView::Register(inst);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"merOSShieldFrame";
    wc.hIcon = LoadIcon(nullptr, IDI_SHIELD);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, wc.lpszClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    ShowWindow(h, show);
    UpdateWindow(h);

    std::wstring start = (cmd && *cmd) ? Trim(cmd) : L"about:home";
    // --snapshot <png> <adres>: sayfayi yukle, goruntusunu kaydet ve kapan (test/otomasyon icin)
    if (StartsWith(start, L"--snapshot ")) {
        std::wstring rest = Trim(start.substr(11));
        size_t sp = rest.find(L' ');
        g_snapshotPath = sp == std::wstring::npos ? rest : rest.substr(0, sp);
        start = sp == std::wstring::npos ? L"about:home" : Trim(rest.substr(sp + 1));
    }
    if (!start.empty() && start[0] == L'"') { start.erase(0, 1); if (!start.empty() && start.back() == L'"') start.pop_back(); }
    Navigate(start, NavPush);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
            WPARAM k = msg.wParam;
            if (k == VK_F5 || (ctrl && k == 'R')) { Reload(); continue; }
            if (k == VK_F12 || (ctrl && GetKeyState(VK_SHIFT) < 0 && k == 'J')) { ToggleConsole(); continue; }
            if (ctrl && GetKeyState(VK_SHIFT) < 0 && k == 'S') { SaveScreenshot(); continue; }
            if ((ctrl && k == 'L') || k == VK_F6 || (alt && k == 'D')) { SetFocus(g_addr); SendMessageW(g_addr, EM_SETSEL, 0, -1); continue; }
            if ((alt && k == VK_LEFT) || k == VK_BROWSER_BACK) { GoBack(); continue; }
            if ((alt && k == VK_RIGHT) || k == VK_BROWSER_FORWARD) { GoForward(); continue; }
            if (alt && k == VK_HOME) { Navigate(L"about:home", NavPush); continue; }
            if (ctrl && (k == VK_OEM_PLUS || k == VK_ADD)) { g_view.SetZoom(g_view.Zoom() * 1.1f); continue; }
            if (ctrl && (k == VK_OEM_MINUS || k == VK_SUBTRACT)) { g_view.SetZoom(g_view.Zoom() / 1.1f); continue; }
            if (ctrl && (k == '0' || k == VK_NUMPAD0)) { g_view.SetZoom(1.f); continue; }
            if (k == VK_BACK && msg.hwnd == g_view.Hwnd() && g_view.FocusNode() < 0) { GoBack(); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    NetShutdown();
    CoUninitialize();
    return int(msg.wParam);
}
