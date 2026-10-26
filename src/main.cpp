// merOS Shield Browser - kendi motoru (MerEngine + merJS) ile güvenlik ve hız odaklı tarayıcı
// Pencere, adres cubugu, gecmis, gezinme, sekme is parcaciklari, formlar ve guvenlik politikalari burada.
// Yalitim: her sekmenin DOM'u, merJS'i ve yerlesimi kendi is parcaciginda calisir; arayuz hicbir zaman
// bir sekmeyi beklemez (donan sekme yalnizca kendisini dondurur).
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
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
#include "svg.h"
#include "youtube.h"
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <psapi.h>
#include "view.h"
#include "i18n.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace mer;


namespace {

const wchar_t* kAppName = L"merOS Shield";
const UINT WM_APP_UI = WM_APP + 1;      // sekme is parcaciklarindan arayuz gorevleri (kuyruk)
const UINT WM_APP_VIDEO = WM_APP + 5;   // oynatici is parcacigindan yeni video karesi

// Video kareleri: oynatici is parcaciklari en son kareyi buraya birakir, arayuz tek mesajla toplar
// (kareler birikmez; arayuz yavassa ara kareler atlanir).
struct VideoMail { int w = 0, h = 0; std::shared_ptr<std::vector<uint8_t>> px; };
std::mutex g_vmMu;
std::map<std::pair<int, std::wstring>, VideoMail> g_vmLatest;
std::atomic<bool> g_vmPosted{false};
enum { ID_BACK = 101, ID_FWD, ID_RELOAD, ID_HOME, ID_ADDR, ID_SEC, ID_STATUS, ID_JS, ID_CONSOLE,
       ID_FIND_EDIT, ID_FIND_INFO, ID_FIND_PREV, ID_FIND_NEXT, ID_FIND_CLOSE,
       ID_HANG_TEXT, ID_HANG_WAIT, ID_HANG_STOP };
// sag tik menusu komutlari
enum { CM_OPEN_TAB = 1, CM_OPEN_BG, CM_COPY_LINK, CM_COPY, CM_SEARCH, CM_BACK, CM_FWD, CM_RELOAD, CM_SELECT_ALL,
       CM_FIND, CM_SOURCE, CM_SHOT, CM_CONSOLE, CM_NEW_TAB };
enum { T_REFRESH = 3, T_SNAPSHOT = 4, T_REPAINT = 5, T_WATCHDOG = 6 };

const size_t kMaxPage = 24u * 1024 * 1024, kMaxCss = 4u * 1024 * 1024, kMaxScript = 48u * 1024 * 1024, kMaxImage = 12u * 1024 * 1024;
const ULONGLONG kHangMs = 5000;   // bir sekme gorevi bu kadar surerse "yanit vermiyor" sayilir

HINSTANCE g_inst;
HWND g_frame, g_back, g_fwd, g_reload, g_home, g_addr, g_sec, g_status, g_js, g_console, g_consoleBtn;
bool g_hasFluentFont = false;
RECT g_urlPillRect{};
bool g_addrFocused = false; HBRUSH g_brUrlBg = CreateSolidBrush(RGB(28, 27, 34));
HBRUSH g_brNavBg = CreateSolidBrush(RGB(43, 42, 51));
HBRUSH g_brStatusBg = CreateSolidBrush(RGB(28, 27, 34));
HBRUSH g_brConBg = CreateSolidBrush(RGB(20, 19, 26));
HWND g_tabsWnd, g_findEdit, g_findInfo, g_findPrev, g_findNext, g_findClose;
HWND g_hangText, g_hangWait, g_hangStop;
bool g_findVisible = false;
bool g_hangVisible = false;
// Arayuz metni: Turkce / Ingilizce
inline const wchar_t* T(const wchar_t* tr, const wchar_t* en) { return i18n::GetCurrentLanguage() == i18n::Language::Turkish ? tr : en; }
HFONT g_uiFont, g_symFont, g_monoFont;
PageView g_view;
std::shared_ptr<DWriteMeasurer> g_measurer;   // tum sekmelerin yerlesimi icin ortak metin olcer (kilitli)

std::atomic<int> g_navId{0};          // benzersiz gezinme kimligi sayaci (iptal sekme bazli)
std::set<std::wstring> g_httpAllowed;   // kullanicinin bu oturumda HTTP'ye izin verdigi sunucular
bool g_jsEnabled = true;
std::atomic<bool> g_ytLiteMode{false};   // false: YouTube kendi arayuzuyle; true: hafif merOS YouTube gorunumu (about:youtube-lite)
std::atomic<bool> g_consoleVisible{false};
std::wstring g_hoverText;
std::wstring g_snapshotPath;   // --snapshot ile: sayfa yuklenince PNG kaydet ve cik
std::wstring g_logPath;        // --log ile: kapanirken gecerli sayfanin konsolunu dosyaya yaz
UINT g_snapshotWait = 4000;    // --wait <ms>

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

// ---------------- Arayuz gorev kuyrugu ----------------
// Sekme is parcaciklari arayuze asla dogrudan dokunmaz (pencereler, sekme listesi, gorunum):
// isteklerini bu kuyruga birakir, arayuz is parcacigi WM_APP_UI ile hepsini sirayla calistirir.
// Arayuz ise hicbir zaman bir sekmeyi beklemez; bu yuzden donan bir sekme pencereyi donduramaz.
std::mutex g_uiMu;
std::vector<std::function<void()>> g_uiQ;
std::atomic<bool> g_uiPosted{false};
std::atomic<bool> g_shuttingDown{false};

void PostUi(std::function<void()> f) {
    if (g_shuttingDown) return;
    {
        std::lock_guard<std::mutex> g(g_uiMu);
        g_uiQ.push_back(std::move(f));
    }
    if (!g_uiPosted.exchange(true)) PostMessageW(g_frame, WM_APP_UI, 0, 0);
}

void DrainUi() {
    g_uiPosted = false;
    std::vector<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> g(g_uiMu);
        q.swap(g_uiQ);
    }
    for (auto& f : q) {
        try { f(); } catch (...) {}
    }
}

// Yerel gelistirme sunuculari (localhost) HTTPS'e yukseltilmez (tarayicilarda oldugu gibi guvenli baglam sayilir)
bool IsLoopbackHost(const std::wstring& h) {
    return h == L"localhost" || h == L"127.0.0.1" || h == L"[::1]" || h == L"::1" || EndsWith(h, L".localhost");
}

// fileDir: sayfa yerel dosyaysa klasorunun adresi ("file:///C:/a/"); yerel sayfalar yalnizca
// kendi klasorlerindeki (ve alt klasorlerdeki) dosyalari yukleyebilir. Web sayfalari hicbir zaman.
bool SubresourceAllowed(const std::wstring& url, bool pageSecure, bool& blocked, const std::wstring& fileDir = L"", bool pageLocal = true) {
    blocked = false;
    Url u = ParseUrl(url);
    if (u.ok && u.scheme == L"file") {
        if (fileDir.empty()) return false;
        if (u.path.find(L"/../") != std::wstring::npos || EndsWith(u.path, L"/..")) return false;
        return StartsWith(ToLower(u.ToString()), ToLower(fileDir));
    }
    if (!u.ok || (u.scheme != L"https" && u.scheme != L"http")) return false;
    if (pageSecure && u.scheme == L"http" && !IsLoopbackHost(u.host)) return false;      // karisik icerik engeli
    if (!pageLocal && IsPrivateOrLocalHost(u.host)) { blocked = true; return false; }    // SSRF / ozel ag erisimi engeli
    if (IsBlockedHost(u.host)) { blocked = true; return false; } // izleyici / reklam
    return true;
}

// ---------------- Sayfa durumu ve JS ana bilgisayari ----------------

struct PageState;
using PagePtr = std::shared_ptr<PageState>;
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
    bool CheckRequest(const std::wstring& url) override;
    bool OverMemory() override;
    bool YouTubeStreamAllowed() override;
    std::function<void(int, int, std::shared_ptr<std::vector<uint8_t>>)> VideoSink(const Node* n) override;
};

// Bir sekmenin sayfasi. Her sayfanin kendi is parcacigi vardir (yukleyici is parcacigi yukleme
// bittikten sonra sayfanin olay dongusune donusur): DOM, stiller, merJS ve yerlesim yalnizca orada
// calisir. Arayuz yalnizca (1) yayimlanan degismez yerlesimi cizer, (2) atomik/kilitli ozet alanlari
// okur, (3) Post() ile gorev gonderir.
struct PageState {
    // --- sekme is parcacigi (yuklemeden sonra degismeyenler arayuzden de okunabilir: *) ---
    int id = 0;                                      // *
    std::wstring url, title;                         // arayuz Url()/Title() kopyalarini okur
    std::wstring base;                               // *
    bool secure = false, internal = false, js = false, localOrigin = false;   // *
    NavMode mode = NavPush;                          // *
    std::unique_ptr<Document> doc;
    std::vector<Style> styles;
    std::map<std::wstring, std::wstring> cssCache;   // harici CSS adres -> metin
    std::set<std::wstring> cssPending;
    std::unique_ptr<js::Runtime> rt;
    PageHost host;
    std::shared_ptr<ImageStore> images = std::make_shared<ImageStore>();   // arayuzle paylasilir (kilitli)
    std::vector<std::pair<Node*, std::wstring>> pendingScripts; // arka plandayken istenen dinamik betikler
    std::vector<std::wstring> imageQueue;
    std::wstring refreshUrl;                         // *
    double refreshDelay = -1;                        // *
    Node* focused = nullptr;
    std::vector<Node*> byId;
    std::wstring fileDir;                            // * yerel sayfa ise klasor adresi
    float styleW = 0;                                // stillerin hesaplandigi gorunur alan genisligi
    std::wstring source;                             // * sunucudan gelen ozgun HTML (dokum icin)
    bool ytLite = false;                             // * merOS YouTube gorunumu
    bool needRestyle = false;                        // arka plandayken CSS/resim geldi
    bool activated = false;                          // ilk kez on plana alindi (load olaylari calisti)
    bool styleQueued = false;                        // stil yeniden hesaplamasi bekliyor (kisitlamali)
    double styleDue = 0;
    bool layoutQueued = false;                       // yeniden yerlesim bekliyor
    std::shared_ptr<const PageLayout> layout;        // sekmenin son yerlesimi
    int tabUid = 0;                                  // *
    std::shared_ptr<std::atomic<int>> token;         // * iptal belirteci (sekmenin gezinme kimligi)
    bool Alive() const { return token && token->load() == id; }

    // --- is parcacigi arasi ---
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::function<void()>> q;
    std::atomic<bool> quit{false};                   // is parcacigi kapanmali (sekme kapandi / yeni sayfa geldi)
    std::atomic<bool> active{false};                 // on planda (arayuz yazar)
    std::atomic<bool> ready{false};                  // yukleme bitti, sekmeye verildi
    std::atomic<bool> killed{false};                 // kullanici sayfanin betiklerini durdurdu
    std::atomic<bool> interrupt{false};              // calisan betigi kes (merJS her 4096 adimda okur)
    std::atomic<long long> busySince{0};             // calisan gorevin baslangici (GetTickCount64), 0 = bos
    std::atomic<long long> inputSince{0};            // islenmeyi bekleyen ilk kullanici girdisi (tik/tus)
    std::atomic<float> viewW{1280.f}, viewH{800.f}, scrollY{0.f};
    std::atomic<int> focusKind{0};                   // odakli alan: 0 yok, 1 input, 2 textarea
    std::atomic<int> jsErrors{0}, blocked{0}, imagesInFlight{0};
    std::atomic<bool> layoutPosted{false}, statusPosted{false};

    // --- arayuzle paylasilan (pubMu) ---
    mutable std::mutex pubMu;
    std::wstring pubUrl, pubTitle;
    std::vector<std::wstring> log;
    std::shared_ptr<const PageLayout> pubLayout;

    std::wstring Url() const { std::lock_guard<std::mutex> g(pubMu); return pubUrl; }
    std::wstring Title() const { std::lock_guard<std::mutex> g(pubMu); return pubTitle; }
    std::shared_ptr<const PageLayout> PubLayout() const { std::lock_guard<std::mutex> g(pubMu); return pubLayout; }
    std::vector<std::wstring> Log() const { std::lock_guard<std::mutex> g(pubMu); return log; }
    void SetUrl(const std::wstring& u) { url = u; std::lock_guard<std::mutex> g(pubMu); pubUrl = u; }
    void SetTitle(const std::wstring& t) { title = t; std::lock_guard<std::mutex> g(pubMu); pubTitle = t; }

    // Sekme is parcacigina gorev gonder (her is parcacigindan cagrilabilir)
    void Post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> g(qmu);
            if (quit) return;
            q.push_back(std::move(f));
        }
        qcv.notify_one();
    }
    // Is parcacigini durdur: calisan betik kesilir, dongu biter, DOM/JS orada serbest birakilir
    void Stop() {
        interrupt = true;
        {
            std::lock_guard<std::mutex> g(qmu);
            quit = true;
            q.clear();
        }
        qcv.notify_all();
    }
    // Donma: tek bir gorev kHangMs'den uzun suruyor ya da kullanicinin tiklamasi/tusu o kadar suredir bekliyor.
    // Donusu: o donma "olayinin" baslangici (Bekle dugmesi ayni olayi bir daha gostermemek icin saklar).
    long long HangKey() const {
        ULONGLONG now = GetTickCount64();
        long long b = busySince.load(), in = inputSince.load();
        if (b && now - ULONGLONG(b) > kHangMs) return b;
        if (in && now - ULONGLONG(in) > kHangMs) return in;
        return 0;
    }
    bool Hung() const { return HangKey() != 0; }
    void MarkInput() {
        long long z = 0, t = (long long)GetTickCount64();
        inputSince.compare_exchange_strong(z, t ? t : 1);
    }
};

// Calisan sayfa is parcaciklari (kimlik -> sayfa): ag/resim is parcaciklari sonuclarini buradan
// sayfanin kuyruguna birakir.
std::mutex g_pagesMu;
std::map<int, std::weak_ptr<PageState>> g_pages;
std::atomic<int> g_pageThreads{0};

PagePtr FindPage(int id) {
    std::lock_guard<std::mutex> g(g_pagesMu);
    auto it = g_pages.find(id);
    return it == g_pages.end() ? nullptr : it->second.lock();
}

void PostPageId(int id, std::function<void(PageState&)> f) {
    PagePtr p = FindPage(id);
    if (!p) return;
    PageState* raw = p.get();
    p->Post([raw, f]() { f(*raw); });
}

// Henuz sekmeye verilmemis (yuklenen) sayfanin betiklerini kes: kullanici baska yere gitti
void InterruptLoading(int jobId) {
    PagePtr p = FindPage(jobId);
    if (p && !p->ready) p->Stop();
}

// ---------------- Sekmeler (yalnizca arayuz is parcacigi) ----------------
struct Tab {
    int uid = 0;
    PagePtr page;
    std::vector<std::wstring> history;
    int histIdx = -1;
    std::shared_ptr<std::atomic<int>> nav = std::make_shared<std::atomic<int>>(0); // gecerli yukleme kimligi
    bool loading = false;
    std::wstring loadingUrl;
    float scroll = 0;
    bool activated = false;     // sayfa en az bir kez on plana alindi (yukleme olaylari calisti)
    bool hungShown = false;     // sekme seridinde "yanit vermiyor" gosterildi
};
std::vector<std::unique_ptr<Tab>> g_tabs;
size_t g_cur = 0;
int g_tabUid = 0;

Tab& CurTab() {
    static Tab empty;   // kapanis sirasinda (sekme yokken) guvenli bos sekme
    return g_tabs.empty() ? empty : *g_tabs[std::min(g_cur, g_tabs.size() - 1)];
}
PagePtr& CurPage() { return CurTab().page; }
void InvalidateTabs() { if (g_tabsWnd) InvalidateRect(g_tabsWnd, nullptr, FALSE); }
Tab* TabByUid(int uid) {
    for (auto& t : g_tabs) if (t->uid == uid) return t.get();
    return nullptr;
}
PageState* PageById(int id) {
    for (auto& t : g_tabs) if (t->page && t->page->id == id) return t->page.get();
    return nullptr;
}
Tab* TabOfPageId(int id) {
    for (auto& t : g_tabs) if (t->page && t->page->id == id) return t.get();
    return nullptr;
}
bool IsCurrentPage(int id) { return CurPage() && CurPage()->id == id; }

std::wstring PageHost::PageUrl() { return page->url; }
std::wstring PageHost::Resolve(const std::wstring& rel) { return ResolveUrl(page->base, rel); }
void PageHost::ViewportSize(double& w, double& h) { w = page->viewW.load(); h = page->viewH.load(); }
void PageHost::Navigate(const std::wstring& url) { if (page->rt) page->rt->pendingNav = url; }
void PageHost::HistoryGo(int d) {
    if (!page->active || d == 0) return;
    int pid = page->id;
    PostUi([pid, d]() {
        if (!IsCurrentPage(pid)) return;
        if (d < 0) GoBack();
        else GoForward();
    });
}

void PostVideoMail(int pid, const std::wstring& key, int w, int h, std::shared_ptr<std::vector<uint8_t>> px) {
    {
        std::lock_guard<std::mutex> g(g_vmMu);
        VideoMail& m = g_vmLatest[std::make_pair(pid, key)];
        m.w = w;
        m.h = h;
        m.px = std::move(px);
    }
    if (!g_vmPosted.exchange(true)) PostMessageW(g_frame, WM_APP_VIDEO, 0, 0);
}

void PageHost::UpdateVideoFrame(const std::wstring& key, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgra) {
    if (!page) return;
    PostVideoMail(page->id, key, int(w), int(h), std::make_shared<std::vector<uint8_t>>(bgra));
}

std::function<void(int, int, std::shared_ptr<std::vector<uint8_t>>)> PageHost::VideoSink(const Node* n) {
    if (!page) return {};
    int pid = page->id;
    std::wstring key = VideoKey(n);
    return [pid, key](int w, int h, std::shared_ptr<std::vector<uint8_t>> px) { PostVideoMail(pid, key, w, h, std::move(px)); };
}

// Surec ozel bellegi fiziksel bellegin yarisini (en fazla 4 GB) asarsa calisan betik durdurulur.
// Boylece tek bir sekmenin bellek patlamasi tum bilgisayari ya da diger sekmeleri bogamaz.
bool PageHost::OverMemory() {
    static ULONGLONG limit = []() {
        MEMORYSTATUSEX ms{sizeof(ms)};
        ULONGLONG phys = GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : (4ull << 30);
        return std::min<ULONGLONG>(phys / 2, 4ull << 30);
    }();
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) return false;
    // PrivateUsage: ayrilmis bellek (Wine bunu 0 bildirir; calisma kumesi de dikkate alinir)
    return std::max<ULONGLONG>(pmc.PrivateUsage, pmc.WorkingSetSize) > limit;
}

// merOS YouTube sayfalari; testlerde (MEROS_YT_API ortam degiskeni) yerel dosya sayfalari da
bool PageHost::YouTubeStreamAllowed() {
    if (!page) return false;
    if (page->ytLite) return true;
    wchar_t v[8];
    return !page->fileDir.empty() && GetEnvironmentVariableW(L"MEROS_YT_API", v, 8) > 0;
}

bool PageHost::CheckRequest(const std::wstring& url) {
    if (!page) return false;
    bool blocked = false;
    bool ok = SubresourceAllowed(url, page->secure, blocked, page->fileDir, page->localOrigin);
    if (blocked) page->blocked++;
    return ok;
}

