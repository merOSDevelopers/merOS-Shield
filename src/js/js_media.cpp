// merOS Shield - merJS medya baglamalari
// HTMLMediaElement (<video>/<audio>), MediaSource ve SourceBuffer (MSE), TimeRanges.
// Gercek oynatma src/media/ altindaki MediaPlayer'dadir (H.264 + AAC, Media Foundation).
// JS tarafi: kodlanmis ornekleri iz arabelleklerine koyar, oynaticiya komut verir ve oynaticinin
// durumunu her "medya tikinde" (RunDueTimers) okuyup HTML olaylarini (timeupdate, seeked, ...) atar.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "js.h"
#include "../dom.h"
#include "../net.h"
#include "../url.h"
#include "../util.h"
#include "../media/media_player.h"
#include "../media/yt_stream.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mer {
namespace js {

using media::BufSample;
using media::MediaPlayer;
using media::Mp4Parser;
using media::PlayerStatus;
using media::Ranges;
using media::TrackBuffer;
using media::TrackConfig;

namespace {

#define A(i) (args.size() > (i) ? args[i] : Value())
#define NATIVE(name) Value name(Runtime& rt, Obj* self, const Value& thisV, std::vector<Value>& args)
#define UNUSED (void)self; (void)thisV; (void)args; (void)rt

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();
const size_t kQuota = 400u * 1024 * 1024;   // SourceBuffer basina bayt siniri

Node* NodeOf(const Value& v) {
    if (v.IsObj() && v.o->cls == Cls::Node && v.o->x) return v.o->x->node;
    return nullptr;
}
bool IsMediaNode(const Node* n) { return n && n->kind == Node::Elem && (n->name == L"video" || n->name == L"audio"); }

[[noreturn]] void ThrowDOM(Runtime& rt, const std::wstring& name, const std::wstring& msg) {
    Value ctor = rt.Get(Value::O(rt.global), L"DOMException");
    if (ctor.IsObj() && ctor.o->IsCallable()) {
        std::vector<Value> a{Value::S(msg), Value::S(name)};
        Value e = rt.Construct(ctor, a);
        throw JSThrow{e};
    }
    rt.Throw(L"Error", name + L": " + msg);
}


struct MSState;

struct SBState {
    Obj* obj = nullptr;
    MSState* ms = nullptr;
    std::wstring mime;
    Mp4Parser parser;
    std::shared_ptr<TrackBuffer> vtb, atb;
    uint32_t vId = 0, aId = 0;
    TrackConfig vcfg, acfg;
    double timestampOffset = 0;
    double appendWindowStart = 0, appendWindowEnd = kInf;
    std::wstring mode = L"segments";
    bool updating = false;
    bool removed = false;
    bool gotInit = false;
    double groupEnd = 0;
    bool groupStartPending = true;
    Ranges Buffered() const {
        if (vtb && atb) return media::IntersectRanges(vtb->Buffered(), atb->Buffered());
        if (vtb) return vtb->Buffered();
        if (atb) return atb->Buffered();
        return {};
    }
};

struct MediaEl;

struct MSState {
    Obj* obj = nullptr;
    std::wstring readyState = L"closed";
    double duration = kNaN;
    std::vector<SBState*> sbs;
    Obj* sbList = nullptr;
    Obj* activeList = nullptr;
    MediaEl* el = nullptr;
    Ranges liveSeekable;
};

// Ilerlemeli indirme (src="...mp4")
struct ProgLoad {
    std::mutex mu;
    bool done = false;
    bool ok = false;
    int errCode = 0;
    std::string err;
    std::vector<TrackConfig> tracks;
    std::vector<media::Sample> samples;
};

struct MediaEl {
    Node* node = nullptr;
    std::shared_ptr<MediaPlayer> player;
    MSState* ms = nullptr;
    std::wstring src, currentSrc;
    bool paused = true;
    bool autoplaying = true;     // spec: "can autoplay" bayragi
    double volume = 1;
    bool muted = false;
    double rate = 1, defaultRate = 1;
    int readyState = 0, networkState = 0;
    bool ended = false;
    bool seeking = false;
    double duration = kNaN;
    double time = 0;              // son bilinen gecerli zaman
    double lastTimeUpdate = 0;
    uint32_t seekBase = 0;        // arama istendiginde oynaticinin tamamlanan arama sayaci
    int vw = 0, vh = 0;
    bool waiting = false;
    bool loadedMeta = false, loadedData = false;
    int errorCode = 0;
    std::wstring errorMsg;
    std::shared_ptr<ProgLoad> prog;
    std::shared_ptr<TrackBuffer> pv, pa;   // ilerlemeli: iz arabellekleri
    bool progEos = false;
    double lastProgress = 0;
    uint64_t lastBufGen = 0;
    Ranges played;
    double playStart = -1;
    std::vector<Obj*> playPromises;
    std::vector<std::pair<int, Value>> frameCallbacks;   // requestVideoFrameCallback
    int nextFrameCb = 1;
    uint32_t lastShown = 0;
    bool sinkSet = false;
    bool eosSent = false;
    double eosDur = kNaN;
    std::shared_ptr<media::YtStream> yt;   // src="meros-yt:VIDEOID": YouTube yerel akisi
    bool ytBound = false;
    uint64_t ytGen = 0;
};

struct QEv {
    Node* node = nullptr;
    Obj* obj = nullptr;
    std::wstring type;
    std::function<void()> pre;   // olaydan hemen once (orn. updating=false)
};

} // namespace

struct MediaState {
    std::unordered_map<Obj*, std::unique_ptr<MSState>> ms;
    std::unordered_map<Obj*, std::unique_ptr<SBState>> sbs;
    std::unordered_map<Node*, std::unique_ptr<MediaEl>> els;
    std::deque<QEv> queue;
    Obj* msProto = nullptr;
    Obj* sbProto = nullptr;
    Obj* trProto = nullptr;
    Obj* textTracksProto = nullptr;
    bool ticking = false;
};

namespace {

MediaState& MS(Runtime& rt) {
    if (!rt.media) rt.media = std::make_shared<MediaState>();
    return *rt.media;
}

void Queue(Runtime& rt, Node* n, const std::wstring& type, std::function<void()> pre = nullptr) {
    QEv e;
    e.node = n;
    e.type = type;
    e.pre = std::move(pre);
    MS(rt).queue.push_back(std::move(e));
}
void QueueObj(Runtime& rt, Obj* o, const std::wstring& type, std::function<void()> pre = nullptr) {
    QEv e;
    e.obj = o;
    e.type = type;
    e.pre = std::move(pre);
    MS(rt).queue.push_back(std::move(e));
}

MediaEl* El(Runtime& rt, Node* n, bool create = true) {
    if (!IsMediaNode(n)) return nullptr;
    auto& m = MS(rt).els;
    auto it = m.find(n);
    if (it != m.end()) return it->second.get();
    if (!create) return nullptr;
    std::unique_ptr<MediaEl> e(new MediaEl());
    e->node = n;
    e->muted = n->Attr(L"muted") != nullptr;
    MediaEl* p = e.get();
    m[n] = std::move(e);
    return p;
}
MediaEl* ElOf(Runtime& rt, const Value& v) { return El(rt, NodeOf(v)); }

SBState* SBOf(Runtime& rt, const Value& v) {
    if (!v.IsObj()) return nullptr;
    auto& m = MS(rt).sbs;
    auto it = m.find(v.o);
    return it == m.end() ? nullptr : it->second.get();
}
MSState* MSOf(Runtime& rt, const Value& v) {
    if (!v.IsObj()) return nullptr;
    auto& m = MS(rt).ms;
    auto it = m.find(v.o);
    return it == m.end() ? nullptr : it->second.get();
}

// ---------------- TimeRanges ----------------

NATIVE(TR_start) {
    UNUSED;
    Value r = rt.Get(thisV, L"\x02r");
    double i = rt.ToNum(A(0));
    if (!r.IsObj() || i < 0 || std::isnan(i) || size_t(i) * 2 >= r.o->elems.size()) ThrowDOM(rt, L"IndexSizeError", L"The index provided is out of range.");
    return r.o->elems[size_t(i) * 2];
}
NATIVE(TR_end) {
    UNUSED;
    Value r = rt.Get(thisV, L"\x02r");
    double i = rt.ToNum(A(0));
    if (!r.IsObj() || i < 0 || std::isnan(i) || size_t(i) * 2 + 1 >= r.o->elems.size()) ThrowDOM(rt, L"IndexSizeError", L"The index provided is out of range.");
    return r.o->elems[size_t(i) * 2 + 1];
}
Value MakeTR(Runtime& rt, const Ranges& rs) {
    Obj* o = rt.NewObject(MS(rt).trProto);
    std::vector<Value> v;
    for (auto& r : rs) { v.push_back(Value::N(r.first)); v.push_back(Value::N(r.second)); }
    rt.DefineData(o, L"\x02r", Value::O(rt.NewArrayFrom(v)), false);
    rt.DefineData(o, L"length", Value::N(double(rs.size())), false);
    return Value::O(o);
}

// ---------------- Eleman yardimcilari ----------------

Ranges ElBuffered(MediaEl* e) {
    if (e->ms) {
        Ranges r;
        bool first = true;
        for (SBState* sb : e->ms->sbs) {
            Ranges b = sb->Buffered();
            r = first ? b : media::IntersectRanges(r, b);
            first = false;
        }
        // ended durumunda son aralik en uzun izin sonuna kadar uzar (spec)
        if (e->ms->readyState == L"ended" && !r.empty()) {
            double hi = r.back().second;
            for (SBState* sb : e->ms->sbs) { Ranges b = sb->Buffered(); if (!b.empty()) hi = std::max(hi, b.back().second); }
            r.back().second = hi;
        }
        return r;
    }
    if (e->pv && e->pa) return media::IntersectRanges(e->pv->Buffered(), e->pa->Buffered());
    if (e->pv) return e->pv->Buffered();
    if (e->pa) return e->pa->Buffered();
    return {};
}

double ElDuration(MediaEl* e) {
    if (e->ms) return e->ms->duration;
    return e->duration;
}

void SetDurationEl(Runtime& rt, MediaEl* e, double d) {
    (void)rt;
    if (e->ms) return;
    if (std::isnan(e->duration) && std::isnan(d)) return;
    if (e->duration == d) return;
    e->duration = d;
    Queue(rt, e->node, L"durationchange");
}

void EnsurePlayer(Runtime& rt, MediaEl* e) {
    if (!e->player) {
        e->player = std::make_shared<MediaPlayer>();
        e->player->SetVolume(e->volume, e->muted);
        e->player->SetRate(e->rate);
    }
    if (!e->sinkSet && rt.host && e->node->name == L"video") {
        auto sink = rt.host->VideoSink(e->node);
        if (sink) e->player->SetSink(sink);
        e->sinkSet = true;
    }
}

// MediaSource iz arabelleklerini oynaticiya bagla (ilk video ve ilk ses izi)
void BindTracks(Runtime& rt, MediaEl* e) {
    std::shared_ptr<TrackBuffer> v, a;
    if (e->ms) {
        for (SBState* sb : e->ms->sbs) {
            if (!v && sb->vtb) v = sb->vtb;
            if (!a && sb->atb) a = sb->atb;
        }
    } else {
        v = e->pv;
        a = e->pa;
    }
    if (!v && !a) return;
    EnsurePlayer(rt, e);
    e->player->SetTracks(v, a);
    if (!e->paused) e->player->Play();
}

void RejectPlayPromises(Runtime& rt, MediaEl* e, const std::wstring& name, const std::wstring& msg) {
    if (e->playPromises.empty()) return;
    Value ctor = rt.Get(Value::O(rt.global), L"DOMException");
    Value err;
    if (ctor.IsObj() && ctor.o->IsCallable()) {
        std::vector<Value> a{Value::S(msg), Value::S(name)};
        err = rt.Construct(ctor, a);
    }
    for (Obj* p : e->playPromises) rt.RejectPromise(p, err);
    e->playPromises.clear();
}
void ResolvePlayPromises(Runtime& rt, MediaEl* e) {
    for (Obj* p : e->playPromises) rt.ResolvePromise(p, Value());
    e->playPromises.clear();
}

void DetachSource(Runtime& rt, MediaEl* e) {
    if (e->ms) {
        MSState* ms = e->ms;
        ms->el = nullptr;
        if (ms->readyState != L"closed") {
            ms->readyState = L"closed";
            ms->duration = kNaN;
            for (SBState* sb : ms->sbs) sb->removed = true;
            ms->sbs.clear();
            if (ms->sbList) ms->sbList->elems.clear();
            if (ms->activeList) ms->activeList->elems.clear();
            QueueObj(rt, ms->obj, L"sourceclose");
        }
        e->ms = nullptr;
    }
    if (e->player) e->player->SetTracks(nullptr, nullptr);
    if (e->yt) { e->yt->Cancel(); e->yt.reset(); }
    e->ytBound = false;
    e->prog.reset();
    e->pv.reset();
    e->pa.reset();
    e->progEos = false;
    e->eosSent = false;
}

// ---------------- YouTube yerel akisi (src="meros-yt:VIDEOID") ----------------

// WinHTTP ile HTTP (YtStream'in ag erisimi). Basliklar: User-Agent / Content-Type ayri alanlara.
int YtHttp(const std::wstring& method, const std::wstring& url, const std::wstring& headers, const std::string& body,
           size_t maxBytes, const std::atomic<bool>*, std::string& out) {
    FetchOptions o;
    o.method = method;
    o.body = body;
    size_t i = 0;
    while (i < headers.size()) {
        size_t e = headers.find(L"\r\n", i);
        if (e == std::wstring::npos) e = headers.size();
        std::wstring line = headers.substr(i, e - i);
        i = e + 2;
        if (line.empty()) continue;
        if (StartsWith(line, L"User-Agent: ")) o.userAgent = line.substr(12);
        else if (StartsWith(line, L"Content-Type: ")) o.contentType = line.substr(14);
        else o.extraHeaders += line + L"\r\n";
    }
    FetchResult f = Fetch(url, maxBytes, nullptr, 0, &o);
    if (!f.ok) return 0;
    out.swap(f.body);
    return int(f.status);
}

void StartYouTube(Runtime& rt, MediaEl* e, const std::wstring& spec) {
    std::wstring id = spec.substr(0, spec.find(L'?'));
    bool okId = id.size() == 11;
    for (wchar_t c : id) if (!(iswalnum(c) || c == L'-' || c == L'_')) okId = false;
    if (!okId || !rt.host || !rt.host->YouTubeStreamAllowed()) {
        e->errorCode = 4;
        e->errorMsg = L"meros-yt: yalnizca merOS YouTube sayfalarinda";
        e->networkState = 3;
        Queue(rt, e->node, L"error");
        return;
    }
    media::YtOptions o;
    o.videoId = id;
    // en yuksek cozunurluk: data-max-height (sayfa gorunur alana gore verir), 144..1080
    if (const std::wstring* mh = e->node->Attr(L"data-max-height")) o.maxHeight = std::max(144, std::min(1080, _wtoi(mh->c_str())));
    if (const std::wstring* hl = e->node->Attr(L"data-hl")) if (hl->size() == 2) o.hl = *hl;
    if (const std::wstring* gl = e->node->Attr(L"data-gl")) if (gl->size() == 2) o.gl = *gl;
    // testler icin: yerel sahte YouTube sunucusu
    wchar_t api[512];
    DWORD an = GetEnvironmentVariableW(L"MEROS_YT_API", api, 512);
    if (an > 0 && an < 512) { o.apiBase = api; o.wwwBase = api; }
    e->yt = std::make_shared<media::YtStream>(o, YtHttp);
    e->ytBound = false;
    e->yt->Start();
}

void DoSeek(Runtime& rt, MediaEl* e, double t);

void TickYouTube(Runtime& rt, MediaEl* e) {
    media::YtStream& y = *e->yt;
    media::YtStream::State st = y.GetState();
    if (st == media::YtStream::Failed) {
        if (e->errorCode == 0) {
            e->errorCode = e->ytBound ? 2 : 4;
            e->errorMsg = y.Error();
            e->networkState = 3;
            Queue(rt, e->node, L"error");
        }
        return;
    }
    if (st != media::YtStream::Ready) return;
    double dur = y.Duration();
    if (!e->ytBound) {
        e->ytBound = true;
        e->pv = y.Video();
        e->pa = y.Audio();
        if (e->pv) { TrackConfig c = e->pv->Config(); e->vw = c.width; e->vh = c.height; }
        SetDurationEl(rt, e, dur > 0 ? dur : kInf);
        BindTracks(rt, e);
        y.Seek(e->time);
        e->player->Seek(e->time);
        e->networkState = 2;
    }
    // baslangic boslugu: ilk ornek biraz ileride (B-kare gecikmesi) ise oraya atla (tarayicilarin "gap jumping"i)
    if (e->readyState < 2 && !e->seeking) {
        Ranges b = ElBuffered(e);
        if (!b.empty() && b[0].first > e->time + 0.1 && b[0].first - e->time < 1.5) DoSeek(rt, e, b[0].first + 0.001);
    }
    y.SetPlayhead(e->time);
    bool eos = y.Eos();
    if (eos != e->progEos) {
        e->progEos = eos;
        if (e->player) e->player->SetEndOfStream(eos, dur);
        if (eos) { e->networkState = 1; Queue(rt, e->node, L"suspend"); }
        else e->networkState = 2;
    }
}

// "media element load algorithm" (basitlestirilmis)
void LoadEl(Runtime& rt, MediaEl* e) {
    bool hadSomething = e->networkState != 0 || e->readyState != 0 || !e->currentSrc.empty();
    RejectPlayPromises(rt, e, L"AbortError", L"The play() request was interrupted by a new load request.");
    DetachSource(rt, e);
    if (hadSomething) {
        Queue(rt, e->node, L"emptied");
        if (!e->paused) { e->paused = true; Queue(rt, e->node, L"pause"); }
    }
    if (e->player) { e->player->Pause(); e->player->Seek(0); }
    e->readyState = 0;
    e->networkState = 0;
    e->seeking = false;
    e->ended = false;
    e->time = 0;
    e->duration = kNaN;
    e->loadedMeta = e->loadedData = false;
    e->errorCode = 0;
    e->paused = true;
    e->autoplaying = true;
    e->played.clear();
    e->vw = e->vh = 0;
    e->currentSrc.clear();
    e->rate = e->defaultRate;
    if (e->player) e->player->SetRate(e->rate);

    // kaynak sec: src ozniteligi ya da <source> cocuklari
    std::wstring url;
    if (auto* s = e->node->Attr(L"src")) url = *s;
    else {
        for (auto& k : e->node->kids) {
            Node* c = k.get();
            if (c->kind != Node::Elem || c->name != L"source") continue;
            auto* ss = c->Attr(L"src");
            if (!ss) continue;
            if (auto* ty = c->Attr(L"type")) if (media::SupportLevel(*ty) == 0) continue;
            url = *ss;
            break;
        }
    }
    if (url.empty()) { e->networkState = 3; return; }   // NETWORK_NO_SOURCE (isaretsiz bekler)
    if (!StartsWith(url, L"blob:") && !StartsWith(url, L"meros-yt:") && rt.host) url = rt.host->Resolve(url);
    e->currentSrc = url;
    e->networkState = 2;   // NETWORK_LOADING
    Queue(rt, e->node, L"loadstart");

    if (StartsWith(url, L"blob:")) {
        auto it = rt.objectUrls.find(url);
        Obj* target = it != rt.objectUrls.end() ? it->second : nullptr;
        MSState* ms = target ? MSOf(rt, Value::O(target)) : nullptr;
        if (ms) {
            if (ms->readyState != L"closed") {
                e->errorCode = 4;
                e->networkState = 3;
                Queue(rt, e->node, L"error");
                return;
            }
            e->ms = ms;
            ms->el = e;
            ms->readyState = L"open";
            QueueObj(rt, ms->obj, L"sourceopen");
            return;
        }
        if (target && target->x && target->x->bufferData) {
            // Blob: bellekten ilerlemeli oynatma
            auto pl = std::make_shared<ProgLoad>();
            std::vector<uint8_t> bytes = *target->x->bufferData;
            pl->ok = Mp4Parser::ParseFile(bytes.data(), bytes.size(), pl->tracks, pl->samples, pl->err);
            if (!pl->ok) pl->errCode = 4;
            pl->done = true;
            e->prog = pl;
            return;
        }
        e->errorCode = 4;
        e->networkState = 3;
        Queue(rt, e->node, L"error");
        return;
    }
    if (StartsWith(url, L"meros-yt:")) {
        StartYouTube(rt, e, url.substr(9));
        return;
    }
    // ag: arka planda indir, sonra ayristir
    if (rt.host && !rt.host->CheckRequest(url)) {
        e->errorCode = 2;
        e->networkState = 3;
        Queue(rt, e->node, L"error");
        return;
    }
    auto pl = std::make_shared<ProgLoad>();
    e->prog = pl;
    std::wstring docUrl = rt.host ? rt.host->PageUrl() : std::wstring();
    std::thread([pl, url, docUrl]() {
        FetchOptions o = SubresourceOpts(docUrl, url);
        FetchResult f = Fetch(url, 512u * 1024 * 1024, nullptr, 0, &o);
        std::vector<TrackConfig> tracks;
        std::vector<media::Sample> samples;
        std::string err;
        bool ok = f.ok && f.status < 400 && !f.truncated;
        int code = ok ? 0 : 2;
        if (ok) {
            ok = Mp4Parser::ParseFile(reinterpret_cast<const uint8_t*>(f.body.data()), f.body.size(), tracks, samples, err);
            if (!ok) code = 4;
        }
        std::lock_guard<std::mutex> g(pl->mu);
        pl->ok = ok;
        pl->errCode = code;
        pl->err = err;
        pl->tracks = std::move(tracks);
        pl->samples = std::move(samples);
        pl->done = true;
    }).detach();
}

// Ornekleri saniyeye cevirip iz arabellegine koy
void InsertSamples(std::shared_ptr<TrackBuffer>& tb, const TrackConfig& tc, std::vector<media::Sample>& samples, uint32_t id,
                   double offset, double winStart, double winEnd, double& groupEnd) {
    std::vector<BufSample> g;
    double ts = tc.timescale ? double(tc.timescale) : 1.0;
    for (auto& s : samples) {
        if (s.trackId != id) continue;
        BufSample b;
        b.pts = double(s.pts) / ts + offset;
        b.dts = double(s.dts) / ts + offset;
        b.dur = double(s.dur) / ts;
        b.key = s.key;
        b.data = s.data;
        if (b.pts < winStart - 1e-6 || b.pts + b.dur > winEnd + 1e-6) continue;
        groupEnd = std::max(groupEnd, b.pts + b.dur);
        g.push_back(std::move(b));
    }
    if (!g.empty() && tb) tb->Insert(g);
}

void ApplyProgressive(Runtime& rt, MediaEl* e) {
    std::shared_ptr<ProgLoad> pl = e->prog;
    {
        std::lock_guard<std::mutex> g(pl->mu);
        if (!pl->done) return;
    }
    e->prog.reset();
    if (!pl->ok) {
        e->errorCode = pl->errCode ? pl->errCode : 4;
        e->errorMsg = std::wstring(pl->err.begin(), pl->err.end());
        e->networkState = 3;
        Queue(rt, e->node, L"error");
        return;
    }
    double dur = 0;
    for (auto& t : pl->tracks) {
        if (t.codec != media::Codec::H264 && t.codec != media::Codec::AAC) continue;
        if (t.video && !e->pv) {
            e->pv = std::make_shared<TrackBuffer>(true);
            e->pv->SetConfig(t);
            double ge = 0;
            InsertSamples(e->pv, t, pl->samples, t.trackId, 0, 0, kInf, ge);
            dur = std::max(dur, ge);
        } else if (!t.video && !e->pa) {
            e->pa = std::make_shared<TrackBuffer>(false);
            e->pa->SetConfig(t);
            double ge = 0;
            InsertSamples(e->pa, t, pl->samples, t.trackId, 0, 0, kInf, ge);
            dur = std::max(dur, ge);
        }
    }
    if (!e->pv && !e->pa) {
        e->errorCode = 4;
        e->networkState = 3;
        Queue(rt, e->node, L"error");
        return;
    }
    e->networkState = 1;
    e->progEos = true;
    if (e->pv) { TrackConfig c = e->pv->Config(); e->vw = c.width; e->vh = c.height; }
    SetDurationEl(rt, e, dur);
    BindTracks(rt, e);
    e->player->SetEndOfStream(true, dur);
    e->player->Seek(e->time);
    Queue(rt, e->node, L"progress");
    Queue(rt, e->node, L"suspend");
}

// readyState ve ilgili olaylar
void UpdateReadyState(Runtime& rt, MediaEl* e) {
    bool haveMeta;
    if (e->ms) {
        haveMeta = !e->ms->sbs.empty();
        for (SBState* sb : e->ms->sbs) if (!sb->gotInit) haveMeta = false;
    } else haveMeta = e->pv || e->pa;
    int rs = 0;
    if (haveMeta) {
        rs = 1;
        Ranges b = ElBuffered(e);
        double t = e->time;
        double dur = ElDuration(e);
        bool msEnded = (e->ms && e->ms->readyState == L"ended") || e->progEos;
        for (auto& r : b) {
            if (t >= r.first - 0.15 && t < r.second) {
                rs = 2;
                double ahead = r.second - t;
                if (ahead > 0.3 || msEnded || (!std::isnan(dur) && r.second >= dur - 0.1)) rs = 4;
                break;
            }
        }
        if (rs < 2 && msEnded && !b.empty() && t >= b.back().second - 0.15) rs = 4;
        if (e->seeking && rs > 1) rs = 1;
    }
    int old = e->readyState;
    if (rs == old) return;
    e->readyState = rs;
    if (old < 1 && rs >= 1) {
        if (!e->loadedMeta) {
            e->loadedMeta = true;
            if (e->ms && std::isnan(e->ms->duration)) e->ms->duration = kInf;
            Queue(rt, e->node, L"durationchange");
            Queue(rt, e->node, L"loadedmetadata");
        }
    }
    if (old < 2 && rs >= 2 && !e->loadedData) {
        e->loadedData = true;
        Queue(rt, e->node, L"loadeddata");
    }
    if (old < 3 && rs >= 3) {
        Queue(rt, e->node, L"canplay");
        if (!e->paused) Queue(rt, e->node, L"playing");
    }
    if (old < 4 && rs >= 4) Queue(rt, e->node, L"canplaythrough");
    if (old >= 3 && rs < 3 && !e->paused && !e->ended) {
        Queue(rt, e->node, L"timeupdate");
        Queue(rt, e->node, L"waiting");
    }
    // otomatik oynatma
    if (rs >= 4 && e->paused && e->autoplaying && e->node->Attr(L"autoplay")) {
        e->paused = false;
        e->autoplaying = false;
        Queue(rt, e->node, L"play");
        Queue(rt, e->node, L"playing");
        if (e->player) e->player->Play();
    }
}

void DoSeek(Runtime& rt, MediaEl* e, double t) {
    if (e->yt) e->yt->Seek(std::max(0.0, t));
    if (e->readyState == 0 && !e->ms && !e->pv && !e->pa) { e->time = std::max(0.0, t); return; }
    double dur = ElDuration(e);
    if (!std::isnan(dur) && !std::isinf(dur) && t > dur) t = dur;
    if (t < 0) t = 0;
    e->seeking = true;
    e->ended = false;
    e->time = t;
    EnsurePlayer(rt, e);
    e->seekBase = e->player->Status().seekSerial;
    e->player->Seek(t);
    Queue(rt, e->node, L"seeking");
    if (e->readyState > 1) e->readyState = 1;
}

void PlayEl(Runtime& rt, MediaEl* e) {
    e->autoplaying = false;
    if (e->networkState == 0) LoadEl(rt, e);
    if (e->ended || (e->time >= ElDuration(e) - 0.01 && !std::isnan(ElDuration(e)) && ElDuration(e) > 0 && !std::isinf(ElDuration(e)))) {
        DoSeek(rt, e, 0);
    }
    if (e->paused) {
        e->paused = false;
        Queue(rt, e->node, L"play");
        if (e->readyState >= 3) Queue(rt, e->node, L"playing");
        else Queue(rt, e->node, L"waiting");
    }
    if (e->player) e->player->Play();
    e->playStart = e->time;
}

void PauseEl(Runtime& rt, MediaEl* e) {
    if (e->networkState == 0) LoadEl(rt, e);
    e->autoplaying = false;
    if (!e->paused) {
        e->paused = true;
        if (e->player) { e->player->Pause(); }
        Queue(rt, e->node, L"timeupdate");
        Queue(rt, e->node, L"pause");
        RejectPlayPromises(rt, e, L"AbortError", L"The play() request was interrupted by a call to pause().");
    }
}

// ---------------- HTMLMediaElement ----------------

#define MEL MediaEl* e = ElOf(rt, thisV); if (!e) return Value();

NATIVE(M_play) {
    UNUSED;
    Obj* p = rt.NewPromise();
    MediaEl* e = ElOf(rt, thisV);
    if (!e) { rt.ResolvePromise(p, Value()); return Value::O(p); }
    if (e->errorCode == 4) {
        Value ctor = rt.Get(Value::O(rt.global), L"DOMException");
        std::vector<Value> a{Value::S(L"The element has no supported sources."), Value::S(L"NotSupportedError")};
        rt.RejectPromise(p, ctor.IsObj() ? rt.Construct(ctor, a) : Value());
        return Value::O(p);
    }
    PlayEl(rt, e);
    if (e->readyState >= 3) rt.ResolvePromise(p, Value());
    else e->playPromises.push_back(p);
    return Value::O(p);
}
NATIVE(M_pause) { UNUSED; MEL; PauseEl(rt, e); return Value(); }
NATIVE(M_load) { UNUSED; MEL; LoadEl(rt, e); return Value(); }
NATIVE(M_canPlayType) {
    UNUSED;
    int l = media::SupportLevel(rt.ToStr(A(0)));
    return Value::S(l == 2 ? L"probably" : l == 1 ? L"maybe" : L"");
}
NATIVE(M_currentTime) { UNUSED; MEL; return Value::N(e->time); }
NATIVE(M_setCurrentTime) {
    UNUSED;
    MEL;
    double t = rt.ToNum(A(0));
    if (std::isnan(t) || std::isinf(t)) rt.Throw(L"TypeError", L"Failed to set the 'currentTime' property on 'HTMLMediaElement': The provided double value is non-finite.");
    DoSeek(rt, e, t);
    return Value();
}
NATIVE(M_fastSeek) { UNUSED; MEL; DoSeek(rt, e, rt.ToNum(A(0))); return Value(); }
NATIVE(M_duration) { UNUSED; MEL; return Value::N(ElDuration(e)); }
NATIVE(M_paused) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::B(e ? e->paused : true); }
NATIVE(M_ended) {
    UNUSED;
    MEL;
    return Value::B(e->ended);
}
NATIVE(M_seeking) { UNUSED; MEL; return Value::B(e->seeking); }
NATIVE(M_readyState) { UNUSED; MEL; return Value::N(e->readyState); }
NATIVE(M_networkState) { UNUSED; MEL; return Value::N(e->networkState); }
NATIVE(M_buffered) { UNUSED; MEL; return MakeTR(rt, ElBuffered(e)); }
NATIVE(M_seekable) {
    UNUSED;
    MEL;
    double d = ElDuration(e);
    if (std::isnan(d) || e->readyState == 0) return MakeTR(rt, {});
    if (std::isinf(d)) {
        if (e->ms && !e->ms->liveSeekable.empty()) return MakeTR(rt, e->ms->liveSeekable);
        Ranges b = ElBuffered(e);
        if (b.empty()) return MakeTR(rt, {});
        return MakeTR(rt, {{b.front().first, b.back().second}});
    }
    return MakeTR(rt, {{0, d}});
}
NATIVE(M_played) { UNUSED; MEL; return MakeTR(rt, e->played); }
NATIVE(M_volume) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e ? e->volume : 1); }
NATIVE(M_setVolume) {
    UNUSED;
    MEL;
    double v = rt.ToNum(A(0));
    if (std::isnan(v) || v < 0 || v > 1) ThrowDOM(rt, L"IndexSizeError", L"The volume provided (" + rt.ToStr(A(0)) + L") is outside the range [0, 1].");
    if (v != e->volume) { e->volume = v; if (e->player) e->player->SetVolume(e->volume, e->muted); Queue(rt, e->node, L"volumechange"); }
    return Value();
}
NATIVE(M_muted) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::B(e ? e->muted : false); }
NATIVE(M_setMuted) {
    UNUSED;
    MEL;
    bool m = rt.Truthy(A(0));
    if (m != e->muted) { e->muted = m; if (e->player) e->player->SetVolume(e->volume, e->muted); Queue(rt, e->node, L"volumechange"); }
    return Value();
}
NATIVE(M_defaultMuted) { UNUSED; Node* n = NodeOf(thisV); return Value::B(n && n->Attr(L"muted")); }
NATIVE(M_rate) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e ? e->rate : 1); }
NATIVE(M_setRate) {
    UNUSED;
    MEL;
    double r = rt.ToNum(A(0));
    if (std::isnan(r) || std::isinf(r)) rt.Throw(L"TypeError", L"The provided double value is non-finite.");
    if (r < 0.0625 || r > 16) ThrowDOM(rt, L"NotSupportedError", L"The provided playback rate (" + rt.ToStr(A(0)) + L") is not in the supported playback range.");
    if (r != e->rate) { e->rate = r; if (e->player) e->player->SetRate(r); Queue(rt, e->node, L"ratechange"); }
    return Value();
}
NATIVE(M_defaultRate) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e ? e->defaultRate : 1); }
NATIVE(M_setDefaultRate) {
    UNUSED;
    MEL;
    double r = rt.ToNum(A(0));
    if (!std::isnan(r) && r != e->defaultRate) { e->defaultRate = r; Queue(rt, e->node, L"ratechange"); }
    return Value();
}
NATIVE(M_videoWidth) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e && e->readyState >= 1 ? e->vw : 0); }
NATIVE(M_videoHeight) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e && e->readyState >= 1 ? e->vh : 0); }
NATIVE(M_currentSrc) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::S(e ? e->currentSrc : L""); }
NATIVE(M_error) {
    UNUSED;
    MediaEl* e = ElOf(rt, thisV);
    if (!e || !e->errorCode) return Value::Null_();
    Obj* o = rt.NewObject();
    rt.DefineData(o, L"code", Value::N(e->errorCode));
    std::wstring msg = e->errorMsg;
    if (msg.empty()) msg = e->errorCode == 2 ? L"MEDIA_ERR_NETWORK" : e->errorCode == 3 ? L"MEDIA_ERR_DECODE" : L"MEDIA_ELEMENT_ERROR: Format error";
    rt.DefineData(o, L"message", Value::S(msg));
    for (auto& kv : std::vector<std::pair<const wchar_t*, double>>{{L"MEDIA_ERR_ABORTED", 1}, {L"MEDIA_ERR_NETWORK", 2}, {L"MEDIA_ERR_DECODE", 3}, {L"MEDIA_ERR_SRC_NOT_SUPPORTED", 4}})
        rt.DefineData(o, kv.first, Value::N(kv.second), false);
    return Value::O(o);
}
NATIVE(M_srcObject) {
    UNUSED;
    MediaEl* e = ElOf(rt, thisV);
    if (!e || !e->ms) return Value::Null_();
    return Value::O(e->ms->obj);
}
NATIVE(M_setSrcObject) {
    UNUSED;
    MEL;
    MSState* ms = MSOf(rt, A(0));
    if (!ms) { LoadEl(rt, e); return Value(); }
    // MediaSource dogrudan: gecici blob URL uzerinden ayni yol
    std::wstring url = L"blob:merjs/srcobject-" + std::to_wstring(rt.nextBlobId++);
    rt.objectUrls[url] = ms->obj;
    e->node->attrs.erase(std::remove_if(e->node->attrs.begin(), e->node->attrs.end(), [](const std::pair<std::wstring, std::wstring>& a) { return a.first == L"src"; }), e->node->attrs.end());
    e->node->attrs.emplace_back(L"src", url);
    LoadEl(rt, e);
    return Value();
}
NATIVE(M_preload) { UNUSED; Node* n = NodeOf(thisV); auto* p = n ? n->Attr(L"preload") : nullptr; return Value::S(p ? *p : L"auto"); }
NATIVE(M_poster) { UNUSED; Node* n = NodeOf(thisV); auto* p = n ? n->Attr(L"poster") : nullptr; return Value::S(p ? (rt.host ? rt.host->Resolve(*p) : *p) : L""); }
NATIVE(M_quality) {
    UNUSED;
    Obj* o = rt.NewObject();
    MediaEl* e = ElOf(rt, thisV);
    PlayerStatus st;
    if (e && e->player) st = e->player->Status();
    rt.DefineData(o, L"creationTime", Value::N(rt.Now()));
    rt.DefineData(o, L"totalVideoFrames", Value::N(st.framesDecoded));
    rt.DefineData(o, L"droppedVideoFrames", Value::N(st.framesDropped));
    rt.DefineData(o, L"corruptedVideoFrames", Value::N(0));
    return Value::O(o);
}
NATIVE(M_decodedFrames) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e && e->player ? e->player->Status().framesDecoded : 0); }
NATIVE(M_droppedFrames) { UNUSED; MediaEl* e = ElOf(rt, thisV); return Value::N(e && e->player ? e->player->Status().framesDropped : 0); }
NATIVE(M_rvfc) {
    UNUSED;
    MEL;
    int id = e->nextFrameCb++;
    if (A(0).IsObj() && A(0).o->IsCallable()) e->frameCallbacks.push_back({id, A(0)});
    return Value::N(id);
}
NATIVE(M_cancelRvfc) {
    UNUSED;
    MEL;
    int id = int(rt.ToNum(A(0)));
    e->frameCallbacks.erase(std::remove_if(e->frameCallbacks.begin(), e->frameCallbacks.end(), [&](const std::pair<int, Value>& p) { return p.first == id; }), e->frameCallbacks.end());
    return Value();
}
NATIVE(M_setMediaKeys) {
    UNUSED;
    Obj* p = rt.NewPromise();
    if (A(0).t == Value::Null || A(0).IsUndef()) rt.ResolvePromise(p, Value());
    else {
        Value ctor = rt.Get(Value::O(rt.global), L"DOMException");
        std::vector<Value> a{Value::S(L"DRM (EME) desteklenmiyor"), Value::S(L"NotSupportedError")};
        rt.RejectPromise(p, ctor.IsObj() ? rt.Construct(ctor, a) : Value());
    }
    return Value::O(p);
}
NATIVE(M_textTracks) {
    UNUSED;
    Node* n = NodeOf(thisV);
    if (!n) return Value();
    Value cached = rt.Get(thisV, L"\x02textTracks");
    if (cached.IsObj()) return cached;
    Obj* o = rt.NewObject(MS(rt).textTracksProto);
    rt.DefineData(o, L"length", Value::N(0));
    rt.DefineData(thisV.o, L"\x02textTracks", Value::O(o), false);
    return Value::O(o);
}
NATIVE(TT_getTrackById) { UNUSED; return Value::Null_(); }
NATIVE(M_addTextTrack) {
    UNUSED;
    Obj* t = rt.NewObject(rt.eventTargetProto);
    rt.DefineData(t, L"kind", Value::S(A(0).IsUndef() ? L"subtitles" : rt.ToStr(A(0))));
    rt.DefineData(t, L"label", Value::S(A(1).IsUndef() ? L"" : rt.ToStr(A(1))));
    rt.DefineData(t, L"language", Value::S(A(2).IsUndef() ? L"" : rt.ToStr(A(2))));
    rt.DefineData(t, L"mode", Value::S(L"hidden"));
    rt.DefineData(t, L"cues", Value::O(rt.NewArray(0)));
    rt.DefineData(t, L"activeCues", Value::O(rt.NewArray(0)));
    rt.DefineMethod(t, L"addCue", [](Runtime& r, Obj*, const Value& tv, std::vector<Value>& a) {
        Value c = r.Get(tv, L"cues");
        if (c.IsObj() && !a.empty()) c.o->elems.push_back(a[0]);
        return Value();
    }, 1);
    rt.DefineMethod(t, L"removeCue", [](Runtime& r, Obj*, const Value& tv, std::vector<Value>& a) {
        Value c = r.Get(tv, L"cues");
        if (c.IsObj() && !a.empty()) {
            auto& el = c.o->elems;
            for (size_t i = 0; i < el.size(); i++) if (el[i].IsObj() && a[0].IsObj() && el[i].o == a[0].o) { el.erase(el.begin() + long(i)); break; }
        }
        return Value();
    }, 1);
    return Value::O(t);
}

