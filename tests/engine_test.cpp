// MerEngine cekirdek testleri (platformdan bagimsiz: HTML -> DOM -> CSS -> stil -> yerlesim)
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include "../src/css.h"
#include "../src/dom.h"
#include "../src/layout.h"
#include "../src/style.h"
#include "../src/svg.h"
#include "../src/url.h"
#include "../src/util.h"

using namespace mer;

class FakeMeasurer : public ITextMeasurer {
public:
    std::vector<float> sizes;
    int Font(const Style& s) override { sizes.push_back(s.fontSize); return int(sizes.size()) - 1; }
    float Width(int f, const std::wstring& t) override { return float(t.size()) * sizes[size_t(f)] * 0.5f; }
    FontMetrics Metrics(int f) override { float z = sizes[size_t(f)]; return {z * 0.9f, z * 1.2f, z * 0.3f}; }
};

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

class FakeImages : public IImageProvider {
public:
    bool Lookup(const Node* img, float& w, float& h, std::wstring& key) override {
        auto* src = img->Attr(L"src");
        if (!src) return false;
        w = 400; h = 200; key = *src;
        return true;
    }
    bool LookupUrl(const std::wstring& url, float& w, float& h) override {
        if (url.empty()) return false;
        w = 20; h = 20;
        return true;
    }
};

static PageLayout Render(const std::wstring& html, float w = 800) {
    auto doc = ParseHTML(html);
    StyleSheet sh;
    sh.vw = w;
    sh.vh = 600;
    for (auto& s : doc->inlineStyles) sh.Add(s, L"https://site.test/css/");
    sh.BuildIndex();
    StyleContext ctx;
    ctx.vw = w;
    ctx.vh = 600;
    auto st = ComputeStyles(*doc, sh, ctx);
    FakeMeasurer fm;
    FakeImages im;
    PageLayout pl;
    LayoutPage(*doc, st, w, fm, pl, &im, 600);
    return pl;
}

static const DisplayItem* FindText(const PageLayout& pl, const std::wstring& t) {
    for (auto& i : pl.items) if (i.kind == DisplayItem::Text && i.text == t) return &i;
    return nullptr;
}
static const DisplayItem* FindFill(const PageLayout& pl, uint32_t color) {
    for (auto& i : pl.items) if (i.kind == DisplayItem::Fill && i.color == color) return &i;
    return nullptr;
}

// SVG geometrisini sayan sahte cizici
class CountPainter : public ISvgPainter {
public:
    int fills = 0, strokes = 0;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    uint32_t lastColor = 0;
    void Fill(const std::vector<SvgSeg>& p, bool, uint32_t c) override { fills++; lastColor = c; Bounds(p); }
    void Stroke(const std::vector<SvgSeg>& p, float, uint32_t c, int, int) override { strokes++; lastColor = c; Bounds(p); }
    void Bounds(const std::vector<SvgSeg>& p) {
        for (auto& s : p) {
            int n = s.kind == 2 ? 3 : s.kind == 3 ? 0 : 1;
            for (int k = 0; k < n; k++) { minX = std::min(minX, s.x[k]); maxX = std::max(maxX, s.x[k]); minY = std::min(minY, s.y[k]); maxY = std::max(maxY, s.y[k]); }
        }
    }
};

static std::wstring AllText(const PageLayout& pl) {
    std::wstring o;
    for (auto& i : pl.items) if (i.kind == DisplayItem::Text) { o += i.text; o += L' '; }
    return o;
}