struct Job {
    int id = 0;
    int tabUid = 0;
    std::shared_ptr<std::atomic<int>> token;
    std::wstring url;
    std::wstring inlineHtml;     // dahili sayfalar icin hazir HTML
    std::wstring sourceTitle;    // kaynak goruntuleme sekmesi basligi
    bool upgradedFromHttp = false;
    std::wstring originalHttp;
    NavMode mode = NavPush;
    FetchOptions opt;
    bool js = true;
    bool ytLite = false;
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
    if (StartsWith(p, L"about:youtube")) {
        if (p.find(L"ac") != std::wstring::npos || p.find(L"on") != std::wstring::npos) g_ytLiteMode = true;
        else if (p.find(L"kapat") != std::wstring::npos || p.find(L"off") != std::wstring::npos) g_ytLiteMode = false;
        bool lite = g_ytLiteMode;
        std::wstring body = lite ? T(L"Şu an: <b>hafif merOS görünümü</b>. YouTube sayfaları kendi verisinden merOS tarafından çizilir; videolar sayfa içinde merOS'un kendi oynatıcısıyla oynar (H.264/AAC, 1080p'ye kadar).",
                                     L"Now: <b>lightweight merOS view</b>. YouTube pages are drawn by merOS from their data; videos play in the page with merOS's own player (H.264/AAC, up to 1080p).")
                                 : T(L"Şu an: <b>YouTube'un kendi arayüzü</b>. Site kendi JavaScript'iyle çalıştırılır (deneysel).",
                                     L"Now: <b>YouTube's own interface</b>. The site runs with its own JavaScript (experimental).");
        return std::wstring(L"<html><head><title>YouTube</title>") + kPageCss + L"</head><body><div class=w><h1>YouTube</h1><p class=alt>" + body +
               L"</p><div class=k><p><a href='about:youtube?ac'>" + T(L"Hafif görünümü aç", L"Use lightweight view") +
               L"</a> · <a href='about:youtube?kapat'>" + T(L"Kendi arayüzünü kullan", L"Use YouTube's own interface") +
               L"</a></p><p><a href='https://www.youtube.com/'>youtube.com</a></p></div></div></body></html>";
    }
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
// Henuz indirilmemis harici CSS adreslerini (@import dahil) 'missing'e ekler.
void BuildSheet(PageState& p, StyleSheet& sheet, std::vector<std::wstring>* missing, float vw, float vh) {
    sheet.vw = vw;
    sheet.vh = vh;
    sheet.pageBase = p.base;
    std::set<std::wstring> missingSet;
    sheet.importer = [&](const std::wstring& url) -> const std::wstring* {
        auto it = p.cssCache.find(url);
        if (it != p.cssCache.end()) return &it->second;
        if (missing && missingSet.insert(url).second) missing->push_back(url);
        return nullptr;
    };
    std::vector<Node*> st{p.doc->root.get()};
    int linkCount = 0;
    while (!st.empty()) {
        Node* n = st.back();
        st.pop_back();
        if (n->kind == Node::Elem) {
            if (n->name == L"style") {
                const std::wstring* m = n->Attr(L"media");
                if (!m || MediaMatches(*m, vw, vh)) sheet.Add(TextContent(n), p.base);
                continue;
            }
            if (n->name == L"link") {
                auto* rel = n->Attr(L"rel");
                auto* href = n->Attr(L"href");
                if (rel && href && ToLower(*rel).find(L"stylesheet") != std::wstring::npos && ToLower(*rel).find(L"alternate") == std::wstring::npos && !n->Attr(L"disabled")) {
                    const std::wstring* m = n->Attr(L"media");
                    if ((!m || MediaMatches(*m, vw, vh)) && ++linkCount <= 32) {
                        std::wstring abs = ResolveUrl(p.base, *href);
                        auto it = p.cssCache.find(abs);
                        if (it != p.cssCache.end()) sheet.Add(it->second, abs);
                        else if (missing && missingSet.insert(abs).second) missing->push_back(abs);
                    }
                }
                continue;
            }
            if (n->name == L"script" || n->name == L"svg" || n->name == L"template") continue;
        }
        for (size_t i = n->kids.size(); i-- > 0;) st.push_back(n->kids[i].get());
    }
    sheet.importer = nullptr;
    sheet.BuildIndex();
}

StyleContext MakeCtx(const PageState& p, float vw, float vh) {
    StyleContext c;
    c.vw = vw;
    c.vh = vh;
    c.scripting = p.js;
    return c;
}

// ---------------- Sekme is parcacigi: gunluk, ileri bildirimler ----------------

void DoLayout(PageState& p);
void Restyle(PageState& p, bool fetchMissing);
void RequestRestyle(PageState& p, double ms);
void ScanImages(PageState& p);
void AfterJs(PageState& p);
void PostStatus(PageState& p);
void Navigate(const std::wstring& input, NavMode mode, const FetchOptions* opt, bool fromUser, Tab* tab);
void OpenTab(const std::wstring& url, bool foreground, bool fromUser);
void UpdateStatus();

double NowMs() { return double(GetTickCount64()); }

void PageLog(PageState* p, int level, const std::wstring& msg) {
    if (!p) return;
    if (level >= 2) p->jsErrors++;
    std::wstring line = (level >= 2 ? L"✖ " : level == 1 ? L"⚠ " : L"") + msg;
    {
        std::lock_guard<std::mutex> g(p->pubMu);
        if (p->log.size() >= 4000) return;
        p->log.push_back(line);
    }
    if (p->active && g_consoleVisible) {
        int pid = p->id;
        PostUi([pid, line]() {
            if (!IsCurrentPage(pid) || !g_consoleVisible) return;
            std::wstring l = line + L"\r\n";
            int len = GetWindowTextLengthW(g_console);
            SendMessageW(g_console, EM_SETSEL, len, len);
            SendMessageW(g_console, EM_REPLACESEL, FALSE, LPARAM(l.c_str()));
        });
    }
    if (level >= 2) PostStatus(*p);
}


// Sayfayi indirir, ayristirir, betiklerini calistirir, stilleri ve ilk yerlesimi hesaplar.
// Bu, sayfanin kendi is parcacigidir; false: iptal edildi.
bool LoadPage(const Job& job, PageState* p) {
    std::atomic<int>* tok = job.token.get();
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
        bool youtube = IsYouTubeUrl(job.url) && job.ytLite;
        if (youtube) {
            // YouTube: masaustu sayfa verisini al; cerez izni penceresini "tumunu reddet" ile gec, oturum/izleyici cerezi yok
            opt.cookies = false;
            opt.userAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 merOSShield/0.3";
            opt.extraHeaders += L"Cookie: SOCS=CAI\r\n";
        }
        FetchResult f = Fetch(job.url, kMaxPage, tok, job.id, &opt);
        if (tok->load() != job.id || p->quit) return false;
        if (!f.ok) {
            p->internal = true;
            if (job.upgradedFromHttp && f.error != ERROR_WINHTTP_NAME_NOT_RESOLVED) {
                std::wstring link = L"<p><a href=\"meros-allow-http:" + HtmlEscape(job.originalHttp) +
                                    L"\">" + i18n::Get(i18n::StringId::InsecureWarningAction) + L"</a></p>";
                html = MessagePage(i18n::Get(i18n::StringId::InsecureWarningTitle),
                                   i18n::Get(i18n::StringId::InsecureWarningBody),
                                   job.originalHttp, link);
                p->SetUrl(job.originalHttp);
            } else {
                html = MessagePage(i18n::Get(i18n::StringId::PageLoadFailedTitle), NetErrorText(f.error), job.url);
            }
        } else {
            html = BodyToHtml(f, f.finalUrl);
            p->SetUrl(f.finalUrl);
            p->base = f.finalUrl;
            if (youtube && IsYouTubeUrl(f.finalUrl)) {
                std::wstring yt;
                if (BuildYouTubePage(html, f.finalUrl, i18n::GetCurrentLanguage() == i18n::Language::Turkish, yt)) {
                    html.swap(yt);
                    p->ytLite = true;
                }
            }
            if (f.truncated) html += i18n::Get(i18n::StringId::PageTruncated);
        }
    }

    p->doc = ParseHTML(html);
    if (!p->internal) p->source = html;
    Document& doc = *p->doc;
    if (!doc.baseHref.empty() && !p->internal) {
        Url bu = ParseUrl(ResolveUrl(p->base, doc.baseHref));
        if (bu.ok && (bu.scheme == L"https" || bu.scheme == L"http")) p->base = bu.ToString();
    }
    Url pageUrl = ParseUrl(p->url);
    p->secure = pageUrl.ok && pageUrl.scheme == L"https";
    p->localOrigin = p->internal || (pageUrl.ok && (pageUrl.scheme == L"file" || pageUrl.scheme == L"about" || IsPrivateOrLocalHost(pageUrl.host)));
    if (pageUrl.ok && pageUrl.scheme == L"file" && !p->internal) {
        std::wstring fu = pageUrl.ToString();
        p->fileDir = fu.substr(0, fu.rfind(L'/') + 1);
    }
    p->images->base = p->base;
    // dahili sayfalarda yalnizca kendi betiklerimiz calisir (about:motor)
    p->js = job.js;

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
        BuildSheet(*p, tmp, &cssMissing, float(job.viewW), float(job.viewH));
    }
    if (!p->internal) {
        // CSS ve betikler paralel indirilir (hiz)
        std::vector<std::thread> th;
        const std::wstring docRef = p->url;
        std::vector<std::wstring> cssText(cssMissing.size());
        for (size_t i = 0; i < cssMissing.size(); i++) {
            bool blocked;
            if (!SubresourceAllowed(cssMissing[i], p->secure, blocked, p->fileDir, p->localOrigin)) { if (blocked) p->blocked++; continue; }
            int id = job.id;
            th.emplace_back([&cssText, &cssMissing, &docRef, i, id, tok]() {
                FetchOptions ro = SubresourceOpts(docRef, cssMissing[i]);
                FetchResult cf = Fetch(cssMissing[i], kMaxCss, tok, id, &ro);
                if (cf.ok && cf.status < 400) cssText[i] = DecodeBody(cf.body, cf.contentType);
            });
        }
        if (p->js) {
            size_t ext = 0;
            for (size_t i = 0; i < scripts.size(); i++) {
                if (scriptUrls[i].empty()) continue;
                bool blocked;
                if (!SubresourceAllowed(scriptUrls[i], p->secure, blocked, p->fileDir, p->localOrigin) || ++ext > 48) { if (blocked) p->blocked++; continue; }
                int id = job.id;
                th.emplace_back([&scriptText, &scriptOk, &scriptUrls, &docRef, i, id, tok]() {
                    FetchOptions ro = SubresourceOpts(docRef, scriptUrls[i]);
                    FetchResult sf = Fetch(scriptUrls[i], kMaxScript, tok, id, &ro);
                    // Yarim kalan betik calistirilmaz (eskiden 6 MB'ta kesilip "'}' bekleniyordu" hatasi veriyordu)
                    if (sf.ok && sf.status < 400 && !sf.truncated) { scriptText[i] = DecodeBody(sf.body, sf.contentType); scriptOk[i] = 1; }
                    else if (sf.truncated) scriptOk[i] = 2;
                });
            }
        }
        for (auto& t : th) t.join();
        if (tok->load() != job.id || p->quit) return false;
        for (size_t i = 0; i < cssMissing.size(); i++) p->cssCache[cssMissing[i]] = cssText[i];
        // @import ile gelen CSS dosyalari (en fazla 2 tur)
        for (int round = 0; round < 2; round++) {
            std::vector<std::wstring> more;
            StyleSheet tmp;
            BuildSheet(*p, tmp, &more, float(job.viewW), float(job.viewH));
            if (more.empty()) break;
            std::vector<std::wstring> texts(more.size());
            std::vector<std::thread> th2;
            for (size_t i = 0; i < more.size() && i < 16; i++) {
                bool blocked;
                if (!SubresourceAllowed(more[i], p->secure, blocked, p->fileDir, p->localOrigin)) { if (blocked) p->blocked++; continue; }
                int id = job.id;
                th2.emplace_back([&texts, &more, &docRef, i, id, tok]() {
                    FetchOptions ro = SubresourceOpts(docRef, more[i]);
                    FetchResult cf = Fetch(more[i], kMaxCss, tok, id, &ro);
                    if (cf.ok && cf.status < 400) texts[i] = DecodeBody(cf.body, cf.contentType);
                });
            }
            for (auto& t : th2) t.join();
            if (tok->load() != job.id || p->quit) return false;
            for (size_t i = 0; i < more.size(); i++) p->cssCache[more[i]] = texts[i];
        }
    }

    // JavaScript: sayfa betiklerini belge sirasiyla calistir (bu is parcaciginda; arayuz donmaz)
    if (p->js) {
        p->rt.reset(new js::Runtime());
        p->rt->stackLimit = 40u * 1024 * 1024;
        p->rt->extInterrupt = &p->interrupt;   // "Sayfayi durdur" / sekme kapandi
        // Sekmeler yalitildigi icin uzun gorevler arayuzu dondurmez; adim butcesi yalnizca son guvenlik agi
        // (Chrome'daki gibi asil karar kullanicinin: "Sayfa yanit vermiyor -> Sayfayi durdur").
        p->rt->stepBudget = 1500000000ull;
        p->rt->AttachDocument(p->doc.get(), &p->host);
        for (size_t i = 0; i < scripts.size(); i++) {
            if (p->killed) break;
            Node* s = scripts[i];
            struct BusyMark {
                PageState* p;
                explicit BusyMark(PageState* q) : p(q) { long long t = (long long)GetTickCount64(); p->busySince = t ? t : 1; }
                ~BusyMark() { p->busySince = 0; }
            } busy(p);
            s->attrs.emplace_back(L"data-merjs-ran", L"1");
            std::wstring type = s->Attr(L"type") ? ToLower(Trim(*s->Attr(L"type"))) : L"";
            bool isMod = (type == L"module");
            std::wstring name = scriptUrls[i].empty() ? (isMod ? p->base : i18n::Get(i18n::StringId::InlineScript)) : scriptUrls[i];
            if (isMod) {
                if (!scriptUrls[i].empty()) {
                    if (scriptOk[i] != 1) continue;
                    p->rt->RunModule(scriptText[i], scriptUrls[i], s);
                } else {
                    p->rt->RunModule(TextContent(s), p->base, s);
                }
            } else {
                if (!scriptUrls[i].empty()) {
                    if (scriptOk[i] == 2) PageLog(p, 2, L"Betik boyut s\u0131n\u0131r\u0131n\u0131 a\u015ft\u0131, \u00e7al\u0131\u015ft\u0131r\u0131lmad\u0131 (" + std::to_wstring(kMaxScript / (1024 * 1024)) + L" MB): " + scriptUrls[i]);
                    if (scriptOk[i] != 1) continue;
                    p->rt->RunScript(scriptText[i], name, s);
                } else {
                    p->rt->RunScript(TextContent(s), name, s);
                }
            }
            if (tok->load() != job.id || p->quit) return false;
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
                        Url ru = ParseUrl(ResolveUrl(p->base, t));
                        if (ru.ok && (ru.scheme == L"https" || (ru.scheme == L"http" && !p->secure)) && (p->localOrigin || !IsPrivateOrLocalHost(ru.host))) {
                            p->refreshUrl = ru.ToString();
                            p->refreshDelay = d;
                        }
                    }
                }
            }
            for (auto& k : n->kids) st.push_back(k.get());
        }
    }

    StyleSheet sheet;
    if (p->js && p->rt) {
        auto drainDynamicScripts = [&]() {
            int dynCount = 0;
            while (!p->pendingScripts.empty() && !p->killed && p->rt->pendingNav.empty() && dynCount < 32) {
                if (tok->load() != job.id || p->quit) return;
                auto ps = p->pendingScripts.front();
                p->pendingScripts.erase(p->pendingScripts.begin());
                dynCount++;
                bool blocked = false;
                if (!SubresourceAllowed(ps.second, p->secure, blocked, p->fileDir, p->localOrigin)) {
                    if (blocked) p->blocked++;
                    p->rt->ScriptLoaded(ps.first, false);
                    continue;
                }
                FetchOptions ro = SubresourceOpts(p->url, ps.second);
                FetchResult sf = Fetch(ps.second, kMaxScript, tok, job.id, &ro);
                if (tok->load() != job.id || p->quit) return;
                bool good = sf.ok && sf.status < 400 && !sf.truncated;
                if (good) {
                    std::wstring code = DecodeBody(sf.body, sf.contentType);
                    std::wstring stype = (ps.first && ps.first->Attr(L"type")) ? ToLower(Trim(*ps.first->Attr(L"type"))) : L"";
                    if (stype == L"module") p->rt->RunModule(code, ps.second, ps.first);
                    else p->rt->RunScript(code, ps.second, ps.first);
                }
                p->rt->ScriptLoaded(ps.first, good);
            }
        };
        drainDynamicScripts();
        if (tok->load() != job.id || p->quit) return false;
        if (!p->killed && p->rt->pendingNav.empty()) {
            p->activated = true;
            p->rt->FireLoadEvents();
            drainDynamicScripts();
        }
        if (tok->load() != job.id || p->quit) return false;
    }
    if (!p->internal) {
        std::vector<std::wstring> dynCss;
        StyleSheet tmp;
        BuildSheet(*p, tmp, &dynCss, float(job.viewW), float(job.viewH));
        for (size_t i = 0; i < dynCss.size() && i < 16; i++) {
            bool blocked = false;
            if (!SubresourceAllowed(dynCss[i], p->secure, blocked, p->fileDir, p->localOrigin)) { if (blocked) p->blocked++; continue; }
            FetchOptions ro = SubresourceOpts(p->url, dynCss[i]);
            FetchResult cf = Fetch(dynCss[i], kMaxCss, tok, job.id, &ro);
            if (tok->load() != job.id || p->quit) return false;
            p->cssCache[dynCss[i]] = (cf.ok && cf.status < 400) ? DecodeBody(cf.body, cf.contentType) : L"";
        }
    }
    BuildSheet(*p, sheet, nullptr, float(job.viewW), float(job.viewH));
    p->styles = ComputeStyles(doc, sheet, MakeCtx(*p, float(job.viewW), float(job.viewH)));
    p->styleW = float(job.viewW);
    p->SetTitle(doc.title);

    if (tok->load() != job.id || p->quit) return false;
    DoLayout(*p);   // ilk yerlesim: sekme ilk cizimi hemen yapabilsin
    return true;
}


// ---------------- Sekme is parcacigi: arayuze bildirimler ----------------

// Durum cubugu (engellenen, JS hatalari, resimler): birlestirilmis tek istek
void PostStatus(PageState& p) {
    if (!p.ready || !p.active || p.statusPosted.exchange(true)) return;
    int pid = p.id;
    PostUi([pid]() {
        PageState* pp = PageById(pid);
        if (pp) pp->statusPosted = false;
        if (IsCurrentPage(pid)) UpdateStatus();
    });
}

void PostTitle(PageState& p) {
    if (!p.ready) return;
    int pid = p.id;
    PostUi([pid]() {
        Tab* t = TabOfPageId(pid);
        if (!t) return;
        if (t == &CurTab()) {
            std::wstring title = t->page->Title();
            SetWindowTextW(g_frame, ((title.empty() ? t->page->Url() : title) + L" — " + kAppName).c_str());
        }
        InvalidateTabs();
    });
}

// Sayfadan gezinme istegi (baglanti, form, location.href): arayuz sekmenin gezinmesini baslatir
void PageNavigate(PageState& p, const std::wstring& url, NavMode mode, const FetchOptions* opt) {
    int pid = p.id;
    std::shared_ptr<FetchOptions> o(opt ? new FetchOptions(*opt) : nullptr);
    PostUi([pid, url, mode, o]() {
        Tab* t = TabOfPageId(pid);
        if (t) Navigate(url, mode, o.get(), false, t);
    });
}

void PostFocus(PageState& p, int nodeId) {
    int pid = p.id;
    PostUi([pid, nodeId]() { if (IsCurrentPage(pid)) g_view.SetFocusNode(nodeId); });
}

// ---------------- Sekme is parcacigi: stil, yerlesim ----------------

// Yerlesimi hesaplar ve arayuze yayimlar (degismez kopya; arayuz eskisini cizmeye devam edebilir)
void DoLayout(PageState& p) {
    p.layoutQueued = false;
    std::shared_ptr<DWriteMeasurer> tm = g_measurer;
    if (!p.doc || !tm) return;
    float vw = p.viewW, vh = p.viewH;
    auto L = std::make_shared<PageLayout>();
    LayoutPage(*p.doc, p.styles, std::max(200.f, vw), *tm, *L, p.images.get(), vh);
    L->viewW = vw;
    p.layout = L;
    {
        std::lock_guard<std::mutex> g(p.pubMu);
        p.pubLayout = L;
    }
    if (p.ready && p.active && !p.layoutPosted.exchange(true)) {
        int pid = p.id;
        PostUi([pid]() {
            PageState* pp = PageById(pid);
            if (!pp) return;
            pp->layoutPosted = false;
            if (IsCurrentPage(pid)) g_view.SetLayout(pp->PubLayout(), true);
        });
    }
}

// Stil yeniden hesaplamasi en fazla ~20/sn (DOM degisiklikleri, gelen CSS/resimler)
void RequestRestyle(PageState& p, double ms) {
    if (p.styleQueued) return;
    p.styleQueued = true;
    p.styleDue = NowMs() + ms;
}

void Restyle(PageState& p, bool fetchMissing) {
    p.styleQueued = false;
    if (!p.doc) return;
    StyleSheet sheet;
    std::vector<std::wstring> missing;
    float vw = p.viewW, vh = p.viewH;
    BuildSheet(p, sheet, &missing, vw, vh);
    p.styles = ComputeStyles(*p.doc, sheet, MakeCtx(p, vw, vh));
    p.styleW = vw;
    if (p.rt) p.rt->dirty = false;
    p.byId.clear();
    p.layoutQueued = true;
    if (fetchMissing) {
        for (auto& u : missing) {
            if (p.cssPending.count(u)) continue;
            bool blocked;
            if (!SubresourceAllowed(u, p.secure, blocked, p.fileDir, p.localOrigin)) { if (blocked) p.blocked++; p.cssCache[u] = L""; continue; }
            p.cssPending.insert(u);
            int id = p.id;
            auto tok = p.token;
            std::wstring docRef = p.url;
            RunThread([u, id, tok, docRef]() {
                FetchOptions ro = SubresourceOpts(docRef, u);
                FetchResult cf = Fetch(u, kMaxCss, tok.get(), id, &ro);
                std::wstring text = cf.ok && cf.status < 400 ? DecodeBody(cf.body, cf.contentType) : L"";
                PostPageId(id, [u, text](PageState& p) {
                    p.cssCache[u] = text;
                    p.cssPending.erase(u);
                    if (p.active) RequestRestyle(p, 30);
                    else p.needRestyle = true;
                });
            });
        }
    }
    ScanImages(p);
}

