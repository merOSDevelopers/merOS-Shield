// merJS + DOM entegrasyon testleri (platformdan bagimsiz)
#include <atomic>
#include <cstdio>
#include "../src/css.h"
#include "../src/dom.h"
#include "../src/js/js.h"
#include "../src/style.h"
#include "../src/url.h"
#include "../src/util.h"
#include "../src/video_decoder.h"
#include "../src/net.h"
#include "../src/i18n.h"
#include <windows.h>
#include <winhttp.h>
#include <unordered_map>
#include <fstream>
#include <iterator>
#include <mutex>
#include <functional>

using namespace mer;

struct TestHost : js::Host {
    std::vector<std::wstring> logs, scripts;
    std::wstring url = L"https://ornek.com/dizin/sayfa.html";
    std::unordered_map<std::wstring, std::vector<uint8_t>> videoFrames;
    void Log(int, const std::wstring& m) override { logs.push_back(m); }
    void Navigate(const std::wstring&) override {}
    std::wstring PageUrl() override { return url; }
    std::wstring Resolve(const std::wstring& r) override { return ResolveUrl(url, r); }
    void ViewportSize(double& w, double& h) override { w = 1200; h = 800; }
    bool NodeRect(const Node*, double& x, double& y, double& w, double& h) override { x = 10; y = 20; w = 100; h = 50; return true; }
    void SetUrlNoNav(const std::wstring& u) override { url = u; }
    void HistoryGo(int) override {}
    void RequestScript(Node*, const std::wstring& u) override { scripts.push_back(u); }
    void SubmitForm(Node*, Node*) override { logs.push_back(L"submit"); }
    void UpdateVideoFrame(const std::wstring& key, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgra) override {
        (void)w; (void)h;
        videoFrames[key] = bgra;
    }
    // Oynatici is parcacigindan gelen kareler
    std::mutex frameMu;
    std::unordered_map<std::wstring, int> frameCount;
    std::unordered_map<std::wstring, std::pair<int, int>> frameSize;
    std::function<void(int, int, std::shared_ptr<std::vector<uint8_t>>)> VideoSink(const Node* n) override {
        std::wstring key = VideoKey(n);
        return [this, key](int w, int h, std::shared_ptr<std::vector<uint8_t>>) {
            std::lock_guard<std::mutex> g(frameMu);
            frameCount[key]++;
            frameSize[key] = {w, h};
        };
    }
    int Frames(const std::wstring& key) { std::lock_guard<std::mutex> g(frameMu); return frameCount[key]; }
};

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::fflush(stdout); fails++; } } while (0)


struct Page {
    std::unique_ptr<Document> doc;
    TestHost host;
    js::Runtime rt;
    explicit Page(const std::wstring& html) {
        doc = ParseHTML(html);
        rt.AttachDocument(doc.get(), &host);
    }
    std::wstring Run(const std::wstring& code) {
        std::wstring out;
        rt.EvalForTest(code, out);
        return out;
    }
    // Zamanlayicilari ve medya olaylarini gercek zamanla calistir
    void Pump(int ms) {
        double end = rt.Now() + ms;
        while (rt.Now() < end) {
            rt.RunDueTimers(rt.Now());
            Sleep(10);
        }
    }
};