// ---------------- MediaSource ----------------

NATIVE(MS_isTypeSupported) { UNUSED; return Value::B(media::SupportLevel(rt.ToStr(A(0))) >= 1); }

NATIVE(MS_ctor) {
    UNUSED;
    Obj* proto = MS(rt).msProto;
    if (rt.nativeNewTarget) {
        Value p = rt.Get(Value::O(rt.nativeNewTarget), L"prototype");
        if (p.IsObj()) proto = p.o;
    }
    Obj* o = rt.NewObject(proto);
    o->X();
    std::unique_ptr<MSState> st(new MSState());
    st->obj = o;
    st->sbList = rt.NewArray(0);
    st->activeList = rt.NewArray(0);
    MS(rt).ms[o] = std::move(st);
    return Value::O(o);
}

MSState* MSThis(Runtime& rt, const Value& thisV) {
    MSState* m = MSOf(rt, thisV);
    if (!m) rt.Throw(L"TypeError", L"Illegal invocation");
    return m;
}

NATIVE(MS_readyState) { UNUSED; return Value::S(MSThis(rt, thisV)->readyState); }
NATIVE(MS_duration) { UNUSED; return Value::N(MSThis(rt, thisV)->duration); }
NATIVE(MS_setDuration) {
    UNUSED;
    MSState* m = MSThis(rt, thisV);
    double d = rt.ToNum(A(0));
    if (std::isnan(d) || d < 0) rt.Throw(L"TypeError", L"Failed to set the 'duration' property on 'MediaSource': The value provided is invalid.");
    if (m->readyState != L"open") ThrowDOM(rt, L"InvalidStateError", L"The MediaSource's readyState is not 'open'.");
    for (SBState* sb : m->sbs) if (sb->updating) ThrowDOM(rt, L"InvalidStateError", L"The 'updating' attribute is true on one or more of this MediaSource's SourceBuffers.");
    if (m->duration == d) return Value();
    m->duration = d;
    if (m->el) {
        if (m->el->player) m->el->player->SetEndOfStream(false, d);
        Queue(rt, m->el->node, L"durationchange");
    }
    return Value();
}
NATIVE(MS_sourceBuffers) { UNUSED; return Value::O(MSThis(rt, thisV)->sbList); }
NATIVE(MS_activeSourceBuffers) { UNUSED; return Value::O(MSThis(rt, thisV)->activeList); }