Node* NodeById(PageState& p, int id) {
    if (id < 0 || !p.doc) return nullptr;
    if (size_t(id) >= p.byId.size() || !p.byId[size_t(id)] || p.byId[size_t(id)]->id != id) {
        p.byId.assign(size_t(p.doc->count) + 1, nullptr);
        std::vector<Node*> st{p.doc->root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->id >= 0 && size_t(n->id) < p.byId.size()) p.byId[size_t(n->id)] = n;
            for (auto& k : n->kids) st.push_back(k.get());
            if (n->shadow) st.push_back(n->shadow.get());
        }
    }
    return size_t(id) < p.byId.size() ? p.byId[size_t(id)] : nullptr;
}

// Her JS gorevinden sonra: DOM degistiyse yeniden stil (en fazla ~20/sn), gezinme istekleri, baslik
void AfterJs(PageState& p) {
    if (p.rt) {
        if (!p.rt->pendingNav.empty()) {
            std::wstring nav;
            nav.swap(p.rt->pendingNav);
            PageNavigate(p, nav, NavPush, nullptr);
            return;
        }
        if (p.rt->dirty) RequestRestyle(p, 50);
        if (p.doc && p.doc->title != p.title) {
            p.SetTitle(p.doc->title);
            PostTitle(p);
        }
    }
    PostStatus(p);
}

// ---------------- Resimler ----------------

struct ImageResult { std::wstring url; UINT w = 0, h = 0; float natW = 0, natH = 0; std::vector<uint8_t> px; bool ok = false, blocked = false; };

void OnImageResult(PageState& p, ImageResult& r);

void PumpImages(PageState& p) {
    while (p.imagesInFlight < 6 && !p.imageQueue.empty()) {
        std::wstring u = p.imageQueue.front();
        p.imageQueue.erase(p.imageQueue.begin());
        p.imagesInFlight++;
        int id = p.id;
        bool secure = p.secure;
        bool localOrigin = p.localOrigin;
        std::wstring fileDir = p.fileDir;
        // satir ici SVG: metni is parcacigina ver
        std::wstring svgText;
        uint32_t svgColor = 0xFF000000;
        {
            std::lock_guard<std::recursive_mutex> lk(p.images->Mu());
            auto sit = p.images->svgSource.find(u);
            if (sit != p.images->svgSource.end()) {
                svgText.swap(sit->second);
                p.images->svgSource.erase(sit);
                auto cit = p.images->svgColor.find(u);
                if (cit != p.images->svgColor.end()) svgColor = cit->second;
            }
        }
        auto tok = p.token;
        RunThread([u, id, secure, localOrigin, fileDir, svgText, svgColor, tok]() {
            std::shared_ptr<ImageResult> r(new ImageResult());
            r->url = u;
            std::string bytes;
            if (!svgText.empty()) {
                uint32_t W = 0, H = 0;
                r->ok = RasterizeSvg(svgText, 2.f, svgColor, W, H, r->natW, r->natH, r->px);
                r->w = W; r->h = H;
            } else if (StartsWith(ToLower(u.substr(0, 5)), L"data:")) {
                DecodeDataUrl(u, bytes);
            } else {
                bool blocked;
                if (SubresourceAllowed(u, secure, blocked, fileDir, localOrigin)) {
                    FetchResult f = Fetch(u, kMaxImage, tok.get(), id);
                    if (f.ok && f.status < 400) bytes.swap(f.body);
                } else r->blocked = blocked;
            }
            if (!bytes.empty()) r->ok = DecodeImage(bytes, r->w, r->h, r->px, &r->natW, &r->natH);
            PostPageId(id, [r](PageState& p) { OnImageResult(p, *r); });
        });
    }
}

void OnImageResult(PageState& p, ImageResult& r) {
    p.imagesInFlight = std::max(0, p.imagesInFlight - 1);
    if (r.blocked) p.blocked++;
    p.images->SetResult(r.url, r.ok, r.w, r.h, r.natW, r.natH, r.px);
    PumpImages(p);
    if (r.ok) {
        if (p.active) RequestRestyle(p, p.imagesInFlight ? 120 : 30);
        else p.needRestyle = true;
    }
    PostStatus(p);
}

// FNV-1a (satir ici SVG anahtari icin)
uint64_t Hash64(const std::wstring& t) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : t) { h ^= uint64_t(c); h *= 1099511628211ull; }
    return h;
}

// images->Mu() kilitliyken cagrilir
void QueueImage(PageState& p, const std::wstring& u) {
    ImageStore& im = *p.images;
    if (u.empty() || im.map.count(u) || im.map.size() >= 1200) return;
    std::wstring lu = ToLower(u.substr(0, std::min<size_t>(u.size(), 12)));
    if (!StartsWith(lu, L"data:") && !StartsWith(lu, L"http") && !StartsWith(lu, L"file:")) return;
    im.map[u].state = 0;
    p.imageQueue.push_back(u);
}

void QueueCssImages(PageState& p, const Style& s) {
    if (s.bgImage) QueueImage(p, s.bgImage->url);
    if (s.mask) QueueImage(p, s.mask->url);
    if (s.pseudo) {
        for (auto* ps : {s.pseudo->before.get(), s.pseudo->after.get()}) {
            if (!ps) continue;
            if (ps->bgImage) QueueImage(p, ps->bgImage->url);
            if (ps->mask) QueueImage(p, ps->mask->url);
        }
    }
}

void ScanImages(PageState& p) {
    if (!p.doc) return;
    {
        ImageStore& im = *p.images;
        std::lock_guard<std::recursive_mutex> lk(im.Mu());
        im.svgKeys.clear();
        std::vector<Node*> st{p.doc->root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->kind == Node::Elem) {
                bool shown = n->id >= 0 && size_t(n->id) < p.styles.size() && p.styles[size_t(n->id)].display != Style::None;
                if (n->id >= 0 && size_t(n->id) < p.styles.size() && !shown) continue;
                if (shown) QueueCssImages(p, p.styles[size_t(n->id)]);
                if (n->name == L"img" || (n->name == L"input" && n->Attr(L"type") && ToLower(*n->Attr(L"type")) == L"image")) {
                    if (shown) QueueImage(p, im.UrlOf(n));
                } else if (n->name == L"svg") {
                    if (shown) {
                        std::wstring markup = SerializeHTML(n, true);
                        if (markup.size() < 2u * 1024 * 1024) {
                            uint32_t color = p.styles[size_t(n->id)].color;
                            wchar_t buf[48];
                            swprintf(buf, 48, L"svg:%016llx:%08x", (unsigned long long)Hash64(markup), color);
                            std::wstring key = buf;
                            im.svgKeys[n] = key;
                            if (!im.map.count(key) && im.map.size() < 1200) {
                                im.map[key].state = 0;
                                im.svgSource[key] = markup;
                                im.svgColor[key] = color;
                                p.imageQueue.push_back(key);
                            }
                        }
                    }
                    continue; // SVG icerigi ayrica taranmaz
                }
            }
            for (auto& k : n->kids) st.push_back(k.get());
            if (n->shadow) st.push_back(n->shadow.get());
        }
    }
    PumpImages(p);
}

// ---------------- Dinamik betikler ----------------

struct ScriptResult { Node* node; std::wstring url, text; bool ok; };

void OnScriptResult(PageState& p, const ScriptResult& r) {
    if (!p.rt || p.killed) return;
    if (r.ok) {
        std::wstring stype = (r.node && r.node->Attr(L"type")) ? ToLower(Trim(*r.node->Attr(L"type"))) : L"";
        if (stype == L"module") p.rt->RunModule(r.text, r.url, r.node);
        else p.rt->RunScript(r.text, r.url, r.node);
    }
    else PageLog(&p, 1, std::wstring(i18n::Get(i18n::StringId::ScriptLoadFailed)) + r.url);
    if (!p.rt) return;
    p.rt->ScriptLoaded(r.node, r.ok);
    if (p.active) AfterJs(p);
    else if (p.rt->dirty) p.needRestyle = true;
}

void FetchScript(PageState* p, Node* node, const std::wstring& url) {
    bool blocked;
    if (!SubresourceAllowed(url, p->secure, blocked, p->fileDir, p->localOrigin)) {
        if (blocked) { p->blocked++; PageLog(p, 0, L"[engellendi] " + url); }
        auto res = std::make_shared<ScriptResult>(ScriptResult{node, url, L"", false});
        p->Post([p, res]() { OnScriptResult(*p, *res); });
        return;
    }
    int id = p->id;
    auto tok = p->token;
    std::wstring docRef = p->url;
    RunThread([node, url, id, tok, docRef]() {
        FetchOptions ro = SubresourceOpts(docRef, url);
        FetchResult f = Fetch(url, kMaxScript, tok.get(), id, &ro);
        bool good = f.ok && f.status < 400 && !f.truncated;   // yarim kalan betik calistirilmaz
        auto res = std::make_shared<ScriptResult>(ScriptResult{node, url, good ? DecodeBody(f.body, f.contentType) : L"", good});
        PostPageId(id, [res](PageState& p) { OnScriptResult(p, *res); });
    });
}

