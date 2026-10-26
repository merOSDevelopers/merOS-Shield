// merOS Shield - YouTube görünümü (bkz. youtube.h)
#include "youtube.h"
#include <algorithm>
#include <cwctype>
#include <set>
#include <vector>
#include "json.h"
#include "url.h"
#include "util.h"

namespace mer {

namespace {

struct Vid {
    std::wstring id, title, channel, channelUrl, meta, duration;
    bool isShort = false, isPlaylist = false, isLive = false;
};

// {simpleText} | {runs:[{text}]} | {content} -> duz metin
std::wstring TextOf(const JsonValue* v) {
    if (!v) return L"";
    if (v->t == JsonValue::Str) return v->s;
    if (const JsonValue* st = v->Get(L"simpleText")) return st->AsStr();
    if (const JsonValue* c = v->Get(L"content")) return c->AsStr();
    if (const JsonValue* runs = v->Get(L"runs")) {
        std::wstring o;
        for (auto& r : runs->a) if (const JsonValue* t = r.Get(L"text")) o += t->AsStr();
        return o;
    }
    return L"";
}

// Ilk "key" alanini derinlemesine ara (sinirli)
const JsonValue* FindKey(const JsonValue& v, const wchar_t* key, int depth = 0) {
    if (depth > 14) return nullptr;
    if (v.t == JsonValue::Obj) {
        for (auto& kv : v.o) if (kv.first == key) return &kv.second;
        for (auto& kv : v.o) if (auto* r = FindKey(kv.second, key, depth + 1)) return r;
    } else if (v.t == JsonValue::Arr) {
        for (auto& x : v.a) if (auto* r = FindKey(x, key, depth + 1)) return r;
    }
    return nullptr;
}

std::wstring ChannelUrlOf(const JsonValue* byline) {
    if (!byline) return L"";
    const JsonValue* runs = byline->Get(L"runs");
    if (!runs || runs->a.empty()) return L"";
    if (const JsonValue* u = runs->a[0].Path(L"navigationEndpoint.commandMetadata.webCommandMetadata.url")) return u->AsStr();
    if (const JsonValue* u = runs->a[0].Path(L"navigationEndpoint.browseEndpoint.canonicalBaseUrl")) return u->AsStr();
    return L"";
}

void FromVideoRenderer(const JsonValue& r, Vid& v) {
    if (const JsonValue* id = r.Get(L"videoId")) v.id = id->AsStr();
    v.title = TextOf(r.Get(L"title"));
    if (v.title.empty()) v.title = TextOf(r.Get(L"headline"));
    const JsonValue* by = r.Get(L"ownerText");
    if (!by) by = r.Get(L"longBylineText");
    if (!by) by = r.Get(L"shortBylineText");
    v.channel = TextOf(by);
    v.channelUrl = ChannelUrlOf(by);
    std::wstring views = TextOf(r.Get(L"viewCountText"));
    if (views.empty()) views = TextOf(r.Get(L"shortViewCountText"));
    std::wstring when = TextOf(r.Get(L"publishedTimeText"));
    v.meta = views;
    if (!when.empty()) v.meta += (v.meta.empty() ? L"" : L" • ") + when;
    v.duration = TextOf(r.Get(L"lengthText"));
    if (v.duration.empty()) {
        if (const JsonValue* ov = FindKey(r, L"thumbnailOverlayTimeStatusRenderer")) {
            v.duration = TextOf(ov->Get(L"text"));
            if (const JsonValue* st = ov->Get(L"style")) { if (st->AsStr() == L"LIVE") v.isLive = true; if (st->AsStr() == L"SHORTS") v.isShort = true; }
        }
    }
}

void FromLockup(const JsonValue& r, Vid& v) {
    std::wstring type = r.Get(L"contentType") ? r.Get(L"contentType")->AsStr() : L"";
    if (const JsonValue* id = r.Get(L"contentId")) v.id = id->AsStr();
    if (type.find(L"PLAYLIST") != std::wstring::npos || type.find(L"PODCAST") != std::wstring::npos) v.isPlaylist = true;
    else if (!type.empty() && type.find(L"VIDEO") == std::wstring::npos) { v.id.clear(); return; }
    const JsonValue* md = r.Path(L"metadata.lockupMetadataViewModel");
    if (md) {
        v.title = TextOf(md->Get(L"title"));
        if (const JsonValue* rows = md->Path(L"metadata.contentMetadataViewModel.metadataRows")) {
            int ri = 0;
            for (auto& row : rows->a) {
                std::wstring line;
                if (const JsonValue* parts = row.Get(L"metadataParts")) {
                    for (auto& part : parts->a) {
                        std::wstring t = TextOf(part.Get(L"text"));
                        if (t.empty()) continue;
                        if (!line.empty()) line += L" • ";
                        line += t;
                        if (ri == 0 && v.channelUrl.empty()) {
                            if (const JsonValue* u = FindKey(part, L"url")) if (u->t == JsonValue::Str && !u->s.empty() && u->s[0] == L'/') v.channelUrl = u->s;
                        }
                    }
                }
                if (ri == 0) v.channel = line;
                else if (!line.empty()) v.meta += (v.meta.empty() ? L"" : L" • ") + line;
                ri++;
            }
        }
    }
    if (const JsonValue* img = r.Get(L"contentImage")) {
        if (const JsonValue* badge = FindKey(*img, L"thumbnailBadgeViewModel")) {
            v.duration = TextOf(badge->Get(L"text"));
            if (const JsonValue* st = badge->Get(L"badgeStyle")) if (st->AsStr().find(L"LIVE") != std::wstring::npos) v.isLive = true;
        }
    }
}

void FromShorts(const JsonValue& r, Vid& v) {
    if (const JsonValue* id = r.Path(L"onTap.innertubeCommand.reelWatchEndpoint.videoId")) v.id = id->AsStr();
    if (v.id.empty()) {
        std::wstring e = r.Get(L"entityId") ? r.Get(L"entityId")->AsStr() : L"";
        size_t p = e.rfind(L'-');
        if (StartsWith(e, L"shorts-shelf-item-")) v.id = e.substr(18);
        else if (p != std::wstring::npos) v.id = e.substr(p + 1);
    }
    v.title = TextOf(r.Path(L"overlayMetadata.primaryText"));
    v.meta = TextOf(r.Path(L"overlayMetadata.secondaryText"));
    v.isShort = true;
}

void FromReel(const JsonValue& r, Vid& v) {
    if (const JsonValue* id = r.Get(L"videoId")) v.id = id->AsStr();
    v.title = TextOf(r.Get(L"headline"));
    v.meta = TextOf(r.Get(L"viewCountText"));
    v.isShort = true;
}

bool ValidId(const std::wstring& id, bool playlist) {
    if (id.empty() || id.size() > 64) return false;
    for (wchar_t c : id) if (!(iswalnum(c) || c == L'-' || c == L'_')) return false;
    return playlist || id.size() == 11;
}

void Collect(const JsonValue& v, std::vector<Vid>& out, std::set<std::wstring>& seen, int depth) {
    if (depth > 60 || out.size() > 400) return;
    if (v.t == JsonValue::Arr) { for (auto& x : v.a) Collect(x, out, seen, depth + 1); return; }
    if (v.t != JsonValue::Obj) return;
    for (auto& kv : v.o) {
        const std::wstring& k = kv.first;
        Vid vid;
        bool handled = true;
        if (k == L"videoRenderer" || k == L"compactVideoRenderer" || k == L"gridVideoRenderer" || k == L"videoWithContextRenderer" ||
            k == L"playlistVideoRenderer" || k == L"playlistPanelVideoRenderer" || k == L"endScreenVideoRenderer") FromVideoRenderer(kv.second, vid);
        else if (k == L"lockupViewModel") FromLockup(kv.second, vid);
        else if (k == L"shortsLockupViewModel") FromShorts(kv.second, vid);
        else if (k == L"reelItemRenderer") FromReel(kv.second, vid);
        else handled = false;
        if (handled) {
            if (ValidId(vid.id, vid.isPlaylist) && !vid.title.empty() && seen.insert(vid.id).second) out.push_back(vid);
            continue;
        }
        Collect(kv.second, out, seen, depth + 1);
    }
}

// "var ytInitialData = {...};" icindeki JSON
bool ExtractJson(const std::wstring& html, const wchar_t* name, JsonValue& out) {
    size_t from = 0;
    while (true) {
        size_t p = html.find(name, from);
        if (p == std::wstring::npos) return false;
        size_t q = p + wcslen(name);
        while (q < html.size() && (html[q] == L' ' || html[q] == L'"' || html[q] == L']' || html[q] == L'\'')) q++;
        if (q < html.size() && html[q] == L'=') {
            q++;
            while (q < html.size() && IsWS(html[q])) q++;
            if (q < html.size() && html[q] == L'{') {
                size_t pos = q;
                if (ParseJson(html, pos, out)) return true;
            }
        }
        from = p + 1;
    }
}

std::wstring Esc(const std::wstring& s) { return HtmlEscape(s); }

std::wstring Thumb(const std::wstring& id, const wchar_t* kind) {
    return L"https://i.ytimg.com/vi/" + id + L"/" + kind + L".jpg";
}

std::wstring Initial(const std::wstring& name) {
    for (wchar_t c : name) if (iswalnum(c)) return std::wstring(1, wchar_t(towupper(c)));
    return L"?";
}

const wchar_t* kCss =
    L"<style>"
    L"body{margin:0;background:#ffffff;color:#0f0f0f;font-family:'Segoe UI',Roboto,Arial,sans-serif;font-size:14px}"
    L"a{color:inherit;text-decoration:none}"
    L".ust{display:flex;align-items:center;gap:16px;padding:10px 24px;border-bottom:1px solid #e5e5e5;background:#fff}"
    L".logo{display:flex;align-items:center;gap:6px;font-size:20px;font-weight:700;letter-spacing:-0.5px;color:#0f0f0f}"
    L".logo .oynat{display:inline-block;width:26px;height:26px;border-radius:50%;background:#0f0f0f;color:#fff;font-size:11px;text-align:center;line-height:26px}"
    L".logo small{font-size:10px;font-weight:600;color:#606060;margin-left:2px;align-self:flex-start}"
    L".ara{flex:1;display:flex;max-width:640px;margin:0 auto}"
    L".ara input{flex:1;padding:8px 14px;font-size:16px;border:1px solid #c6c6c6;border-radius:40px 0 0 40px}"
    L".ara button{padding:8px 22px;font-size:15px;border:1px solid #c6c6c6;border-left:0;background:#f8f8f8;border-radius:0 40px 40px 0}"
    L".rozet{font-size:12px;color:#065fd4;background:#e8f0fe;padding:4px 10px;border-radius:12px;white-space:nowrap}"
    L".kap{padding:20px 24px;max-width:1800px;margin:0 auto}"
    L".cipler{display:flex;flex-wrap:wrap;gap:10px;margin:0 0 20px}"
    L".cip{background:#f2f2f2;padding:7px 13px;border-radius:8px;font-weight:600;font-size:14px}"
    L".cip.secili{background:#0f0f0f;color:#fff}"
    L".izgara{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:36px 16px}"
    L".kart{display:block}"
    L".kapak{position:relative;border-radius:12px;overflow:hidden;background:#e5e5e5;aspect-ratio:16/9}"
    L".kapak img{display:block;width:100%;aspect-ratio:16/9}"
    L".sure{position:absolute;right:6px;bottom:6px;background:rgba(0,0,0,0.8);color:#fff;font-size:12px;font-weight:600;padding:2px 5px;border-radius:4px}"
    L".sure.canli{background:#cc0000}"
    L".bilgi{display:flex;gap:12px;margin-top:12px}"
    L".avatar{flex:0 0 36px;width:36px;height:36px;border-radius:50%;background:#8a6bd1;color:#fff;font-weight:600;text-align:center;line-height:36px;font-size:15px}"
    L".baslik{font-size:16px;font-weight:600;line-height:22px;margin:0 0 4px;color:#0f0f0f}"
    L".kanal,.meta{font-size:14px;color:#606060;line-height:20px}"
    L".bos{text-align:center;color:#606060;padding:60px 0;font-size:16px}"
    L".izle{display:flex;gap:24px;align-items:flex-start}"
    L".sol{flex:1;min-width:0}"
    L".sag{flex:0 0 402px;width:402px}"
    L".oynatici{position:relative;display:block;border-radius:12px 12px 0 0;overflow:hidden;background:#000;aspect-ratio:16/9}"
    L".oynatici video{position:absolute;left:0;top:0;width:100%;height:100%;background:#000}"
    L".durum{position:absolute;left:0;right:0;bottom:0;padding:10px 14px;background:rgba(0,0,0,0.65);color:#fff;font-size:13px}"
    L".kontrol{display:flex;align-items:center;gap:12px;background:#0f0f0f;color:#fff;padding:8px 12px;border-radius:0 0 12px 12px}"
    L".kontrol button{background:transparent;color:#fff;border:0;font-size:15px;font-weight:600;padding:4px 8px;min-width:34px;cursor:pointer}"
    L".zaman{font-size:13px;white-space:nowrap;color:#eee}"
    L".cubuk{position:relative;flex:1;height:6px;background:#3d3d3d;border-radius:3px;cursor:pointer}"
    L".tampon{position:absolute;left:0;top:0;height:6px;width:0;background:#777;border-radius:3px}"
    L".dolu{position:absolute;left:0;top:0;height:6px;width:0;background:#f00;border-radius:3px}"
    L".kalite{font-size:12px;color:#ccc;white-space:nowrap}"
    L"h1.vbas{font-size:20px;font-weight:700;margin:12px 0 8px;line-height:28px}"
    L".sahip{display:flex;align-items:center;gap:12px;margin:0 0 12px}"
    L".sahip b{font-size:16px}"
    L".abone{margin-left:auto;background:#0f0f0f;color:#fff;padding:8px 16px;border-radius:18px;font-weight:600}"
    L".aciklama{background:#f2f2f2;border-radius:12px;padding:12px;white-space:pre-wrap;line-height:20px;font-size:14px}"
    L".aciklama b{display:block;margin-bottom:6px}"
    L".oneri{display:flex;gap:8px;margin:0 0 10px}"
    L".oneri .kapak{flex:0 0 168px;width:168px;border-radius:8px}"
    L".oneri .baslik{font-size:14px;line-height:20px}"
    L".oneri .kanal,.oneri .meta{font-size:12px;line-height:18px}"
    L".kanalbas{display:flex;align-items:center;gap:16px;margin:0 0 24px}"
    L".kanalbas .avatar{flex:0 0 80px;width:80px;height:80px;line-height:80px;font-size:32px}"
    L".kanalbas h1{margin:0;font-size:28px}"
    L"</style>";

std::wstring Header(const std::wstring& query, bool tr) {
    std::wstring h = L"<div class=ust><a class=logo href=\"https://www.youtube.com/\"><span class=oynat>▶</span>YouTube<small>merOS</small></a>"
                     L"<form class=ara action=\"https://www.youtube.com/results\" method=get><input name=search_query value=\"" + Esc(query) +
                     L"\" placeholder=\"" + (tr ? L"Ara" : L"Search") + L"\"><button>" + (tr ? L"Ara" : L"Search") + L"</button></form>"
                     L"<span class=rozet>" + (tr ? L"✓ Reklamsız • İzleyicisiz" : L"✓ No ads • No trackers") + L"</span></div>";
    return h;
}

std::wstring Card(const Vid& v, bool tr) {
    std::wstring href = v.isPlaylist ? L"https://www.youtube.com/playlist?list=" + v.id : L"https://www.youtube.com/watch?v=" + v.id;
    std::wstring o = L"<a class=kart href=\"" + Esc(href) + L"\"><div class=kapak>";
    if (!v.isPlaylist) o += L"<img src=\"" + Thumb(v.id, L"mqdefault") + L"\" alt=\"\">";
    if (!v.duration.empty()) o += std::wstring(L"<span class=\"sure") + (v.isLive ? L" canli" : L"") + L"\">" + Esc(v.duration) + L"</span>";
    else if (v.isShort) o += L"<span class=sure>Shorts</span>";
    else if (v.isPlaylist) o += std::wstring(L"<span class=sure>") + (tr ? L"Oynatma listesi" : L"Playlist") + L"</span>";
    o += L"</div><div class=bilgi><div class=avatar>" + Esc(Initial(v.channel.empty() ? v.title : v.channel)) + L"</div><div>";
    o += L"<div class=baslik>" + Esc(v.title) + L"</div>";
    if (!v.channel.empty()) o += L"<div class=kanal>" + Esc(v.channel) + L"</div>";
    if (!v.meta.empty()) o += L"<div class=meta>" + Esc(v.meta) + L"</div>";
    o += L"</div></div></a>";
    return o;
}

std::wstring SmallCard(const Vid& v) {
    std::wstring o = L"<a class=oneri href=\"https://www.youtube.com/watch?v=" + Esc(v.id) + L"\"><div class=kapak><img src=\"" + Thumb(v.id, L"mqdefault") + L"\" alt=\"\">";
    if (!v.duration.empty()) o += std::wstring(L"<span class=\"sure") + (v.isLive ? L" canli" : L"") + L"\">" + Esc(v.duration) + L"</span>";
    o += L"</div><div><div class=baslik>" + Esc(v.title) + L"</div>";
    if (!v.channel.empty()) o += L"<div class=kanal>" + Esc(v.channel) + L"</div>";
    if (!v.meta.empty()) o += L"<div class=meta>" + Esc(v.meta) + L"</div>";
    o += L"</div></a>";
    return o;
}

std::wstring QueryParam(const std::wstring& url, const wchar_t* key) {
    size_t q = url.find(L'?');
    if (q == std::wstring::npos) return L"";
    std::wstring qs = url.substr(q + 1);
    size_t h = qs.find(L'#');
    if (h != std::wstring::npos) qs = qs.substr(0, h);
    std::wstring k = std::wstring(key) + L"=";
    size_t st = 0;
    while (st <= qs.size()) {
        size_t e = qs.find(L'&', st);
        if (e == std::wstring::npos) e = qs.size();
        std::wstring part = qs.substr(st, e - st);
        if (StartsWith(part, k)) {
            std::wstring v = part.substr(k.size());
            // %XX ve + cozumu (UTF-8)
            std::string bytes;
            for (size_t i = 0; i < v.size(); i++) {
                wchar_t c = v[i];
                auto hx = [](wchar_t x) { return x >= L'0' && x <= L'9' ? x - L'0' : x >= L'a' && x <= L'f' ? x - L'a' + 10 : x >= L'A' && x <= L'F' ? x - L'A' + 10 : -1; };
                if (c == L'+') bytes += ' ';
                else if (c == L'%' && i + 2 < v.size() && hx(v[i + 1]) >= 0 && hx(v[i + 2]) >= 0) { bytes += char(hx(v[i + 1]) * 16 + hx(v[i + 2])); i += 2; }
                else bytes += ToUtf8(std::wstring(1, c));
            }
            std::wstring out;
            // UTF-8 -> UTF-16 (basit)
            for (size_t i = 0; i < bytes.size();) {
                unsigned char b = (unsigned char)bytes[i];
                uint32_t cp;
                int n;
                if (b < 0x80) { cp = b; n = 1; }
                else if ((b >> 5) == 6) { cp = b & 0x1F; n = 2; }
                else if ((b >> 4) == 14) { cp = b & 0x0F; n = 3; }
                else if ((b >> 3) == 30) { cp = b & 0x07; n = 4; }
                else { i++; continue; }
                if (i + size_t(n) > bytes.size()) break;
                for (int k2 = 1; k2 < n; k2++) cp = (cp << 6) | ((unsigned char)bytes[i + size_t(k2)] & 0x3F);
                AppendCodepoint(out, cp);
                i += size_t(n);
            }
            return out;
        }
        st = e + 1;
    }
    return L"";
}

std::wstring FormatViews(const std::wstring& n, bool tr) {
    std::wstring digits;
    for (wchar_t c : n) if (iswdigit(c)) digits += c;
    if (digits.empty()) return L"";
    std::wstring o;
    int cnt = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        o.insert(o.begin(), digits[i]);
        if (++cnt % 3 == 0 && i > 0) o.insert(o.begin(), tr ? L'.' : L',');
    }
    return o + (tr ? L" görüntüleme" : L" views");
}

std::wstring FormatLength(const std::wstring& secs) {
    long s = long(wcstol(secs.c_str(), nullptr, 10));
    if (s <= 0) return L"";
    wchar_t buf[32];
    if (s >= 3600) swprintf(buf, 32, L"%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(buf, 32, L"%ld:%02ld", s / 60, s % 60);
    return buf;
}

std::wstring Linkify(const std::wstring& text) {
    std::wstring o;
    size_t i = 0;
    while (i < text.size()) {
        size_t p = text.find(L"http", i);
        if (p == std::wstring::npos) { o += Esc(text.substr(i)); break; }
        o += Esc(text.substr(i, p - i));
        size_t e = p;
        while (e < text.size() && !IsWS(text[e]) && text[e] != L'"' && text[e] != L'<' && text[e] != L')') e++;
        std::wstring u = text.substr(p, e - p);
        if (StartsWith(u, L"https://") || StartsWith(u, L"http://")) o += L"<a style=\"color:#065fd4\" href=\"" + Esc(u) + L"\">" + Esc(u) + L"</a>";
        else o += Esc(u);
        i = e;
    }
    return o;
}

// Video sayfasindaki yerel oynatici: <video src="meros-yt:ID"> (motorun kendi H.264/AAC oynaticisi)
// ve kucuk bir kontrol cubugu (oynat/duraklat, zaman, arama cubugu, ses, kalite).
std::wstring PlayerHtml(const std::wstring& id, const std::wstring& lengthSecs, bool tr) {
    std::wstring len = std::to_wstring(std::max(0, _wtoi(lengthSecs.c_str())));
    std::wstring h = L"<div class=oynatici><video id=yv src=\"meros-yt:" + id + L"\" autoplay data-max-height=720" +
                     (tr ? L" data-hl=tr data-gl=TR" : L" data-hl=en data-gl=US") + L"></video>"
                     L"<div class=durum id=ydurum>" + (tr ? L"Video hazırlanıyor…" : L"Preparing video…") + L"</div></div>"
                     L"<div class=kontrol><button id=yoyn>❚❚</button><span class=zaman id=yzaman>0:00</span>"
                     L"<div class=cubuk id=ycubuk><div class=tampon id=ytampon></div><div class=dolu id=ydolu></div></div>"
                     L"<button id=yses>" + (tr ? L"Ses" : L"Sound") + L"</button><button id=ykal>720p</button><span class=kalite id=ybilgi></span></div>";
    const wchar_t* T1 = tr ? L"Yükleniyor…" : L"Loading…";
    const wchar_t* T2 = tr ? L"Video oynatılamadı" : L"Couldn't play the video";
    const wchar_t* T3 = tr ? L"Sessiz" : L"Muted";
    const wchar_t* T4 = tr ? L"Ses" : L"Sound";
    const wchar_t* T5 = tr ? L"Sistem tarayıcısında aç" : L"Open in system browser";
    h += std::wstring(L"<script>(function(){"
         L"var v=document.getElementById('yv'),d=document.getElementById('ydurum'),b=document.getElementById('yoyn'),z=document.getElementById('yzaman'),"
         L"c=document.getElementById('ycubuk'),f=document.getElementById('ydolu'),t=document.getElementById('ytampon'),s=document.getElementById('yses'),"
         L"q=document.getElementById('ykal'),bi=document.getElementById('ybilgi'),LEN=") + len + L";"
         L"var KAL=[360,480,720,1080],kal=720;try{var x=parseInt(localStorage.getItem('merosKalite'),10);if(KAL.indexOf(x)>=0)kal=x;}catch(e){}"
         L"function dur(){var du=v.duration;return (du>0&&du!==Infinity)?du:LEN;}"
         L"function fmt(x){x=Math.max(0,Math.floor(x||0));var h=Math.floor(x/3600),m=Math.floor(x%3600/60),n=x%60;return (h?h+':'+(m<10?'0':''):'')+m+':'+(n<10?'0':'')+n;}"
         L"function ciz(){var du=dur();z.textContent=fmt(v.currentTime)+' / '+fmt(du);f.style.width=(du?Math.min(100,v.currentTime/du*100):0)+'%';"
         L"var bu=v.buffered;if(bu.length&&du)t.style.width=Math.min(100,bu.end(bu.length-1)/du*100)+'%';b.textContent=v.paused?'▶':'❚❚';}"
         L"function goster(m){d.textContent=m;d.style.display=m?'block':'none';}"
         L"function ac(){if(v.paused||v.ended)v.play();else v.pause();ciz();}"
         L"['timeupdate','play','pause','progress','durationchange','seeked'].forEach(function(e){v.addEventListener(e,ciz);});"
         L"v.addEventListener('waiting',function(){goster('" + T1 + L"');});"
         L"v.addEventListener('playing',function(){goster('');});"
         L"v.addEventListener('canplay',function(){if(!v.paused)goster('');});"
         L"v.addEventListener('loadedmetadata',function(){bi.textContent=v.videoWidth?v.videoWidth+'×'+v.videoHeight:'';});"
         L"v.addEventListener('error',function(){goster('" + T2 + L"'+(v.error&&v.error.message?': '+v.error.message:''));"
         L"var a=document.createElement('a');a.href='meros-yt-play:" + id + L"';a.textContent='" + T5 + L"';a.style.color='#8ab4f8';a.style.marginLeft='10px';d.appendChild(a);});"
         L"v.addEventListener('click',ac);b.addEventListener('click',ac);"
         L"c.addEventListener('click',function(e){var r=c.getBoundingClientRect();if(!r.width||e.clientX===undefined)return;"
         L"var x=(e.clientX-r.left)/r.width;if(x<0)x=0;if(x>1)x=1;v.currentTime=x*dur();ciz();});"
         L"s.addEventListener('click',function(){v.muted=!v.muted;s.textContent=v.muted?'" + T3 + L"':'" + T4 + L"';});"
         L"function kalYaz(){q.textContent=kal+'p';}kalYaz();"
         L"if(kal!==720){v.setAttribute('data-max-height',String(kal));v.src=v.getAttribute('src');}"
         L"q.addEventListener('click',function(){kal=KAL[(KAL.indexOf(kal)+1)%KAL.length];kalYaz();try{localStorage.setItem('merosKalite',String(kal));}catch(e){}"
         L"var ta=v.currentTime,oy=!v.paused;v.setAttribute('data-max-height',String(kal));v.src=v.getAttribute('src');"
         L"v.addEventListener('loadedmetadata',function g(){v.removeEventListener('loadedmetadata',g);if(ta>0)v.currentTime=ta;if(oy)v.play();});});"
         L"setInterval(ciz,500);ciz();"
         L"})();</script>";
    return h;
}

std::wstring Page(const std::wstring& title, const std::wstring& body) {
    return L"<!DOCTYPE html><html><head><meta charset=utf-8><title>" + Esc(title) + L"</title>" + kCss + L"</head><body>" + body + L"</body></html>";
}

} // namespace

bool IsYouTubeUrl(const std::wstring& url) {
    Url u = ParseUrl(url);
    if (!u.ok || (u.scheme != L"https" && u.scheme != L"http")) return false;
    return u.host == L"youtube.com" || u.host == L"www.youtube.com" || u.host == L"m.youtube.com" || u.host == L"youtu.be";
}

std::wstring NormalizeYouTubeUrl(const std::wstring& url) {
    Url u = ParseUrl(url);
    if (!u.ok) return url;
    if (u.host == L"youtu.be") {
        std::wstring id = u.path.substr(1);
        size_t q = id.find(L'?');
        if (q != std::wstring::npos) id = id.substr(0, q);
        return L"https://www.youtube.com/watch?v=" + id;
    }
    std::wstring path = u.path;
    if (StartsWith(path, L"/shorts/")) {
        std::wstring id = path.substr(8);
        size_t q = id.find_first_of(L"?/");
        if (q != std::wstring::npos) id = id.substr(0, q);
        return L"https://www.youtube.com/watch?v=" + id;
    }
    return L"https://www.youtube.com" + path;
}

bool YouTubeExternalTarget(const std::wstring& href, std::wstring& target) {
    const std::wstring pre = L"meros-yt-play:";
    if (!StartsWith(href, pre)) return false;
    std::wstring id = href.substr(pre.size());
    if (!ValidId(id, false)) return false;
    target = L"https://www.youtube.com/watch?v=" + id;
    return true;
}

bool BuildYouTubePage(const std::wstring& html, const std::wstring& pageUrl, bool tr, std::wstring& out) {
    JsonValue data, player;
    bool hasData = ExtractJson(html, L"ytInitialData", data);
    bool hasPlayer = ExtractJson(html, L"ytInitialPlayerResponse", player);
    if (!hasData && !hasPlayer) return false;
    Url u = ParseUrl(pageUrl);
    std::wstring path = u.path;
    size_t qm = path.find(L'?');
    std::wstring pathOnly = qm == std::wstring::npos ? path : path.substr(0, qm);
    std::wstring query = QueryParam(pageUrl, L"search_query");

    // ---- video sayfasi ----
    const JsonValue* details = hasPlayer ? player.Get(L"videoDetails") : nullptr;
    if (pathOnly == L"/watch" && details) {
        std::wstring id = details->Get(L"videoId") ? details->Get(L"videoId")->AsStr() : QueryParam(pageUrl, L"v");
        if (!ValidId(id, false)) return false;
        std::wstring title = details->Get(L"title") ? details->Get(L"title")->AsStr() : L"";
        std::wstring author = details->Get(L"author") ? details->Get(L"author")->AsStr() : L"";
        std::wstring views = details->Get(L"viewCount") ? FormatViews(details->Get(L"viewCount")->AsStr(), tr) : L"";
        std::wstring length = details->Get(L"lengthSeconds") ? FormatLength(details->Get(L"lengthSeconds")->AsStr()) : L"";
        std::wstring desc = details->Get(L"shortDescription") ? details->Get(L"shortDescription")->AsStr() : L"";
        std::wstring channelId = details->Get(L"channelId") ? details->Get(L"channelId")->AsStr() : L"";
        std::wstring date, subs;
        if (hasData) {
            if (const JsonValue* p = FindKey(data, L"videoPrimaryInfoRenderer")) date = TextOf(p->Get(L"dateText"));
            if (const JsonValue* o = FindKey(data, L"videoOwnerRenderer")) subs = TextOf(o->Get(L"subscriberCountText"));
        }
        std::vector<Vid> related;
        std::set<std::wstring> seen{id};
        if (hasData) {
            const JsonValue* sec = FindKey(data, L"secondaryResults");
            Collect(sec ? *sec : data, related, seen, 0);
        }
        std::wstring b = Header(L"", tr);
        b += L"<div class=kap><div class=izle><div class=sol>";
        b += PlayerHtml(id, details->Get(L"lengthSeconds") ? details->Get(L"lengthSeconds")->AsStr() : L"0", tr);
        b += L"<h1 class=vbas>" + Esc(title) + L"</h1>";
        b += L"<div class=sahip><div class=avatar>" + Esc(Initial(author)) + L"</div><div><b><a href=\"https://www.youtube.com/channel/" + Esc(channelId) + L"\">" + Esc(author) +
             L"</a></b>" + (subs.empty() ? L"" : L"<div class=meta>" + Esc(subs) + L"</div>") + L"</div><a class=abone href=\"https://www.youtube.com/channel/" + Esc(channelId) + L"\">" +
             (tr ? L"Kanala git" : L"Open channel") + L"</a></div>";
        std::wstring metaLine = views;
        if (!date.empty()) metaLine += (metaLine.empty() ? L"" : L"  •  ") + date;
        b += L"<div class=aciklama><b>" + Esc(metaLine) + L"</b>" + Linkify(desc) + L"</div>";
        b += L"</div><div class=sag>";
        for (size_t i = 0; i < related.size() && i < 30; i++) if (!related[i].isPlaylist) b += SmallCard(related[i]);
        b += L"</div></div></div>";
        out = Page(title + L" - YouTube", b);
        return true;
    }

    // ---- liste sayfalari: ana sayfa, arama, kanal, oynatma listesi ----
    std::vector<Vid> vids;
    std::set<std::wstring> seen;
    if (hasData) Collect(data, vids, seen, 0);
    std::wstring heading;
    std::wstring channelName;
    if (hasData) {
        if (const JsonValue* cm = FindKey(data, L"channelMetadataRenderer")) channelName = cm->Get(L"title") ? cm->Get(L"title")->AsStr() : L"";
        if (channelName.empty()) if (const JsonValue* ph = FindKey(data, L"pageHeaderViewModel")) channelName = TextOf(ph->Path(L"title.dynamicTextViewModel.text"));
    }
    std::wstring b = Header(query, tr);
    b += L"<div class=kap>";
    if (!channelName.empty() && pathOnly != L"/results") {
        b += L"<div class=kanalbas><div class=avatar>" + Esc(Initial(channelName)) + L"</div><h1>" + Esc(channelName) + L"</h1></div>";
    } else {
        static const wchar_t* chipsTr[] = {L"Müzik", L"Oyun", L"Haberler", L"Spor", L"Bilim", L"Komedi", L"Yemek", L"Teknoloji", L"Belgesel", L"Canlı"};
        static const wchar_t* chipsEn[] = {L"Music", L"Gaming", L"News", L"Sports", L"Science", L"Comedy", L"Cooking", L"Technology", L"Documentary", L"Live"};
        b += L"<div class=cipler>";
        for (int i = 0; i < 10; i++) {
            std::wstring c = tr ? chipsTr[i] : chipsEn[i];
            b += std::wstring(L"<a class=\"cip") + (c == query ? L" secili" : L"") + L"\" href=\"https://www.youtube.com/results?search_query=" + UrlEncodeComponent(c) + L"\">" + Esc(c) + L"</a>";
        }
        b += L"</div>";
    }
    if (vids.empty()) {
        b += std::wstring(L"<div class=bos>") + (tr ? L"Burada henüz video yok. Yukarıdan arama yapabilir ya da bir kategori seçebilirsin."
                                                : L"No videos here yet. Search above or pick a category.") + L"</div>";
    } else {
        b += L"<div class=izgara>";
        for (size_t i = 0; i < vids.size() && i < 120; i++) b += Card(vids[i], tr);
        b += L"</div>";
    }
    b += L"</div>";
    if (!query.empty()) heading = query + L" - YouTube";
    else if (!channelName.empty()) heading = channelName + L" - YouTube";
    else heading = L"YouTube";
    out = Page(heading, b);
    return true;
}

} // namespace mer