void RefreshLists(MSState* m) {
    m->sbList->elems.clear();
    m->activeList->elems.clear();
    for (SBState* sb : m->sbs) {
        m->sbList->elems.push_back(Value::O(sb->obj));
        m->activeList->elems.push_back(Value::O(sb->obj));
    }
}

NATIVE(MS_addSourceBuffer) {
    UNUSED;
    MSState* m = MSThis(rt, thisV);
    std::wstring type = rt.ToStr(A(0));
    if (type.empty()) rt.Throw(L"TypeError", L"Failed to execute 'addSourceBuffer' on 'MediaSource': The type provided is empty.");
    if (media::SupportLevel(type) != 2 && media::SupportLevel(type) != 1) ThrowDOM(rt, L"NotSupportedError", L"Failed to execute 'addSourceBuffer' on 'MediaSource': The type provided ('" + type + L"') is unsupported.");
    if (m->readyState != L"open") ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'addSourceBuffer' on 'MediaSource': The MediaSource's readyState is not 'open'.");
    if (m->sbs.size() >= 4) ThrowDOM(rt, L"QuotaExceededError", L"This MediaSource has reached the limit of SourceBuffer objects it can handle.");
    Obj* o = rt.NewObject(MS(rt).sbProto);
    o->X();
    std::unique_ptr<SBState> st(new SBState());
    st->obj = o;
    st->ms = m;
    st->mime = type;
    SBState* sb = st.get();
    MS(rt).sbs[o] = std::move(st);
    m->sbs.push_back(sb);
    RefreshLists(m);
    QueueObj(rt, m->sbList, L"addsourcebuffer");
    return Value::O(o);
}