void PageRequestScript(PageState* p, Node* node, const std::wstring& url) {
    if (!p) return;
    if (!p->ready || !p->active) { p->pendingScripts.emplace_back(node, url); return; }
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

bool JsOn(const PageState& p) { return p.rt && !p.killed; }

void PageSubmit(PageState* p, Node* form, Node* submitter, bool fromScript) {
    if (!p || !p->ready || !form) return;
    if (!fromScript && JsOn(*p)) {
        if (!p->rt->DispatchSimple(form, L"submit", true)) { AfterJs(*p); return; }
        if (!p->rt->pendingNav.empty()) { AfterJs(*p); return; }
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
    Url cur = ParseUrl(p->url), dst = ParseUrl(action);
    if (!dst.ok) return;
    if (dst.scheme != L"http" && dst.scheme != L"https") {
        if (!(dst.scheme == L"file" && !p->fileDir.empty() && StartsWith(ToLower(dst.ToString()), ToLower(p->fileDir)))) return;
    }
    if (post) {
        FetchOptions opt;
        opt.method = L"POST";
        opt.body = ToUtf8(q);
        opt.contentType = L"application/x-www-form-urlencoded";
        if (IsSameOrigin(cur, dst)) opt.referer = cur.Origin() + L"/"; // bazi siteler CSRF icin ister (yalnizca koken)
        PageNavigate(*p, action, NavPush, &opt);
    } else {
        size_t qm = action.find(L'?');
        if (qm != std::wstring::npos) action = action.substr(0, qm);
        size_t hs = action.find(L'#');
        if (hs != std::wstring::npos) action = action.substr(0, hs);
        PageNavigate(*p, action + L"?" + q, NavPush, nullptr);
    }
}

Node* FormOf(Node* n) {
    for (Node* p = n; p; p = p->parent) if (p->kind == Node::Elem && p->name == L"form") return p;
    return nullptr;
}

// ---------------- Etkilesim (sekme is parcacigi) ----------------

void Unfocus(PageState& p) {
    p.focused = nullptr;
    p.focusKind = 0;
    PostFocus(p, -1);
}

void FocusField(PageState& p, Node* c) {
    p.focused = c;
    p.focusKind = c->name == L"textarea" ? 2 : 1;
    PostFocus(p, c->id);
}

void PageLinkHref(PageState& p, const std::wstring& href, bool ctrl, bool shift) {
    std::wstring h = Trim(href);
    if (StartsWith(ToLower(h), L"meros-allow-http:")) {
        if (!p.internal) return; // yalnizca kendi uyari sayfamiz bu izni verebilir
        Url u = ParseUrl(h.substr(17));
        if (u.ok && u.scheme == L"http") {
            std::wstring host = u.host, target = u.ToString();
            int pid = p.id;
            PostUi([pid, host, target]() {
                g_httpAllowed.insert(host);
                Tab* t = TabOfPageId(pid);
                if (t) Navigate(target, NavPush, nullptr, false, t);
            });
        }
        return;
    }
    if (StartsWith(ToLower(h), L"meros-yt-play:")) {
        // Yalnizca merOS YouTube gorunumunun kendi "oynat" dugmesi; hedef her zaman youtube.com/watch
        std::wstring target;
        if (p.ytLite && YouTubeExternalTarget(h, target))
            PostUi([target]() { ShellExecuteW(g_frame, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        return;
    }
    if (h.empty() || h[0] == L'#') return;
    if (StartsWith(ToLower(h), L"javascript:")) return;
    std::wstring abs = ResolveUrl(p.base, h);
    if (ctrl) { PostUi([abs, shift]() { OpenTab(abs, shift, false); }); return; }   // Ctrl+tik: arka plan sekmesi
    PageNavigate(p, abs, NavPush, nullptr);
}

void PageClick(PageState& p, int nodeId, int link, bool ctrl, bool shift, float docX = -1, float docY = -1) {
    Node* n = NodeById(p, nodeId);
    const PageLayout* L = p.layout.get();
    if (!n) {
        Unfocus(p);
        if (L && link >= 0 && size_t(link) < L->links.size()) PageLinkHref(p, L->links[size_t(link)], ctrl, shift);
        return;
    }
    bool allowDefault = true;
    if (JsOn(p)) {
        allowDefault = docX >= 0 ? p.rt->DispatchClickAt(n, docX, docY - p.scrollY.load(), docX, docY) : p.rt->DispatchClick(n);
        if (!p.rt->pendingNav.empty()) { AfterJs(p); return; }
    }
    if (p.quit) return;
    bool js = JsOn(p);
    if (allowDefault) {
        bool handled = false;
        for (Node* c = n; c && !handled; c = c->parent ? c->parent : (c->kind == Node::Shadow ? c->host : nullptr)) {
            if (c->kind != Node::Elem) continue;
            const std::wstring& t = c->name;
            if (t == L"a" && c->Attr(L"href")) { Unfocus(p); PageLinkHref(p, *c->Attr(L"href"), ctrl, shift); return; }
            if (t == L"video" && c->Attr(L"controls") && js) { js::MediaTogglePlay(*p.rt, c); AfterJs(p); return; }   // yerlesik denetimler: tikla oynat/duraklat
            if (t == L"input") {
                std::wstring type = c->Attr(L"type") ? ToLower(*c->Attr(L"type")) : L"text";
                if (type == L"checkbox") { c->checked = !IsChecked(c); c->dirtyChecked = true; handled = true; if (js) p.rt->DispatchSimple(c, L"change", true); }
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
                    if (js) p.rt->DispatchSimple(c, L"change", true);
                } else if (type == L"submit" || type == L"image") { Unfocus(p); PageSubmit(&p, FormOf(c), c, false); return; }
                else if (type == L"button" || type == L"reset" || type == L"hidden") handled = true;
                else {
                    FocusField(p, c);
                    if (js) p.rt->DispatchSimple(c, L"focus", false);
                    AfterJs(p);
                    return;
                }
            } else if (t == L"textarea") {
                FocusField(p, c);
                AfterJs(p);
                return;
            } else if (t == L"button") {
                std::wstring type = c->Attr(L"type") ? ToLower(*c->Attr(L"type")) : L"submit";
                if (type == L"submit" && FormOf(c)) { Unfocus(p); PageSubmit(&p, FormOf(c), c, false); return; }
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
                    if (js) p.rt->DispatchSimple(c, L"change", true);
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
                if (target && target != n) { PageClick(p, target->id, -1, ctrl, shift); return; }
            }
        }
    }
    if (L && link >= 0 && allowDefault && size_t(link) < L->links.size()) { PageLinkHref(p, L->links[size_t(link)], ctrl, shift); return; }
    Unfocus(p);
    if (p.rt) p.rt->dirty = true;
    AfterJs(p);
    p.layoutQueued = true;
}

void EditFocused(PageState& p, const std::wstring& insert, bool backspace) {
    Node* n = p.focused;
    if (!n) return;
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
    p.layoutQueued = true;
    if (JsOn(p)) { p.rt->DispatchSimple(n, L"input", true); AfterJs(p); }
}

void PageChar(PageState& p, wchar_t ch) {
    if (!p.focused) return;
    if (ch == 8) { EditFocused(p, L"", true); return; }
    if (ch == L'\r') {
        if (p.focused->name == L"textarea") EditFocused(p, L"\n", false);
        return;
    }
    if (ch < 32) return;
    EditFocused(p, std::wstring(1, ch), false);
}

// Odakli alanda ozel tuslar (arayuz hangi tuslarin buraya gelecegine karar verir; pano arayuzde okunur)
void PageKey(PageState& p, UINT vk, const std::wstring& clip) {
    Node* n = p.focused;
    if (!n) return;
    if (vk == VK_ESCAPE || vk == VK_TAB) { Unfocus(p); return; }
    if (vk == VK_RETURN && n->name != L"textarea") {
        if (JsOn(p)) {
            bool ok = p.rt->DispatchKey(n, L"keydown", L"Enter");
            p.rt->DispatchSimple(n, L"change", true);
            if (!ok || !p.rt->pendingNav.empty()) { AfterJs(p); return; }
        }
        if (Node* f = FormOf(n)) { Unfocus(p); PageSubmit(&p, f, nullptr, false); }
        else AfterJs(p);
        return;
    }
    if (vk == 'V') {
        std::wstring s = clip;
        if (n->name != L"textarea") for (auto& c : s) if (c == L'\r' || c == L'\n') c = L' ';
        EditFocused(p, s.substr(0, 20000), false);
    }
}

// getBoundingClientRect: sekmenin kendi son yerlesiminden
bool PageNodeRect(PageState* p, const Node* n, double& x, double& y, double& w, double& h) {
    if (!p || !n) return false;
    if ((!p->layout || (p->rt && p->rt->dirty)) && p->doc && g_measurer) {
        StyleSheet sheet;
        float vw = p->viewW.load(), vh = p->viewH.load();
        BuildSheet(*p, sheet, nullptr, vw, vh);
        p->styles = ComputeStyles(*p->doc, sheet, MakeCtx(*p, vw, vh));
        p->styleW = vw;
        if (p->rt) p->rt->dirty = false;
        DoLayout(*p);
    }
    if (!p->layout) return false;
    std::unordered_set<int> ids;
    std::vector<const Node*> st{n};
    while (!st.empty() && ids.size() < 200000) {
        const Node* c = st.back();
        st.pop_back();
        ids.insert(c->id);
        for (auto& k : c->kids) st.push_back(k.get());
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& d : p->layout->items) {
        if (d.node < 0 || !ids.count(d.node)) continue;
        x0 = std::min(x0, d.x); y0 = std::min(y0, d.y);
        x1 = std::max(x1, d.x + d.w); y1 = std::max(y1, d.y + d.h);
    }
    if (x1 < x0) {
        if (n->name == L"html" || n->name == L"body") {
            x = 0; y = 0; w = p->viewW.load(); h = std::max(p->layout->height, p->viewH.load());
            return true;
        }
        return false;
    }
    x = x0;
    y = y0 - p->scrollY.load();
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

void PageSetUrl(PageState* p, const std::wstring& url) {
    if (!p || !p->ready) return;
    Url u = ParseUrl(url);
    Url cur = ParseUrl(p->url);
    if (!IsSameOrigin(u, cur)) return; // pushState yalnizca ayni HTTP(S) koken
    p->SetUrl(url);
    int pid = p->id;
    PostUi([pid, url]() {
        Tab* t = TabOfPageId(pid);
        if (!t) return;
        if (t->histIdx >= 0 && size_t(t->histIdx) < t->history.size()) t->history[size_t(t->histIdx)] = url;
        if (t == &CurTab()) SetWindowTextW(g_addr, url.c_str());
    });
}

// Sekme on plana geldi (arayuz gorunume bagladi)
void PageActivate(PageState& p) {
    if (!p.doc) return;
    if (p.needRestyle || p.styleQueued || std::fabs(p.viewW - p.styleW) > 1.f) { p.needRestyle = false; Restyle(p, true); }
    else {
        ScanImages(p);
        if (!p.layout || std::fabs(p.layout->viewW - p.viewW) > 0.5f) p.layoutQueued = true;
    }
    bool first = !p.activated;
    p.activated = true;
    if (JsOn(p)) {
        if (!p.rt->pendingNav.empty()) { AfterJs(p); return; }
        for (auto& ps : p.pendingScripts) FetchScript(&p, ps.first, ps.second);
        p.pendingScripts.clear();
        if (first) {
            p.rt->FireLoadEvents();
            if (p.rt->dirty) Restyle(p, true);
        }
        AfterJs(p);
    } else {
        PostStatus(p);
    }
}

// Gorunur alan degisti (pencere boyutu / yakinlastirma)
void PageResized(PageState& p) {
    p.layoutQueued = true;
    if (std::fabs(p.viewW - p.styleW) > 1.f) RequestRestyle(p, 120);
}

// ---------------- Sekme is parcacigi: olay dongusu ----------------

void RunPageTask(PageState& p, const std::function<void()>& f) {
    long long t = (long long)GetTickCount64();
    p.busySince = t ? t : 1;
    try {
        f();
    } catch (const std::bad_alloc&) {
        PageLog(&p, 2, L"Bellek yetersiz: görev durduruldu");
    } catch (...) {
        PageLog(&p, 2, L"Beklenmeyen hata: görev durduruldu");
    }
    p.busySince = 0;
}

// Gorevler (arayuz olaylari, ag sonuclari), zamanlayicilar (yalnizca on plandayken), kisitlamali
// stil ve yerlesim. Bir gorev uzun surerse yalnizca bu sekme bekler.
void PageLoop(PageState& p) {
    while (!p.quit) {
        double wait = -1;
        bool fg = p.active;
        if (fg && JsOn(p)) {
            double due = p.rt->NextTimerDue();
            if (due >= 0) wait = std::max(4.0, due - p.rt->Now());
        }
        if (fg && p.styleQueued) {
            double w2 = std::max(0.0, p.styleDue - NowMs());
            wait = wait < 0 ? w2 : std::min(wait, w2);
        }
        if (fg && p.layoutQueued) wait = 0;
        std::deque<std::function<void()>> tasks;
        {
            std::unique_lock<std::mutex> lk(p.qmu);
            if (p.q.empty() && !p.quit && wait != 0) {
                if (wait < 0) p.qcv.wait(lk);
                else p.qcv.wait_for(lk, std::chrono::milliseconds((long long)std::ceil(wait)));
            }
            tasks.swap(p.q);
        }
        for (auto& t : tasks) {
            if (p.quit) break;
            RunPageTask(p, t);
        }
        if (p.quit) break;
        fg = p.active;
        if (fg && JsOn(p)) {
            double due = p.rt->NextTimerDue();
            if (due >= 0 && due <= p.rt->Now()) RunPageTask(p, [&p]() { p.rt->RunDueTimers(p.rt->Now()); AfterJs(p); });
        }
        if (fg && p.styleQueued && NowMs() >= p.styleDue) RunPageTask(p, [&p]() { Restyle(p, true); });
        if (fg && p.layoutQueued) RunPageTask(p, [&p]() { DoLayout(p); });
    }
}

void OnPageReady(PagePtr p);

// Sayfa is parcacigi: yukle, sekmeye ver, sonra sekme kapanana / yeni sayfa gelene kadar olay dongusu
void LoadJob(Job job) {
    PagePtr sp = std::make_shared<PageState>();
    PageState* p = sp.get();
    p->id = job.id;
    p->tabUid = job.tabUid;
    p->token = job.token;
    p->mode = job.mode;
    p->SetUrl(job.url);
    p->base = job.url;
    p->host.page = p;
    p->viewW = float(job.viewW);
    p->viewH = float(job.viewH);
    g_pageThreads++;
    {
        std::lock_guard<std::mutex> g(g_pagesMu);
        g_pages[p->id] = sp;
    }
    bool ok = false;
    try {
        ok = LoadPage(job, p);
    } catch (...) {
        ok = false;
    }
    if (ok && !p->quit && !g_shuttingDown) {
        p->ready = true;
        PostUi([sp]() { OnPageReady(sp); });
        PageLoop(*p);
    }
    // temizlik bu is parcaciginda: merJS, DOM ve oynaticilar sekmeyle birlikte kapanir
    p->quit = true;
    {
        std::lock_guard<std::mutex> g(p->qmu);
        p->q.clear();
    }
    try {
        p->rt.reset();
        p->doc.reset();
        p->styles.clear();
        p->layout.reset();
    } catch (...) {}
    {
        std::lock_guard<std::mutex> g(g_pagesMu);
        auto it = g_pages.find(p->id);
        if (it != g_pages.end() && it->second.lock().get() == p) g_pages.erase(it);
    }
    g_pageThreads--;
}


// ---------------- Gezinme ----------------

std::wstring NormalizeInput(const std::wstring& in) {
    std::wstring t = Trim(in);
    if (t.empty()) return L"";
    std::wstring lower = ToLower(t);
    if (StartsWith(lower, L"about:")) return lower;
    if (t.size() >= 3 && iswalpha(t[0]) && t[1] == L':' && (t[2] == L'\\' || t[2] == L'/')) {
        std::wstring f = t;
        for (auto& c : f) if (c == L'\\') c = L'/';
        return L"file:///" + f;
    }
    if (lower.find(L"://") != std::wstring::npos) return t;
    bool looksLikeHost = t.find(L' ') == std::wstring::npos &&
                         (t.find(L'.') != std::wstring::npos || StartsWith(lower, L"localhost"));
    if (!looksLikeHost) return L"https://html.duckduckgo.com/html/?q=" + UrlEncodeComponent(t);
    return L"https://" + t;
}

// ---------------- Arayuz: durum, gezinme ----------------

void UpdateStatus() {
    std::wstring s = g_hoverText;
    if (s.empty() && CurPage()) {
        PageState& p = *CurPage();
        std::wstring right;
        if (int b = p.blocked.load()) {
            wchar_t trk[64];
            swprintf(trk, 64, i18n::Get(i18n::StringId::BlockedTrackers), b);
            right += trk;
        }
        if (p.js) {
            if (p.killed) {
                right += T(L"  ·  Betikler durduruldu", L"  ·  Scripts stopped");
            } else if (int e = p.jsErrors.load()) {
                wchar_t errs[64];
                swprintf(errs, 64, i18n::Get(i18n::StringId::JsErrors), e);
                right += errs;
            } else {
                right += i18n::Get(i18n::StringId::JsOk);
            }
        } else {
            right += i18n::Get(i18n::StringId::JsDisabled);
        }
        if (int im = p.imagesInFlight.load()) {
            wchar_t imgb[64];
            swprintf(imgb, 64, i18n::Get(i18n::StringId::LoadingImages), im);
            right = imgb + right;
        }
        s = right;
    }
    SetWindowTextW(g_status, s.c_str());
}

void SetStatus(const std::wstring& s) { g_hoverText = s; UpdateStatus(); }

void UpdateButtons() {
    EnableWindow(g_back, CurTab().histIdx > 0);
    EnableWindow(g_fwd, CurTab().histIdx >= 0 && CurTab().histIdx + 1 < int(CurTab().history.size()));
    if (g_back) InvalidateRect(g_back, nullptr, FALSE);
    if (g_fwd) InvalidateRect(g_fwd, nullptr, FALSE);
}

void StartJob(Job job, Tab* tab = nullptr) {
    if (!tab) tab = &CurTab();
    InterruptLoading(tab->nav->load());   // ayni sekmenin onceki (bitmemis) yuklemesinin betiklerini kes
    job.id = ++g_navId;
    job.tabUid = tab->uid;
    job.token = tab->nav;
    tab->nav->store(job.id);   // sekmenin onceki yuklemesi iptal
    tab->loading = true;
    tab->loadingUrl = job.url;
    job.js = g_jsEnabled;
    job.ytLite = g_ytLiteMode;
    job.viewW = g_view.ViewW();
    job.viewH = g_view.ViewH();
    if (tab == &CurTab()) {
        KillTimer(g_frame, T_REFRESH);
        SetStatus(std::wstring(i18n::Get(i18n::StringId::Loading)) + job.url);
    }
    InvalidateTabs();
    RunThread([job]() { LoadJob(job); });
}

// fromUser: adres cubugu, komut satiri ya da gecmis. Web sayfalari yerel dosyalara gidemez.
void Navigate(const std::wstring& input, NavMode mode, const FetchOptions* opt = nullptr, bool fromUser = false, Tab* tab = nullptr) {
    std::wstring target = NormalizeInput(input);
    if (target.empty()) return;
    // yerel dosya izni: istegi yapan sayfa (sekmenin kendi sayfasi; yeni sekmede acan sayfa)
    const PageState* opener = (tab && tab->page) ? tab->page.get() : CurPage().get();
    Job job;
    job.mode = mode;
    if (opt) job.opt = *opt;
    Url u = ParseUrl(target);
    if (!u.ok) {
        job.url = L"about:hata";
        job.inlineHtml = MessagePage(i18n::Get(i18n::StringId::InvalidAddressTitle), i18n::Get(i18n::StringId::InvalidAddressBody), target);
    } else if (u.scheme == L"file" && !fromUser && !(opener && !opener->fileDir.empty() && StartsWith(ToLower(u.ToString()), ToLower(opener->fileDir)))) {
        // yerel sayfa yalnizca kendi klasorundeki dosyalara gidebilir; web sayfalari hicbirine
        job.url = L"about:engellendi";
        job.inlineHtml = MessagePage(T(L"Engellendi", L"Blocked"), T(L"Bu sayfa bilgisayarındaki bu dosyayı açamaz.", L"This page can't open that file on your computer."), target);
    } else if (u.scheme == L"about" && !fromUser && !(opener && opener->internal) && ToLower(u.ToString()) != L"about:blank") {
        // dis web sayfalari dahili ayar sayfalarina (about:lang, about:youtube vb.) yonlendiremez
        job.url = L"about:engellendi";
        job.inlineHtml = MessagePage(T(L"Engellendi", L"Blocked"), T(L"Web sayfaları dahili tarayıcı sayfalarını açamaz.", L"Web pages cannot open internal browser pages."), target);
    } else if (u.scheme == L"file") {
        job.url = u.ToString();
    } else if (IsYouTubeUrl(u.ToString())) {
        job.url = NormalizeYouTubeUrl(u.ToString());
    } else if (u.scheme != L"http" && u.scheme != L"https" && u.scheme != L"about") {
        job.url = L"about:engellendi";
        wchar_t bbuf[256];
        swprintf(bbuf, 256, i18n::Get(i18n::StringId::BlockedSchemeBody), u.scheme.c_str());
        job.inlineHtml = MessagePage(i18n::Get(i18n::StringId::BlockedSchemeTitle), bbuf, target);
    } else if (u.scheme == L"http" && !g_httpAllowed.count(u.host) && !IsLoopbackHost(u.host)) {
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
    StartJob(std::move(job), tab);
}

void GoBack() { if (CurTab().histIdx > 0) { CurTab().histIdx--; UpdateButtons(); Navigate(CurTab().history[size_t(CurTab().histIdx)], NavHistory, nullptr, true); } }
void GoForward() { if (CurTab().histIdx + 1 < int(CurTab().history.size())) { CurTab().histIdx++; UpdateButtons(); Navigate(CurTab().history[size_t(CurTab().histIdx)], NavHistory, nullptr, true); } }
void Reload() { if (CurPage()) Navigate(CurPage()->Url(), NavReload, nullptr, true); }

// ---------------- Arayuz: gorunum olaylari -> sekme is parcacigi ----------------
// Tiklama, yazma, tuslar kuyruga girer; sekme mesgulse (betik calisiyor) sirasi gelince islenir.

PageState* CurRaw() { return CurPage() ? CurPage().get() : nullptr; }

void OnViewClick(int nodeId, int link, bool ctrl, float x, float y) {
    PageState* p = CurRaw();
    if (!p) return;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    p->MarkInput();
    p->Post([p, nodeId, link, ctrl, shift, x, y]() { p->inputSince = 0; PageClick(*p, nodeId, link, ctrl, shift, x, y); });
}

void OnViewChar(wchar_t ch) {
    PageState* p = CurRaw();
    if (!p || g_view.FocusNode() < 0) return;
    p->MarkInput();
    p->Post([p, ch]() { p->inputSince = 0; PageChar(*p, ch); });
}

bool OnViewKey(UINT vk) {
    PageState* p = CurRaw();
    if (!p || g_view.FocusNode() < 0) return false;
    bool ctrlV = vk == 'V' && GetKeyState(VK_CONTROL) < 0;
    bool enter = vk == VK_RETURN && p->focusKind.load() != 2;   // textarea'da Enter yeni satirdir (WM_CHAR)
    if (vk != VK_ESCAPE && vk != VK_TAB && !enter && !ctrlV) return false;
    std::wstring clip;
    if (ctrlV && OpenClipboard(g_frame)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                clip = t;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    if (vk == VK_ESCAPE || vk == VK_TAB) g_view.SetFocusNode(-1);   // aninda gorsel geri bildirim
    p->MarkInput();
    p->Post([p, vk, clip]() { p->inputSince = 0; PageKey(*p, vk, clip); });
    return true;
}

// Gorunur alan boyutu degisti: sekme yeni yerlesimi kendi is parcaciginda hesaplar
void RequestViewLayout() {
    PageState* p = CurRaw();
    if (!p) return;
    p->viewW = g_view.ViewW();
    p->viewH = g_view.ViewH();
    p->Post([p]() { PageResized(*p); });
}


// ---------------- Sayfa etkinlestirme (arayuz) ----------------

void LayoutFrame();

// "Sayfa yanit vermiyor" cubugu: gecerli sekmenin bir gorevi kHangMs'den uzun suruyorsa
long long g_hangAck = 0;   // kullanicinin "Bekle" dedigi mesguliyetin baslangici

// Gecerli sekmenin izlenecek sayfasi: yuklenmekte olan (betikleri calisan) sayfa varsa o, yoksa gosterilen
PagePtr WatchedPage() {
    Tab& t = CurTab();
    if (t.loading) {
        PagePtr lp = FindPage(t.nav->load());
        if (lp && !lp->ready) return lp;
    }
    return t.page;
}

void UpdateHangBar() {
    bool show = false;
    if (PagePtr p = WatchedPage()) {
        long long k = p->HangKey();
        show = k && k != g_hangAck;
    }
    if (show == g_hangVisible) return;
    g_hangVisible = show;
    LayoutFrame();
}

void HangWait() {
    if (PagePtr p = WatchedPage()) g_hangAck = p->HangKey();
    UpdateHangBar();
}

// Sayfanin betiklerini durdur: calisan betik kesilir, bundan sonra bu sayfada JS calismaz.
// Sayfa gorunur ve kaydirilabilir kalir; baglantilar ve formlar betiksiz calisir.
void StopPageScripts() {
    PagePtr sp = WatchedPage();
    if (!sp) return;
    PageState* p = sp.get();
    p->killed = true;
    p->interrupt = true;
    g_hangAck = p->HangKey();
    p->Post([p]() {
        PageLog(p, 1, T(L"Sayfa yanıt vermediği için betikleri durduruldu.", L"Page scripts were stopped because the page was not responding."));
        PostStatus(*p);
    });
    UpdateHangBar();
    UpdateStatus();
    InvalidateTabs();
}

// Gecerli sekmeden ayrilirken: kaydirma konumunu sakla, sayfa arka plana (zamanlayicilar durur)
void LeaveCurrentTab() {
    Tab& t = CurTab();
    t.scroll = g_view.ScrollY();
    if (t.page) t.page->active = false;
    KillTimer(g_frame, T_REFRESH);
    g_view.ClearSelection();
}

void UpdateSecLabel(const PageState* pg) {
    if (!pg) { SetWindowTextW(g_sec, L""); if (g_frame) InvalidateRect(g_frame, &g_urlPillRect, FALSE); return; }
    Url u = ParseUrl(pg->Url());
    if ((u.ok && u.scheme == L"about") || pg->internal) SetWindowTextW(g_sec, L" merOS");
    else if (u.ok && u.scheme == L"file") SetWindowTextW(g_sec, T(L" Dosya", L" File"));
    else if (pg->secure) SetWindowTextW(g_sec, L" HTTPS");
    else SetWindowTextW(g_sec, L" HTTP");
    if (g_frame) InvalidateRect(g_frame, &g_urlPillRect, FALSE);
}

void RefreshFind();

// Gecerli sekmeyi gorunume baglar. fresh: sayfa yeni geldi (kaydirma basa).
void ShowCurrentTab(bool fresh) {
    Tab& t = CurTab();
    UpdateButtons();
    InvalidateTabs();
    g_hoverText.clear();
    if (!t.page) {
        g_view.SetPage(ViewPage());
        if (GetFocus() != g_addr) SetWindowTextW(g_addr, t.loadingUrl == L"about:home" ? L"" : t.loadingUrl.c_str());
        SetWindowTextW(g_frame, kAppName);
        UpdateSecLabel(nullptr);
        SetStatus(t.loading ? std::wstring(i18n::Get(i18n::StringId::Loading)) + t.loadingUrl : L"");
        UpdateHangBar();
        return;
    }
    PageState& pg = *t.page;
    pg.viewW = g_view.ViewW();
    pg.viewH = g_view.ViewH();
    pg.active = true;
    std::wstring url = pg.Url(), title = pg.Title();
    std::wstring shownUrl = t.loading ? t.loadingUrl : url;
    if (shownUrl == L"about:home") shownUrl.clear();
    if (GetFocus() != g_addr || fresh) SetWindowTextW(g_addr, shownUrl.c_str());
    SetWindowTextW(g_frame, ((title.empty() ? url : title) + L" — " + kAppName).c_str());
    UpdateSecLabel(&pg);
    ViewPage vp;
    vp.images = pg.images;
    vp.layout = pg.PubLayout();
    pg.layoutPosted = false;
    g_view.SetPage(vp);
    if (g_consoleVisible) {
        std::wstring all;
        for (auto& l : pg.Log()) all += l + L"\r\n";
        SetWindowTextW(g_console, all.c_str());
    }
    if (!fresh) g_view.ScrollTo(t.scroll);
    pg.scrollY = g_view.ScrollY();
    bool first = !t.activated;
    t.activated = true;
    if (g_findVisible) RefreshFind();
    PageState* raw = &pg;
    pg.Post([raw]() { PageActivate(*raw); });
    UpdateStatus();
    UpdateHangBar();
    SetTimer(g_frame, T_REPAINT, 250, nullptr);   // ilk cizim pencere eslenmeden once kaybolursa
    if (first) {
        if (pg.refreshDelay >= 0 && !pg.refreshUrl.empty()) SetTimer(g_frame, T_REFRESH, UINT(pg.refreshDelay * 1000) + 1, nullptr);
        if (!g_snapshotPath.empty()) SetTimer(g_frame, T_SNAPSHOT, g_snapshotWait, nullptr);
    }
}

// Sekme is parcacigi yuklemeyi bitirdi
void OnPageReady(PagePtr p) {
    Tab* t = TabByUid(p->tabUid);
    if (!t || !p->Alive()) { p->Stop(); return; }   // sekme kapandi ya da yeni bir gezinme basladi
    std::wstring url = p->Url();
    if (p->mode == NavPush) {
        if (t->histIdx + 1 < int(t->history.size())) t->history.resize(size_t(t->histIdx + 1));
        if (t->history.empty() || t->history.back() != url) t->history.push_back(url);
        t->histIdx = int(t->history.size()) - 1;
    } else if (p->mode == NavHistory && t->histIdx >= 0 && size_t(t->histIdx) < t->history.size()) {
        t->history[size_t(t->histIdx)] = url;
    }
    t->loading = false;
    t->loadingUrl.clear();
    t->activated = false;
    t->scroll = 0;
    bool current = t == &CurTab();
    if (current) {
        KillTimer(g_frame, T_REFRESH);
        g_view.SetPage(ViewPage());
    }
    if (t->page) t->page->Stop();   // eski sayfanin is parcacigi DOM/JS'i kendisi serbest birakir
    t->page = std::move(p);
    if (current) ShowCurrentTab(true);
    else InvalidateTabs();
}

// ---------------- Sekme islemleri ----------------

std::vector<std::wstring> g_closedTabs;   // Ctrl+Shift+T ile geri acilabilir

Tab* InsertTab(size_t pos) {
    std::unique_ptr<Tab> t(new Tab());
    t->uid = ++g_tabUid;
    Tab* raw = t.get();
    pos = std::min(pos, g_tabs.size());
    g_tabs.insert(g_tabs.begin() + long(pos), std::move(t));
    if (!g_tabs.empty() && pos <= g_cur && g_tabs.size() > 1) g_cur++;
    return raw;
}

size_t TabIndex(const Tab* t) {
    for (size_t i = 0; i < g_tabs.size(); i++) if (g_tabs[i].get() == t) return i;
    return g_cur;
}

void ActivateTab(size_t i) {
    if (i >= g_tabs.size() || i == g_cur) return;
    LeaveCurrentTab();
    g_cur = i;
    ShowCurrentTab(false);
}

// Baglantilar: gecerli sekmenin hemen sagina; yeni bos sekme: en sona
size_t g_openerRun = 0;   // ayni sekmeden art arda acilan arka plan sekmeleri sirayla dizilir
void OpenTab(const std::wstring& url, bool foreground, bool fromUser) {
    size_t pos = fromUser ? g_tabs.size() : std::min(g_tabs.size(), g_cur + 1 + g_openerRun);
    Tab* t = InsertTab(pos);
    if (!fromUser && !foreground) g_openerRun++;
    if (foreground) { g_openerRun = 0; LeaveCurrentTab(); g_cur = TabIndex(t); }
    Navigate(url, NavPush, nullptr, fromUser, t);
    if (foreground) ShowCurrentTab(false);
    InvalidateTabs();
}

void NewTab() {
    OpenTab(L"about:home", true, true);
    SetFocus(g_addr);
}

void CloseTab(size_t i) {
    if (i >= g_tabs.size()) return;
    if (g_tabs.size() == 1) { DestroyWindow(g_frame); return; }   // son sekme: pencereyi kapat
    Tab& t = *g_tabs[i];
    if (t.page && !t.page->internal) g_closedTabs.push_back(t.page->Url());
    if (g_closedTabs.size() > 25) g_closedTabs.erase(g_closedTabs.begin());
    InterruptLoading(t.nav->load());
    t.nav->store(0);   // bekleyen yuklemeleri iptal et
    bool wasCur = i == g_cur;
    if (wasCur) { LeaveCurrentTab(); g_view.SetPage(ViewPage()); }
    if (t.page) t.page->Stop();   // donmus olsa bile: betik kesilir, is parcacigi kendini kapatir
    g_tabs.erase(g_tabs.begin() + long(i));
    g_openerRun = 0;
    if (wasCur) {
        g_cur = std::min(i, g_tabs.size() - 1);
        ShowCurrentTab(false);
    } else if (i < g_cur) {
        g_cur--;
    }
    InvalidateTabs();
}

void ReopenClosedTab() {
    if (g_closedTabs.empty()) return;
    std::wstring u = g_closedTabs.back();
    g_closedTabs.pop_back();
    OpenTab(u, true, true);
}


// Sayfa kaynagi (Ctrl+U): yeni sekmede satir numarali, renklendirilmis kaynak
void OpenSourceTab(std::wstring src, const std::wstring& pageUrl) {
    if (src.size() > 3u * 1024 * 1024) src.resize(3u * 1024 * 1024);
    std::wstring html = L"<html><head><title>" + HtmlEscape(T(L"Kaynak: ", L"Source: ") + pageUrl) + L"</title><style>"
        L"body{margin:0;background:#fff;color:#1f2430;font:13px Consolas,monospace}"
        L"table{border-collapse:collapse}td{padding:0 10px;white-space:pre-wrap;vertical-align:top}"
        L"td.n{color:#9aa1ad;text-align:right;background:#f6f7f9;border-right:1px solid #e3e6eb;user-select:none;white-space:pre}"
        L".t{color:#8b1fa9}.a{color:#9a3412}.v{color:#1d4ed8}.c{color:#6b7280}</style></head><body><table>";
    size_t line = 1, i = 0;
    std::wstring row;
    auto flush = [&]() {
        html += L"<tr><td class=n>" + std::to_wstring(line++) + L"</td><td>" + row + L"</td></tr>";
        row.clear();
    };
    // basit renklendirme: etiket, oznitelik, deger, yorum
    int state = 0;   // 0 metin, 1 etiket, 2 yorum, 3 deger
    wchar_t quote = 0;
    for (; i < src.size() && line < 60000; i++) {
        wchar_t c = src[i];
        if (c == L'\r') continue;
        if (c == L'\n') {
            if (state == 1) row += L"</span>";
            else if (state == 2) row += L"</span>";
            else if (state == 3) row += L"</span>";
            flush();
            if (state == 1) row += L"<span class=t>";
            else if (state == 2) row += L"<span class=c>";
            else if (state == 3) row += L"<span class=v>";
            continue;
        }
        std::wstring e = c == L'<' ? L"&lt;" : c == L'>' ? L"&gt;" : c == L'&' ? L"&amp;" : std::wstring(1, c);
        if (state == 0 && c == L'<') {
            if (src.compare(i, 4, L"<!--") == 0) { row += L"<span class=c>&lt;"; state = 2; }
            else { row += L"<span class=t>&lt;"; state = 1; }
        } else if (state == 2) {
            row += e;
            if (c == L'>' && i >= 2 && src[i - 1] == L'-' && src[i - 2] == L'-') { row += L"</span>"; state = 0; }
        } else if (state == 1) {
            if (c == L'"' || c == L'\'') { quote = c; row += L"<span class=v>" + e; state = 3; }
            else if (c == L'>') { row += L"&gt;</span>"; state = 0; }
            else row += e;
        } else if (state == 3) {
            row += e;
            if (c == quote) { row += L"</span>"; state = 1; }
        } else row += e;
    }
    if (state) row += L"</span>";
    flush();
    html += L"</table></body></html>";
    size_t pos = std::min(g_tabs.size(), g_cur + 1);
    Tab* t = InsertTab(pos);
    LeaveCurrentTab();
    g_cur = TabIndex(t);
    Job job;
    job.url = L"view-source:" + pageUrl;
    job.inlineHtml = html;
    StartJob(std::move(job), t);
    ShowCurrentTab(false);
}

void ViewSource() {
    PageState* p = CurRaw();
    if (!p) return;
    if (!p->source.empty()) { OpenSourceTab(p->source, p->Url()); return; }
    // kaynak yok (dahili sayfa): DOM'u sekmenin kendi is parcacigi yazar
    p->Post([p]() {
        if (!p->doc) return;
        std::wstring src = L"<!DOCTYPE html>\n" + SerializeHTML(p->doc->root.get(), false);
        std::wstring u = p->url;
        PostUi([src, u]() { OpenSourceTab(src, u); });
    });
}


void SaveScreenshot() {
    if (!CurPage()) return;
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

// Sayfa dokumu (Ctrl+Shift+D): hata ayiklama icin HTML, CSS, konsol ve goruntu bir klasore yazilir.
bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string u = ToUtf8(text);
    DWORD wr = 0;
    bool ok = WriteFile(h, u.data(), DWORD(u.size()), &wr, nullptr) && wr == u.size();
    CloseHandle(h);
    return ok;
}

// Sayfa dokumu (Ctrl+Shift+D): hata ayiklama icin HTML, CSS, konsol ve goruntu bir klasore yazilir.
// Arayuz klasoru ve ekran goruntusunu, sekme is parcacigi DOM/CSS dosyalarini yazar.
void SavePageDump() {
    PageState* p = CurRaw();
    if (!p) return;
    wchar_t dir[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(dir) + L"\\Documents" : L".";
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, 96, L"\\merOS-Dokum-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring folder = base + name;
    if (!CreateDirectoryW(folder.c_str(), nullptr)) { SetStatus(L"Döküm klasörü oluşturulamadı: " + folder); return; }
    g_view.SaveSnapshot(folder + L"\\goruntu.png");
    std::wstring view = std::to_wstring(int(g_view.ViewW())) + L" x " + std::to_wstring(int(g_view.ViewH()));
    size_t items = g_view.Layout().items.size();
    SetStatus(L"Sayfa dökümü yazılıyor: " + folder);
    p->Post([p, folder, view, items]() {
        PageState& pg = *p;
        std::wstring info = L"Adres: " + pg.url + L"\r\nTaban: " + pg.base + L"\r\nGörünür alan: " + view +
                            L"\r\nJS: " + (pg.js ? (pg.killed ? L"durduruldu" : L"açık") : L"kapalı") + L", hata: " + std::to_wstring(pg.jsErrors.load()) +
                            L"\r\nDüğüm: " + std::to_wstring(pg.doc ? pg.doc->count : 0) + L", çizim öğesi: " + std::to_wstring(items) +
                            L"\r\nMotor: MerEngine 0.3 / merJS 0.1\r\n\r\nCSS dosyaları:\r\n";
        int i = 0;
        for (auto& kv : pg.cssCache) {
            std::wstring fn = L"stil-" + std::to_wstring(++i) + L".css";
            info += fn + L"  <-  " + kv.first + L"\r\n";
            WriteUtf8File(folder + L"\\" + fn, kv.second);
        }
        info += L"\r\nResimler:\r\n";
        {
            std::lock_guard<std::recursive_mutex> lk(pg.images->Mu());
            for (auto& kv : pg.images->map) {
                if (StartsWith(kv.first, L"data:") || StartsWith(kv.first, L"svg:")) continue;
                info += (kv.second.state == 1 ? L"[tamam] " : kv.second.state == 2 ? L"[HATA]  " : L"[bekle] ") + kv.first + L"\r\n";
            }
        }
        WriteUtf8File(folder + L"\\bilgi.txt", info);
        if (!pg.source.empty()) WriteUtf8File(folder + L"\\kaynak.html", pg.source);
        if (pg.doc) WriteUtf8File(folder + L"\\dom.html", L"<!DOCTYPE html>\n" + SerializeHTML(pg.doc->root.get(), false));
        std::wstring log;
        for (auto& l : pg.Log()) log += l + L"\r\n";
        WriteUtf8File(folder + L"\\konsol.txt", log);
        PostUi([folder]() { SetStatus(L"Sayfa dökümü kaydedildi: " + folder); });
    });
}


// ---------------- Pencere ----------------

float Scale() { return float(GetDpiForWindow(g_frame)) / 96.f; }

static bool FontExists(const wchar_t* face) {
    bool found = false;
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
        *reinterpret_cast<bool*>(lp) = true;
        return 0;
    }, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// ---------------- Direct2D merOS Shield Cyber-Slate Chrome Yardimcilari ----------------

ID2D1Factory* g_chromeD2D = nullptr;
IDWriteFactory* g_chromeDW = nullptr;
ID2D1DCRenderTarget* g_chromeRT = nullptr;
ID2D1StrokeStyle* g_roundStroke = nullptr;
HWND g_menuBtn = nullptr;
enum { ID_MENU = 199 };

bool EnsureChromeD2D() {
    if (!g_chromeD2D) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_chromeD2D))) return false;
    }
    if (!g_chromeDW) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_chromeDW)))) return false;
    }
    if (!g_roundStroke && g_chromeD2D) {
        D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        g_chromeD2D->CreateStrokeStyle(ssp, nullptr, 0, &g_roundStroke);
    }
    if (!g_chromeRT && g_chromeD2D) {
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        if (FAILED(g_chromeD2D->CreateDCRenderTarget(&rtp, &g_chromeRT))) return false;
    }
    return g_chromeRT != nullptr;
}