int main() {
    // URL
    CHECK(ParseUrl(L"https://Example.COM/a/./b/../c?x=1#f").ToString() == L"https://example.com/a/c?x=1");
    CHECK(ResolveUrl(L"https://a.com/x/y.html", L"../z.css") == L"https://a.com/z.css" ||
          ParseUrl(ResolveUrl(L"https://a.com/x/y.html", L"../z.css")).ToString() == L"https://a.com/z.css");
    CHECK(ResolveUrl(L"https://a.com/x/y", L"//cdn.b.com/q") == L"https://cdn.b.com/q");
    CHECK(ParseUrl(L"https://banka.com@kotu.site/").host == L"kotu.site");
    CHECK(ParseUrl(L"https://banka.com@kotu.site/").hadUserInfo);
    CHECK(ParseUrl(L"https://a.com/boşluk yol").path == L"/bo%C5%9Fluk%20yol");
    CHECK(!ParseUrl(L"https://a.com:99999/").ok);

    // Entity
    CHECK(DecodeEntities(L"a &amp; b &lt;c&gt; &#246;&#x15F; &ccedil; &copy &bogus;") == L"a & b <c> öş ç © &bogus;");

    // Ayristirici: ortuk kapanislar
    {
        auto d = ParseHTML(L"<ul><li>a<li>b</ul><p>x<p>y<table><tr><td>1<td>2<tr><td>3</table>");
        int lis = 0, ps = 0, tds = 0;
        std::vector<Node*> st{d->root.get()};
        while (!st.empty()) {
            Node* n = st.back(); st.pop_back();
            if (n->name == L"li") { lis++; CHECK(n->parent->name == L"ul"); }
            if (n->name == L"p") { ps++; CHECK(n->parent->kind == Node::Doc); }
            if (n->name == L"td") { tds++; CHECK(n->parent->name == L"tr"); }
            for (auto& k : n->kids) st.push_back(k.get());
        }
        CHECK(lis == 2 && ps == 2 && tds == 3);
    }
    // Ham metin (script) ve title
    {
        auto d = ParseHTML(L"<title>Merhaba &amp; Dünya</title><script>if(a<b){x='</div>'}</script><b>ok</b>");
        CHECK(d->title == L"Merhaba & Dünya");
    }
    // Derinlik saldirisi: 100000 ic ice div cokertmemeli
    {
        std::wstring h;
        for (int i = 0; i < 100000; i++) h += L"<div>";
        h += L"derin";
        auto pl = Render(h);
        CHECK(pl.height >= 0);
    }

    // CSS: secici, ozgulluk, degisken, !important, medya
    {
        auto pl = Render(
            L"<style>:root{--ana:#ff0000} p{color:blue} .k{color:var(--ana)} #i{color:green} "
            L"div > p.k{font-size:20px} p{color:black!important} @media (max-width:600px){.k{display:none}}"
            L"@media print{body{display:none}} .gizli{position:absolute;width:1px;height:1px;overflow:hidden}</style>"
            L"<div><p class=k id=i>yazi</p></div><span class=gizli>gizli</span>");
        bool found = false;
        for (auto& it : pl.items) {
            if (it.kind == DisplayItem::Text && it.text == L"yazi") { found = true; CHECK(it.color == 0xFF000000); CHECK(it.h > 20 * 1.1f); }
            if (it.kind == DisplayItem::Text) CHECK(it.text != L"gizli");
        }
        CHECK(found);
    }
    {
        auto pl = Render(L"<style>.a{color:var(--yok, rgb(0 128 0))}</style><p class=a>x</p>");
        for (auto& it : pl.items) if (it.text == L"x") CHECK(it.color == 0xFF008000);
    }
    // Kelime kaydirma
    {
        std::wstring h = L"<p>";
        for (int i = 0; i < 200; i++) h += L"kelime ";
        h += L"</p>";
        auto pl = Render(h, 300);
        float maxRight = 0;
        for (auto& it : pl.items) if (it.kind == DisplayItem::Text) maxRight = std::max(maxRight, it.x + it.w);
        CHECK(maxRight <= 300 + 0.5f);
        CHECK(pl.height > 600);
    }
    // Ortalama: max-width + margin auto
    {
        auto pl = Render(L"<body style='margin:0'><div style='max-width:400px;margin:0 auto;background:#eee'>x</div>", 1000);
        bool ok = false;
        for (auto& it : pl.items) if (it.kind == DisplayItem::Fill && it.color == 0xFFEEEEEE) { ok = std::abs(it.x - 300) < 1 && std::abs(it.w - 400) < 1; }
        CHECK(ok);
    }
    // Liste isaretleri
    {
        auto pl = Render(L"<ol><li>bir<li>iki</ol><ul><li>uc</ul>");
        std::wstring t = AllText(pl);
        CHECK(t.find(L"1.") != std::wstring::npos && t.find(L"2.") != std::wstring::npos && t.find(L"•") != std::wstring::npos);
    }
    // Tablo: hucreler yan yana
    {
        auto pl = Render(L"<table><tr><td>sol</td><td>sag</td></tr></table>");
        float xs = -1, xr = -1, ys = -1, yr = -1;
        for (auto& it : pl.items) {
            if (it.text == L"sol") { xs = it.x; ys = it.y; }
            if (it.text == L"sag") { xr = it.x; yr = it.y; }
        }
        CHECK(xs >= 0 && xr > xs && std::abs(ys - yr) < 0.5f);
    }
    // Linkler
    {
        auto pl = Render(L"<a href='/x'>git</a> <a href='javascript:alert(1)'>kotu</a>");
        CHECK(pl.links.size() == 1 && pl.links[0] == L"/x");
        for (auto& it : pl.items) if (it.text == L"kotu") CHECK(it.link == -1);
    }
    // <br> ve pre
    {
        auto pl = Render(L"<pre>a\n  b</pre>a<br><br>b");
        CHECK(AllText(pl).find(L"  b") != std::wstring::npos);
    }
    // Renkler
    {
        uint32_t c;
        CHECK(ParseColor(L"#abc", c) && c == 0xFFAABBCC);
        CHECK(ParseColor(L"rgba(255,0,0,.5)", c) && c == 0x80FF0000);
        CHECK(ParseColor(L"hsl(120,100%,25%)", c) && c == 0xFF008000);
        CHECK(ParseColor(L"ff6600", c) && c == 0xFFFF6600);
    }
    // Oznitelik ve yapisal seciciler
    {
        auto pl = Render(L"<style>a[href^='https']{color:red} li:first-child{color:green} p + p{color:blue} input[type=text]{width:300px}</style>"
                         L"<a href='https://x'>g</a><a href='/y'>h</a><ul><li>bir</li><li>iki</li></ul><p>p1</p><p>p2</p>");
        for (auto& it : pl.items) {
            if (it.text == L"g") CHECK(it.color == 0xFFFF0000);
            if (it.text == L"h") CHECK(it.color != 0xFFFF0000);
            if (it.text == L"bir") CHECK(it.color == 0xFF008000);
            if (it.text == L"iki") CHECK(it.color != 0xFF008000);
            if (it.text == L"p2") CHECK(it.color == 0xFF0000FF);
            if (it.text == L"p1") CHECK(it.color != 0xFF0000FF);
        }
        std::vector<Selector> sl;
        CHECK(ParseSelectorList(L"div.a > p, [data-x=\"1\"], :not(.b)", sl) && sl.size() == 3);
        CHECK(!ParseSelectorList(L"a::placeholder", sl));
        std::vector<Selector> hv;
        auto hd = ParseHTML(L"<a href=x>y</a>");
        CHECK(ParseSelectorList(L"a:hover", hv) && !Matches(hv[0], hd->root->kids[0].get()));
    }
    // Flexbox: yan yana, justify space-between
    {
        auto pl = Render(L"<body style='margin:0'><nav style='display:flex;justify-content:space-between'><a>sol</a><a>orta</a><a>sag</a></nav>", 600);
        float xs = -1, xm = -1, xr = -1, ys = -1, yr = -2;
        for (auto& it : pl.items) {
            if (it.text == L"sol") { xs = it.x; ys = it.y; }
            if (it.text == L"orta") xm = it.x;
            if (it.text == L"sag") { xr = it.x + it.w; yr = it.y; }
        }
        CHECK(xs == 0 && xm > 200 && xm < 400 && std::abs(xr - 600) < 2 && std::abs(ys - yr) < 0.5f);
    }
    // flex: 1 esit paylasim
    {
        auto pl = Render(L"<body style='margin:0'><div style='display:flex;gap:20px'><div style='flex:1;background:#111'>a</div><div style='flex:1;background:#222'>b</div></div>", 620);
        float w1 = 0, w2 = 0, x2 = 0;
        for (auto& it : pl.items) {
            if (it.kind == DisplayItem::Fill && it.color == 0xFF111111) w1 = it.w;
            if (it.kind == DisplayItem::Fill && it.color == 0xFF222222) { w2 = it.w; x2 = it.x; }
        }
        CHECK(std::abs(w1 - 300) < 1 && std::abs(w2 - 300) < 1 && std::abs(x2 - 320) < 1);
    }
    // Grid 3 sutun
    {
        auto pl = Render(L"<body style='margin:0'><div style='display:grid;grid-template-columns:repeat(3,1fr)'><p>1</p><p>2</p><p>3</p><p>4</p></div>", 900);
        float x3 = -1, y1 = -1, y4 = -1;
        for (auto& it : pl.items) {
            if (it.text == L"3") x3 = it.x;
            if (it.text == L"1") y1 = it.y;
            if (it.text == L"4") y4 = it.y;
        }
        CHECK(std::abs(x3 - 600) < 1 && y4 > y1);
    }
    // Resimler (yuklu) ve form alanlari
    {
        auto pl = Render(L"<p>once <img src='a.png'> sonra</p><img src='b.png' style='display:block;max-width:100px'><input name=q placeholder='Ara'><input type=submit value=Git>");
        int imgs = 0; bool small = false, input = false, button = false;
        for (auto& it : pl.items) {
            if (it.kind == DisplayItem::Image) { imgs++; if (it.text == L"b.png") small = std::abs(it.w - 100) < 1 && std::abs(it.h - 50) < 1; }
            if (it.role == DisplayItem::TextInput && it.kind == DisplayItem::Text) input = it.text.find(L"Ara") != std::wstring::npos && it.node >= 0;
            if (it.role == DisplayItem::Button && it.kind == DisplayItem::Text) button = it.text.find(L"Git") != std::wstring::npos;
        }
        CHECK(imgs == 2 && small && input && button);
    }
    // ---------------- MerEngine 0.3 ----------------
    // calc(): yuzde + piksel
    {
        auto pl = Render(L"<body style='margin:0'><div style='width:calc(100% - 40px);background:#123456'>x</div>", 800);
        auto* f = FindFill(pl, 0xFF123456);
        CHECK(f && std::abs(f->w - 760) < 0.5f);
        auto pl2 = Render(L"<body style='margin:0'><div style='width:min(100%, 300px);background:#123456'>x</div>", 800);
        f = FindFill(pl2, 0xFF123456);
        CHECK(f && std::abs(f->w - 300) < 0.5f);
        auto pl3 = Render(L"<body style='margin:0'><div style='width:50vw;background:#123456'>x</div>", 800);
        f = FindFill(pl3, 0xFF123456);
        CHECK(f && std::abs(f->w - 400) < 0.5f);
    }
    // Medya sorgulari gercek genislige gore
    {
        CHECK(MediaMatches(L"screen and (min-width: 600px)", 800, 600));
        CHECK(!MediaMatches(L"(min-width: 1000px)", 800, 600));
        CHECK(MediaMatches(L"(width >= 600px) and (orientation: landscape)", 800, 600));
        CHECK(MediaMatches(L"(400px <= width <= 900px)", 800, 600));
        CHECK(!MediaMatches(L"print", 800, 600));
        CHECK(MediaMatches(L"print, (max-width: 900px)", 800, 600));
        CHECK(!MediaMatches(L"(prefers-color-scheme: dark)", 800, 600));
        CHECK(MediaMatches(L"not print", 800, 600));
        auto pl = Render(L"<style>.a{color:#ff0000} @media (max-width: 500px){.a{color:#00ff00}}</style><p class=a>m</p>", 800);
        CHECK(FindText(pl, L"m") && FindText(pl, L"m")->color == 0xFFFF0000);
        auto pl2 = Render(L"<style>.a{color:#ff0000} @media (max-width: 500px){.a{color:#00ff00}}</style><p class=a>m</p>", 400);
        CHECK(FindText(pl2, L"m") && FindText(pl2, L"m")->color == 0xFF00FF00);
    }
    // Yeni seciciler
    {
        auto pl = Render(L"<style>li:nth-child(2n){color:#ff0000} li:nth-child(2n+1){color:#0000ff} :is(.x,.y) b{color:#00ff00}"
                         L" p:not(.a .b){color:#123456} div:has(> i){background:#abcdef} li:last-of-type{font-size:30px}</style>"
                         L"<ul><li>t1</li><li>t2</li><li>t3</li></ul><div class=y><b>bb</b></div><div class=a><p class=b>pp</p></div><p class=b>qq</p><div><i>ii</i></div>");
        CHECK(FindText(pl, L"t1") && FindText(pl, L"t1")->color == 0xFF0000FF);
        CHECK(FindText(pl, L"t2") && FindText(pl, L"t2")->color == 0xFFFF0000);
        CHECK(FindText(pl, L"bb") && FindText(pl, L"bb")->color == 0xFF00FF00);
        CHECK(FindText(pl, L"pp") && FindText(pl, L"pp")->color != 0xFF123456);
        CHECK(FindText(pl, L"qq") && FindText(pl, L"qq")->color == 0xFF123456);
        CHECK(FindFill(pl, 0xFFABCDEF) != nullptr);
        CHECK(FindText(pl, L"t3") && FindText(pl, L"t3")->h > 30);
    }
    // ::before / ::after
    {
        auto pl = Render(L"<style>.m li{display:inline} .m li+li::before{content:' \\00b7 '} .q::after{content:\"[\" attr(data-n) \"]\"} .ic::before{content:'\\f007'}</style>"
                         L"<ul class=m><li>bir</li><li>iki</li></ul><p class=q data-n=7>not</p><p class=ic>ikon</p>");
        std::wstring t = AllText(pl);
        CHECK(t.find(L"\u00B7") != std::wstring::npos);
        CHECK(t.find(L"[7]") != std::wstring::npos);
        CHECK(t.find(L"\xF007") == std::wstring::npos); // ikon fontu karakteri cizilmez
        auto* a = FindText(pl, L"bir");
        auto* b = FindText(pl, L"iki");
        CHECK(a && b && b->x > a->x + a->w + 2);
    }
    // Float: metin resmin yanindan akar, clear altina iner
    {
        std::wstring h = L"<body style='margin:0'><div><img src='r.png' style='float:left;width:200px;height:100px'>";
        for (int i = 0; i < 30; i++) h += L"kelime ";
        h += L"<p style='clear:both;margin:0'>alt</p></div>";
        auto pl = Render(h, 800);
        auto* k = FindText(pl, L"kelime");
        CHECK(k && k->x >= 200 && k->y < 100);
        auto* alt = FindText(pl, L"alt");
        CHECK(alt && alt->y >= 100);
        std::wstring h2 = L"<body style='margin:0'><div style='float:right;width:100px;background:#00aa00'>sag</div><p style='margin:0'>sol metin</p>";
        auto pl2 = Render(h2, 800);
        auto* r = FindFill(pl2, 0xFF00AA00);
        CHECK(r && std::abs(r->x - 700) < 0.5f && std::abs(r->y) < 0.5f);
    }
    // Konumlandirma: absolute (relative kapsayici), fixed, relative kaydirma
    {
        auto pl = Render(L"<body style='margin:0'><div style='position:relative;width:400px;height:100px;margin-left:100px'>"
                         L"<span style='position:absolute;top:10px;right:0;width:50px;background:#ff00ff'>ab</span>govde</div>"
                         L"<div style='position:fixed;bottom:0;left:0;right:0;height:40px;background:#0000aa'>alt</div>"
                         L"<p style='position:relative;left:30px;margin:0'>kayik</p>", 800);
        auto* a = FindFill(pl, 0xFFFF00FF);
        CHECK(a && std::abs(a->x - 450) < 0.5f && std::abs(a->y - 10) < 0.5f && std::abs(a->w - 50) < 0.5f);
        auto* f = FindFill(pl, 0xFF0000AA);
        CHECK(f && std::abs(f->y - 560) < 0.5f && std::abs(f->w - 800) < 0.5f);
        auto* g = FindText(pl, L"govde");
        CHECK(g && g->y < 30); // absolute oge akista yer kaplamaz
        auto* k = FindText(pl, L"kayik");
        CHECK(k && std::abs(k->x - 30) < 0.5f);
    }
    // Gizleme: opacity 0 + absolute (Vector menu kutusu), visibility, sr-only, ekran disi
    {
        auto pl = Render(L"<div style='position:relative'><input type=checkbox style='position:absolute;opacity:0'><label>etiket</label></div>"
                         L"<p style='visibility:hidden'>gizli</p><p>gorunur</p><h1 style='text-indent:-9999px'>logo</h1>");
        int checks = 0;
        for (auto& i : pl.items) if (i.role == DisplayItem::Check) checks++;
        CHECK(checks == 0);
        CHECK(!FindText(pl, L"gizli") && FindText(pl, L"gorunur"));
        CHECK(!FindText(pl, L"logo"));
    }
    // inline-block: yan yana, padding'li
    {
        auto pl = Render(L"<body style='margin:0'><span style='display:inline-block;width:100px;background:#111111'>a</span>"
                         L"<span style='display:inline-block;padding:0 10px;background:#222222'>b</span>", 800);
        auto* a = FindFill(pl, 0xFF111111);
        auto* b = FindFill(pl, 0xFF222222);
        CHECK(a && b && std::abs(a->y - b->y) < 2 && b->x >= a->x + 100);
        CHECK(b && FindText(pl, L"b") && std::abs(FindText(pl, L"b")->x - (b->x + 10)) < 0.5f);
    }
    // Grid: alan adlari (Vector duzeni) ve auto-fill
    {
        auto pl = Render(L"<body style='margin:0'><div style=\"display:grid;grid-template-columns:200px minmax(0,1fr);column-gap:20px;"
                         L"grid-template-areas:'bas bas' 'yan icerik' 'alt alt'\">"
                         L"<div style='grid-area:bas;background:#aa0000'>b</div><div style='grid-area:yan;background:#00aa00'>y</div>"
                         L"<div style='grid-area:icerik;background:#0000aa'>i</div><div style='grid-area:alt;background:#aaaa00'>a</div></div>", 1000);
        auto* b = FindFill(pl, 0xFFAA0000);
        auto* y = FindFill(pl, 0xFF00AA00);
        auto* i = FindFill(pl, 0xFF0000AA);
        auto* a = FindFill(pl, 0xFFAAAA00);
        CHECK(b && std::abs(b->w - 1000) < 0.5f);
        CHECK(y && std::abs(y->w - 200) < 0.5f && std::abs(y->x) < 0.5f);
        CHECK(i && std::abs(i->x - 220) < 0.5f && std::abs(i->w - 780) < 0.5f && y && std::abs(i->y - y->y) < 0.5f);
        CHECK(a && b && a->y > i->y);
        auto pl2 = Render(L"<body style='margin:0'><div style='display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px'>"
                          L"<p style='margin:0'>k1</p><p style='margin:0'>k2</p><p style='margin:0'>k3</p><p style='margin:0'>k4</p><p style='margin:0'>k5</p></div>", 650);
        auto* k3 = FindText(pl2, L"k3");
        auto* k4 = FindText(pl2, L"k4");
        CHECK(k3 && k4 && k4->y > k3->y && std::abs(k4->x) < 0.5f); // 650px: 3 sutun
    }
    // Flex: otomatik kenar boslugu, metin ogeleri, min-content tabani (ust uste binme yok), sutun + ortala
    {
        auto pl = Render(L"<body style='margin:0'><nav style='display:flex'><a>logo</a><a style='margin-left:auto'>giris</a></nav>"
                         L"<div style='display:flex;width:120px'><span>Anasayfa</span><span>Tartisma</span></div>"
                         L"<button style='display:inline-flex'>Ara</button>"
                         L"<div style='display:flex;flex-direction:column;align-items:center;width:400px'><b>orta</b></div>", 600);
        auto* g = FindText(pl, L"giris");
        CHECK(g && std::abs(g->x + g->w - 600) < 2);
        auto* a = FindText(pl, L"Anasayfa");
        auto* t = FindText(pl, L"Tartisma");
        CHECK(a && t && t->x >= a->x + a->w - 0.5f);
        CHECK(FindText(pl, L"Ara") != nullptr);
        auto* o = FindText(pl, L"orta");
        CHECK(o && std::abs(o->x + o->w / 2 - 200) < 3);
    }
    // display:contents ve mask-image ikonu (renkle boyanir)
    {
        auto pl = Render(L"<div style='display:contents'><p>icerik</p></div>"
                         L"<span style='display:inline-block;width:20px;height:20px;background-color:#202122;mask-image:url(ikon.svg)'></span>", 600);
        CHECK(FindText(pl, L"icerik") != nullptr);
        bool masked = false;
        for (auto& i : pl.items) if (i.kind == DisplayItem::BgImage && i.tint == 0xFF202122 && EndsWith(i.text, L"ikon.svg")) masked = true;
        CHECK(masked);
        CHECK(!FindFill(pl, 0xFF202122)); // maskeli ogede duz kare cizilmez
    }
    // SVG geometri: viewBox olcegi, transform, currentColor, <use>
    {
        CountPainter cp;
        CHECK(RenderSvg(L"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10' width='20' height='20'>"
                        L"<circle cx='5' cy='5' r='5' fill='currentColor'/></svg>", 40, 40, 0xFF336699, cp));
        CHECK(cp.fills == 1 && cp.lastColor == 0xFF336699 && std::abs(cp.minX) < 0.01f && std::abs(cp.maxX - 40) < 0.01f);
        CountPainter cp2;
        RenderSvg(L"<svg viewBox='0 0 100 50'><defs><path id='p' d='M0 0h10v10z'/></defs><g transform='translate(50 10)'>"
                  L"<use href='#p' x='5'/></g><rect x='0' y='0' width='10' height='10' rx='2' stroke='red' fill='none'/>"
                  L"<path d='M10 40 a5 5 0 1 0 10 0'/></svg>", 100, 50, 0, cp2);
        CHECK(cp2.fills == 2 && cp2.strokes == 1);
        float w = 0, h = 0;
        CHECK(SvgNaturalSize(L"<svg viewBox='0 0 120 30'></svg>", w, h) && std::abs(w - 120) < 0.01f && std::abs(h - 30) < 0.01f);
        CHECK(SvgNaturalSize(L"<?xml version='1.0'?><!DOCTYPE svg><svg width='2em' viewBox='0 0 10 5'></svg>", w, h) && std::abs(w - 32) < 0.01f && std::abs(h - 16) < 0.01f);
        CountPainter cp3;
        RenderSvg(L"<svg viewBox='0 0 10 10'><style>.k{fill:#00ff00}</style><rect class='k' width='10' height='10'/></svg>", 10, 10, 0, cp3);
        CHECK(cp3.lastColor == 0xFF00FF00);
    }
    // Shadow DOM: bildirimsel shadow root, slotlar, kapsamli stil, :host, ::slotted
    {
        auto pl = Render(L"<style>p{color:#ff0000} .ic{color:#ff0000}</style>"
                         L"<div id=h><template shadowrootmode=open>"
                         L"<style>:host{display:block;background:#00ff00} .ic{color:#0000ff} ::slotted(span){color:#00ffff} p{color:#123456}</style>"
                         L"<p class=ic>ICERIK</p><slot name=bas></slot><slot>YEDEK</slot></template>"
                         L"<span slot=bas>BASLIK</span><span>GOVDE</span><b slot=yok>GIZLI</b></div><p>DIS</p>");
        const DisplayItem* ic = FindText(pl, L"ICERIK");
        const DisplayItem* bas = FindText(pl, L"BASLIK");
        const DisplayItem* gov = FindText(pl, L"GOVDE");
        const DisplayItem* dis = FindText(pl, L"DIS");
        CHECK(ic && bas && gov && dis);
        CHECK(!FindText(pl, L"GIZLI"));          // eslesen slotu olmayan light cocuk cizilmez
        CHECK(!FindText(pl, L"YEDEK"));          // atanmis dugum varsa yedek icerik gosterilmez
        CHECK(ic && ic->color == 0xFF0000FF);    // shadow stili; belge stili (.ic kirmizi) iceri girmez
        CHECK(bas && bas->color == 0xFF00FFFF);  // ::slotted(span)
        CHECK(dis && dis->color == 0xFFFF0000);  // shadow 'p' kurali disari sizmaz
        CHECK(FindFill(pl, 0xFF00FF00));         // :host arka plani
        CHECK(ic && bas && gov && ic->y < bas->y && bas->y <= gov->y);   // flat agac sirasi
        // yedek icerik: atanmis dugum yoksa
        auto pl2 = Render(L"<x-a><template shadowrootmode=open><slot>YEDEK</slot></template></x-a>");
        CHECK(FindText(pl2, L"YEDEK") != nullptr);
        // :host stili dis kurala yenilir, kalitim host'tan shadow'a gecer
        auto pl3 = Render(L"<style>x-b{color:#112233}</style><x-b><template shadowrootmode=open><style>:host{color:#445566}</style><i>MIRAS</i></template></x-b>");
        const DisplayItem* mi = FindText(pl3, L"MIRAS");
        CHECK(mi && mi->color == 0xFF112233);
        // :defined: tanimsiz ozel oge eslesmez
        auto pl4 = Render(L"<style>x-c:not(:defined){display:none}</style><x-c>TANIMSIZ</x-c><p>VAR</p>");
        CHECK(!FindText(pl4, L"TANIMSIZ") && FindText(pl4, L"VAR"));
    }
    {
        // box-shadow, degrade, text-overflow: ellipsis, -webkit-line-clamp
        auto pl = Render(L"<style>.c{width:200px;height:50px;background:#fff;border-radius:8px;box-shadow:0 4px 10px rgba(0,0,0,.5),inset 0 0 2px red}"
                         L".g{width:100px;height:40px;background:linear-gradient(to right,red,blue 80%)}"
                         L".t{width:100px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
                         L".k{width:120px;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}</style>"
                         L"<div class=c><p>ic</p></div><div class=g></div><div class=t>bir iki uc dort bes alti yedi sekiz dokuz on</div>"
                         L"<div class=k>alfa beta gama delta epsilon zeta eta teta iota kappa lambda mu nu ksi omikron pi ro sigma tau</div>");
        int shadows = 0, grads = 0;
        const DisplayItem* sh = nullptr;
        for (auto& i : pl.items) {
            if (i.kind == DisplayItem::Shadow) { shadows++; sh = &i; }
            if (i.kind == DisplayItem::Gradient) { grads++; CHECK(i.grad && i.grad->stops.size() == 2 && i.grad->angle == 90.f); CHECK(std::fabs(i.grad->stops[1].pos - 0.8f) < 1e-3f); }
        }
        CHECK(shadows == 1);   // inset cizilmez, alt <p> golgeyi miras almaz
        CHECK(sh && sh->y > sh->iy && sh->blur == 10 && sh->tint == 1 && (sh->color >> 24) >= 127 && (sh->color >> 24) <= 128);
        CHECK(grads == 1);
        // tek satir: ... ile biter, kutuya sigar
        int tLines = 0; bool ell = false;
        for (auto& i : pl.items) if (i.kind == DisplayItem::Text && (i.text.find(L"bir") == 0 || i.text.find(L"…") != std::wstring::npos)) {
            if (i.text.find(L"bir") == 0) tLines++;
            if (i.text.find(L"…") != std::wstring::npos && i.text.find(L"alfa") == std::wstring::npos && i.text.find(L"lambda") == std::wstring::npos) ell = true;
        }
        CHECK(ell);
        // iki satir sinirli: "tau" gorunmez, ikinci satir ... ile biter
        std::wstring all = AllText(pl);
        CHECK(all.find(L"tau") == std::wstring::npos && all.find(L"alfa") != std::wstring::npos);
        CHECK(all.find(L"dokuz on") == std::wstring::npos);
    }
    // Guvenlik regresyon testleri: URL gecis (path traversal), null byte, IsSameOrigin, SSRF / ozel ag
    {
        CHECK(ResolveUrl(L"file:///C:/a/b/index.html", L"%2e%2e%2f%2e%2e%5cgizli.txt") == L"file:///C:/gizli.txt");
        CHECK(ResolveUrl(L"file:///C:/a/b/index.html", L"..\\..\\gizli.txt") == L"file:///C:/gizli.txt");
        CHECK(!ParseUrl(L"file:///C:/a/test%00.html").ok);
        CHECK(!ParseUrl(L"https://ornek.com/yol%00/a").ok);
        CHECK(IsSameOrigin(ParseUrl(L"https://ornek.com:443/a"), ParseUrl(L"https://ornek.com/b")));
        CHECK(!IsSameOrigin(ParseUrl(L"https://ornek.com/a"), ParseUrl(L"http://ornek.com/a")));
        CHECK(!IsSameOrigin(ParseUrl(L"file:///C:/a.html"), ParseUrl(L"file:///C:/b.html")));
        CHECK(!IsSameOrigin(ParseUrl(L"about:home"), ParseUrl(L"about:blank")));
        CHECK(IsPrivateOrLocalHost(L"localhost"));
        CHECK(IsPrivateOrLocalHost(L"api.localhost"));
        CHECK(IsPrivateOrLocalHost(L"127.0.0.1"));
        CHECK(IsPrivateOrLocalHost(L"10.0.0.1"));
        CHECK(IsPrivateOrLocalHost(L"172.16.5.4"));
        CHECK(IsPrivateOrLocalHost(L"192.168.1.1"));
        CHECK(IsPrivateOrLocalHost(L"169.254.169.254"));
        CHECK(IsPrivateOrLocalHost(L"[::1]"));
        CHECK(IsPrivateOrLocalHost(L"[fe80::1]"));
        CHECK(IsPrivateOrLocalHost(L"2130706433"));
        CHECK(IsPrivateOrLocalHost(L"0177.0.0.1"));
        CHECK(IsPrivateOrLocalHost(L"0x7f.0.0.1"));
        CHECK(!IsPrivateOrLocalHost(L"ornek.com"));
        CHECK(!IsPrivateOrLocalHost(L"93.184.216.34"));
    }
    std::printf(fails ? "%d HATA\n" : "TUM TESTLER GECTI\n", fails);
    return fails ? 1 : 0;
}