NATIVE(MS_removeSourceBuffer) {
    UNUSED;
    MSState* m = MSThis(rt, thisV);
    SBState* sb = SBOf(rt, A(0));
    if (!sb) rt.Throw(L"TypeError", L"Failed to execute 'removeSourceBuffer' on 'MediaSource': parameter 1 is not of type 'SourceBuffer'.");
    auto it = std::find(m->sbs.begin(), m->sbs.end(), sb);
    if (it == m->sbs.end()) ThrowDOM(rt, L"NotFoundError", L"The SourceBuffer provided is not contained in this MediaSource.");
    if (sb->updating) {
        sb->updating = false;
        QueueObj(rt, sb->obj, L"abort");
        QueueObj(rt, sb->obj, L"updateend");
    }
    m->sbs.erase(it);
    sb->removed = true;
    RefreshLists(m);
    if (m->el) BindTracks(rt, m->el);
    QueueObj(rt, m->sbList, L"removesourcebuffer");
    return Value();
}

void EndOfStream(Runtime& rt, MSState* m, const std::wstring& err) {
    m->readyState = L"ended";
    QueueObj(rt, m->obj, L"sourceended");
    if (!err.empty()) {
        if (m->el) {
            m->el->errorCode = err == L"network" ? 2 : 3;
            Queue(rt, m->el->node, L"error");
        }
        return;
    }
    // sure = en yuksek tampon sonu
    double hi = 0;
    for (SBState* sb : m->sbs) { Ranges b = sb->Buffered(); if (!b.empty()) hi = std::max(hi, b.back().second); }
    if (hi > 0 && (std::isnan(m->duration) || std::isinf(m->duration) || std::fabs(hi - m->duration) > 1e-6)) {
        m->duration = hi;
        if (m->el) Queue(rt, m->el->node, L"durationchange");
    }
    if (m->el && m->el->player) m->el->player->SetEndOfStream(true, m->duration);
}