ID2D1DCRenderTarget* BindChromeDC(HDC dc, const RECT& rc) {
    if (!EnsureChromeD2D()) return nullptr;
    if (FAILED(g_chromeRT->BindDC(dc, &rc))) return nullptr;
    g_chromeRT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_chromeRT->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return g_chromeRT;
}

D2D1_COLOR_F D2DCol(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

void DrawD2DText(ID2D1RenderTarget* rt, const std::wstring& txt, const D2D1_RECT_F& r, float pxSize, D2D1_COLOR_F col,
                 DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                 bool trimEllipsis = true, const wchar_t* family = L"Segoe UI") {
    if (txt.empty() || !g_chromeDW || r.right <= r.left) return;
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(g_chromeDW->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, pxSize, L"", &fmt)) || !fmt) return;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetTextAlignment(align);
    IDWriteInlineObject* ell = nullptr;
    if (trimEllipsis && SUCCEEDED(g_chromeDW->CreateEllipsisTrimmingSign(fmt, &ell)) && ell) {
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&tr, ell);
    }
    ID2D1SolidColorBrush* br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(col, &br)) && br) {
        rt->DrawText(txt.c_str(), UINT32(txt.size()), fmt, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        br->Release();
    }
    if (ell) ell->Release();
    fmt->Release();
}

void DrawBrandOrbD2D(ID2D1RenderTarget* rt, float cx, float cy, float rad) {
    // merOS Shield ozgun Siber-Mavi / Turkuaz / Zumrut Kalkan amblemi
    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, D2DCol(0x0EA5E9)},
        {0.55f, D2DCol(0x2563EB)},
        {1.0f, D2DCol(0x10B981)}
    };
    ID2D1GradientStopCollection* gsc = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, &gsc)) && gsc) {
        ID2D1LinearGradientBrush* lgb = nullptr;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - rad, cy - rad), D2D1::Point2F(cx + rad, cy + rad)),
                gsc, &lgb)) && lgb) {
            D2D1_RECT_F sq = D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
            rt->FillRoundedRectangle(D2D1::RoundedRect(sq, rad * 0.55f, rad * 0.55f), lgb);
            lgb->Release();
        }
        gsc->Release();
    }
    ID2D1SolidColorBrush* wb = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0xFFFFFF, 0.95f), &wb)) && wb) {
        if (g_chromeD2D) {
            ID2D1PathGeometry* pg = nullptr;
            if (SUCCEEDED(g_chromeD2D->CreatePathGeometry(&pg)) && pg) {
                ID2D1GeometrySink* sk = nullptr;
                if (SUCCEEDED(pg->Open(&sk)) && sk) {
                    float r2 = rad * 0.54f;
                    sk->BeginFigure(D2D1::Point2F(cx, cy - r2), D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(D2D1::Point2F(cx + r2 * 0.88f, cy - r2 * 0.45f));
                    sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx + r2 * 0.82f, cy + r2 * 0.55f), D2D1::Point2F(cx, cy + r2)));
                    sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx - r2 * 0.82f, cy + r2 * 0.55f), D2D1::Point2F(cx - r2 * 0.88f, cy - r2 * 0.45f)));
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    sk->Release();
                    rt->FillGeometry(pg, wb);
                }
                pg->Release();
            }
        }
        wb->Release();
    }
}

void DrawShieldVectorD2D(ID2D1RenderTarget* rt, float cx, float cy, float r, D2D1_COLOR_F col, float strokeW) {
    if (!g_chromeD2D) return;
    ID2D1PathGeometry* pg = nullptr;
    if (FAILED(g_chromeD2D->CreatePathGeometry(&pg)) || !pg) return;
    ID2D1GeometrySink* sk = nullptr;
    if (SUCCEEDED(pg->Open(&sk)) && sk) {
        sk->BeginFigure(D2D1::Point2F(cx, cy - r), D2D1_FIGURE_BEGIN_FILLED);
        sk->AddLine(D2D1::Point2F(cx + r * 0.85f, cy - r * 0.52f));
        sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx + r * 0.82f, cy + r * 0.52f), D2D1::Point2F(cx, cy + r)));
        sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx - r * 0.82f, cy + r * 0.52f), D2D1::Point2F(cx - r * 0.85f, cy - r * 0.52f)));
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close();
        sk->Release();
        ID2D1SolidColorBrush* br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(col, &br)) && br) {
            rt->DrawGeometry(pg, br, strokeW, g_roundStroke);
            br->Release();
        }
    }
    pg->Release();
}

const wchar_t* g_fluentFontName = L"Segoe UI Symbol";

void MakeFonts() {
    for (HFONT f : {g_uiFont, g_symFont, g_monoFont}) if (f) DeleteObject(f);
    int dpi = int(GetDpiForWindow(g_frame));
    g_uiFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g_fluentFontName = L"Segoe UI Symbol";
    g_hasFluentFont = false;
    if (FontExists(L"Segoe Fluent Icons")) { g_fluentFontName = L"Segoe Fluent Icons"; g_hasFluentFont = true; }
    else if (FontExists(L"Segoe MDL2 Assets")) { g_fluentFontName = L"Segoe MDL2 Assets"; g_hasFluentFont = true; }
    g_symFont = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_fluentFontName);

    g_monoFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    for (HWND h : {g_addr, g_status, g_findEdit, g_findInfo, g_hangText, g_hangWait, g_hangStop}) if (h) SendMessageW(h, WM_SETFONT, WPARAM(g_uiFont), TRUE);
    for (HWND h : {g_back, g_fwd, g_reload, g_home, g_sec, g_js, g_consoleBtn, g_menuBtn, g_findPrev, g_findNext, g_findClose}) if (h) SendMessageW(h, WM_SETFONT, WPARAM(g_symFont), TRUE);
    SendMessageW(g_console, WM_SETFONT, WPARAM(g_monoFont), TRUE);
}

// ---------------- Yer İmleri Cubugu (Bookmarks Bar) ----------------

struct BookmarkItem {
    const wchar_t* titleTr;
    const wchar_t* titleEn;
    const wchar_t* url;
    const wchar_t* badge;
    uint32_t color;
};

const BookmarkItem kBookmarks[] = {
    {L"YouTube",       L"YouTube",       L"https://www.youtube.com/",      L"\u25B6", 0xE62117},
    {L"Vikipedi",      L"Wikipedia",     L"https://tr.wikipedia.org/",     L"W",      0x334155},
    {L"GitHub",        L"GitHub",        L"https://github.com/",           L"G",      0x1E293B},
    {L"Hacker News",   L"Hacker News",   L"https://news.ycombinator.com/", L"Y",      0xFF6600},
    {L"CNN Lite",      L"CNN Lite",      L"https://lite.cnn.com/",         L"C",      0xCC0000},
    {L"BBC Haber",     L"BBC News",      L"https://www.bbc.com/news",      L"B",      0x991B1B},
    {L"MerEngine",     L"MerEngine",     L"about:engine",                  L"M",      0x0284C7},
};
const int kBookmarkCount = int(sizeof(kBookmarks) / sizeof(kBookmarks[0]));
int g_bmHover = -1;

void GetBookmarkRects(int winW, float s, int bmTop, int bmH, std::vector<RECT>& out) {
    out.clear();
    int x = int(10 * s);
    int padY = int(3 * s);
    for (int i = 0; i < kBookmarkCount; i++) {
        const wchar_t* lbl = T(kBookmarks[i].titleTr, kBookmarks[i].titleEn);
        int w = int((34 + wcslen(lbl) * 6.8f) * s);
        RECT r{x, bmTop + padY, x + w, bmTop + bmH - padY};
        out.push_back(r);
        x += w + int(6 * s);
        if (x > winW - int(40 * s)) break;
    }
}

bool ShouldShowStatusBar() {
    if (!g_hoverText.empty()) return true;
    if (CurPage() && CurPage()->imagesInFlight.load() > 0) return true;
    return false;
}

void LayoutFrame() {
    RECT rc;
    GetClientRect(g_frame, &rc);
    float s = Scale();
    int pad = int(8 * s), bh = int(32 * s), bw = int(32 * s);
    int tabsH = int(42 * s);
    int navH = int(42 * s);
    int bmH = int(28 * s);
    int bar = tabsH + navH + bmH;
    int con = g_consoleVisible ? int(170 * s) : 0;
    bool showSb = ShouldShowStatusBar();
    int sb = showSb ? int(22 * s) : 0;

    MoveWindow(g_tabsWnd, 0, 0, rc.right, tabsH, TRUE);
    int y0 = tabsH + (navH - bh) / 2;
    int x = pad;
    for (HWND b : {g_back, g_fwd, g_reload, g_home}) { MoveWindow(b, x, y0, bw, bh, TRUE); x += bw + int(3 * s); }

    int rx = int(rc.right) - pad - bw;
    if (g_menuBtn) { MoveWindow(g_menuBtn, rx, y0, bw, bh, TRUE); rx -= bw + int(4 * s); }
    if (g_consoleBtn) { MoveWindow(g_consoleBtn, rx, y0, bw, bh, TRUE); rx -= bw + int(4 * s); }
    if (g_js) { MoveWindow(g_js, rx, y0, bw, bh, TRUE); rx -= int(8 * s); }

    int minLeft = x + int(10 * s);
    int maxRight = std::max(minLeft + int(180 * s), rx);
    int availW = maxRight - minLeft;
    int idealW = std::min(availW, int(760 * s));
    int pillLeft = minLeft + (availW - idealW) / 2;
    int pillRight = pillLeft + idealW;
    g_urlPillRect = RECT{pillLeft, y0, pillRight, y0 + bh};

    int leftBadgesW = int(82 * s); // [Kalkan merOS / HTTPS] rozeti
    ShowWindow(g_sec, SW_HIDE);
    int editTop = y0 + int(6 * s), editH = bh - int(12 * s);
    MoveWindow(g_addr, pillLeft + leftBadgesW, editTop, std::max(40, pillRight - pillLeft - leftBadgesW - int(14 * s)), editH, TRUE);

    int top = bar;
    int findH = g_findVisible ? int(38 * s) : 0;
    if (g_findVisible) {
        int fy = top + int(5 * s), fh = int(28 * s);
        int fw = int(260 * s), iw = int(90 * s), bw2 = int(30 * s);
        int fx = std::max(pad, int(rc.right) - pad - fw - iw - 3 * bw2 - int(14 * s));
        MoveWindow(g_findEdit, fx, fy + int(4 * s), fw - int(8 * s), fh - int(8 * s), TRUE);
        MoveWindow(g_findInfo, fx + fw + int(6 * s), fy + int(5 * s), iw, fh - int(8 * s), TRUE);
        int bx = fx + fw + iw + int(8 * s);
        MoveWindow(g_findPrev, bx, fy, bw2, fh, TRUE);
        MoveWindow(g_findNext, bx + bw2 + int(2 * s), fy, bw2, fh, TRUE);
        MoveWindow(g_findClose, bx + 2 * bw2 + int(6 * s), fy, bw2, fh, TRUE);
    }
    for (HWND f : {g_findEdit, g_findInfo, g_findPrev, g_findNext, g_findClose}) ShowWindow(f, g_findVisible ? SW_SHOW : SW_HIDE);
    top += findH;
    int hangH = g_hangVisible ? int(40 * s) : 0;
    if (g_hangVisible) {
        int hy = top + int(6 * s), hh = int(28 * s), bw2 = int(130 * s);
        MoveWindow(g_hangStop, int(rc.right) - pad - bw2, hy, bw2, hh, TRUE);
        MoveWindow(g_hangWait, int(rc.right) - pad - 2 * bw2 - int(6 * s), hy, bw2, hh, TRUE);
        MoveWindow(g_hangText, pad + int(6 * s), hy + int(5 * s), std::max(50, int(rc.right) - 2 * bw2 - 3 * pad - int(20 * s)), hh - int(6 * s), TRUE);
    }
    for (HWND hw : {g_hangText, g_hangWait, g_hangStop}) ShowWindow(hw, g_hangVisible ? SW_SHOW : SW_HIDE);
    top += hangH;
    MoveWindow(g_view.Hwnd(), 0, top, rc.right, std::max(0, int(rc.bottom) - top - sb - con), TRUE);
    MoveWindow(g_console, 0, int(rc.bottom) - sb - con, rc.right, con, TRUE);
    ShowWindow(g_console, g_consoleVisible ? SW_SHOW : SW_HIDE);
    if (showSb) {
        MoveWindow(g_status, pad, rc.bottom - sb + int(3 * s), rc.right - 2 * pad, sb - int(4 * s), TRUE);
        ShowWindow(g_status, SW_SHOW);
    } else {
        ShowWindow(g_status, SW_HIDE);
    }
    InvalidateTabs();
    InvalidateRect(g_frame, nullptr, FALSE);
}

