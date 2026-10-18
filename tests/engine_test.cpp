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
};

static PageLayout Render(const std::wstring& html, float w = 800) {
    auto doc = ParseHTML(html);
    StyleSheet sh;
    for (auto& s : doc->inlineStyles) sh.Add(s);
    sh.BuildIndex();
    auto st = ComputeStyles(*doc, sh);
    FakeMeasurer fm;
    FakeImages im;
    PageLayout pl;
    LayoutPage(*doc, st, w, fm, pl, &im);
    return pl;
}

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
        CHECK(!ParseSelectorList(L"a:hover", sl));
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
    std::printf(fails ? "%d HATA\n" : "TUM TESTLER GECTI\n", fails);
    return fails ? 1 : 0;
}