NATIVE(MS_endOfStream) {
    UNUSED;
    MSState* m = MSThis(rt, thisV);
    if (m->readyState != L"open") ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'endOfStream' on 'MediaSource': The MediaSource's readyState is not 'open'.");
    for (SBState* sb : m->sbs) if (sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'endOfStream' on 'MediaSource': The 'updating' attribute is true on one or more of this MediaSource's SourceBuffers.");
    std::wstring err = A(0).IsUndef() ? L"" : rt.ToStr(A(0));
    if (!err.empty() && err != L"network" && err != L"decode") rt.Throw(L"TypeError", L"The provided value '" + err + L"' is not a valid enum value of type EndOfStreamError.");
    EndOfStream(rt, m, err);
    return Value();
}
NATIVE(MS_setLiveSeekableRange) {
    UNUSED;
    MSState* m = MSThis(rt, thisV);
    double s = rt.ToNum(A(0)), e = rt.ToNum(A(1));
    if (m->readyState != L"open") ThrowDOM(rt, L"InvalidStateError", L"The MediaSource's readyState is not 'open'.");
    if (std::isnan(s) || std::isnan(e) || s < 0 || s > e) rt.Throw(L"TypeError", L"invalid range");
    m->liveSeekable = {{s, e}};
    return Value();
}
NATIVE(MS_clearLiveSeekableRange) { UNUSED; MSThis(rt, thisV)->liveSeekable.clear(); return Value(); }

// ---------------- SourceBuffer ----------------

SBState* SBThis(Runtime& rt, const Value& thisV) {
    SBState* s = SBOf(rt, thisV);
    if (!s) rt.Throw(L"TypeError", L"Illegal invocation");
    return s;
}

NATIVE(SB_updating) { UNUSED; return Value::B(SBThis(rt, thisV)->updating); }
NATIVE(SB_buffered) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    if (sb->removed) ThrowDOM(rt, L"InvalidStateError", L"This SourceBuffer has been removed from the parent media source.");
    return MakeTR(rt, sb->Buffered());
}
NATIVE(SB_mode) { UNUSED; return Value::S(SBThis(rt, thisV)->mode); }
NATIVE(SB_setMode) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    std::wstring m = rt.ToStr(A(0));
    if (m != L"segments" && m != L"sequence") return Value();
    if (sb->removed || sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to set the 'mode' property on 'SourceBuffer'.");
    sb->mode = m;
    if (m == L"sequence") sb->groupStartPending = true;
    return Value();
}
NATIVE(SB_tsOffset) { UNUSED; return Value::N(SBThis(rt, thisV)->timestampOffset); }
NATIVE(SB_setTsOffset) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    double v = rt.ToNum(A(0));
    if (std::isnan(v) || std::isinf(v)) rt.Throw(L"TypeError", L"The provided double value is non-finite.");
    if (sb->removed || sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to set the 'timestampOffset' property on 'SourceBuffer': This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
    sb->timestampOffset = v;
    if (sb->mode == L"sequence") sb->groupStartPending = true;
    return Value();
}
NATIVE(SB_awStart) { UNUSED; return Value::N(SBThis(rt, thisV)->appendWindowStart); }
NATIVE(SB_setAwStart) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    double v = rt.ToNum(A(0));
    if (sb->removed || sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to set the 'appendWindowStart' property on 'SourceBuffer'.");
    if (std::isnan(v) || v < 0 || v >= sb->appendWindowEnd) rt.Throw(L"TypeError", L"Failed to set the 'appendWindowStart' property on 'SourceBuffer': The value provided is invalid.");
    sb->appendWindowStart = v;
    return Value();
}
NATIVE(SB_awEnd) { UNUSED; return Value::N(SBThis(rt, thisV)->appendWindowEnd); }
NATIVE(SB_setAwEnd) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    double v = rt.ToNum(A(0));
    if (sb->removed || sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to set the 'appendWindowEnd' property on 'SourceBuffer'.");
    if (std::isnan(v) || v <= sb->appendWindowStart) rt.Throw(L"TypeError", L"Failed to set the 'appendWindowEnd' property on 'SourceBuffer': The value provided is invalid.");
    sb->appendWindowEnd = v;
    return Value();
}