void ToggleConsole() {
    g_consoleVisible = !g_consoleVisible.load();
    if (g_consoleVisible && CurPage()) {
        std::wstring all;
        for (auto& l : CurPage()->Log()) all += l + L"\r\n";
        SetWindowTextW(g_console, all.c_str());
    }
    if (g_consoleBtn) InvalidateRect(g_consoleBtn, nullptr, FALSE);
    LayoutFrame();
}

// ---------------- Sekme seridi & merOS Shield baslik cubugu (Direct2D) ----------------

struct TabGeom { RECT tab; RECT close; };
int g_tabHover = -1, g_tabHoverClose = -1;
bool g_tabPlusHover = false;
int g_winBtnHover = 0; // 0: none, 1: min, 2: max, 3: close, 4: chevron

std::wstring TabTitle(const Tab& t) {
    std::wstring pre = t.hungShown ? std::wstring(T(L"(Yanıt vermiyor) ", L"(Not responding) ")) : std::wstring();
    if (t.loading && t.hungShown) return pre + (t.loadingUrl.empty() ? std::wstring() : t.loadingUrl);
    if (t.page) {
        std::wstring title = t.page->Title(), url = t.page->Url();
        if (!title.empty()) return pre + title;
        if (url == L"about:home") return pre + T(L"Yeni Sekme", L"New Tab");
        return pre + url;
    }
    if (t.loadingUrl == L"about:home") return T(L"Yeni Sekme", L"New Tab");
    return t.loadingUrl.empty() ? std::wstring(T(L"Yeni Sekme", L"New Tab")) : t.loadingUrl;
}

void WinCaptionRects(const RECT& rc, RECT& rChev, RECT& rMin, RECT& rMax, RECT& rClose) {
    float s = Scale();
    int bw = int(44 * s);
    int cw = int(34 * s);
    int bh = int(rc.bottom);
    rClose = RECT{rc.right - bw, 0, rc.right, bh};
    rMax   = RECT{rc.right - 2 * bw, 0, rc.right - bw, bh};
    rMin   = RECT{rc.right - 3 * bw, 0, rc.right - 2 * bw, bh};
    rChev  = RECT{rc.right - 3 * bw - cw - int(6 * s), 0, rc.right - 3 * bw - int(6 * s), bh};
}

void TabLayout(std::vector<TabGeom>& out, RECT& plus) {
    RECT rc;
    GetClientRect(g_tabsWnd, &rc);
    float s = Scale();
    int left = int(8 * s), top = int(5 * s), bottom = int(rc.bottom) - int(5 * s);
    int plusW = int(32 * s);
    int captionReserve = int(184 * s);
    int avail = std::max(80, int(rc.right) - left - plusW - captionReserve);
    int n = int(std::max<size_t>(1, g_tabs.size()));
    int w = std::min(int(224 * s), std::max(int(68 * s), avail / n));
    out.clear();
    for (int i = 0; i < int(g_tabs.size()); i++) {
        TabGeom g;
        g.tab = RECT{left + i * w, top, left + (i + 1) * w - int(5 * s), bottom};
        int cs = int(20 * s);
        int cx = g.tab.right - int(7 * s) - cs, cy = (top + bottom - cs) / 2;
        g.close = RECT{cx, cy, cx + cs, cy + cs};
        out.push_back(g);
    }
    int px = left + int(g_tabs.size()) * w + int(2 * s);
    int ph = int(28 * s);
    plus = RECT{px, (top + bottom - ph) / 2, px + ph, (top + bottom - ph) / 2 + ph};
}

void PaintTabs(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    float s = Scale();

    if (ID2D1DCRenderTarget* rt = BindChromeDC(dc, rc)) {
        rt->BeginDraw();

        // merOS Shield Siber-Obsidyen sekme seridi arka plani (#0C111A -> #101723)
        D2D1_GRADIENT_STOP bgStops[2] = {
            {0.0f, D2DCol(0x0C111A)},
            {1.0f, D2DCol(0x101723)}
        };
        ID2D1GradientStopCollection* bgGsc = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(bgStops, 2, &bgGsc)) && bgGsc) {
            ID2D1LinearGradientBrush* bgBr = nullptr;
            if (SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(float(rc.right), float(rc.bottom))),
                    bgGsc, &bgBr)) && bgBr) {
                rt->FillRectangle(D2D1::RectF(0, 0, float(rc.right), float(rc.bottom)), bgBr);
                bgBr->Release();
            }
            bgGsc->Release();
        }

        std::vector<TabGeom> geo;
        RECT plus;
        TabLayout(geo, plus);
        for (size_t i = 0; i < geo.size(); i++) {
            const Tab& t = *g_tabs[i];
            bool active = i == g_cur, hover = int(i) == g_tabHover;
            RECT r = geo[i].tab;
            D2D1_RECT_F rf = D2D1::RectF(float(r.left) + 0.5f, float(r.top) + 0.5f, float(r.right) - 0.5f, float(r.bottom) - 0.5f);
            float rad = 8.0f * s;
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rf, rad, rad);

            if (active) {
                // Aktif sekme ic dolgusu (#1E293B -> #161F30)
                D2D1_GRADIENT_STOP tStops[2] = {
                    {0.0f, D2DCol(0x1E293B)},
                    {1.0f, D2DCol(0x161F30)}
                };
                ID2D1GradientStopCollection* tGsc = nullptr;
                if (SUCCEEDED(rt->CreateGradientStopCollection(tStops, 2, &tGsc)) && tGsc) {
                    ID2D1LinearGradientBrush* tBr = nullptr;
                    if (SUCCEEDED(rt->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rf.left, rf.top), D2D1::Point2F(rf.left, rf.bottom)),
                            tGsc, &tBr)) && tBr) {
                        rt->FillRoundedRectangle(rr, tBr);
                        tBr->Release();
                    }
                    tGsc->Release();
                }
                // merOS Shield Siber-Mavi / Zumrut gradyan sekme cercevesi (#0EA5E9 -> #3B82F6 -> #10B981)
                D2D1_GRADIENT_STOP bStops[3] = {
                    {0.0f, D2DCol(0x0EA5E9, 0.90f)},
                    {0.5f, D2DCol(0x3B82F6, 0.85f)},
                    {1.0f, D2DCol(0x10B981, 0.85f)}
                };
                ID2D1GradientStopCollection* bGsc = nullptr;
                if (SUCCEEDED(rt->CreateGradientStopCollection(bStops, 3, &bGsc)) && bGsc) {
                    ID2D1LinearGradientBrush* bBr = nullptr;
                    if (SUCCEEDED(rt->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rf.left, rf.top), D2D1::Point2F(rf.right, rf.bottom)),
                            bGsc, &bBr)) && bBr) {
                        rt->DrawRoundedRectangle(rr, bBr, 1.3f * s);
                        bBr->Release();
                    }
                    bGsc->Release();
                }
            } else if (hover) {
                ID2D1SolidColorBrush* hBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x182234), &hBr)) && hBr) {
                    rt->FillRoundedRectangle(rr, hBr);
                    hBr->Release();
                }
                ID2D1SolidColorBrush* hbBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x28364E), &hbBr)) && hbBr) {
                    rt->DrawRoundedRectangle(rr, hbBr, 1.0f);
                    hbBr->Release();
                }
            }

            // Sekme ikonu (merOS Kalkan amblemi veya site rozeti)
            float icx = float(r.left) + 18.0f * s, icy = (float(r.top) + float(r.bottom)) * 0.5f, ir = 8.0f * s;
            bool isInternal = !t.page || t.page->internal || StartsWith(ToLower(t.page->Url()), L"about:");
            if (t.loading) {
                ID2D1SolidColorBrush* lBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x38BDF8), &lBr)) && lBr) {
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(icx, icy), ir, ir), lBr, 2.0f * s);
                    lBr->Release();
                }
            } else if (isInternal) {
                DrawBrandOrbD2D(rt, icx, icy, ir);
            } else {
                Url u = ParseUrl(t.page->Url());
                uint32_t col = 0x10B981;
                if (t.hungShown) col = 0xF97316;
                else if (t.page->ytLite || (u.ok && u.host.find(L"youtube") != std::wstring::npos)) col = 0xEF4444;
                else if (!t.page->secure) col = 0x64748B;
                ID2D1SolidColorBrush* fBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(col), &fBr)) && fBr) {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(icx, icy), ir, ir), fBr);
                    fBr->Release();
                }
                std::wstring letter = L"W";
                if (u.ok && !u.host.empty()) {
                    std::wstring host = u.host;
                    if (StartsWith(host, L"www.")) host = host.substr(4);
                    if (!host.empty()) letter = std::wstring(1, wchar_t(towupper(host[0])));
                }
                DrawD2DText(rt, letter, D2D1::RectF(icx - ir, icy - ir, icx + ir, icy + ir), 10.0f * s,
                            D2DCol(0xFFFFFF), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
            }

            // Sekme basligi (DirectWrite ClearType)
            std::wstring title = TabTitle(t);
            D2D1_RECT_F tr = D2D1::RectF(icx + ir + 8.0f * s, float(r.top), float(geo[i].close.left) - 4.0f * s, float(r.bottom));
            DrawD2DText(rt, title, tr, 12.0f * s,
                        active ? D2DCol(0xF8FAFC) : hover ? D2DCol(0xE2E8F0) : D2DCol(0x94A3B8),
                        active ? DWRITE_FONT_WEIGHT_MEDIUM : DWRITE_FONT_WEIGHT_NORMAL);

            // Sekme kapat (x) dugmesi
            if (active || hover || (r.right - r.left) > int(90 * s)) {
                RECT cr = geo[i].close;
                D2D1_RECT_F crf = D2D1::RectF(float(cr.left), float(cr.top), float(cr.right), float(cr.bottom));
                if (int(i) == g_tabHoverClose) {
                    ID2D1SolidColorBrush* cb = nullptr;
                    if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x334155), &cb)) && cb) {
                        rt->FillRoundedRectangle(D2D1::RoundedRect(crf, 5.0f * s, 5.0f * s), cb);
                        cb->Release();
                    }
                }
                ID2D1SolidColorBrush* xb = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(int(i) == g_tabHoverClose ? 0xFFFFFF : 0xCBD5E1), &xb)) && xb) {
                    float m = 5.8f * s;
                    rt->DrawLine(D2D1::Point2F(crf.left + m, crf.top + m), D2D1::Point2F(crf.right - m, crf.bottom - m), xb, 1.4f * s, g_roundStroke);
                    rt->DrawLine(D2D1::Point2F(crf.right - m, crf.top + m), D2D1::Point2F(crf.left + m, crf.bottom - m), xb, 1.4f * s, g_roundStroke);
                    xb->Release();
                }
            }
        }

        // Yeni sekme (+) dugmesi
        D2D1_RECT_F pf = D2D1::RectF(float(plus.left), float(plus.top), float(plus.right), float(plus.bottom));
        if (g_tabPlusHover) {
            ID2D1SolidColorBrush* pb = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x1E293B), &pb)) && pb) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(pf, 7.0f * s, 7.0f * s), pb);
                pb->Release();
            }
        }
        ID2D1SolidColorBrush* pxb = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0xE2E8F0), &pxb)) && pxb) {
            float pcx = (pf.left + pf.right) * 0.5f, pcy = (pf.top + pf.bottom) * 0.5f, pm = 5.5f * s;
            rt->DrawLine(D2D1::Point2F(pcx - pm, pcy), D2D1::Point2F(pcx + pm, pcy), pxb, 1.5f * s, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(pcx, pcy - pm), D2D1::Point2F(pcx, pcy + pm), pxb, 1.5f * s, g_roundStroke);
            pxb->Release();
        }

        // Sag ust pencere kontrol dugmeleri
        RECT rChev, rMin, rMax, rClose;
        WinCaptionRects(rc, rChev, rMin, rMax, rClose);
        auto drawCapBg = [&](const RECT& br, int id) {
            if (g_winBtnHover == id) {
                ID2D1SolidColorBrush* hb = nullptr;
                uint32_t hc = (id == 3) ? 0xE81123 : 0x1E293B;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(hc), &hb)) && hb) {
                    rt->FillRectangle(D2D1::RectF(float(br.left), float(br.top), float(br.right), float(br.bottom)), hb);
                    hb->Release();
                }
            }
        };
        drawCapBg(rChev, 4);
        drawCapBg(rMin, 1);
        drawCapBg(rMax, 2);
        drawCapBg(rClose, 3);

        ID2D1SolidColorBrush* icBr = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0xCBD5E1), &icBr)) && icBr) {
            {
                float cx = (rChev.left + rChev.right) * 0.5f, cy = (rChev.top + rChev.bottom) * 0.5f, d = 3.8f * s;
                rt->DrawLine(D2D1::Point2F(cx - d, cy - d * 0.45f), D2D1::Point2F(cx, cy + d * 0.55f), icBr, 1.3f * s, g_roundStroke);
                rt->DrawLine(D2D1::Point2F(cx, cy + d * 0.55f), D2D1::Point2F(cx + d, cy - d * 0.45f), icBr, 1.3f * s, g_roundStroke);
            }
            {
                ID2D1SolidColorBrush* sepBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x1E293B), &sepBr)) && sepBr) {
                    float sx = float(rMin.left) - 2.0f * s;
                    rt->DrawLine(D2D1::Point2F(sx, 11.0f * s), D2D1::Point2F(sx, float(rc.bottom) - 11.0f * s), sepBr, 1.0f);
                    sepBr->Release();
                }
            }
            {
                float cx = (rMin.left + rMin.right) * 0.5f, cy = (rMin.top + rMin.bottom) * 0.5f, d = 5.0f * s;
                rt->DrawLine(D2D1::Point2F(cx - d, cy), D2D1::Point2F(cx + d, cy), icBr, 1.2f * s);
            }
            {
                float cx = (rMax.left + rMax.right) * 0.5f, cy = (rMax.top + rMax.bottom) * 0.5f, d = 4.6f * s;
                rt->DrawRectangle(D2D1::RectF(cx - d, cy - d, cx + d, cy + d), icBr, 1.2f * s);
            }
            {
                icBr->SetColor(D2DCol(g_winBtnHover == 3 ? 0xFFFFFF : 0xCBD5E1));
                float cx = (rClose.left + rClose.right) * 0.5f, cy = (rClose.top + rClose.bottom) * 0.5f, d = 4.8f * s;
                rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), icBr, 1.3f * s, g_roundStroke);
                rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), icBr, 1.3f * s, g_roundStroke);
            }
            icBr->Release();
        }

        rt->EndDraw();
    }

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void TabHit(int x, int y, int& tab, int& close, bool& plusHit, int& winBtn) {
    RECT rc;
    GetClientRect(g_tabsWnd, &rc);
    RECT rChev, rMin, rMax, rClose;
    WinCaptionRects(rc, rChev, rMin, rMax, rClose);
    POINT pt{x, y};
    winBtn = 0;
    if (PtInRect(&rMin, pt)) winBtn = 1;
    else if (PtInRect(&rMax, pt)) winBtn = 2;
    else if (PtInRect(&rClose, pt)) winBtn = 3;
    else if (PtInRect(&rChev, pt)) winBtn = 4;

    std::vector<TabGeom> geo;
    RECT plus;
    TabLayout(geo, plus);
    tab = close = -1;
    plusHit = (winBtn == 0) && (PtInRect(&plus, pt) != 0);
    if (winBtn == 0) {
        for (size_t i = 0; i < geo.size(); i++) {
            if (PtInRect(&geo[i].tab, pt)) {
                tab = int(i);
                RECT c = geo[i].close;
                InflateRect(&c, 3, 3);
                if (PtInRect(&c, pt)) close = int(i);
            }
        }
    }
}