int main() {
    {
        Page p(L"<html><head><title>Eski</title></head><body><div id=k class='a b'><p>Merhaba</p><p class=x>Dünya</p></div><ul></ul></body></html>");
        CHECK(p.Run(L"document.getElementById('k').children.length") == L"2");
        CHECK(p.Run(L"document.querySelector('#k p.x').textContent") == L"Dünya");
        CHECK(p.Run(L"document.querySelectorAll('p').length") == L"2");
        CHECK(p.Run(L"document.title") == L"Eski");
        p.Run(L"document.title = 'Yeni'");
        CHECK(p.doc->title == L"Yeni");
        p.Run(L"var ul = document.querySelector('ul'); ['bir','iki','üç'].forEach(function(t){ var li = document.createElement('li'); li.textContent = t; li.className = 'oge'; ul.appendChild(li); });");
        CHECK(p.Run(L"document.querySelectorAll('li.oge').length") == L"3");
        CHECK(p.Run(L"ul.lastChild.textContent") == L"üç");
        p.Run(L"document.getElementById('k').innerHTML = '<b>kalın</b> <i>eğik</i>'");
        CHECK(p.Run(L"document.getElementById('k').innerHTML") == L"<b>kalın</b> <i>eğik</i>");
        CHECK(p.Run(L"document.querySelector('b').parentNode.id") == L"k");
        p.Run(L"var k = document.getElementById('k'); k.classList.add('c'); k.classList.remove('a'); k.classList.toggle('b');");
        CHECK(p.Run(L"k.className") == L"c");
        p.Run(L"k.style.backgroundColor = 'red'; k.style.marginTop = '4px'");
        CHECK(p.Run(L"k.getAttribute('style')") == L"background-color: red; margin-top: 4px;");
        CHECK(p.Run(L"k.style.backgroundColor") == L"red");
        p.Run(L"k.dataset.kullaniciId = '42'");
        CHECK(p.Run(L"k.getAttribute('data-kullanici-id')") == L"42");
        CHECK(p.Run(L"k.dataset.kullaniciId") == L"42");
        CHECK(p.Run(L"k.closest('body').tagName") == L"BODY");
        CHECK(p.Run(L"k.matches('div.c')") == L"true");
        CHECK(p.Run(L"document.body.contains(k)") == L"true");
        p.Run(L"k.remove()");
        CHECK(p.Run(L"document.body.contains(k) + ',' + (k.parentNode === null)") == L"false,true");
        CHECK(p.Run(L"k.textContent") == L"kalın eğik"); // ayrilmis dugum hala erisilebilir
        CHECK(p.rt.dirty);
    }
    // Olaylar
    {
        Page p(L"<body><button id=b onclick=\"window.inline = (window.inline||0) + 1\">Tikla</button><a id=a href='/x'>link</a></body>");
        p.Run(L"var hits = []; var b = document.getElementById('b');"
              L"b.addEventListener('click', function(e){ hits.push('btn:' + e.type + ':' + (this === b)); });"
              L"document.body.addEventListener('click', function(e){ hits.push('body:' + e.target.id); });"
              L"window.addEventListener('click', function(){ hits.push('win'); });"
              L"document.getElementById('a').onclick = function(e){ e.preventDefault(); hits.push('a'); };");
        Node* btn = nullptr;
        Node* link = nullptr;
        std::vector<Node*> st{p.doc->root.get()};
        while (!st.empty()) { Node* n = st.back(); st.pop_back(); if (n->idAttr == L"b") btn = n; if (n->idAttr == L"a") link = n; for (auto& k : n->kids) st.push_back(k.get()); }
        CHECK(btn && link);
        CHECK(p.rt.DispatchClick(btn));
        CHECK(p.Run(L"hits.join('|')") == L"btn:click:true|body:b|win");
        CHECK(p.Run(L"window.inline") == L"1");
        CHECK(!p.rt.DispatchClick(link)); // preventDefault
        p.Run(L"var once = 0; b.addEventListener('ping', function(){ once++; }, {once:true}); b.dispatchEvent(new Event('ping')); b.dispatchEvent(new Event('ping'));");
        CHECK(p.Run(L"once") == L"1");
        p.Run(L"var loaded = []; document.addEventListener('DOMContentLoaded', function(){ loaded.push('dcl'); }); window.onload = function(){ loaded.push('load'); };");
        p.rt.FireLoadEvents();
        CHECK(p.Run(L"loaded.join()") == L"dcl,load");
    }
    // Zamanlayicilar, location, storage, document.write, dinamik betik
    {
        Page p(L"<body><script>document.write('<p id=yazildi>merhaba</p>')</script><div id=d></div></body>");
        Node* script = nullptr;
        std::vector<Node*> st{p.doc->root.get()};
        while (!st.empty()) { Node* n = st.back(); st.pop_back(); if (n->name == L"script") script = n; for (auto& k : n->kids) st.push_back(k.get()); }
        p.rt.RunScript(TextContent(script), L"satir-ici", script);
        CHECK(p.Run(L"document.getElementById('yazildi').textContent") == L"merhaba");
        CHECK(p.Run(L"document.getElementById('yazildi').previousElementSibling.tagName") == L"SCRIPT");
        p.Run(L"var t = []; setTimeout(function(){ t.push('a'); }, 0); var iv = setInterval(function(){ t.push('i'); if (t.length > 3) clearInterval(iv); }, 1);");
        for (int i = 0; i < 20 && p.rt.NextTimerDue() >= 0; i++) p.rt.RunDueTimers(p.rt.Now() + 1000.0 * (i + 1));
        CHECK(p.Run(L"t.join('')") == L"aiii");
        CHECK(p.Run(L"location.hostname + location.pathname") == L"ornek.com/dizin/sayfa.html");
        p.Run(L"location.href = 'diger.html'");
        CHECK(p.rt.pendingNav == L"https://ornek.com/dizin/diger.html");
        p.Run(L"localStorage.setItem('tema', 'koyu'); localStorage.dil = 'tr'");
        CHECK(p.Run(L"localStorage.getItem('tema') + localStorage.getItem('dil') + localStorage.length") == L"koyutr2");
        p.Run(L"var s = document.createElement('script'); s.src = 'https://cdn.ornek.com/lib.js'; document.head ? document.head.appendChild(s) : document.body.appendChild(s);");
        CHECK(p.host.scripts.size() == 1 && p.host.scripts[0] == L"https://cdn.ornek.com/lib.js");
        p.Run(L"var s2 = document.createElement('script'); s2.textContent = 'window.dinamik = 7'; document.body.appendChild(s2);");
        CHECK(p.Run(L"window.dinamik") == L"7");
        CHECK(p.Run(L"new URL('../a?b=1#c', 'https://x.com/p/q').href") == L"https://x.com/a?b=1#c");
        CHECK(p.Run(L"new URLSearchParams('?q=t%C3%BCrk%C3%A7e&n=2').get('q')") == L"türkçe");
        CHECK(p.Run(L"document.getElementById('d').getBoundingClientRect().width") == L"100");
        CHECK(p.Run(L"document.body instanceof HTMLElement && document.body instanceof Node") == L"true");
        CHECK(p.Run(L"matchMedia('(max-width: 600px)').matches") == L"false");
    }
    // Form degerleri
    {
        Page p(L"<form id=f><input id=q name=q value=ilk><input type=checkbox id=c><select id=s><option value=a>A<option value=b selected>B</select></form>");
        CHECK(p.Run(L"document.getElementById('q').value") == L"ilk");
        p.Run(L"document.getElementById('q').value = 'yeni'");
        CHECK(p.Run(L"document.getElementById('q').value + document.getElementById('q').getAttribute('value')") == L"yeniilk");
        CHECK(p.Run(L"document.getElementById('s').value") == L"b");
        p.Run(L"document.getElementById('c').click()");
        CHECK(p.Run(L"document.getElementById('c').checked") == L"true");
        CHECK(p.Run(L"document.getElementById('f').elements.length") == L"3");
        CHECK(p.Run(L"document.getElementById('f').namedItem('q').value") == L"yeni");
        CHECK(p.Run(L"document.getElementById('f').namedItem('c').id") == L"c");
        CHECK(p.Run(L"document.getElementById('f').namedItem('yok') === null") == L"true");
        CHECK(p.Run(L"document.getElementById('f').elements.namedItem('q').value") == L"yeni");
        CHECK(p.Run(L"document.getElementById('f').elements.namedItem('s').id") == L"s");
        CHECK(p.Run(L"document.getElementById('f').elements.namedItem('yok') === null") == L"true");
        CHECK(p.Run(L"typeof HTMLFormElement.prototype.namedItem") == L"function");
    }
    // Cerez destegi (cookie getter/setter) ve iframe
    {
        Page p(L"<body><iframe id=ifr></iframe></body>");
        CHECK(p.Run(L"navigator.cookieEnabled") == L"true");
        CHECK(p.Run(L"document.cookie") == L"");
        p.Run(L"document.cookie = 'user=meruser; path=/'");
        p.Run(L"document.cookie = 'theme=dark; path=/'");
        CHECK(p.Run(L"document.cookie.indexOf('user=meruser') !== -1") == L"true");
        CHECK(p.Run(L"document.cookie.indexOf('theme=dark') !== -1") == L"true");
        // Guncelleme
        p.Run(L"document.cookie = 'theme=light'");
        CHECK(p.Run(L"document.cookie.indexOf('theme=light') !== -1") == L"true");
        CHECK(p.Run(L"document.cookie.indexOf('theme=dark') === -1") == L"true");
        // max-age=0 ile silme
        p.Run(L"document.cookie = 'theme=; max-age=0'");
        CHECK(p.Run(L"document.cookie.indexOf('theme') === -1") == L"true");
        CHECK(p.Run(L"document.cookie") == L"user=meruser");
        // expires gecmis tarih ile silme
        p.Run(L"document.cookie = 'user=; expires=Thu, 01 Jan 1970 00:00:00 GMT'");
        CHECK(p.Run(L"document.cookie") == L"");
        // iframe uzerinden cookie erisimi (Lenovo cökmesini onleyen contentDocument / contentWindow)
        CHECK(p.Run(L"typeof document.getElementById('ifr').contentDocument.cookie") == L"string");
        CHECK(p.Run(L"typeof document.getElementById('ifr').contentWindow.document.cookie") == L"string");
    }
    // Ag API'leri (fetch, Headers, Response, Request, XMLHttpRequest)
    {
        Page p(L"<body></body>");
        // fetch ve Headers
        CHECK(p.Run(L"typeof fetch") == L"function");
        CHECK(p.Run(L"typeof Headers") == L"function");
        CHECK(p.Run(L"typeof Response") == L"function");
        CHECK(p.Run(L"typeof Request") == L"function");
        p.Run(L"var h = new Headers({'Content-Type':'application/json'}); h.append('X-Custom','foo');");
        CHECK(p.Run(L"h.get('content-type')") == L"application/json");
        CHECK(p.Run(L"h.get('x-custom')") == L"foo");
        CHECK(p.Run(L"h.has('X-Custom')") == L"true");
        // Response text/json
        p.Run(L"var res = new Response('{\"mesaj\":\"selam\",\"sayi\":42}', {status: 200, statusText: 'OK'});");
        CHECK(p.Run(L"res.status") == L"200");
        CHECK(p.Run(L"res.ok") == L"true");
        p.Run(L"var txt = ''; res.text().then(function(t){ txt = t; });");
        CHECK(p.Run(L"txt.indexOf('selam') !== -1") == L"true");
        p.Run(L"var num = 0; new Response('{\"mesaj\":\"selam\",\"sayi\":42}').json().then(function(j){ num = j.sayi; });");
        CHECK(p.Run(L"num") == L"42");
        // fetch Promise doner ve gecersiz adreste reject eder
        p.Run(L"var err = false; fetch('http://localhost:59999/nonexistent').catch(function(e){ err = true; });");
        CHECK(p.Run(L"err") == L"true");
        // XMLHttpRequest
        CHECK(p.Run(L"typeof XMLHttpRequest") == L"function");
        CHECK(p.Run(L"XMLHttpRequest.UNSENT + ',' + XMLHttpRequest.DONE") == L"0,4");
        p.Run(L"var xhr = new XMLHttpRequest();");
        CHECK(p.Run(L"xhr.readyState") == L"0");
        CHECK(p.Run(L"typeof xhr.upload") == L"object");
        p.Run(L"var stateChanges = []; xhr.onreadystatechange = function(){ stateChanges.push(xhr.readyState); };");
        p.Run(L"xhr.open('GET', 'http://localhost:59999/test');");
        CHECK(p.Run(L"xhr.readyState") == L"1");
        CHECK(p.Run(L"stateChanges.join()") == L"1");
        p.Run(L"xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');");
        p.Run(L"var errFired = false; xhr.onerror = function(){ errFired = true; }; xhr.send();");
        CHECK(p.Run(L"xhr.readyState") == L"4");
        CHECK(p.Run(L"errFired") == L"true");
    }
    // Typed Arrays & ArrayBuffer & DataView (window.Uint8Array, window.Uint16Array, etc.)
    {
        Page p(L"<body></body>");
        // Varlik kontrolleri
        CHECK(p.Run(L"typeof ArrayBuffer") == L"function");
        CHECK(p.Run(L"typeof DataView") == L"function");
        CHECK(p.Run(L"typeof Uint8Array") == L"function");
        CHECK(p.Run(L"typeof Uint16Array") == L"function");
        CHECK(p.Run(L"typeof Uint8ClampedArray") == L"function");
        CHECK(p.Run(L"typeof Int32Array") == L"function");
        CHECK(p.Run(L"typeof Float32Array") == L"function");

        // ArrayBuffer
        p.Run(L"var buf = new ArrayBuffer(16);");
        CHECK(p.Run(L"buf.byteLength") == L"16");
        CHECK(p.Run(L"ArrayBuffer.isView(buf)") == L"false");

        // Uint8Array temel ozellikler ve indeksleme
        p.Run(L"var u8 = new Uint8Array(buf, 0, 8);");
        CHECK(p.Run(L"ArrayBuffer.isView(u8)") == L"true");
        CHECK(p.Run(L"u8.length") == L"8");
        CHECK(p.Run(L"u8.byteLength") == L"8");
        CHECK(p.Run(L"u8.byteOffset") == L"0");
        CHECK(p.Run(L"u8.buffer === buf") == L"true");
        p.Run(L"u8[0] = 42; u8[1] = 255; u8[2] = 300;"); // 300 -> 300 & 255 = 44
        CHECK(p.Run(L"u8[0]") == L"42");
        CHECK(p.Run(L"u8[1]") == L"255");
        CHECK(p.Run(L"u8[2]") == L"44");
        CHECK(p.Run(L"u8[8] === undefined") == L"true"); // sinir disi erisim undefined donmeli

        // Uint16Array ve ayni tamponu (ArrayBuffer) paylasma
        // u8[0] = 42 (0x2A), u8[1] = 1 (0x01) -> Uint16 kucuk soncul (little endian) = 0x012A = 298
        p.Run(L"u8[1] = 1; var u16 = new Uint16Array(buf, 0, 4);");
        CHECK(p.Run(L"u16.BYTES_PER_ELEMENT") == L"2");
        CHECK(p.Run(L"u16[0]") == L"298");

        // Uint8ClampedArray
        p.Run(L"var clamped = new Uint8ClampedArray(4); clamped[0] = -10; clamped[1] = 150; clamped[2] = 350;");
        CHECK(p.Run(L"clamped[0]") == L"0");
        CHECK(p.Run(L"clamped[1]") == L"150");
        CHECK(p.Run(L"clamped[2]") == L"255");

        // Array / iterable ile olusturma
        p.Run(L"var fromArr = new Uint8Array([10, 20, 30]);");
        CHECK(p.Run(L"fromArr.length") == L"3");
        CHECK(p.Run(L"fromArr.join('-')") == L"10-20-30");

        // subarray (tampon paylasir) vs slice (tampon kopyalar)
        p.Run(L"var sub = fromArr.subarray(1, 3); sub[0] = 99;");
        CHECK(p.Run(L"fromArr[1]") == L"99"); // subarray degisikligi ana diziyi etkiler
        p.Run(L"var sl = fromArr.slice(0, 2); sl[0] = 77;");
        CHECK(p.Run(L"fromArr[0]") == L"10"); // slice bagimsiz kopya

        // set() ve fill()
        p.Run(L"var target = new Uint8Array(5); target.set([1, 2, 3], 1); target.fill(9, 4, 5);");
        CHECK(p.Run(L"target.join()") == L"0,1,2,3,9");

        // Dizi metotlari: map, filter, reduce, indexOf, includes
        CHECK(p.Run(L"fromArr.indexOf(99)") == L"1");
        CHECK(p.Run(L"fromArr.includes(30)") == L"true");
        CHECK(p.Run(L"new Uint8Array([1, 2, 3]).map(function(x){ return x * 2; }).join()") == L"2,4,6");
        CHECK(p.Run(L"new Uint8Array([1, 2, 3, 4]).filter(function(x){ return x % 2 === 0; }).join()") == L"2,4");
        CHECK(p.Run(L"new Uint8Array([1, 2, 3, 4]).reduce(function(a, b){ return a + b; }, 0)") == L"10");

        // for..of iterasyonu
        p.Run(L"var sum = 0; for (var v of new Uint8Array([5, 10, 15])) sum += v;");
        CHECK(p.Run(L"sum") == L"30");

        // DataView
        p.Run(L"var dvBuf = new ArrayBuffer(8); var dv = new DataView(dvBuf);");
        p.Run(L"dv.setInt16(0, 1000, true); dv.setInt32(2, 70000, false);");
        CHECK(p.Run(L"dv.getInt16(0, true)") == L"1000");
        CHECK(p.Run(L"dv.getInt32(2, false)") == L"70000");

        // Response.arrayBuffer() entegrasyonu
        p.Run(L"var r = new Response('ABC'); var rBufLen = 0; r.arrayBuffer().then(function(b){ rBufLen = b.byteLength; });");
        CHECK(p.Run(L"rBufLen") == L"3");

        // TextEncoder testleri
        CHECK(p.Run(L"new TextEncoder().encoding") == L"utf-8");
        p.Run(L"var enc = new TextEncoder(); var u8 = enc.encode('Hello Dünya!');");
        CHECK(p.Run(L"u8 instanceof Uint8Array") == L"true");
        CHECK(p.Run(L"u8.length") == L"13"); // 'ü' 2 bayt UTF-8
        CHECK(p.Run(L"u8[0]") == L"72"); // 'H'
        CHECK(p.Run(L"u8[6]") == L"68"); // 'D'
        CHECK(p.Run(L"u8[7]") == L"195"); // 0xC3 (ü'nün ilk baytı)
        CHECK(p.Run(L"u8[8]") == L"188"); // 0xBC (ü'nün ikinci baytı)

        // encodeInto testleri
        p.Run(L"var dest = new Uint8Array(20); var res = enc.encodeInto('ABC', dest);");
        CHECK(p.Run(L"res.read") == L"3");
        CHECK(p.Run(L"res.written") == L"3");
        CHECK(p.Run(L"dest[0]") == L"65");

        // TextDecoder testleri
        CHECK(p.Run(L"new TextDecoder().encoding") == L"utf-8");
        CHECK(p.Run(L"new TextDecoder().fatal") == L"false");
        CHECK(p.Run(L"new TextDecoder().ignoreBOM") == L"false");
        CHECK(p.Run(L"new TextDecoder().decode(u8)") == L"Hello Dünya!");

        // Unicode 4-bayt Emoji testi (🌍 U+1F30E -> UTF-8 [240, 159, 140, 141])
        p.Run(L"var emojiU8 = enc.encode('🌍'); var emojiDec = new TextDecoder().decode(emojiU8);");
        CHECK(p.Run(L"emojiU8.length") == L"4");
        CHECK(p.Run(L"emojiDec") == L"🌍");

        // BOM Testi (EF BB BF)
        p.Run(L"var bomU8 = new Uint8Array([0xEF, 0xBB, 0xBF, 65, 66]);");
        CHECK(p.Run(L"new TextDecoder().decode(bomU8)") == L"AB"); // ignoreBOM: false -> BOM elenir
        CHECK(p.Run(L"new TextDecoder('utf-8', {ignoreBOM: true}).decode(bomU8).charCodeAt(0)") == L"65279"); // U+FEFF

        // TextDecoder fatal: true hata testi
        p.Run(L"var badU8 = new Uint8Array([0xFF]); var threw = false;"
              L"try { new TextDecoder('utf-8', {fatal: true}).decode(badU8); } catch(e) { threw = (e instanceof TypeError); }");
        CHECK(p.Run(L"threw") == L"true");

        // HTMLMediaElement / Video / Audio / YouTube Embed testleri
        p.Run(L"var vid = document.createElement('video');");
        CHECK(p.Run(L"vid.canPlayType('video/mp4; codecs=\"avc1.42E01E\"')") == L"probably");
        CHECK(p.Run(L"vid.canPlayType('video/webm')") == L"");          // yalnizca MP4 (H.264/AAC) cozulebiliyor
        CHECK(p.Run(L"vid.canPlayType('audio/mpeg')") == L"");
        CHECK(p.Run(L"vid.canPlayType('video/mp4')") == L"maybe");
        CHECK(p.Run(L"vid.canPlayType('invalid/type')") == L"");
        CHECK(p.Run(L"vid.paused") == L"true");

        // play() ve pause()
        p.Run(L"var playedPromise = vid.play();");
        CHECK(p.Run(L"playedPromise instanceof Promise") == L"true");
        CHECK(p.Run(L"vid.paused") == L"false");
        p.Run(L"vid.pause();");
        CHECK(p.Run(L"vid.paused") == L"true");

        // currentTime, duration, volume, muted, readyState, dimensions
        p.Run(L"vid.currentTime = 15.5; vid.volume = 0.5; vid.muted = true;");
        CHECK(p.Run(L"vid.currentTime") == L"15.5");
        CHECK(p.Run(L"vid.volume") == L"0.5");
        CHECK(p.Run(L"vid.muted") == L"true");
        CHECK(p.Run(L"vid.readyState") == L"0");      // kaynak yok: HAVE_NOTHING
        CHECK(p.Run(L"vid.videoWidth") == L"0");
        CHECK(p.Run(L"vid.videoHeight") == L"0");
        CHECK(p.Run(L"HTMLVideoElement.HAVE_ENOUGH_DATA") == L"4");
        CHECK(p.Run(L"HTMLMediaElement.NETWORK_IDLE") == L"1");

        // new Audio() kurucusu
        p.Run(L"var aud = new Audio('sarki.mp3');");
        CHECK(p.Run(L"aud.tagName") == L"AUDIO");
        CHECK(p.Run(L"aud.src.indexOf('sarki.mp3') !== -1") == L"true");
        CHECK(p.Run(L"aud.canPlayType('audio/mp4; codecs=\"mp4a.40.2\"')") == L"probably");

        // MediaSource
        CHECK(p.Run(L"typeof MediaSource !== 'undefined'") == L"true");
        CHECK(p.Run(L"MediaSource.isTypeSupported('video/mp4')") == L"true");

        // ---------------- MSE & Blob & URL.createObjectURL Testleri ----------------
        // 1. Blob constructor, size, type, text(), arrayBuffer(), slice()
        p.Run(L"var b1 = new Blob(['Hello ', 'World!'], { type: 'text/plain' });");
        CHECK(p.Run(L"b1.size") == L"12");
        CHECK(p.Run(L"b1.type") == L"text/plain");
        p.Run(L"var b1Text = ''; b1.text().then(function(t) { b1Text = t; });");
        CHECK(p.Run(L"b1Text") == L"Hello World!");
        p.Run(L"var b1BufLen = 0; b1.arrayBuffer().then(function(buf) { b1BufLen = buf.byteLength; });");
        CHECK(p.Run(L"b1BufLen") == L"12");

        p.Run(L"var bSlice = b1.slice(0, 5, 'text/plain');");
        CHECK(p.Run(L"bSlice.size") == L"5");
        p.Run(L"var bSliceText = ''; bSlice.text().then(function(t) { bSliceText = t; });");
        CHECK(p.Run(L"bSliceText") == L"Hello");

        // 2. URL.createObjectURL & revokeObjectURL & fetch(blob:)
        p.Run(L"var blobUrl = URL.createObjectURL(b1);");
        CHECK(p.Run(L"blobUrl.startsWith('blob:')") == L"true");
        CHECK(p.Run(L"window.webkitURL === URL") == L"true");

        p.Run(L"var fetchedBlobText = ''; fetch(blobUrl).then(function(r) { return r.text(); }).then(function(t) { fetchedBlobText = t; });");
        CHECK(p.Run(L"fetchedBlobText") == L"Hello World!");

        p.Run(L"URL.revokeObjectURL(blobUrl);");
        p.Run(L"var fetchRevokedFailed = false; fetch(blobUrl).catch(function(e) { fetchRevokedFailed = true; });");
        CHECK(p.Run(L"fetchRevokedFailed") == L"true");

        // 3. MediaSource constructor, readyState, addSourceBuffer, removeSourceBuffer, endOfStream
        CHECK(p.Run(L"window.WebKitMediaSource === MediaSource") == L"true");
        p.Run(L"var ms = new MediaSource();");
        CHECK(p.Run(L"ms.readyState") == L"closed");
        CHECK(p.Run(L"isNaN(ms.duration)") == L"true");
        CHECK(p.Run(L"ms.sourceBuffers.length") == L"0");
        CHECK(p.Run(L"MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.4d401f\"')") == L"true");
        CHECK(p.Run(L"MediaSource.isTypeSupported('audio/webm')") == L"false");
        CHECK(p.Run(L"MediaSource.isTypeSupported('video/unsupported')") == L"false");

        // 4. Baglama: video.src = URL.createObjectURL(ms) -> sourceopen tetiklenmeli
        p.Run(L"var msUrl = URL.createObjectURL(ms); var sourceOpenFired = false;");
        p.Run(L"ms.addEventListener('sourceopen', function() { sourceOpenFired = true; });");
        p.Run(L"var testVid = document.createElement('video'); testVid.src = msUrl;");
        p.Pump(50);
        CHECK(p.Run(L"sourceOpenFired") == L"true");
        CHECK(p.Run(L"ms.readyState") == L"open");

        // 5. addSourceBuffer & appendBuffer (YouTube DASH chunk simulasyonu)
        p.Run(L"var sb = ms.addSourceBuffer('video/mp4; codecs=\"avc1.4d401f\"');");
        CHECK(p.Run(L"ms.sourceBuffers.length") == L"1");
        CHECK(p.Run(L"ms.sourceBuffers[0] === sb") == L"true");
        CHECK(p.Run(L"sb.updating") == L"false");

        p.Run(L"var updateStartFired = false; var updateFired = false; var updateEndFired = false;");
        p.Run(L"sb.addEventListener('updatestart', function() { updateStartFired = true; });");
        p.Run(L"sb.addEventListener('update', function() { updateFired = true; });");
        p.Run(L"sb.addEventListener('updateend', function() { updateEndFired = true; });");

        // Simule YouTube DASH chunk (Uint8Array)
        p.Run(L"var chunk = new Uint8Array([0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70]);");
        p.Run(L"sb.appendBuffer(chunk);");
        CHECK(p.Run(L"sb.updating") == L"true");      // olaylar gorev olarak gelir
        p.Pump(50);
        CHECK(p.Run(L"updateStartFired") == L"true");
        CHECK(p.Run(L"updateFired") == L"true");
        CHECK(p.Run(L"updateEndFired") == L"true");
        CHECK(p.Run(L"sb.updating") == L"false");
        CHECK(p.Run(L"sb.buffered.length") == L"0");  // yarim ftyp kutusu: henuz ornek yok
        CHECK(p.Run(L"testVid.readyState") == L"0");

        // 6. endOfStream
        p.Run(L"var sourceEndedFired = false; ms.onsourceended = function() { sourceEndedFired = true; };");
        p.Run(L"ms.endOfStream();");
        CHECK(p.Run(L"ms.readyState") == L"ended");
        p.Pump(30);
        CHECK(p.Run(L"sourceEndedFired") == L"true");

        // 7. removeSourceBuffer
        p.Run(L"ms.removeSourceBuffer(sb);");
        CHECK(p.Run(L"ms.sourceBuffers.length") == L"0");

        // document.open, document.close ve window.writeEmbed (YouTube embed fallback fix)
        p.Run(L"document.open();");
        p.Run(L"window.writeEmbed('<div id=\"yt-player\">YouTube Embed Content</div>');");
        p.Run(L"document.close();");
        CHECK(p.Run(L"document.getElementById('yt-player').textContent") == L"YouTube Embed Content");
    }
    // Hata dayanikliligi: hatali betik digerlerini durdurmaz
    {
        Page p(L"<body></body>");
        CHECK(!p.rt.RunScript(L"this is not js", L"bozuk"));
        CHECK(p.rt.RunScript(L"window.ok = 1", L"saglam"));
        CHECK(p.Run(L"ok") == L"1");
        CHECK(!p.rt.RunScript(L"while(true){}", L"sonsuz"));
        CHECK(p.Run(L"1+1") == L"2");
        CHECK(!p.host.logs.empty());
    }
    // ---------------- ES Modülleri (Item 6) Testleri ----------------
    {
        Page p(L"<html><head></head><body><div id=app></div></body></html>");

        // 1. RunModule ile temel export / import mantığı
        std::wstring mathMod = 
            L"export function add(a, b) { return a + b; }\n"
            L"export const PI = 3.14159;\n"
            L"export default function greet(name) { return 'Merhaba ' + name; }\n";
        bool okMath = p.rt.RunModule(mathMod, L"https://ornek.com/math.js");
        CHECK(okMath == true);
        auto modRecord = p.rt.modules[L"https://ornek.com/math.js"];
        CHECK(modRecord != nullptr);
        CHECK(modRecord->evaluated == true);
        CHECK(p.rt.ToStr(p.rt.Get(js::Value::O(modRecord->namespaceObj), L"PI")) == L"3.14159");

        // 2. Import Meta URL
        std::wstring metaMod = L"window.moduleUrl = import.meta.url;\n";
        p.rt.RunModule(metaMod, L"https://ornek.com/components/widget.js");
        CHECK(p.Run(L"window.moduleUrl") == L"https://ornek.com/components/widget.js");

        // 3. Dinamik import() Promise desteği
        p.Run(L"var dynamicAddResult = 0; var defaultGreetResult = '';\n"
              L"import('https://ornek.com/math.js').then(function(m) {\n"
              L"    dynamicAddResult = m.add(10, 25);\n"
              L"    defaultGreetResult = m.default('Dünya');\n"
              L"});");
        CHECK(p.Run(L"dynamicAddResult") == L"35");
        CHECK(p.Run(L"defaultGreetResult") == L"Merhaba Dünya");

        // 4. <script type="module"> etiketinin DOM'a eklenmesiyle dinamik çalışması
        p.Run(L"var modScript = document.createElement('script');\n"
              L"modScript.type = 'module';\n"
              L"modScript.textContent = 'window.moduleExecuted = true; window.moduleScore = 100;';\n"
              L"document.body.appendChild(modScript);\n");
        CHECK(p.Run(L"window.moduleExecuted") == L"true");
        CHECK(p.Run(L"window.moduleScore") == L"100");

        // 5. Named export listesi ve rename: export { x as myX }
        std::wstring renameMod =
            L"const secret = 42;\n"
            L"export { secret as answer };\n";
        p.rt.RunModule(renameMod, L"https://ornek.com/rename.js");
        auto renameRecord = p.rt.modules[L"https://ornek.com/rename.js"];
        CHECK(renameRecord != nullptr);
        CHECK(p.rt.ToStr(p.rt.Get(js::Value::O(renameRecord->namespaceObj), L"answer")) == L"42");
    }

    // ---------------- Video Codec (WMF & Direct2D Entegrasyonu) Testleri ----------------
    {
        // 1. WMF VideoDecoder alt sistemi başlatma
        bool wmfOk = VideoDecoder::Startup();
        CHECK(wmfOk == true);

        // 2. Player Preview Frame oluşturma (Kırmızı Play butonu, oynatma çubuğu)
        VideoFrame preview = VideoDecoder::CreatePlayerPreview(640, 360, 15.0, 60.0);
        CHECK(preview.valid == true);
        CHECK(preview.width == 640);
        CHECK(preview.height == 360);
        CHECK(preview.bgra.size() == 640 * 360 * 4);

        // Arka plan koyu gri kontrolü
        CHECK(preview.bgra[0] == 18 && preview.bgra[1] == 18 && preview.bgra[2] == 18);

        // 3. Gercek oynatma: ilerlemeli MP4 (H.264 + AAC), Blob URL uzerinden
        std::vector<uint8_t> clip;
        for (const char* path : {"tests/js/media/small.mp4", "../tests/js/media/small.mp4", "../src/merOSShield/tests/js/media/small.mp4", "/home/claude/src/merOSShield/tests/js/media/small.mp4"}) {
            std::ifstream f(path, std::ios::binary);
            if (f) { clip.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); break; }
        }
        if (clip.empty()) {
            std::printf("UYARI: tests/js/media/small.mp4 bulunamadi, video oynatma testi atlandi\n");
        } else {
            Page p(L"<body><video id='vid' width='320' height='180'></video></body>");
            p.rt.DefineData(p.rt.global, L"clipBytes", js::Value::O(p.rt.NewArrayBufferFrom(clip.data(), clip.size())));
            p.Run(L"var v = document.getElementById('vid'); var evs = [];"
                  L"['loadedmetadata','canplay','playing','seeked','ended'].forEach(function(t){ v.addEventListener(t, function(){ evs.push(t); }); });");
            CHECK(p.Run(L"v.paused") == L"true");
            p.Run(L"v.muted = true; v.src = URL.createObjectURL(new Blob([clipBytes], {type: 'video/mp4'})); v.play();");
            CHECK(p.Run(L"v.paused") == L"false");
            p.Pump(800); for (int w = 0; w < 20 && p.Run(L"v.currentTime > 0.3") != L"true"; w++) p.Pump(100);
            CHECK(p.Run(L"evs.slice(0,3).join(',')") == L"loadedmetadata,canplay,playing");
            CHECK(p.Run(L"v.videoWidth + 'x' + v.videoHeight") == L"160x90");
            CHECK(p.Run(L"v.currentTime > 0.3") == L"true");
            CHECK(p.Run(L"Math.abs(v.duration - 2.04) < 0.06") == L"true");
            Node* vn = p.doc->root.get();
            Node* found = nullptr;
            std::vector<Node*> st{vn};
            while (!st.empty() && !found) { Node* n = st.back(); st.pop_back(); if (n->name == L"video") found = n; for (auto& k : n->kids) st.push_back(k.get()); }
            CHECK(found && p.host.Frames(VideoKey(found)) > 5);
            p.Run(L"v.pause(); v.currentTime = 1.5;");
            p.Pump(300);
            CHECK(p.Run(L"evs.indexOf('seeked') >= 0") == L"true");
            CHECK(p.Run(L"Math.abs(v.currentTime - 1.5) < 0.05") == L"true");
            p.Run(L"v.play();");
            // kalan ~0.54 sn; soguk baslangicta kod cozucu yeniden acilisi gecikebilir: 3 sn'ye kadar bekle
            for (int w = 0; w < 30 && p.Run(L"v.ended") != L"true"; w++) p.Pump(100);
            CHECK(p.Run(L"v.ended") == L"true");
            CHECK(p.Run(L"evs[evs.length-1]") == L"ended");
            // Response.body akisi (Streams)
            p.Run(L"var sz = 0; fetch(URL.createObjectURL(new Blob([clipBytes]))).then(function(r){ return r.body.getReader().read(); }).then(function(x){ sz = x.value.length; });");
            p.Pump(30);
            CHECK(p.Run(L"sz") == std::to_wstring(clip.size()));
        }

        VideoDecoder::Shutdown();
    }
    {
        // ---------------- Intl API Testleri ----------------
        Page p(L"<html><body></body></html>");

        // 1. Intl nesnesi ve getCanonicalLocales
        CHECK(p.Run(L"typeof Intl") == L"object");
        CHECK(p.Run(L"Object.prototype.toString.call(Intl)") == L"[object Intl]");
        CHECK(p.Run(L"Intl.getCanonicalLocales('en-US')[0]") == L"en-US");
        CHECK(p.Run(L"Intl.getCanonicalLocales(['tr_tr', 'EN-us']).join(',')") == L"tr-TR,en-US");

        // 2. Intl.DateTimeFormat
        p.Run(L"var d = new Date(Date.UTC(2026, 9, 18, 12, 0, 0));");
        p.Run(L"var dtf = new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' });");
        CHECK(p.Run(L"dtf.resolvedOptions().calendar") == L"gregory");
        CHECK(p.Run(L"dtf.resolvedOptions().numberingSystem") == L"latn");
        CHECK(p.Run(L"dtf.format(d).indexOf('2026') !== -1") == L"true");
        CHECK(p.Run(L"dtf.formatToParts(d).length > 0") == L"true");

        // Date.prototype.toLocale* delegasyonu
        CHECK(p.Run(L"d.toLocaleDateString('en-US', { timeZone: 'UTC' }).indexOf('2026') !== -1") == L"true");

        // 3. Intl.NumberFormat
        p.Run(L"var nf = new Intl.NumberFormat('en-US');");
        CHECK(p.Run(L"nf.format(12345.67).indexOf('12,345') !== -1") == L"true");
        p.Run(L"var nfTry = new Intl.NumberFormat('tr-TR', { style: 'currency', currency: 'TRY' });");
        CHECK(p.Run(L"nfTry.format(500).indexOf('₺') !== -1") == L"true");
        CHECK(p.Run(L"(1234.5).toLocaleString('en-US').indexOf('1,234') !== -1") == L"true");

        // 4. Intl.Collator
        p.Run(L"var col = new Intl.Collator('en');");
        CHECK(p.Run(L"col.compare('a', 'b')") == L"-1");
        CHECK(p.Run(L"col.compare('b', 'a')") == L"1");
        CHECK(p.Run(L"col.compare('a', 'a')") == L"0");
        CHECK(p.Run(L"'apple'.localeCompare('banana', 'en')") == L"-1");

        // 5. Intl.PluralRules
        p.Run(L"var prEn = new Intl.PluralRules('en');");
        CHECK(p.Run(L"prEn.select(1)") == L"one");
        CHECK(p.Run(L"prEn.select(5)") == L"other");
        p.Run(L"var prTr = new Intl.PluralRules('tr');");
        CHECK(p.Run(L"prTr.select(5)") == L"other");

        // 6. Intl.RelativeTimeFormat
        p.Run(L"var rtf = new Intl.RelativeTimeFormat('en');");
        CHECK(p.Run(L"rtf.format(-1, 'day')") == L"1 day ago");
        p.Run(L"var rtfAuto = new Intl.RelativeTimeFormat('en', { numeric: 'auto' });");
        CHECK(p.Run(L"rtfAuto.format(-1, 'day')") == L"yesterday");

        // 7. Intl.ListFormat
        p.Run(L"var lf = new Intl.ListFormat('en');");
        CHECK(p.Run(L"lf.format(['Alpha', 'Beta'])") == L"Alpha and Beta");

        // 8. Intl.DisplayNames
        p.Run(L"var dn = new Intl.DisplayNames('en', { type: 'language' });");
        CHECK(p.Run(L"dn.of('en')") == L"English");

        // ---------------- RegEx Unicode & Property Escapes ----------------
        CHECK(p.Run(L"/\\u0000/.test('\\u0000')") == L"true");
        CHECK(p.Run(L"/\\0/.test('\\0')") == L"true");
        CHECK(p.Run(L"/\\p{Letter}+/u.test('merOSShield')") == L"true");
        CHECK(p.Run(L"/\\p{L}+/u.test('Türkçe')") == L"true");
        CHECK(p.Run(L"/\\p{Number}+/u.test('2026')") == L"true");
        CHECK(p.Run(L"/\\p{ASCII}+/u.test('Hello')") == L"true");

        // ---------------- i18n & English Default Language Support ----------------
        // 1. Varsayılan dilin İngilizce (en-US) olduğunu doğrula
        CHECK(p.Run(L"navigator.language") == L"en-US");
        CHECK(p.Run(L"navigator.languages.length") == L"4");
        CHECK(p.Run(L"navigator.languages[0]") == L"en-US");
        CHECK(p.Run(L"navigator.languages[1]") == L"en");
        CHECK(p.Run(L"navigator.languages[2]") == L"tr-TR");
        CHECK(p.Run(L"navigator.languages[3]") == L"tr");

        CHECK(std::wstring(i18n::GetLanguageCode()) == L"en-US");
        CHECK(std::wstring(i18n::GetLanguageCodeShort()) == L"en");
        CHECK(i18n::GetAcceptLanguageHeader().find(L"en-US") == 17); // Accept-Language: en-US...

        // Varsayılan ağ hata mesajının İngilizce olduğunu doğrula
        CHECK(NetErrorText(ERROR_WINHTTP_CANNOT_CONNECT) == L"Could not connect to the server.");
        CHECK(NetErrorText(ERROR_WINHTTP_TIMEOUT) == L"The server did not respond in time.");

        // 2. Türkçe diline geçiş ve doğrulama
        i18n::SetCurrentLanguage(i18n::Language::Turkish);
        CHECK(std::wstring(i18n::GetLanguageCode()) == L"tr-TR");
        CHECK(std::wstring(i18n::GetLanguageCodeShort()) == L"tr");
        CHECK(NetErrorText(ERROR_WINHTTP_CANNOT_CONNECT) == L"Sunucuya ba\u011Flan\u0131lamad\u0131.");
        {
            Page pTr(L"<body></body>");
            CHECK(pTr.Run(L"navigator.language") == L"tr-TR");
            CHECK(pTr.Run(L"navigator.languages[0]") == L"tr-TR");
            CHECK(pTr.Run(L"navigator.languages[2]") == L"en-US");
        }

        // 3. Tekrar varsayılan İngilizceye dön ve doğrula
        i18n::SetCurrentLanguage(i18n::Language::English);
        CHECK(std::wstring(i18n::GetLanguageCode()) == L"en-US");
        CHECK(NetErrorText(ERROR_WINHTTP_CANNOT_CONNECT) == L"Could not connect to the server.");
        // merJS: ES2023 Array non-mutating methods
        {
            Page p(L"<body></body>");
            CHECK(p.Run(L"let a = [3, 1, 2]; let r = a.toReversed(); a.join(',') + '|' + r.join(',')") == L"3,1,2|2,1,3");
            CHECK(p.Run(L"let a = [3, 1, 2]; let s = a.toSorted(); a.join(',') + '|' + s.join(',')") == L"3,1,2|1,2,3");
            CHECK(p.Run(L"let a = [1, 2, 3, 4]; let sp = a.toSpliced(1, 2, 99); a.join(',') + '|' + sp.join(',')") == L"1,2,3,4|1,99,4");
            CHECK(p.Run(L"let a = [10, 20, 30]; let w = a.with(1, 99); a.join(',') + '|' + w.join(',')") == L"10,20,30|10,99,30");
        }

        // merJS: structuredClone
        {
            Page p(L"<body></body>");
            CHECK(p.Run(L"let o = { a: 1, b: [2, 3], c: { d: 'test' } }; let cl = structuredClone(o); cl.b.push(4); cl.c.d = 'mod'; o.b.length + '|' + o.c.d + '|' + cl.b.length + '|' + cl.c.d") == L"2|test|3|mod");
        }

        // merJS: Promise.withResolvers
        {
            Page p(L"<body></body>");
            p.Run(L"var { promise, resolve, reject } = Promise.withResolvers(); var res = 0; promise.then(function(v) { res = v; }); resolve(42);");
            CHECK(p.Run(L"res") == L"42");
        }

        // DOM: Element.hasAttributes
        {
            Page p(L"<body><div></div></body>");
            CHECK(p.Run(L"let el = document.createElement('div'); let h1 = el.hasAttributes(); el.setAttribute('id', 'x'); let h2 = el.hasAttributes(); h1 + '|' + h2") == L"false|true");
        }

        // Web APIs: window.crypto (randomUUID & getRandomValues)
        {
            Page p(L"<body></body>");
            CHECK(p.Run(L"let u = crypto.randomUUID(); u.length === 36 && u[14] === '4'") == L"true");
            CHECK(p.Run(L"let arr = new Uint8Array(8); crypto.getRandomValues(arr); arr.length === 8") == L"true");
        }

        // Web APIs: navigator.clipboard (writeText & readText)
        {
            Page p(L"<body></body>");
            p.Run(L"var ok = false; navigator.clipboard.writeText('mer-shield-test'); navigator.clipboard.readText().then(function(t) { ok = (t === 'mer-shield-test'); });");
            CHECK(p.Run(L"ok") == L"true");
        }

        // DOM: DocumentFragment insertion unpacking
        {
            Page p(L"<body><div id=p></div></body>");
            CHECK(p.Run(L"let d = document.getElementById('p'); let frag = document.createDocumentFragment(); let c1 = document.createElement('span'); let c2 = document.createElement('b'); frag.appendChild(c1); frag.appendChild(c2); d.appendChild(frag); d.children.length + '|' + frag.childNodes.length") == L"2|0");
        }

        // merJS: gercek async/await (zamanlayici bekleyen await, sira, DOM ile)
        {
            Page p(L"<body><ul id=liste></ul></body>");
            p.Run(L"var sira = []; var bitti = '';"
                  L"const uyu = (ms, v) => new Promise(r => setTimeout(() => r(v), ms));"
                  L"async function yukle() {"
                  L"  sira.push('bas');"
                  L"  for (const ad of ['a', 'b', 'c']) {"
                  L"    const li = document.createElement('li'); li.textContent = await uyu(5, ad);"
                  L"    document.getElementById('liste').appendChild(li);"
                  L"  }"
                  L"  try { await Promise.reject(new Error('ag hatasi')); } catch (e) { sira.push(e.message); }"
                  L"  return document.querySelectorAll('#liste li').length;"
                  L"}"
                  L"yukle().then(n => { bitti = 'n=' + n; }); sira.push('senkron');");
            CHECK(p.Run(L"sira.join(',') + '|' + bitti") == L"bas,senkron|");
            for (int i = 0; i < 50 && p.rt.NextTimerDue() >= 0; i++) p.rt.RunDueTimers(p.rt.NextTimerDue());
            CHECK(p.Run(L"sira.join(',') + '|' + bitti") == L"bas,senkron,ag hatasi|n=3");
            CHECK(p.Run(L"document.getElementById('liste').textContent") == L"abc");
        }

        // Custom Elements: yukseltme, yasam dongusu geri cagirmalari, siralama
        {
            Page p(L"<body><x-sayac id=s1 deger=3></x-sayac><div id=kap></div></body>");
            p.Run(L"var log = [];"
                  L"class XSayac extends HTMLElement {"
                  L"  static get observedAttributes() { return ['deger']; }"
                  L"  constructor() { super(); this.tik = 0; log.push('ctor'); }"
                  L"  connectedCallback() { log.push('bagli:' + this.id); }"
                  L"  disconnectedCallback() { log.push('ayrildi:' + this.id); }"
                  L"  attributeChangedCallback(n, o, v) { log.push('attr:' + n + ':' + o + '>' + v); }"
                  L"  artir() { return ++this.tik; }"
                  L"}"
                  L"log.push('once');"
                  L"customElements.define('x-sayac', XSayac);"
                  L"log.push('sonra');");
            CHECK(p.Run(L"log.join(',')") == L"once,ctor,attr:deger:null>3,bagli:s1,sonra");
            CHECK(p.Run(L"var s = document.getElementById('s1'); (s instanceof XSayac) + ':' + (s instanceof HTMLElement) + ':' + s.artir() + s.artir()") == L"true:true:12");
            p.Run(L"log = []; s.setAttribute('deger', '9'); s.setAttribute('baska', '1'); s.removeAttribute('deger');");
            CHECK(p.Run(L"log.join(',')") == L"attr:deger:3>9,attr:deger:9>null");
            p.Run(L"log = []; var y = document.createElement('x-sayac'); y.id = 'y'; log.push('olustu:' + (y instanceof XSayac));"
                  L"document.getElementById('kap').appendChild(y); y.remove();");
            CHECK(p.Run(L"log.join(',')") == L"ctor,olustu:true,bagli:y,ayrildi:y");
            p.Run(L"log = []; document.getElementById('kap').innerHTML = '<x-sayac id=z deger=1></x-sayac>';");
            CHECK(p.Run(L"log.join(',')") == L"ctor,attr:deger:null>1,bagli:z");
            p.Run(L"log = []; var d = new XSayac(); log.push(d.tagName);");
            CHECK(p.Run(L"log.join(',')") == L"ctor,X-SAYAC");
            CHECK(p.Run(L"customElements.get('x-sayac') === XSayac") == L"true");
            CHECK(p.Run(L"customElements.getName(XSayac)") == L"x-sayac");
            CHECK(p.Run(L"var hata = ''; try { customElements.define('x-sayac', class extends HTMLElement {}); } catch (e) { hata = e.message; } hata.indexOf('already') >= 0") == L"true");
            CHECK(p.Run(L"var h2 = ''; try { customElements.define('gecersiz', class extends HTMLElement {}); } catch (e) { h2 = e.name; } h2") == L"SyntaxError");
            CHECK(p.Run(L"var h3 = ''; try { new HTMLElement(); } catch (e) { h3 = e.name; } h3") == L"TypeError");
            // whenDefined, sonradan tanimlama
            p.Run(L"var wd = ''; customElements.whenDefined('x-gec').then(c => { wd = c.name; }); document.body.appendChild(document.createElement('x-gec'));");
            CHECK(p.Run(L"wd") == L"");
            p.Run(L"class XGec extends HTMLElement { connectedCallback() { this.textContent = 'hazir'; } } customElements.define('x-gec', XGec);");
            CHECK(p.Run(L"wd + ':' + document.querySelector('x-gec').textContent") == L"XGec:hazir");
        }

        // Shadow DOM: attachShadow, kapsam, slotlar, olaylar
        {
            Page p(L"<body><kart-ogesi id=k><span slot=baslik>Merhaba</span><p>Govde</p></kart-ogesi></body>");
            p.Run(L"class Kart extends HTMLElement {"
                  L"  constructor() { super(); const r = this.attachShadow({mode: 'open'});"
                  L"    r.innerHTML = '<style>:host{display:block}</style><h2><slot name=baslik>Varsayilan</slot></h2><div id=ic><slot></slot></div><button id=b>Tikla</button>'; }"
                  L"}"
                  L"customElements.define('kart-ogesi', Kart);"
                  L"var k = document.getElementById('k'); var r = k.shadowRoot;");
            CHECK(p.Run(L"r.nodeType + ':' + r.mode + ':' + (r.host === k) + ':' + (r instanceof ShadowRoot)") == L"11:open:true:true");
            CHECK(p.Run(L"(document.querySelector('#ic') === null) + ':' + (r.querySelector('#ic') !== null) + ':' + (r.getElementById('b').textContent)") == L"true:true:Tikla");
            CHECK(p.Run(L"r.getElementById('ic').getRootNode() === r") == L"true");
            CHECK(p.Run(L"r.getElementById('ic').getRootNode({composed: true}) === document") == L"true");
            CHECK(p.Run(L"r.getElementById('ic').isConnected + ':' + k.contains(r.getElementById('ic'))") == L"true:false");
            CHECK(p.Run(L"var sl = r.querySelector('slot[name=baslik]'); sl.assignedNodes().length + ':' + sl.assignedElements()[0].textContent") == L"1:Merhaba");
            CHECK(p.Run(L"var ds = r.querySelector('#ic slot'); ds.assignedElements().map(e => e.tagName).join()") == L"P");
            CHECK(p.Run(L"(k.querySelector('p').assignedSlot === ds) + ':' + k.querySelector('span').slot") == L"true:baslik");
            // Olaylar: shadow icinden tiklama host'a hedeflenir; composed:false olay disari cikmaz
            p.Run(L"var ev = []; k.addEventListener('click', e => { const cp = e.composedPath(); ev.push('host:' + e.target.id + ':' + (cp[1] === r) + (cp[2] === k) + (cp[cp.length - 1] === window)); });"
                  L"r.getElementById('b').addEventListener('click', e => ev.push('ic:' + e.target.id));"
                  L"document.body.addEventListener('ozel', e => ev.push('disari'));"
                  L"r.getElementById('b').dispatchEvent(new CustomEvent('ozel', {bubbles: true}));"
                  L"r.getElementById('b').dispatchEvent(new CustomEvent('ozel', {bubbles: true, composed: true}));");
            Node* btn = nullptr;
            {
                std::vector<Node*> st{p.doc->root.get()};
                while (!st.empty()) { Node* n = st.back(); st.pop_back(); if (n->idAttr == L"b") btn = n; for (auto& c : n->kids) st.push_back(c.get()); if (n->shadow) st.push_back(n->shadow.get()); }
            }
            CHECK(btn != nullptr);
            if (btn) p.rt.DispatchClick(btn);
            CHECK(p.Run(L"ev.join(',')") == L"disari,ic:b,host:k:truetruetrue");
            // kapali shadow root
            CHECK(p.Run(L"var c = document.createElement('div'); var cr = c.attachShadow({mode:'closed'}); (c.shadowRoot === null) + ':' + cr.mode") == L"true:closed");
            CHECK(p.Run(L"var e2 = ''; try { k.attachShadow({mode:'open'}); } catch (e) { e2 = 'NotSupported'; } e2") == L"NotSupported");
            // template.content + cloneNode
            p.Run(L"var t = document.createElement('template'); t.innerHTML = '<b>sablon</b><i>2</i>';"
                  L"var h = document.createElement('div'); h.attachShadow({mode:'open'}).appendChild(t.content.cloneNode(true)); document.body.appendChild(h);");
            CHECK(p.Run(L"h.shadowRoot.childNodes.length + ':' + h.shadowRoot.querySelector('b').textContent + ':' + t.content.childNodes.length") == L"2:sablon:2");
            // slotchange
            p.Run(L"var scn = 0; ds.addEventListener('slotchange', () => scn++); var yeni = document.createElement('em'); k.appendChild(yeni); k.appendChild(document.createElement('em'));");
            CHECK(p.Run(L"scn") == L"1");
        }

        // Bildirimsel shadow DOM (ayristirici) + Lit benzeri bilesen
        {
            Page p(L"<body><x-dek><template shadowrootmode=open><p id=dp>dekl</p><slot></slot></template><b>L</b></x-dek></body>");
            CHECK(p.Run(L"document.querySelector('x-dek').shadowRoot.getElementById('dp').textContent") == L"dekl");
            CHECK(p.Run(L"document.querySelectorAll('template').length") == L"0");
            p.Run(L"class LitBenzeri extends HTMLElement {"
                  L"  static properties = { ad: {} };"
                  L"  #ic = 0;"
                  L"  constructor() { super(); this.ad = 'dunya'; this.renderRoot = this.attachShadow({mode: 'open'}); }"
                  L"  connectedCallback() { this.guncelle(); }"
                  L"  async guncelle() { await null; this.#ic++; this.renderRoot.innerHTML = `<style>p{margin:0}</style><p>Merhaba ${this.ad} #${this.#ic}</p>`; }"
                  L"}"
                  L"customElements.define('lit-benzeri', LitBenzeri);"
                  L"document.body.insertAdjacentHTML('beforeend', '<lit-benzeri></lit-benzeri>');");
            CHECK(p.Run(L"document.querySelector('lit-benzeri').shadowRoot.querySelector('p').textContent") == L"Merhaba dunya #1");
        }

        // MutationObserver + ek DOM API'leri
        {
            Page p(L"<body><div id=m><span>a</span></div></body>");
            p.Run(L"var kayit = []; var mo = new MutationObserver((rs, o) => { for (const r of rs) kayit.push(r.type + ':' + (r.attributeName || '') + ':' + r.addedNodes.length + r.removedNodes.length + ':' + (r.oldValue === null ? '-' : r.oldValue)); });"
                  L"var m = document.getElementById('m'); mo.observe(m, {childList: true, attributes: true, attributeOldValue: true, subtree: true, characterData: true, characterDataOldValue: true});"
                  L"m.setAttribute('x', '1'); m.setAttribute('x', '2'); m.appendChild(document.createElement('b')); m.firstChild.remove(); m.firstChild.textContent = 'q';"
                  L"var t = document.createTextNode('eski'); m.appendChild(t); t.data = 'yeni'; kayit.push('senkron:' + kayit.length);");
            CHECK(p.Run(L"kayit.join(',')") == L"senkron:0,attributes:x:00:-,attributes:x:00:1,childList::10:-,childList::01:-,childList::10:-,childList::10:-,characterData::00:eski");
            p.Run(L"kayit = []; mo.disconnect(); m.setAttribute('y', '1');");
            CHECK(p.Run(L"kayit.length") == L"0");
            p.Run(L"var mo2 = new MutationObserver(() => {}); mo2.observe(m, {attributeFilter: ['z']}); m.setAttribute('y', '2'); m.setAttribute('z', '3'); var trs = mo2.takeRecords().map(r => r.attributeName).join();");
            CHECK(p.Run(L"trs") == L"z");
            // Polymer'in mikro gorev zamanlayicisi: metin dugumu + characterData
            p.Run(L"var tik = 0; var tn = document.createTextNode(''); new MutationObserver(() => tik++).observe(tn, {characterData: true}); tn.textContent = 1; tn.textContent = 2;");
            CHECK(p.Run(L"tik") == L"1");
            CHECK(p.Run(L"var a = document.createElement('i'), b = document.createElement('u'); m.append(a, b); a.compareDocumentPosition(b) + ':' + b.compareDocumentPosition(a) + ':' + m.compareDocumentPosition(a) + ':' + a.compareDocumentPosition(m)") == L"4:2:20:10");
            CHECK(p.Run(L"var x = document.createTextNode('merhaba'); m.appendChild(x); var y = x.splitText(3); x.data + '|' + y.data + '|' + (x.nextSibling === y)") == L"mer|haba|true");
            CHECK(p.Run(L"m.normalize(); m.lastChild.data") == L"merhaba");
            CHECK(p.Run(L"document.createRange().createContextualFragment('<p>1</p><p>2</p>').childNodes.length") == L"2");
            CHECK(p.Run(L"var c = document.createElement('canvas').getContext('2d'); c.fillStyle = 'rgb(10, 20, 30)'; c.fillRect(0,0,1,1); Array.from(c.getImageData(0,0,1,1).data).join()") == L"10,20,30,255");
            CHECK(p.Run(L"(window instanceof Window) + ':' + typeof Comment + ':' + (document.createComment('z').nodeType) + ':' + CSS.escape('1a')") == L"true:function:8:\\31 a");
            CHECK(p.Run(L"try { null.x } catch (e) { e.stack.indexOf(':1') > 0 }") == L"true");
        }

        // merJS: tembel ureteçler ve async ureteçler
        {
            Page p(L"<body></body>");
            CHECK(p.Run(L"function* say() { let i = 0; while (true) yield i++; } const g = say(); g.next(); g.next(); g.next().value") == L"2");
            CHECK(p.Run(L"function* ic() { const x = yield 1; yield x * 2; } const h = ic(); h.next(); h.next(21).value") == L"42");
            CHECK(p.Run(L"let fin = 0; function* k() { try { yield 1; yield 2; } finally { fin++; } } for (const v of k()) break; fin") == L"1");
            p.Run(L"var ag = ''; async function* ak() { yield 'x'; await null; yield 'y'; }"
                  L"(async () => { for await (const v of ak()) ag += v; ag += '!'; })();");
            CHECK(p.Run(L"ag") == L"xy!");
        }
    }
    {
        // Sekme yalitimi: baska is parcacigindan kesme bayragi sonsuz donguyu durdurur, calisma ortami kullanilabilir kalir
        Page p(L"<html><body><p id=s>0</p></body></html>");
        std::atomic<bool> stop{false};
        p.rt.extInterrupt = &stop;
        p.rt.stepBudget = 4000000000ull;
        HANDLE th = CreateThread(nullptr, 0, [](LPVOID a) -> DWORD { Sleep(300); static_cast<std::atomic<bool>*>(a)->store(true); return 0; }, &stop, 0, nullptr);
        ULONGLONG t0 = GetTickCount64();
        std::wstring ab = p.Run(L"var x = 0; while (true) { x++; }");
        ULONGLONG dt = GetTickCount64() - t0;
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        CHECK(dt >= 250 && dt < 3000);
        CHECK(ab.find(L"ABORT") == 0 && ab.find(L"durduruldu") != std::wstring::npos);
        stop = false;
        CHECK(p.Run(L"x > 1000") == L"true");
        CHECK(p.Run(L"document.getElementById('s').textContent = 'tamam'; document.getElementById('s').textContent") == L"tamam");
        // zamanlayici icinde de
        p.Run(L"var z = 0; setTimeout(function(){ while (true) z++; }, 0); setTimeout(function(){ z = -1; }, 1);");
        stop = true;
        p.rt.RunDueTimers(p.rt.Now() + 10);
        stop = false;
        CHECK(p.Run(L"z > 0") == L"true");
        // OverMemory: ana bilgisayar bellek siniri bildirirse uzun dongu durur
        struct MemHost : TestHost { int calls = 0; bool OverMemory() override { return ++calls > 3; } };
        Page q(L"<html><body></body></html>");
        MemHost mh;
        q.rt.AttachDocument(q.doc.get(), &mh);
        q.rt.stepBudget = 4000000000ull;
        std::wstring mr = q.Run(L"var a = []; while (true) { a.push({}); if (a.length > 1000) a = []; }");
        CHECK(mr.find(L"ABORT") == 0 && mr.find(L"bellek") != std::wstring::npos && mh.calls > 3);
    }
    // Guvenlik regresyon testleri: HttpOnly cerez korumasi, CRLF enjeksiyonu, outerHTML script engeli, pano yalitimi
    {
        Page p(L"<body><div id=hedef><span>eski</span></div></body>");
        js::Cookie ho;
        ho.value = L"gizli_oturum";
        ho.httpOnly = true;
        p.rt.cookies[L"sid"] = ho;
        CHECK(p.Run(L"document.cookie.indexOf('gizli_oturum')") == L"-1");
        p.Run(L"document.cookie = 'sid=sahte; path=/'; document.cookie = 'sid=; max-age=0';");
        CHECK(p.rt.cookies[L"sid"].value == L"gizli_oturum");
        p.Run(L"document.cookie = ' yeni=1; HttpOnly';");
        CHECK(p.rt.cookies.count(L"yeni") == 0);
        p.Run(L"document.cookie = 'crlf=1\\r\\nSet-Cookie: kotu=2';");
        CHECK(p.rt.cookies.count(L"crlf") == 0 && p.rt.cookies.count(L"kotu") == 0);

        // outerHTML ve insertAdjacentHTML <script> calistirmamali
        p.Run(L"window.xss = 0; document.getElementById('hedef').firstChild.outerHTML = '<script>window.xss = 1;</script>';"
              L"document.getElementById('hedef').insertAdjacentHTML('beforeend', '<script>window.xss = 2;</script>');");
        CHECK(p.Run(L"window.xss") == L"0");

        // Pano sekme yalitimi
        p.Run(L"navigator.clipboard.writeText('sekme1_ozel');");
        Page p2(L"<body></body>");
        p2.Run(L"var okunan = 'bas'; navigator.clipboard.readText().then(t => { okunan = t; });");
        CHECK(p2.Run(L"okunan") == L"");
    }
    std::printf(fails ? "%d HATA\n" : "TUM DOM TESTLERI GECTI\n", fails);
    std::fflush(stdout);
    return fails ? 1 : 0;
}