void AppendError(Runtime& rt, SBState* sb, const std::string& why) {
    sb->parser.ResetParserState();
    if (rt.host) rt.host->Log(1, L"[MSE] appendBuffer hatasi: " + std::wstring(why.begin(), why.end()));
    QueueObj(rt, sb->obj, L"error", [sb]() { sb->updating = false; });
    QueueObj(rt, sb->obj, L"updateend");
    if (sb->ms && sb->ms->readyState == L"open") {
        MSState* m = sb->ms;
        // spec: "end of stream" algoritmasi, hata = decode
        m->readyState = L"ended";
        QueueObj(rt, m->obj, L"sourceended");
        if (m->el) { m->el->errorCode = 3; m->el->errorMsg = std::wstring(why.begin(), why.end()); Queue(rt, m->el->node, L"error"); }
    }
}

NATIVE(SB_appendBuffer) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    Value a0 = A(0);
    const uint8_t* ptr = nullptr;
    size_t len = 0;
    if (a0.IsObj() && a0.o->x && a0.o->cls == Cls::ArrayBuffer) {
        ptr = a0.o->x->bufferData ? a0.o->x->bufferData->data() : nullptr;
        len = a0.o->x->bufferData ? a0.o->x->bufferData->size() : 0;
    } else if (a0.IsObj() && a0.o->x && (a0.o->cls == Cls::TypedArray || a0.o->cls == Cls::DataView)) {
        if (a0.o->x->bufferData && a0.o->x->byteOffset + a0.o->x->byteLength <= a0.o->x->bufferData->size()) {
            ptr = a0.o->x->bufferData->data() + a0.o->x->byteOffset;
            len = a0.o->x->byteLength;
        }
    } else {
        rt.Throw(L"TypeError", L"Failed to execute 'appendBuffer' on 'SourceBuffer': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
    }
    if (sb->removed) ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'appendBuffer' on 'SourceBuffer': This SourceBuffer has been removed from the parent media source.");
    if (sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'appendBuffer' on 'SourceBuffer': This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
    MSState* m = sb->ms;
    if (m->el && m->el->errorCode) ThrowDOM(rt, L"InvalidStateError", L"The HTMLMediaElement.error attribute is not null.");
    if (m->readyState == L"ended") {
        m->readyState = L"open";
        QueueObj(rt, m->obj, L"sourceopen");
        if (m->el && m->el->player) m->el->player->SetEndOfStream(false, m->duration);
    }
    size_t used = (sb->vtb ? sb->vtb->Bytes() : 0) + (sb->atb ? sb->atb->Bytes() : 0);
    if (used + len > kQuota) ThrowDOM(rt, L"QuotaExceededError", L"Failed to execute 'appendBuffer' on 'SourceBuffer': The SourceBuffer is full, and cannot free space to append additional buffers.");

    sb->updating = true;
    QueueObj(rt, sb->obj, L"updatestart");

    if (ptr && len) {
        if (!sb->parser.Append(ptr, len)) { AppendError(rt, sb, sb->parser.error); return Value(); }
        if (sb->parser.initChanged) {
            sb->parser.initChanged = false;
            bool any = false;
            for (auto& t : sb->parser.tracks) {
                if (t.codec != media::Codec::H264 && t.codec != media::Codec::AAC) {
                    if (t.codec == media::Codec::Unsupported) { AppendError(rt, sb, "desteklenmeyen kodek: " + t.fourcc); return Value(); }
                    continue;
                }
                if (t.video) {
                    if (!sb->vtb) sb->vtb = std::make_shared<TrackBuffer>(true);
                    sb->vtb->SetConfig(t);
                    sb->vId = t.trackId;
                    sb->vcfg = t;
                    any = true;
                } else {
                    if (!sb->atb) sb->atb = std::make_shared<TrackBuffer>(false);
                    sb->atb->SetConfig(t);
                    sb->aId = t.trackId;
                    sb->acfg = t;
                    any = true;
                }
            }
            if (!any) { AppendError(rt, sb, "baslatma bolumunde desteklenen iz yok"); return Value(); }
            sb->gotInit = true;
            if (m->el) {
                MediaEl* e = m->el;
                if (sb->vtb) {
                    int w = sb->vcfg.width, h = sb->vcfg.height;
                    if (w > 0 && h > 0 && (w != e->vw || h != e->vh)) {
                        bool had = e->vw > 0;
                        e->vw = w;
                        e->vh = h;
                        if (had) Queue(rt, e->node, L"resize");
                    }
                }
                BindTracks(rt, e);
            }
        }
        if (!sb->parser.out.empty()) {
            if (sb->mode == L"sequence" && sb->groupStartPending) {
                // sequence kipi: yeni grup onceki grubun sonundan baslar
                double first = kInf;
                for (auto& s : sb->parser.out) {
                    const TrackConfig* tc = sb->parser.Track(s.trackId);
                    if (tc && tc->timescale) first = std::min(first, double(s.pts) / tc->timescale);
                }
                if (!std::isinf(first)) sb->timestampOffset = sb->groupEnd - first;
                sb->groupStartPending = false;
            }
            double ge = sb->groupEnd;
            if (sb->vtb && sb->vId) InsertSamples(sb->vtb, sb->vcfg, sb->parser.out, sb->vId, sb->timestampOffset, sb->appendWindowStart, sb->appendWindowEnd, ge);
            if (sb->atb && sb->aId) InsertSamples(sb->atb, sb->acfg, sb->parser.out, sb->aId, sb->timestampOffset, sb->appendWindowStart, sb->appendWindowEnd, ge);
            sb->groupEnd = ge;
            sb->parser.out.clear();
            // sure, tamponun sonunu asarsa uzar
            if (!std::isnan(m->duration) && !std::isinf(m->duration) && ge > m->duration + 1e-6) {
                m->duration = ge;
                if (m->el) Queue(rt, m->el->node, L"durationchange");
            }
            if (m->el && m->el->player) m->el->player->Wake();
        }
    }
    QueueObj(rt, sb->obj, L"update", [sb]() { sb->updating = false; });
    QueueObj(rt, sb->obj, L"updateend");
    return Value();
}

NATIVE(SB_remove) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    double s = rt.ToNum(A(0)), e = rt.ToNum(A(1));
    if (sb->removed) ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'remove' on 'SourceBuffer': This SourceBuffer has been removed from the parent media source.");
    if (sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'remove' on 'SourceBuffer': This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
    double dur = sb->ms->duration;
    if (std::isnan(dur) || std::isnan(s) || s < 0 || (!std::isnan(dur) && s > dur)) rt.Throw(L"TypeError", L"Failed to execute 'remove' on 'SourceBuffer': The start provided is outside the range.");
    if (std::isnan(e) || e <= s) rt.Throw(L"TypeError", L"Failed to execute 'remove' on 'SourceBuffer': The end value provided must be greater than the start value provided.");
    if (sb->ms->readyState == L"ended") { sb->ms->readyState = L"open"; QueueObj(rt, sb->ms->obj, L"sourceopen"); }
    sb->updating = true;
    QueueObj(rt, sb->obj, L"updatestart", [sb, s, e]() {
        if (sb->vtb) sb->vtb->Remove(s, e);
        if (sb->atb) sb->atb->Remove(s, e);
    });
    QueueObj(rt, sb->obj, L"update", [sb]() { sb->updating = false; });
    QueueObj(rt, sb->obj, L"updateend");
    return Value();
}

NATIVE(SB_abort) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    if (sb->removed || sb->ms->readyState != L"open") ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'abort' on 'SourceBuffer'.");
    sb->parser.ResetParserState();
    sb->groupStartPending = true;
    if (sb->updating) {
        // kuyruktaki bu SourceBuffer olaylarini at, abort + updateend
        auto& q = MS(rt).queue;
        q.erase(std::remove_if(q.begin(), q.end(), [sb](const QEv& e) { return e.obj == sb->obj && (e.type == L"update" || e.type == L"updateend" || e.type == L"updatestart"); }), q.end());
        sb->updating = false;
        JsDispatchObj(rt, sb->obj, L"abort");
        JsDispatchObj(rt, sb->obj, L"updateend");
    }
    sb->appendWindowStart = 0;
    sb->appendWindowEnd = kInf;
    return Value();
}