LRESULT CALLBACK TabsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintTabs(h); return 0;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &pt);
        int t, c, wb;
        bool p;
        TabHit(pt.x, pt.y, t, c, p, wb);
        if (t < 0 && !p && wb == 0) return HTTRANSPARENT;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        int t, c, wb;
        bool p;
        TabHit(GET_X_LPARAM(l), GET_Y_LPARAM(l), t, c, p, wb);
        if (t != g_tabHover || c != g_tabHoverClose || p != g_tabPlusHover || wb != g_winBtnHover) {
            g_tabHover = t; g_tabHoverClose = c; g_tabPlusHover = p; g_winBtnHover = wb;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT te{sizeof(te), TME_LEAVE, h, 0};
            TrackMouseEvent(&te);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tabHover = g_tabHoverClose = -1;
        g_tabPlusHover = false;
        g_winBtnHover = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int t, c, wb;
        bool p;
        TabHit(GET_X_LPARAM(l), GET_Y_LPARAM(l), t, c, p, wb);
        if (wb == 1) { ShowWindow(g_frame, SW_MINIMIZE); return 0; }
        if (wb == 2) { ShowWindow(g_frame, IsZoomed(g_frame) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
        if (wb == 3) { PostMessageW(g_frame, WM_CLOSE, 0, 0); return 0; }
        if (wb == 4) {
            HMENU tm = CreatePopupMenu();
            for (size_t i = 0; i < g_tabs.size(); i++) {
                AppendMenuW(tm, MF_STRING | (i == g_cur ? MF_CHECKED : 0), UINT_PTR(1000 + i), TabTitle(*g_tabs[i]).c_str());
            }
            RECT rChev, rMin, rMax, rClose;
            RECT rc; GetClientRect(h, &rc);
            WinCaptionRects(rc, rChev, rMin, rMax, rClose);
            POINT spt{rChev.left, rChev.bottom};
            ClientToScreen(h, &spt);
            int sel = TrackPopupMenu(tm, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, spt.x, spt.y, 0, g_frame, nullptr);
            DestroyMenu(tm);
            if (sel >= 1000 && size_t(sel - 1000) < g_tabs.size()) ActivateTab(size_t(sel - 1000));
            return 0;
        }
        if (p) NewTab();
        else if (c >= 0) CloseTab(size_t(c));
        else if (t >= 0) { ActivateTab(size_t(t)); SetFocus(g_view.Hwnd()); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int t, c, wb;
        bool p;
        TabHit(GET_X_LPARAM(l), GET_Y_LPARAM(l), t, c, p, wb);
        if (t < 0 && !p && wb == 0) NewTab();
        return 0;
    }
    case WM_MBUTTONUP: {
        int t, c, wb;
        bool p;
        TabHit(GET_X_LPARAM(l), GET_Y_LPARAM(l), t, c, p, wb);
        if (t >= 0) CloseTab(size_t(t));
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        if (g_tabs.size() > 1) ActivateTab((g_cur + (d < 0 ? 1 : g_tabs.size() - 1)) % g_tabs.size());
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------- Modern dugme hover alt sinifi ----------------

LRESULT CALLBACK ModernBtnProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE) {
        if (!GetPropW(h, L"merHover")) {
            SetPropW(h, L"merHover", HANDLE(1));
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT te{sizeof(te), TME_LEAVE, h, 0};
            TrackMouseEvent(&te);
        }
    } else if (m == WM_MOUSELEAVE) {
        RemovePropW(h, L"merHover");
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, m, w, l);
}

// ---------------- Sayfada bul ----------------

std::wstring WindowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring t(size_t(n) + 1, L'\0');
    GetWindowTextW(h, &t[0], n + 1);
    t.resize(size_t(n));
    return t;
}

void UpdateFindInfo(int count) {
    std::wstring q = WindowText(g_findEdit);
    wchar_t buf[64] = L"";
    if (!q.empty()) {
        if (count == 0) swprintf(buf, 64, L"%ls", T(L"Bulunamadı", L"No results"));
        else swprintf(buf, 64, L"%d/%d", g_view.FindCurrent() + 1, count);
    }
    SetWindowTextW(g_findInfo, buf);
    InvalidateRect(g_findEdit, nullptr, TRUE);
    InvalidateRect(g_frame, nullptr, FALSE);
}

void RefreshFind() { UpdateFindInfo(g_view.Find(WindowText(g_findEdit), 0)); }
void FindStep(int dir) { UpdateFindInfo(g_view.Find(WindowText(g_findEdit), dir)); }

void LayoutFrame();
void ShowFind(bool show) {
    g_findVisible = show;
    if (show) {
        std::wstring sel = g_view.SelectedText();
        if (!sel.empty() && sel.size() < 200 && sel.find(L'\n') == std::wstring::npos) SetWindowTextW(g_findEdit, sel.c_str());
        LayoutFrame();
        SetFocus(g_findEdit);
        SendMessageW(g_findEdit, EM_SETSEL, 0, -1);
        RefreshFind();
    } else {
        g_view.ClearFind();
        LayoutFrame();
        SetFocus(g_view.Hwnd());
    }
}

LRESULT CALLBACK FindProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { FindStep(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { ShowFind(false); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == L'\n' || w == 0x1B)) return 0;
    if (m == WM_CHAR && w == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    return DefSubclassProc(h, m, w, l);
}

// ---------------- Sag tik menusu & Uygulama Menusu ----------------

void CopyToClipboard(const std::wstring& t) {
    if (t.empty() || !OpenClipboard(g_frame)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        if (void* mem = GlobalLock(g)) {
            memcpy(mem, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        } else GlobalFree(g);
    }
    CloseClipboard();
}

void SaveScreenshot();
void ToggleConsole();

void ShowAppMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CM_NEW_TAB, T(L"Yeni Sekme\tCtrl+T", L"New Tab\tCtrl+T"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CM_FIND, T(L"Sayfada Bul…\tCtrl+F", L"Find in Page…\tCtrl+F"));
    AppendMenuW(menu, MF_STRING, CM_SHOT, T(L"Ekran Görüntüsü Kaydet\tCtrl+Shift+S", L"Save Screenshot\tCtrl+Shift+S"));
    AppendMenuW(menu, MF_STRING, CM_SOURCE, T(L"Sayfa Kaynağını Görüntüle\tCtrl+U", L"View Page Source\tCtrl+U"));
    AppendMenuW(menu, MF_STRING | (g_consoleVisible ? MF_CHECKED : 0), CM_CONSOLE, T(L"Geliştirici Konsolu\tF12", L"Developer Console\tF12"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2001, T(L"Motor Bilgisi (MerEngine + merJS)", L"Engine Info (MerEngine + merJS)"));
    AppendMenuW(menu, MF_STRING, 2002, T(L"Dil: English / Türkçe Değiştir", L"Language: Switch English / Türkçe"));
    RECT br{};
    GetWindowRect(g_menuBtn ? g_menuBtn : g_frame, &br);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, br.right, br.bottom + 4, 0, g_frame, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case CM_NEW_TAB: NewTab(); break;
    case CM_FIND: ShowFind(true); break;
    case CM_SHOT: SaveScreenshot(); break;
    case CM_SOURCE: ViewSource(); break;
    case CM_CONSOLE: ToggleConsole(); break;
    case 2001: Navigate(L"about:engine", NavPush); break;
    case 2002:
        i18n::SetCurrentLanguage(i18n::GetCurrentLanguage() == i18n::Language::English ? i18n::Language::Turkish : i18n::Language::English);
        InvalidateRect(g_addr, nullptr, TRUE);
        InvalidateRect(g_frame, nullptr, FALSE);
        Reload();
        break;
    }
}

void ShowContextMenu(int link, POINT pt) {
    HMENU menu = CreatePopupMenu();
    std::wstring href;
    if (link >= 0 && size_t(link) < g_view.Layout().links.size()) {
        std::wstring raw = Trim(g_view.Layout().links[size_t(link)]);
        std::wstring lower = ToLower(raw);
        if (!raw.empty() && raw[0] != L'#' && !StartsWith(lower, L"javascript:") && !StartsWith(lower, L"meros-"))
            href = ResolveUrl(CurPage() ? CurPage()->base : L"", raw);
    }
    std::wstring sel = g_view.SelectedText();
    if (!href.empty()) {
        AppendMenuW(menu, MF_STRING, CM_OPEN_TAB, T(L"Bağlantıyı yeni sekmede aç", L"Open link in new tab"));
        AppendMenuW(menu, MF_STRING, CM_OPEN_BG, T(L"Bağlantıyı arka planda aç\tCtrl+Tık", L"Open link in background\tCtrl+Click"));
        AppendMenuW(menu, MF_STRING, CM_COPY_LINK, T(L"Bağlantı adresini kopyala", L"Copy link address"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (!sel.empty()) {
        AppendMenuW(menu, MF_STRING, CM_COPY, T(L"Kopyala\tCtrl+C", L"Copy\tCtrl+C"));
        std::wstring shortSel = sel.size() > 24 ? sel.substr(0, 24) + L"…" : sel;
        for (auto& c : shortSel) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        std::wstring item = std::wstring(T(L"Web'de ara: “", L"Search the web for “")) + shortSel + L"”";
        AppendMenuW(menu, MF_STRING, CM_SEARCH, item.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | (CurTab().histIdx > 0 ? 0 : MF_GRAYED), CM_BACK, T(L"Geri\tAlt+Sol", L"Back\tAlt+Left"));
    AppendMenuW(menu, MF_STRING | (CurTab().histIdx + 1 < int(CurTab().history.size()) ? 0 : MF_GRAYED), CM_FWD, T(L"İleri\tAlt+Sağ", L"Forward\tAlt+Right"));
    AppendMenuW(menu, MF_STRING, CM_RELOAD, T(L"Yeniden yükle\tCtrl+R", L"Reload\tCtrl+R"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CM_NEW_TAB, T(L"Yeni sekme\tCtrl+T", L"New tab\tCtrl+T"));
    AppendMenuW(menu, MF_STRING, CM_SELECT_ALL, T(L"Tümünü seç\tCtrl+A", L"Select all\tCtrl+A"));
    AppendMenuW(menu, MF_STRING, CM_FIND, T(L"Sayfada bul…\tCtrl+F", L"Find in page…\tCtrl+F"));
    AppendMenuW(menu, MF_STRING, CM_SOURCE, T(L"Sayfa kaynağını görüntüle\tCtrl+U", L"View page source\tCtrl+U"));
    AppendMenuW(menu, MF_STRING, CM_SHOT, T(L"Ekran görüntüsü kaydet\tCtrl+Shift+S", L"Save screenshot\tCtrl+Shift+S"));
    AppendMenuW(menu, MF_STRING, CM_CONSOLE, T(L"Geliştirici konsolu\tF12", L"Developer console\tF12"));
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_frame, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case CM_OPEN_TAB: OpenTab(href, true, false); break;
    case CM_OPEN_BG: OpenTab(href, false, false); break;
    case CM_COPY_LINK: CopyToClipboard(href); break;
    case CM_COPY: CopyToClipboard(sel); break;
    case CM_SEARCH: OpenTab(L"https://html.duckduckgo.com/html/?q=" + UrlEncodeComponent(sel.substr(0, 400)), true, false); break;
    case CM_BACK: GoBack(); break;
    case CM_FWD: GoForward(); break;
    case CM_RELOAD: Reload(); break;
    case CM_NEW_TAB: NewTab(); break;
    case CM_SELECT_ALL: g_view.SelectAll(); break;
    case CM_FIND: ShowFind(true); break;
    case CM_SOURCE: ViewSource(); break;
    case CM_SHOT: SaveScreenshot(); break;
    case CM_CONSOLE: ToggleConsole(); break;
    }
}

LRESULT CALLBACK AddrProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        int n = GetWindowTextLengthW(h);
        std::wstring t(size_t(n) + 1, L'\0');
        GetWindowTextW(h, &t[0], n + 1);
        t.resize(size_t(n));
        Navigate(t, NavPush, nullptr, true);
        SetFocus(g_view.Hwnd());
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { SetWindowTextW(h, CurPage() ? CurPage()->Url().c_str() : L""); SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == L'\n' || w == 0x1B)) return 0;
    if (m == WM_CHAR && w == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_SETFOCUS) { g_addrFocused = true; InvalidateRect(h, nullptr, TRUE); InvalidateRect(g_frame, &g_urlPillRect, FALSE); PostMessageW(h, EM_SETSEL, 0, -1); }
    if (m == WM_KILLFOCUS) { g_addrFocused = false; InvalidateRect(h, nullptr, TRUE); InvalidateRect(g_frame, &g_urlPillRect, FALSE); }
    if (m == WM_PAINT && !g_addrFocused && GetWindowTextLengthW(h) == 0) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brUrlBg);
        HGDIOBJ of = SelectObject(dc, g_uiFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(148, 163, 184));
        const wchar_t* cue = i18n::Get(i18n::StringId::SearchOrAddress);
        DrawTextW(dc, cue, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

void PaintFrameChrome(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    float s = Scale();
    int tabsH = int(42 * s), navH = int(42 * s), bmH = int(28 * s);
    bool showSb = ShouldShowStatusBar();
    int sb = showSb ? int(22 * s) : 0;
    int barBottom = tabsH + navH + bmH + (g_findVisible ? int(38 * s) : 0) + (g_hangVisible ? int(40 * s) : 0);

    if (ps.rcPaint.top < barBottom) {
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, barBottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        RECT bandRc{0, 0, rc.right, barBottom};

        if (ID2D1DCRenderTarget* rt = BindChromeDC(dc, bandRc)) {
            rt->BeginDraw();

            // merOS Shield Siber-Kayrak Arac Cubugu (#161E2E)
            ID2D1SolidColorBrush* navBr = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x161E2E), &navBr)) && navBr) {
                rt->FillRectangle(D2D1::RectF(0, float(tabsH), float(rc.right), float(barBottom)), navBr);
                navBr->Release();
            }

            // Stadyum/Hap Adres Cubugu (#0E1420)
            D2D1_RECT_F pf = D2D1::RectF(float(g_urlPillRect.left) + 0.5f, float(g_urlPillRect.top) + 0.5f,
                                         float(g_urlPillRect.right) - 0.5f, float(g_urlPillRect.bottom) - 0.5f);
            float pillRad = (pf.bottom - pf.top) * 0.5f;
            D2D1_ROUNDED_RECT pillRR = D2D1::RoundedRect(pf, pillRad, pillRad);

            ID2D1SolidColorBrush* urlBgBr = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x0E1420), &urlBgBr)) && urlBgBr) {
                rt->FillRoundedRectangle(pillRR, urlBgBr);
                urlBgBr->Release();
            }

            ID2D1SolidColorBrush* urlBdBr = nullptr;
            uint32_t bdCol = g_addrFocused ? 0x38BDF8 : 0x28354C;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(bdCol), &urlBdBr)) && urlBdBr) {
                rt->DrawRoundedRectangle(pillRR, urlBdBr, g_addrFocused ? 1.8f * s : 1.0f);
                urlBdBr->Release();
            }

            // Adres cubugu sol ic rozeti: merOS Shield Guvenlik Kapsulu ([Kalkan HTTPS / merOS])
            std::wstring secTxt(32, L'\0');
            secTxt.resize(size_t(GetWindowTextW(g_sec, &secTxt[0], 32)));
            bool isHttps = secTxt.find(L"HTTPS") != std::wstring::npos;
            bool isHttp  = secTxt.find(L"HTTP") != std::wstring::npos && !isHttps;
            uint32_t shieldHex = isHttps ? 0x34D399 : isHttp ? 0xF87171 : 0x38BDF8;
            const wchar_t* badgeWord = isHttps ? L"HTTPS" : isHttp ? L"HTTP" : L"merOS";

            D2D1_RECT_F badgePill = D2D1::RectF(pf.left + 4.0f * s, pf.top + 4.0f * s, pf.left + 74.0f * s, pf.bottom - 4.0f * s);
            float bpRad = (badgePill.bottom - badgePill.top) * 0.5f;
            ID2D1SolidColorBrush* bpBr = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x182234), &bpBr)) && bpBr) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(badgePill, bpRad, bpRad), bpBr);
                bpBr->Release();
            }
            float pyMid = (pf.top + pf.bottom) * 0.5f;
            DrawShieldVectorD2D(rt, badgePill.left + 13.0f * s, pyMid, 6.2f * s, D2DCol(shieldHex), 1.5f * s);
            DrawD2DText(rt, badgeWord, D2D1::RectF(badgePill.left + 23.0f * s, badgePill.top, badgePill.right - 4.0f * s, badgePill.bottom),
                        11.0f * s, D2DCol(shieldHex), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);

            // Yer İmleri Cubugu (Bookmarks Bar)
            int bmTop = tabsH + navH;
            std::vector<RECT> bmRects;
            GetBookmarkRects(rc.right, s, bmTop, bmH, bmRects);
            for (size_t i = 0; i < bmRects.size(); i++) {
                RECT br = bmRects[i];
                D2D1_RECT_F brf = D2D1::RectF(float(br.left), float(br.top), float(br.right), float(br.bottom));
                if (int(i) == g_bmHover) {
                    ID2D1SolidColorBrush* hBr = nullptr;
                    if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x222E44), &hBr)) && hBr) {
                        rt->FillRoundedRectangle(D2D1::RoundedRect(brf, 6.0f * s, 6.0f * s), hBr);
                        hBr->Release();
                    }
                }
                float bx = brf.left + 6.0f * s, by = (brf.top + brf.bottom) * 0.5f - 7.5f * s, bs = 15.0f * s;
                D2D1_RECT_F icoRf = D2D1::RectF(bx, by, bx + bs, by + bs);
                ID2D1SolidColorBrush* icoBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(kBookmarks[i].color), &icoBr)) && icoBr) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(icoRf, 4.0f * s, 4.0f * s), icoBr);
                    icoBr->Release();
                }
                DrawD2DText(rt, kBookmarks[i].badge, icoRf, 9.0f * s, D2DCol(0xFFFFFF), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);

                const wchar_t* lbl = T(kBookmarks[i].titleTr, kBookmarks[i].titleEn);
                D2D1_RECT_F lblRf = D2D1::RectF(bx + bs + 6.0f * s, brf.top, brf.right - 4.0f * s, brf.bottom);
                DrawD2DText(rt, lbl, lblRf, 11.5f * s, D2DCol(0xCBD5E1), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
            }

            int yCur = bmTop + bmH;
            if (g_findVisible) {
                int findH = int(38 * s);
                RECT feRc{};
                GetWindowRect(g_findEdit, &feRc);
                MapWindowPoints(nullptr, h, reinterpret_cast<LPPOINT>(&feRc), 2);
                InflateRect(&feRc, int(6 * s), int(4 * s));
                D2D1_RECT_F fef = D2D1::RectF(float(feRc.left), float(feRc.top), float(feRc.right), float(feRc.bottom));
                ID2D1SolidColorBrush* fbBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x0E1420), &fbBr)) && fbBr) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(fef, 6.0f * s, 6.0f * s), fbBr);
                    fbBr->Release();
                }
                ID2D1SolidColorBrush* fbdBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x334155), &fbdBr)) && fbdBr) {
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(fef, 6.0f * s, 6.0f * s), fbdBr, 1.0f);
                    fbdBr->Release();
                }
                yCur += findH;
            }
            if (g_hangVisible) {
                int hangH = int(40 * s);
                ID2D1SolidColorBrush* hBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x441C14), &hBr)) && hBr) {
                    rt->FillRectangle(D2D1::RectF(0, float(yCur), float(rc.right), float(yCur + hangH)), hBr);
                    hBr->Release();
                }
                yCur += hangH;
            }

            ID2D1SolidColorBrush* botBr = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x0B0F17), &botBr)) && botBr) {
                rt->DrawLine(D2D1::Point2F(0, float(barBottom) - 0.5f), D2D1::Point2F(float(rc.right), float(barBottom) - 0.5f), botBr, 1.0f);
                botBr->Release();
            }

            rt->EndDraw();
        }

        BitBlt(wdc, 0, tabsH, rc.right, barBottom - tabsH, dc, 0, tabsH, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    if (showSb) {
        RECT sbRc{0, rc.bottom - sb, rc.right, rc.bottom};
        FillRect(wdc, &sbRc, g_brStatusBg);
    }

    EndPaint(h, &ps);
}

void DrawModernButton(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    float s = Scale();
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool hovered  = GetPropW(dis->hwndItem, L"merHover") != nullptr;
    int id = int(dis->CtlID);

    ID2D1DCRenderTarget* rt = BindChromeDC(dc, rc);
    if (!rt) return;
    rt->BeginDraw();

    uint32_t bgHex = (id == ID_HANG_WAIT || id == ID_HANG_STOP) ? 0x441C14 : 0x161E2E;
    ID2D1SolidColorBrush* bgBr = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(bgHex), &bgBr)) && bgBr) {
        rt->FillRectangle(D2D1::RectF(0, 0, float(rc.right), float(rc.bottom)), bgBr);
        bgBr->Release();
    }

    bool drawPill = (!disabled && (hovered || pressed)) || id == ID_HANG_WAIT || id == ID_HANG_STOP;
    if (drawPill) {
        uint32_t pillHex = pressed ? 0x2D3B54 : hovered ? 0x222E44 : 0x1E293B;
        ID2D1SolidColorBrush* pBr = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(pillHex), &pBr)) && pBr) {
            D2D1_RECT_F rf = D2D1::RectF(1.0f, 1.0f, float(rc.right) - 1.0f, float(rc.bottom) - 1.0f);
            rt->FillRoundedRectangle(D2D1::RoundedRect(rf, 7.0f * s, 7.0f * s), pBr);
            pBr->Release();
        }
    }

    uint32_t fgHex = disabled ? 0x475569 : 0xE2E8F0;
    D2D1_RECT_F fullRf = D2D1::RectF(0, 0, float(rc.right), float(rc.bottom));

    // Windows 11 Segoe Fluent Icons / Segoe MDL2 Assets ile saat yonunde dogal, kusursuz vektor ikonlar
    if (g_hasFluentFont && (id == ID_BACK || id == ID_FWD || id == ID_RELOAD || id == ID_HOME)) {
        const wchar_t* glyph = L"\uE72B";
        if (id == ID_FWD) glyph = L"\uE72A";
        else if (id == ID_RELOAD) glyph = L"\uE72C";
        else if (id == ID_HOME) glyph = L"\uE80F";
        DrawD2DText(rt, glyph, fullRf, 14.5f * s, D2DCol(fgHex), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, g_fluentFontName);
        rt->EndDraw();
        return;
    }

    ID2D1SolidColorBrush* fgBr = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(fgHex), &fgBr)) && fgBr) {
        float cx = float(rc.right) * 0.5f, cy = float(rc.bottom) * 0.5f;
        float sw = 1.65f * s;
        float d = 6.2f * s;

        if (id == ID_BACK) {
            rt->DrawLine(D2D1::Point2F(cx + d, cy), D2D1::Point2F(cx - d, cy), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - 0.5f * s, cy - d), D2D1::Point2F(cx - d, cy), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - d, cy), D2D1::Point2F(cx - 0.5f * s, cy + d), fgBr, sw, g_roundStroke);
        } else if (id == ID_FWD) {
            rt->DrawLine(D2D1::Point2F(cx - d, cy), D2D1::Point2F(cx + d, cy), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + 0.5f * s, cy - d), D2D1::Point2F(cx + d, cy), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + d, cy), D2D1::Point2F(cx + 0.5f * s, cy + d), fgBr, sw, g_roundStroke);
        } else if (id == ID_RELOAD) {
            // Saat yonunde dogru 270 derece yay (sag-alttan baslayip saat yonunde sag-uste)
            if (g_chromeD2D) {
                ID2D1PathGeometry* pg = nullptr;
                if (SUCCEEDED(g_chromeD2D->CreatePathGeometry(&pg)) && pg) {
                    ID2D1GeometrySink* sk = nullptr;
                    if (SUCCEEDED(pg->Open(&sk)) && sk) {
                        float r = 6.2f * s;
                        float aStart = 0.55f;  // ~31 derece (sag-alt)
                        float aEnd   = -0.55f; // ~-31 derece (sag-ust)
                        D2D1_POINT_2F pStart = D2D1::Point2F(cx + r * std::cos(aStart), cy + r * std::sin(aStart));
                        D2D1_POINT_2F pEnd   = D2D1::Point2F(cx + r * std::cos(aEnd),   cy + r * std::sin(aEnd));
                        sk->BeginFigure(pStart, D2D1_FIGURE_BEGIN_HOLLOW);
                        sk->AddArc(D2D1::ArcSegment(pEnd, D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sk->EndFigure(D2D1_FIGURE_END_OPEN);
                        sk->Close();
                        sk->Release();
                        rt->DrawGeometry(pg, fgBr, sw, g_roundStroke);
                        rt->DrawLine(D2D1::Point2F(pEnd.x - 3.8f * s, pEnd.y - 0.4f * s), pEnd, fgBr, sw, g_roundStroke);
                        rt->DrawLine(D2D1::Point2F(pEnd.x + 0.2f * s, pEnd.y - 3.8f * s), pEnd, fgBr, sw, g_roundStroke);
                    }
                    pg->Release();
                }
            }
        } else if (id == ID_HOME) {
            float r = 6.5f * s;
            rt->DrawLine(D2D1::Point2F(cx - r, cy - 0.5f * s), D2D1::Point2F(cx, cy - r), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx, cy - r), D2D1::Point2F(cx + r, cy - 0.5f * s), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.72f, cy - 1.0f * s), D2D1::Point2F(cx - r * 0.72f, cy + r * 0.85f), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + r * 0.72f, cy - 1.0f * s), D2D1::Point2F(cx + r * 0.72f, cy + r * 0.85f), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.72f, cy + r * 0.85f), D2D1::Point2F(cx + r * 0.72f, cy + r * 0.85f), fgBr, sw, g_roundStroke);
        } else if (id == ID_JS) {
            DrawShieldVectorD2D(rt, cx, cy, 6.8f * s, D2DCol(g_jsEnabled ? 0xE2E8F0 : 0x64748B), sw);
            ID2D1SolidColorBrush* dotBr = nullptr;
            uint32_t dotCol = g_jsEnabled ? 0x10B981 : 0xEF4444;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(dotCol), &dotBr)) && dotBr) {
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 5.5f * s, cy + 5.5f * s), 3.0f * s, 3.0f * s), dotBr);
                dotBr->Release();
            }
        } else if (id == ID_CONSOLE) {
            D2D1_RECT_F tr = D2D1::RectF(cx - 6.8f * s, cy - 5.5f * s, cx + 6.8f * s, cy + 5.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(tr, 2.5f * s, 2.5f * s), fgBr, 1.4f * s);
            rt->DrawLine(D2D1::Point2F(cx - 3.6f * s, cy - 2.0f * s), D2D1::Point2F(cx - 1.0f * s, cy), fgBr, 1.3f * s, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - 1.0f * s, cy), D2D1::Point2F(cx - 3.6f * s, cy + 2.0f * s), fgBr, 1.3f * s, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + 0.5f * s, cy + 2.2f * s), D2D1::Point2F(cx + 3.6f * s, cy + 2.2f * s), fgBr, 1.3f * s, g_roundStroke);
            if (g_consoleVisible.load()) {
                ID2D1SolidColorBrush* dotBr = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2DCol(0x38BDF8), &dotBr)) && dotBr) {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 5.8f * s, cy + 5.8f * s), 2.8f * s, 2.8f * s), dotBr);
                    dotBr->Release();
                }
            }
        } else if (id == ID_MENU) {
            float w2 = 6.2f * s, gap = 4.2f * s;
            rt->DrawLine(D2D1::Point2F(cx - w2, cy - gap), D2D1::Point2F(cx + w2, cy - gap), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - w2, cy),       D2D1::Point2F(cx + w2, cy),       fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - w2, cy + gap), D2D1::Point2F(cx + w2, cy + gap), fgBr, sw, g_roundStroke);
        } else if (id == ID_FIND_PREV) {
            float r = 4.2f * s;
            rt->DrawLine(D2D1::Point2F(cx - r, cy + r * 0.5f), D2D1::Point2F(cx, cy - r * 0.5f), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx, cy - r * 0.5f), D2D1::Point2F(cx + r, cy + r * 0.5f), fgBr, sw, g_roundStroke);
        } else if (id == ID_FIND_NEXT) {
            float r = 4.2f * s;
            rt->DrawLine(D2D1::Point2F(cx - r, cy - r * 0.5f), D2D1::Point2F(cx, cy + r * 0.5f), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx, cy + r * 0.5f), D2D1::Point2F(cx + r, cy - r * 0.5f), fgBr, sw, g_roundStroke);
        } else if (id == ID_FIND_CLOSE) {
            float r = 4.2f * s;
            rt->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx + r, cy + r), fgBr, sw, g_roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + r, cy - r), D2D1::Point2F(cx - r, cy + r), fgBr, sw, g_roundStroke);
        } else {
            std::wstring txt = WindowText(dis->hwndItem);
            DrawD2DText(rt, txt, fullRf, 12.0f * s, D2DCol(fgHex),
                        DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        }
        fgBr->Release();
    }

    rt->EndDraw();
}

int HitBookmark(HWND h, int x, int y) {
    RECT rc;
    GetClientRect(h, &rc);
    float s = Scale();
    int bmTop = int(42 * s) + int(42 * s), bmH = int(28 * s);
    if (y < bmTop || y >= bmTop + bmH) return -1;
    std::vector<RECT> bmRects;
    GetBookmarkRects(rc.right, s, bmTop, bmH, bmRects);
    POINT pt{x, y};
    for (size_t i = 0; i < bmRects.size(); i++) {
        if (PtInRect(&bmRects[i], pt)) return int(i);
    }
    return -1;
}

LRESULT CALLBACK FrameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE: {
        if (w == TRUE) {
            NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            if (IsZoomed(h)) {
                int frame = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left   += frame;
                p->rgrc[0].top    += frame;
                p->rgrc[0].right  -= frame;
                p->rgrc[0].bottom -= frame;
            }
            return 0;
        }
        break;
    }
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT wr;
        GetWindowRect(h, &wr);
        float s = Scale();
        int border = int(6 * s);
        bool zoomed = IsZoomed(h) != FALSE;
        if (!zoomed) {
            bool left   = pt.x < wr.left + border;
            bool right  = pt.x >= wr.right - border;
            bool top    = pt.y < wr.top + border;
            bool bottom = pt.y >= wr.bottom - border;
            if (top && left)     return HTTOPLEFT;
            if (top && right)    return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left)   return HTLEFT;
            if (right)  return HTRIGHT;
            if (top)    return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        POINT cpt = pt;
        ScreenToClient(h, &cpt);
        int tabsH = int(42 * s);
        if (cpt.y >= 0 && cpt.y < tabsH) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        int bm = HitBookmark(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (bm != g_bmHover) {
            g_bmHover = bm;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT te{sizeof(te), TME_LEAVE, h, 0};
            TrackMouseEvent(&te);
        }
        if (bm >= 0) SetCursor(LoadCursor(nullptr, IDC_HAND));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_bmHover != -1) {
            g_bmHover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int bm = HitBookmark(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (bm >= 0 && bm < kBookmarkCount) {
            Navigate(kBookmarks[bm].url, NavPush, nullptr, true);
            SetFocus(g_view.Hwnd());
            return 0;
        }
        break;
    }
    case WM_MBUTTONUP: {
        int bm = HitBookmark(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (bm >= 0 && bm < kBookmarkCount) {
            OpenTab(kBookmarks[bm].url, false, true);
            return 0;
        }
        break;
    }
    case WM_CREATE: {
        g_frame = h;
        if (g_brNavBg) DeleteObject(g_brNavBg);
        if (g_brUrlBg) DeleteObject(g_brUrlBg);
        if (g_brStatusBg) DeleteObject(g_brStatusBg);
        if (g_brConBg) DeleteObject(g_brConBg);
        g_brNavBg    = CreateSolidBrush(RGB(22, 30, 46));
        g_brUrlBg    = CreateSolidBrush(RGB(14, 20, 32));
        g_brStatusBg = CreateSolidBrush(RGB(14, 20, 32));
        g_brConBg    = CreateSolidBrush(RGB(11, 15, 23));

        MARGINS margins{0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(h, &margins);
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));

        auto btn = [&](const wchar_t* t, int id) {
            HWND b = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, h, HMENU(INT_PTR(id)), g_inst, nullptr);
            SetWindowSubclass(b, ModernBtnProc, 3, 0);
            return b;
        };
        g_back = btn(L"", ID_BACK);
        g_fwd = btn(L"", ID_FWD);
        g_reload = btn(L"", ID_RELOAD);
        g_home = btn(L"", ID_HOME);
        g_js = btn(L"", ID_JS);
        g_consoleBtn = btn(L"", ID_CONSOLE);
        g_menuBtn = btn(L"", ID_MENU);
        g_sec = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_SEC)), g_inst, nullptr);
        g_addr = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_ADDR)), g_inst, nullptr);
        SendMessageW(g_addr, EM_SETLIMITTEXT, 8192, 0);
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_STATUS)), g_inst, nullptr);
        g_console = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_CONSOLE)), g_inst, nullptr);
        SendMessageW(g_console, EM_SETLIMITTEXT, 0, 0);
        SetWindowSubclass(g_addr, AddrProc, 1, 0);
        g_tabsWnd = CreateWindowExW(0, L"merOSTabStrip", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, nullptr, g_inst, nullptr);
        g_findEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_FIND_EDIT)), g_inst, nullptr);
        SendMessageW(g_findEdit, EM_SETCUEBANNER, TRUE, LPARAM(T(L"Sayfada bul", L"Find in page")));
        SetWindowSubclass(g_findEdit, FindProc, 2, 0);
        g_findInfo = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_FIND_INFO)), g_inst, nullptr);
        g_findPrev = btn(L"", ID_FIND_PREV);
        g_findNext = btn(L"", ID_FIND_NEXT);
        g_findClose = btn(L"", ID_FIND_CLOSE);
        for (HWND f : {g_findPrev, g_findNext, g_findClose}) ShowWindow(f, SW_HIDE);
        g_hangText = CreateWindowExW(0, L"STATIC", T(L"Bu sayfa yanıt vermiyor. Bekleyebilir ya da sayfanın betiklerini durdurabilirsiniz; diğer sekmeler etkilenmez.",
                                                    L"This page isn't responding. You can wait or stop its scripts; other tabs are not affected."),
                                     WS_CHILD | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 10, 10, h, HMENU(INT_PTR(ID_HANG_TEXT)), g_inst, nullptr);
        g_hangWait = btn(T(L"Bekle", L"Wait"), ID_HANG_WAIT);
        g_hangStop = btn(T(L"Sayfayı durdur", L"Stop page"), ID_HANG_STOP);
        for (HWND f : {g_hangText, g_hangWait, g_hangStop}) ShowWindow(f, SW_HIDE);
        g_view.Create(h, g_inst);
        g_measurer = g_view.Measurer();
        g_view.onRelayout = RequestViewLayout;
        g_view.onScrolled = [](float y) { if (CurPage()) CurPage()->scrollY = y; };
        SetTimer(h, T_WATCHDOG, 1000, nullptr);
        g_view.onMiddleClick = [](int link) {
            if (link >= 0 && size_t(link) < g_view.Layout().links.size()) {
                std::wstring raw = Trim(g_view.Layout().links[size_t(link)]);
                std::wstring lower = ToLower(raw);
                if (raw.empty() || raw[0] == L'#' || StartsWith(lower, L"javascript:") || StartsWith(lower, L"meros-")) return;
                OpenTab(ResolveUrl(CurPage() ? CurPage()->base : L"", raw), false, false);
            }
        };
        g_view.onContext = [](int link, int, POINT pt) { ShowContextMenu(link, pt); };
        g_view.onClick = OnViewClick;
        g_view.onChar = OnViewChar;
        g_view.onKey = OnViewKey;
        g_view.onHover = [](const std::wstring& href) {
            bool before = ShouldShowStatusBar();
            SetStatus(href.empty() ? L"" : ResolveUrl(CurPage() ? CurPage()->base : L"", href));
            if (ShouldShowStatusBar() != before) LayoutFrame();
        };
        MakeFonts();
        UpdateButtons();
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintFrameChrome(h);
        return 0;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(l);
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawModernButton(dis);
            return TRUE;
        }
        break;
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
    case WM_CTLCOLOREDIT: {
        HDC dc = HDC(w);
        if (HWND(l) == g_addr || HWND(l) == g_findEdit) {
            SetTextColor(dc, RGB(248, 250, 252));
            SetBkColor(dc, RGB(14, 20, 32));
            return LRESULT(g_brUrlBg);
        }
        if (HWND(l) == g_console) {
            SetTextColor(dc, RGB(226, 232, 240));
            SetBkColor(dc, RGB(11, 15, 23));
            return LRESULT(g_brConBg);
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = HDC(w);
        if (HWND(l) == g_console) {
            SetTextColor(dc, RGB(226, 232, 240));
            SetBkColor(dc, RGB(11, 15, 23));
            return LRESULT(g_brConBg);
        }
        if (HWND(l) == g_hangText) {
            SetTextColor(dc, RGB(254, 215, 170));
            SetBkColor(dc, RGB(68, 28, 20));
            SetBkMode(dc, OPAQUE);
            static HBRUSH s_hangBr = CreateSolidBrush(RGB(68, 28, 20));
            return LRESULT(s_hangBr);
        }
        if (HWND(l) == g_status) {
            SetTextColor(dc, RGB(148, 163, 184));
            SetBkColor(dc, RGB(14, 20, 32));
            return LRESULT(g_brStatusBg);
        }
        SetTextColor(dc, RGB(203, 213, 225));
        SetBkColor(dc, RGB(22, 30, 46));
        return LRESULT(g_brNavBg);
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_FIND_EDIT && HIWORD(w) == EN_CHANGE) { RefreshFind(); return 0; }
        switch (LOWORD(w)) {
        case ID_FIND_PREV: FindStep(-1); SetFocus(g_findEdit); return 0;
        case ID_FIND_NEXT: FindStep(1); SetFocus(g_findEdit); return 0;
        case ID_FIND_CLOSE: ShowFind(false); return 0;
        case ID_HANG_WAIT: HangWait(); SetFocus(g_view.Hwnd()); return 0;
        case ID_HANG_STOP: StopPageScripts(); SetFocus(g_view.Hwnd()); return 0;
        case ID_BACK: GoBack(); return 0;
        case ID_FWD: GoForward(); return 0;
        case ID_RELOAD: Reload(); return 0;
        case ID_HOME: Navigate(L"about:home", NavPush); return 0;
        case ID_CONSOLE: ToggleConsole(); return 0;
        case ID_MENU: ShowAppMenu(); return 0;
        case ID_JS:
            g_jsEnabled = !g_jsEnabled;
            InvalidateRect(g_js, nullptr, FALSE);
            Reload();
            SetFocus(g_view.Hwnd());
            return 0;
        }
        break;
    case WM_TIMER:
        if (w == T_WATCHDOG) {
            UpdateHangBar();
            bool changed = false;
            for (auto& t : g_tabs) {
                bool hung = t->page && t->page->Hung();
                if (!hung && t->loading) { PagePtr lp = FindPage(t->nav->load()); hung = lp && !lp->ready && lp->Hung(); }
                if (hung != t->hungShown) { t->hungShown = hung; changed = true; }
            }
            if (changed) InvalidateTabs();
            return 0;
        }
        KillTimer(h, w);
        if (w == T_REPAINT) {
            InvalidateRect(g_view.Hwnd(), nullptr, FALSE);
            InvalidateTabs();
        } else if (w == T_SNAPSHOT) {
            g_view.SaveSnapshot(g_snapshotPath);
            if (!g_logPath.empty() && CurPage()) {
                std::wstring all = L"URL: " + CurPage()->Url() + L"\r\n";
                for (auto& ln : CurPage()->Log()) all += ln + L"\r\n";
                WriteUtf8File(g_logPath, all);
            }
            DestroyWindow(h);
        } else if (w == T_REFRESH && CurPage() && !CurPage()->refreshUrl.empty()) {
            Navigate(CurPage()->refreshUrl, NavPush);
        }
        return 0;
    case WM_APP_UI:
        DrainUi();
        return 0;
    case WM_APP_VIDEO: {
        std::map<std::pair<int, std::wstring>, VideoMail> mail;
        {
            std::lock_guard<std::mutex> g(g_vmMu);
            mail.swap(g_vmLatest);
            g_vmPosted = false;
        }
        for (auto& kv : mail) {
            PageState* pp = PageById(kv.first.first);
            if (!pp || !kv.second.px) continue;
            bool resized = pp->images->UpdateVideo(kv.first.second, UINT(kv.second.w), UINT(kv.second.h), *kv.second.px, !g_snapshotPath.empty());
            bool cur = CurPage().get() == pp;
            if (resized) {
                pp->Post([pp, cur]() { if (cur) pp->layoutQueued = true; else pp->needRestyle = true; });
            }
            if (cur) g_view.InvalidateImageKey(kv.first.second);
        }
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(g_view.Hwnd());
        return 0;
    case WM_DESTROY: {
        KillTimer(h, T_WATCHDOG);
        g_shuttingDown = true;
        for (auto& t : g_tabs) {
            t->nav->store(0);
            if (t->page) t->page->Stop();
        }
        {
            std::lock_guard<std::mutex> g(g_pagesMu);
            for (auto& kv : g_pages) if (PagePtr p = kv.second.lock()) p->Stop();
        }
        g_view.SetPage(ViewPage());
        g_tabs.clear();
        if (g_chromeRT) { g_chromeRT->Release(); g_chromeRT = nullptr; }
        if (g_roundStroke) { g_roundStroke->Release(); g_roundStroke = nullptr; }
        if (g_chromeDW) { g_chromeDW->Release(); g_chromeDW = nullptr; }
        if (g_chromeD2D) { g_chromeD2D->Release(); g_chromeD2D = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH || PRIMARYLANGID(GetUserDefaultLangID()) == LANG_TURKISH) i18n::SetCurrentLanguage(i18n::Language::Turkish);
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

    WNDCLASSEXW tc{sizeof(tc)};
    tc.style = CS_DBLCLKS | CS_HREDRAW;
    tc.lpfnWndProc = TabsProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    tc.lpszClassName = L"merOSTabStrip";
    RegisterClassExW(&tc);

    InsertTab(0);   // ilk sekme (pencere olusturulmadan once: arayuz CurTab() kullanir)

    // Komut satiri: [--snapshot <png>] [--window <G>x<Y>] [adres veya dosya]
    //   --snapshot: sayfayi yukle, goruntusunu kaydet ve kapan (test/otomasyon icin)
    std::wstring start = (cmd && *cmd) ? Trim(cmd) : L"";
    int winW = 1280, winH = 860;
    auto nextToken = [&]() {
        std::wstring tok;
        start = Trim(start);
        if (!start.empty() && start[0] == L'"') {
            size_t e = start.find(L'"', 1);
            tok = start.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
            start = e == std::wstring::npos ? L"" : start.substr(e + 1);
        } else {
            size_t e = start.find(L' ');
            tok = start.substr(0, e);
            start = e == std::wstring::npos ? L"" : start.substr(e + 1);
        }
        return tok;
    };
    std::vector<std::wstring> extraTabs;   // --newtab <adres>: ek sekmeler
    while (StartsWith(Trim(start), L"--")) {
        std::wstring flag = nextToken();
        if (flag == L"--snapshot") g_snapshotPath = nextToken();
        else if (flag == L"--log") g_logPath = nextToken();
        else if (flag == L"--wait") g_snapshotWait = UINT(std::max(500, std::min(_wtoi(nextToken().c_str()), 600000)));
        else if (flag == L"--newtab") { std::wstring u = nextToken(); if (!u.empty() && extraTabs.size() < 20) extraTabs.push_back(u); }
        else if (flag == L"--window") {
            std::wstring v = nextToken();
            size_t x = v.find(L'x');
            if (x != std::wstring::npos) {
                winW = std::max(400, std::min(_wtoi(v.c_str()), 7680));
                winH = std::max(300, std::min(_wtoi(v.c_str() + x + 1), 4320));
            }
        }
    }
    start = Trim(start);
    if (!start.empty() && start[0] == L'"') { start.erase(0, 1); if (!start.empty() && start.back() == L'"') start.pop_back(); }
    if (start.empty()) start = L"about:home";

    HWND h = CreateWindowExW(0, wc.lpszClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    ShowWindow(h, show);
    UpdateWindow(h);
    Navigate(start, NavPush, nullptr, true);
    for (auto& u : extraTabs) OpenTab(u, false, true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
            WPARAM k = msg.wParam;
            bool shift = GetKeyState(VK_SHIFT) < 0;
            if (k == VK_F5 || (ctrl && k == 'R')) { Reload(); continue; }
            if (ctrl && k == 'T' && !shift) { NewTab(); continue; }
            if (ctrl && shift && k == 'T') { ReopenClosedTab(); continue; }
            if ((ctrl && k == 'W') || (ctrl && k == VK_F4)) { CloseTab(g_cur); continue; }
            if (ctrl && k == VK_TAB) { if (g_tabs.size() > 1) ActivateTab((g_cur + (shift ? g_tabs.size() - 1 : 1)) % g_tabs.size()); continue; }
            if (ctrl && (k == VK_NEXT || k == VK_PRIOR)) { if (g_tabs.size() > 1) ActivateTab((g_cur + (k == VK_PRIOR ? g_tabs.size() - 1 : 1)) % g_tabs.size()); continue; }
            if (ctrl && !shift && k >= '1' && k <= '9') { ActivateTab(k == '9' ? g_tabs.size() - 1 : size_t(k - '1')); continue; }
            if ((ctrl && k == 'F') || k == VK_F3) {
                if (k == VK_F3 && g_findVisible) FindStep(shift ? -1 : 1);
                else ShowFind(true);
                continue;
            }
            if (k == VK_ESCAPE && g_findVisible && msg.hwnd != g_addr) { ShowFind(false); continue; }
            if (ctrl && k == 'U') { ViewSource(); continue; }
            if (k == VK_F12 || (ctrl && GetKeyState(VK_SHIFT) < 0 && k == 'J')) { ToggleConsole(); continue; }
            if (ctrl && shift && k == 'S') { SaveScreenshot(); continue; }
            if (ctrl && GetKeyState(VK_SHIFT) < 0 && k == 'D') { SavePageDump(); continue; }
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
    // sekme is parcaciklarinin kapanmasini kisa sure bekle (ag istegi bekleyen varsa birakilir)
    for (int i = 0; i < 100 && g_pageThreads.load() > 0; i++) Sleep(20);
    {
        std::lock_guard<std::mutex> g(g_uiMu);
        g_uiQ.clear();
    }
    bool threadsLeft = g_pageThreads.load() > 0;
    if (!threadsLeft) g_measurer.reset();
    NetShutdown();
    CoUninitialize();
    // hala calisan bir sekme is parcacigi varsa (orn. yanit bekleyen ag istegi) statik nesneler onun
    // altindan yok edilmesin: sureci dogrudan sonlandir
    if (threadsLeft) ExitProcess(UINT(msg.wParam));
    return int(msg.wParam);
}