NATIVE(SB_changeType) {
    UNUSED;
    SBState* sb = SBThis(rt, thisV);
    std::wstring t = rt.ToStr(A(0));
    if (t.empty()) rt.Throw(L"TypeError", L"The type provided is empty.");
    if (sb->removed || sb->updating) ThrowDOM(rt, L"InvalidStateError", L"Failed to execute 'changeType' on 'SourceBuffer'.");
    if (media::SupportLevel(t) == 0) ThrowDOM(rt, L"NotSupportedError", L"Failed to execute 'changeType' on 'SourceBuffer': Changing to the type provided ('" + t + L"') is not supported.");
    sb->parser.ResetParserState();
    sb->mime = t;
    sb->groupStartPending = true;
    return Value();
}

// ---------------- Tik: oynatici durumu -> HTML olaylari ----------------

void AddPlayed(MediaEl* e, double a, double b) {
    if (b <= a) return;
    e->played.push_back({a, b});
    std::sort(e->played.begin(), e->played.end());
    Ranges m;
    for (auto& r : e->played) {
        if (!m.empty() && r.first <= m.back().second + 0.01) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    }
    e->played.swap(m);
}

void TickEl(Runtime& rt, MediaEl* e) {
    if (e->prog) ApplyProgressive(rt, e);
    if (e->yt) TickYouTube(rt, e);
    if (!e->player) { UpdateReadyState(rt, e); return; }
    PlayerStatus st = e->player->Status();
    double now = rt.Now();
    double prevTime = e->time;
    if (!e->seeking) e->time = st.time;
    // arama tamamlandi
    if (e->seeking && st.seekSerial > e->seekBase && !st.seeking) {
        e->seeking = false;
        e->time = st.time;
        Queue(rt, e->node, L"timeupdate");
        Queue(rt, e->node, L"seeked");
        e->lastTimeUpdate = now;
    }
    if (st.videoW > 0 && st.videoH > 0 && (st.videoW != e->vw || st.videoH != e->vh)) {
        bool had = e->vw > 0;
        e->vw = st.videoW;
        e->vh = st.videoH;
        if (had) Queue(rt, e->node, L"resize");
    }
    if (!e->paused && !e->seeking && e->time > prevTime) AddPlayed(e, prevTime, e->time);
    // oynatma suruyor: timeupdate ~250 ms
    if (!e->paused && !e->seeking && st.playing && now - e->lastTimeUpdate >= 250) {
        e->lastTimeUpdate = now;
        Queue(rt, e->node, L"timeupdate");
    }
    // bekleme / devam
    if (!e->paused && !e->seeking) {
        if (st.waiting && !e->waiting) {
            e->waiting = true;
            Queue(rt, e->node, L"waiting");
        } else if (!st.waiting && e->waiting && st.playing) {
            e->waiting = false;
            Queue(rt, e->node, L"playing");
        }
    }
    // son
    if (st.ended && !e->ended && !e->seeking) {
        bool loop = e->node->Attr(L"loop") != nullptr;
        if (loop) {
            DoSeek(rt, e, 0);
            e->player->Play();
        } else {
            e->ended = true;
            Queue(rt, e->node, L"timeupdate");
            if (!e->paused) { e->paused = true; Queue(rt, e->node, L"pause"); }
            Queue(rt, e->node, L"ended");
        }
    }
    // ilerleme
    uint64_t gen = 0;
    if (e->ms) for (SBState* sb : e->ms->sbs) { if (sb->vtb) gen += sb->vtb->Gen(); if (sb->atb) gen += sb->atb->Gen(); }
    if (e->yt && e->ytBound) { if (e->pv) gen += e->pv->Gen(); if (e->pa) gen += e->pa->Gen(); }
    if (gen != e->lastBufGen && now - e->lastProgress > 350) {
        e->lastBufGen = gen;
        e->lastProgress = now;
        Queue(rt, e->node, L"progress");
    }
    UpdateReadyState(rt, e);
    if (e->readyState >= 3 && !e->paused && !e->playPromises.empty()) {
        // play() Promise'leri olaylardan (playing) sonra, sirayla cozulur
        QEv q;
        q.pre = [&rt, e]() { ResolvePlayPromises(rt, e); };
        MS(rt).queue.push_back(std::move(q));
    }
    // requestVideoFrameCallback: yeni kare gosterildiyse
    if (!e->frameCallbacks.empty() && st.framesShown != e->lastShown) {
        e->lastShown = st.framesShown;
        auto cbs = std::move(e->frameCallbacks);
        e->frameCallbacks.clear();
        Obj* meta = rt.NewObject();
        rt.DefineData(meta, L"mediaTime", Value::N(e->time));
        rt.DefineData(meta, L"presentedFrames", Value::N(st.framesShown));
        rt.DefineData(meta, L"width", Value::N(e->vw));
        rt.DefineData(meta, L"height", Value::N(e->vh));
        rt.DefineData(meta, L"presentationTime", Value::N(now));
        rt.DefineData(meta, L"expectedDisplayTime", Value::N(now));
        for (auto& c : cbs) {
            std::vector<Value> a{Value::N(now), Value::O(meta)};
            rt.Enqueue(c.second, a);
        }
    }
}

} // namespace

// ---------------- Disa acik ----------------

void MediaSrcChanged(Runtime& rt, Node* n) {
    MediaEl* e = El(rt, n);
    if (!e) return;
    LoadEl(rt, e);
}

// Ayristirilan / eklenen ogeler: src'li ya da <source>'lu <video>/<audio> yuklenmeye baslar
void MediaScan(Runtime& rt, Node* root) {
    if (!root) return;
    std::vector<Node*> st{root};
    while (!st.empty()) {
        Node* n = st.back();
        st.pop_back();
        if (IsMediaNode(n)) {
            if (!El(rt, n, false)) {
                bool has = n->Attr(L"src") != nullptr;
                for (auto& k : n->kids) if (k->kind == Node::Elem && k->name == L"source" && k->Attr(L"src")) has = true;
                if (has) LoadEl(rt, El(rt, n));
            }
            continue;
        }
        for (auto& k : n->kids) st.push_back(k.get());
        if (n->shadow) st.push_back(n->shadow.get());
    }
}

void MediaTogglePlay(Runtime& rt, Node* n) {
    MediaEl* e = El(rt, n);
    if (!e) return;
    if (e->paused || e->ended) PlayEl(rt, e);
    else PauseEl(rt, e);
}

void MediaTick(Runtime& rt) {
    if (!rt.media) return;
    MediaState& M = *rt.media;
    if (M.ticking) return;
    M.ticking = true;
    struct G { bool& b; ~G() { b = false; } } guard{M.ticking};
    // 1) oynatici durumlari -> olay kuyrugu
    std::vector<MediaEl*> els;
    for (auto& kv : M.els) els.push_back(kv.second.get());
    for (MediaEl* e : els) TickEl(rt, e);
    // 2) kuyruk: siradaki olaylari at (her olay bir gorev)
    for (int i = 0; i < 2000 && !M.queue.empty(); i++) {
        QEv ev = std::move(M.queue.front());
        M.queue.pop_front();
        if (ev.pre) ev.pre();
        if (ev.node) {
            rt.DispatchSimple(ev.node, ev.type, false);
        } else if (ev.obj) {
            JsDispatchObj(rt, ev.obj, ev.type);
        }
        // olaylar arasinda durum degisti: yeni olaylar kuyruga eklenmis olabilir
    }
}

double MediaNextDue(Runtime& rt) {
    if (!rt.media) return -1;
    MediaState& M = *rt.media;
    double now = rt.Now();
    if (!M.queue.empty()) return now;
    bool active = false;
    for (auto& kv : M.els) {
        MediaEl* e = kv.second.get();
        if (e->prog || e->seeking || (!e->paused && !e->ended) || (e->player && e->readyState < 4 && e->networkState == 2)) { active = true; break; }
        if (e->yt && !e->ytBound && e->errorCode == 0) { active = true; break; }
        if (!e->frameCallbacks.empty()) { active = true; break; }
    }
    return active ? now + 40 : -1;
}

void MediaGcRoots(Runtime& rt, std::vector<Cell*>& work) {
    if (!rt.media) return;
    MediaState& M = *rt.media;
    auto root = [&](Cell* c) { if (c) work.push_back(c); };
    root(M.msProto);
    root(M.sbProto);
    root(M.trProto);
    root(M.textTracksProto);
    for (auto& kv : M.ms) { root(kv.first); root(kv.second->sbList); root(kv.second->activeList); }
    for (auto& kv : M.sbs) root(kv.first);
    for (auto& kv : M.els) {
        for (Obj* p : kv.second->playPromises) root(p);
        for (auto& c : kv.second->frameCallbacks) if (c.second.IsObj()) root(c.second.o);
    }
}

void InstallMedia(Runtime& rt, Obj* EP) {
    MediaState& M = MS(rt);
    auto Acc = [&](Obj* o, const wchar_t* k, NativeFn g, NativeFn s) { rt.DefineAccessor(o, k, g, s); };
    auto Mt = [&](Obj* o, const wchar_t* k, NativeFn f, int n) { rt.DefineMethod(o, k, f, n); };

    // HTMLMediaElement / HTMLVideoElement (tum ogeler ortak prototipi kullanir)
    Mt(EP, L"play", M_play, 0);
    Mt(EP, L"pause", M_pause, 0);
    Mt(EP, L"load", M_load, 0);
    Mt(EP, L"canPlayType", M_canPlayType, 1);
    Mt(EP, L"fastSeek", M_fastSeek, 1);
    Mt(EP, L"getVideoPlaybackQuality", M_quality, 0);
    Mt(EP, L"requestVideoFrameCallback", M_rvfc, 1);
    Mt(EP, L"cancelVideoFrameCallback", M_cancelRvfc, 1);
    Mt(EP, L"setMediaKeys", M_setMediaKeys, 1);
    Mt(EP, L"addTextTrack", M_addTextTrack, 3);
    Acc(EP, L"currentTime", M_currentTime, M_setCurrentTime);
    Acc(EP, L"duration", M_duration, nullptr);
    Acc(EP, L"paused", M_paused, nullptr);
    Acc(EP, L"ended", M_ended, nullptr);
    Acc(EP, L"seeking", M_seeking, nullptr);
    Acc(EP, L"readyState", M_readyState, nullptr);
    Acc(EP, L"networkState", M_networkState, nullptr);
    Acc(EP, L"buffered", M_buffered, nullptr);
    Acc(EP, L"seekable", M_seekable, nullptr);
    Acc(EP, L"played", M_played, nullptr);
    Acc(EP, L"volume", M_volume, M_setVolume);
    Acc(EP, L"muted", M_muted, M_setMuted);
    Acc(EP, L"defaultMuted", M_defaultMuted, nullptr);
    Acc(EP, L"playbackRate", M_rate, M_setRate);
    Acc(EP, L"defaultPlaybackRate", M_defaultRate, M_setDefaultRate);
    Acc(EP, L"videoWidth", M_videoWidth, nullptr);
    Acc(EP, L"videoHeight", M_videoHeight, nullptr);
    Acc(EP, L"currentSrc", M_currentSrc, nullptr);
    Acc(EP, L"error", M_error, nullptr);
    Acc(EP, L"srcObject", M_srcObject, M_setSrcObject);
    Acc(EP, L"preload", M_preload, nullptr);
    Acc(EP, L"poster", M_poster, nullptr);
    Acc(EP, L"textTracks", M_textTracks, nullptr);
    Acc(EP, L"webkitDecodedFrameCount", M_decodedFrames, nullptr);
    Acc(EP, L"webkitDroppedFrameCount", M_droppedFrames, nullptr);
    rt.DefineData(EP, L"mediaKeys", Value::Null_(), false);
    rt.DefineData(EP, L"preservesPitch", Value::B(true));
    rt.DefineData(EP, L"disableRemotePlayback", Value::B(false));
    for (const wchar_t* ev : {L"loadstart", L"progress", L"suspend", L"abort", L"emptied", L"stalled", L"loadedmetadata", L"loadeddata",
                              L"canplay", L"canplaythrough", L"playing", L"waiting", L"seeking", L"seeked", L"ended", L"durationchange",
                              L"timeupdate", L"play", L"pause", L"ratechange", L"volumechange", L"resize", L"encrypted", L"waitingforkey"}) {
        std::wstring k = std::wstring(L"on") + ev;
        if (!EP->props.Find(k)) rt.DefineData(EP, k, Value::Null_(), false);
    }

    M.textTracksProto = rt.NewObject(rt.eventTargetProto);
    Mt(M.textTracksProto, L"getTrackById", TT_getTrackById, 1);

    // TimeRanges
    M.trProto = rt.NewObject();
    Mt(M.trProto, L"start", TR_start, 1);
    Mt(M.trProto, L"end", TR_end, 1);
    JsNativeCtor(rt, L"TimeRanges", [](Runtime& r, Obj*, const Value&, std::vector<Value>&) -> Value { r.Throw(L"TypeError", L"Illegal constructor"); }, M.trProto);

    // MediaSource
    M.msProto = rt.NewObject(rt.eventTargetProto);
    rt.mediaSourceProto = M.msProto;
    Mt(M.msProto, L"addSourceBuffer", MS_addSourceBuffer, 1);
    Mt(M.msProto, L"removeSourceBuffer", MS_removeSourceBuffer, 1);
    Mt(M.msProto, L"endOfStream", MS_endOfStream, 0);
    Mt(M.msProto, L"setLiveSeekableRange", MS_setLiveSeekableRange, 2);
    Mt(M.msProto, L"clearLiveSeekableRange", MS_clearLiveSeekableRange, 0);
    Acc(M.msProto, L"readyState", MS_readyState, nullptr);
    Acc(M.msProto, L"duration", MS_duration, MS_setDuration);
    Acc(M.msProto, L"sourceBuffers", MS_sourceBuffers, nullptr);
    Acc(M.msProto, L"activeSourceBuffers", MS_activeSourceBuffers, nullptr);
    for (const wchar_t* ev : {L"onsourceopen", L"onsourceended", L"onsourceclose"}) rt.DefineData(M.msProto, ev, Value::Null_(), false);
    Obj* msCtor = JsNativeCtor(rt, L"MediaSource", MS_ctor, M.msProto);
    rt.DefineMethod(msCtor, L"isTypeSupported", MS_isTypeSupported, 1);
    rt.DefineData(msCtor, L"canConstructInDedicatedWorker", Value::B(false), false);
    rt.DefineData(rt.global, L"WebKitMediaSource", Value::O(msCtor), false);

    // SourceBuffer
    M.sbProto = rt.NewObject(rt.eventTargetProto);
    rt.sourceBufferProto = M.sbProto;
    Mt(M.sbProto, L"appendBuffer", SB_appendBuffer, 1);
    Mt(M.sbProto, L"remove", SB_remove, 2);
    Mt(M.sbProto, L"abort", SB_abort, 0);
    Mt(M.sbProto, L"changeType", SB_changeType, 1);
    Acc(M.sbProto, L"updating", SB_updating, nullptr);
    Acc(M.sbProto, L"buffered", SB_buffered, nullptr);
    Acc(M.sbProto, L"mode", SB_mode, SB_setMode);
    Acc(M.sbProto, L"timestampOffset", SB_tsOffset, SB_setTsOffset);
    Acc(M.sbProto, L"appendWindowStart", SB_awStart, SB_setAwStart);
    Acc(M.sbProto, L"appendWindowEnd", SB_awEnd, SB_setAwEnd);
    for (const wchar_t* ev : {L"onupdatestart", L"onupdate", L"onupdateend", L"onerror", L"onabort"}) rt.DefineData(M.sbProto, ev, Value::Null_(), false);
    JsNativeCtor(rt, L"SourceBuffer", [](Runtime& r, Obj*, const Value&, std::vector<Value>&) -> Value { r.Throw(L"TypeError", L"Illegal constructor"); }, M.sbProto);
    JsNativeCtor(rt, L"SourceBufferList", [](Runtime& r, Obj*, const Value&, std::vector<Value>&) -> Value { r.Throw(L"TypeError", L"Illegal constructor"); }, rt.NewObject(rt.eventTargetProto));
}

} // namespace js
} // namespace mer
