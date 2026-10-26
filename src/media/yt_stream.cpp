// merOS Shield - YouTube yerel oynatici akisi (bkz. yt_stream.h)
#include "yt_stream.h"
#include "../json.h"
#include "../util.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace mer {
namespace media {

namespace {

// visionOS istemcisi (NewPipeExtractor, Ekim 2026): dogrudan oynatma adresleri, "n" parametresi yok
const wchar_t* kClientName = L"VISIONOS";
const wchar_t* kClientVersion = L"1.04";
const wchar_t* kDeviceModel = L"RealityDevice17,1";
const wchar_t* kOsVersion = L"26.6.0.23O770";
const wchar_t* kUaVersion = L"26_6_0";

std::string Narrow(const std::wstring& w) { return ToUtf8(w); }

std::string JsonEsc(const std::wstring& w) {
    std::string s = ToUtf8(w), o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}

long long NumOf(const JsonValue* v) {
    if (!v) return -1;
    if (v->t == JsonValue::Num) return (long long)v->n;
    if (v->t == JsonValue::Str) return std::wcstoll(v->s.c_str(), nullptr, 10);
    return -1;
}

std::wstring TextOf(const JsonValue* v) {
    if (!v) return L"";
    if (v->t == JsonValue::Str) return v->s;
    if (const JsonValue* s = v->Get(L"simpleText")) return s->AsStr();
    std::wstring o;
    if (const JsonValue* runs = v->Get(L"runs"))
        for (auto& r : runs->a) if (const JsonValue* t = r.Get(L"text")) o += t->AsStr();
    return o;
}

uint64_t Be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

} // namespace

std::wstring YtNonce(int len) {
    static const wchar_t* abc = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    static thread_local std::mt19937_64 rng(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0xA5A5F00Dull);
    std::wstring s;
    for (int i = 0; i < len; i++) s += abc[rng() % 64];
    return s;
}

bool YtParsePlayer(const std::wstring& json, YtInfo& out) {
    out = YtInfo();
    JsonValue root;
    size_t pos = json.find(L'{');
    if (pos == std::wstring::npos || !ParseJson(json, pos, root) || root.t != JsonValue::Obj) {
        out.error = L"YouTube yanıtı okunamadı";
        return false;
    }
    if (const JsonValue* ps = root.Get(L"playabilityStatus")) {
        if (const JsonValue* st = ps->Get(L"status")) out.status = st->AsStr();
        std::wstring reason = TextOf(ps->Get(L"reason"));
        if (reason.empty()) reason = TextOf(ps->Path(L"errorScreen.playerErrorMessageRenderer.reason"));
        if (out.status != L"OK") out.error = reason.empty() ? (L"Video oynatılamıyor (" + out.status + L")") : reason;
    }
    if (const JsonValue* vd = root.Get(L"videoDetails")) {
        if (const JsonValue* t = vd->Get(L"title")) out.title = t->AsStr();
        if (const JsonValue* a = vd->Get(L"author")) out.author = a->AsStr();
        out.duration = double(std::max(0ll, NumOf(vd->Get(L"lengthSeconds"))));
        if (const JsonValue* l = vd->Get(L"isLive")) out.live = l->t == JsonValue::Bool && l->b;
    }
    const JsonValue* sd = root.Get(L"streamingData");
    if (sd) {
        for (const wchar_t* key : {L"adaptiveFormats", L"formats"}) {
            const JsonValue* arr = sd->Get(key);
            if (!arr || arr->t != JsonValue::Arr) continue;
            for (auto& f : arr->a) {
                const JsonValue* u = f.Get(L"url");
                if (!u || u->t != JsonValue::Str || u->s.empty()) continue;   // signatureCipher: desteklenmiyor
                YtFormat y;
                y.itag = int(NumOf(f.Get(L"itag")));
                y.url = u->s;
                std::string mt = f.Get(L"mimeType") ? Narrow(f.Get(L"mimeType")->AsStr()) : "";
                size_t semi = mt.find(';');
                y.mime = mt.substr(0, semi);
                size_t c = mt.find("codecs=\"");
                if (c != std::string::npos) {
                    size_t e = mt.find('"', c + 8);
                    y.codecs = mt.substr(c + 8, e == std::string::npos ? std::string::npos : e - c - 8);
                }
                y.video = y.mime.compare(0, 6, "video/") == 0;
                y.width = int(std::max(0ll, NumOf(f.Get(L"width"))));
                y.height = int(std::max(0ll, NumOf(f.Get(L"height"))));
                y.fps = int(std::max(0ll, NumOf(f.Get(L"fps"))));
                y.bitrate = std::max(0ll, NumOf(f.Get(L"bitrate")));
                y.contentLength = NumOf(f.Get(L"contentLength"));
                y.initStart = NumOf(f.Path(L"initRange.start"));
                y.initEnd = NumOf(f.Path(L"initRange.end"));
                y.indexStart = NumOf(f.Path(L"indexRange.start"));
                y.indexEnd = NumOf(f.Path(L"indexRange.end"));
                y.durationMs = double(std::max(0ll, NumOf(f.Get(L"approxDurationMs"))));
                if (const JsonValue* at = f.Get(L"audioTrack")) {
                    if (const JsonValue* id = at->Get(L"id")) y.audioTrackId = Narrow(id->AsStr());
                    if (const JsonValue* d = at->Get(L"audioIsDefault")) y.audioDefault = d->t == JsonValue::Bool && d->b;
                }
                out.formats.push_back(y);
            }
        }
    }
    if (out.duration <= 0)
        for (auto& f : out.formats) out.duration = std::max(out.duration, f.durationMs / 1000.0);
    out.ok = out.status == L"OK" && !out.formats.empty();
    if (out.status == L"OK" && out.formats.empty()) out.error = out.live ? L"Canlı yayınlar henüz desteklenmiyor" : L"Oynatılabilir biçim bulunamadı";
    return out.ok;
}

bool YtPick(const YtInfo& info, int maxHeight, const YtFormat** video, const YtFormat** audio) {
    *video = nullptr;
    *audio = nullptr;
    const YtFormat* bestV = nullptr;
    const YtFormat* smallestV = nullptr;
    for (auto& f : info.formats) {
        if (!f.video || f.mime != "video/mp4" || f.codecs.compare(0, 4, "avc1") != 0) continue;
        if (f.indexEnd <= 0 || f.initEnd < 0) continue;            // DASH degil (ilerlemeli itag 18 vb.)
        if (!smallestV || f.height < smallestV->height) smallestV = &f;
        if (f.height > maxHeight) continue;
        if (!bestV || f.height > bestV->height || (f.height == bestV->height && (f.fps > bestV->fps || (f.fps == bestV->fps && f.bitrate > bestV->bitrate))))
            bestV = &f;
    }
    if (!bestV) bestV = smallestV;
    const YtFormat* bestA = nullptr;
    auto score = [](const YtFormat& f) {
        int s = 0;
        if (f.audioDefault) s += 1000;
        if (f.codecs == "mp4a.40.2") s += 100;   // AAC-LC (HE-AAC'den guvenli)
        return s;
    };
    for (auto& f : info.formats) {
        if (f.video || f.mime != "audio/mp4" || f.codecs.compare(0, 4, "mp4a") != 0) continue;
        if (f.indexEnd <= 0 || f.initEnd < 0) continue;
        if (!bestA || score(f) > score(*bestA) || (score(f) == score(*bestA) && f.bitrate > bestA->bitrate)) bestA = &f;
    }
    *video = bestV;
    *audio = bestA;
    return bestV || bestA;
}

bool ParseSidx(const uint8_t* p, size_t n, uint64_t boxAbs, std::vector<SidxRef>& out) {
    out.clear();
    if (n < 32) return false;
    uint64_t size = Be(p, 4);
    if (size > n || size < 32 || p[4] != 's' || p[5] != 'i' || p[6] != 'd' || p[7] != 'x') return false;
    const uint8_t* q = p + 8;
    const uint8_t* end = p + size;
    int version = q[0];
    q += 4;            // version + flags
    q += 4;            // reference_ID
    uint32_t timescale = uint32_t(Be(q, 4));
    q += 4;
    uint64_t ept, first;
    if (version == 0) {
        if (end - q < 8) return false;
        ept = Be(q, 4); first = Be(q + 4, 4); q += 8;
    } else {
        if (end - q < 16) return false;
        ept = Be(q, 8); first = Be(q + 8, 8); q += 16;
    }
    if (end - q < 4 || timescale == 0) return false;
    q += 2;            // reserved
    int count = int(Be(q, 2));
    q += 2;
    uint64_t off = boxAbs + size + first;
    double t = double(ept) / timescale;
    for (int i = 0; i < count; i++) {
        if (end - q < 12) return false;
        uint32_t a = uint32_t(Be(q, 4));
        uint32_t dur = uint32_t(Be(q + 4, 4));
        q += 12;
        if (a & 0x80000000u) return false;   // ic ice sidx (YouTube kullanmaz)
        SidxRef r;
        r.offset = off;
        r.size = a & 0x7FFFFFFFu;
        r.start = t;
        r.dur = double(dur) / timescale;
        out.push_back(r);
        off += r.size;
        t += r.dur;
    }
    return !out.empty();
}

bool FindSidx(const uint8_t* p, size_t n, uint64_t dataAbs, std::vector<SidxRef>& out) {
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t size = Be(p + i, 4);
        if (size < 8) return false;
        if (p[i + 4] == 's' && p[i + 5] == 'i' && p[i + 6] == 'd' && p[i + 7] == 'x')
            return ParseSidx(p + i, n - i, dataAbs + i, out);
        i += size_t(size);
    }
    return false;
}

// ---------------- Akis ----------------

struct YtStream::Track {
    YtFormat fmt;
    std::shared_ptr<TrackBuffer> tb;
    Mp4Parser parser;
    TrackConfig cfg;
    std::vector<SidxRef> segs;
    std::vector<char> have;
    uint64_t parserGen = 0;     // ayristiricinin son sifirlandigi arama sayaci
    size_t lastFetched = size_t(-1);
    int failures = 0;
};

YtStream::YtStream(const YtOptions& o, HttpFn http) : opt_(o), http_(std::move(http)) {
    ua_ = std::wstring(L"com.google.visionos.youtube/") + kClientVersion + L"(" + kDeviceModel + L"; U; CPU visionOS " + kUaVersion +
          L" like Mac OS X; " + (opt_.gl.empty() ? L"US" : opt_.gl) + L")";
}

YtStream::~YtStream() {
    cancel_ = true;
}

void YtStream::Start() {
    // is parcacigi nesneye sahip cikar: JS tarafi birakip Cancel() dese de bitene kadar yasar
    std::shared_ptr<YtStream> self = shared_from_this();
    std::thread([self]() { self->Run(); }).detach();
}

void YtStream::Cancel() {
    cancel_ = true;
    cv_.notify_all();
}

void YtStream::SetPlayhead(double t) {
    std::lock_guard<std::mutex> g(mu_);
    if (std::fabs(t - playhead_) > 0.5) cv_.notify_all();
    playhead_ = t;
}

void YtStream::Seek(double t) {
    {
        std::lock_guard<std::mutex> g(mu_);
        seekGen_++;
        seekTo_ = t;
        playhead_ = t;
    }
    eos_ = false;
    cv_.notify_all();
}

int YtStream::Http(const std::wstring& method, const std::wstring& url, const std::wstring& headers, const std::string& body,
                   size_t maxBytes, std::string& out) {
    out.clear();
    if (cancel_) return 0;
    return http_(method, url, headers, body, maxBytes, &cancel_, out);
}

bool YtStream::Resolve(YtInfo& info) {
    std::wstring hdr = L"User-Agent: " + ua_ + L"\r\nX-Goog-Api-Format-Version: 2\r\nContent-Type: application/json\r\nAccept-Language: " +
                       opt_.hl + L"-" + opt_.gl + L", " + opt_.hl + L";q=0.9\r\n";
    auto context = [&](const std::wstring& visitor) {
        std::string c = "{\"context\":{\"client\":{\"clientName\":\"" + Narrow(kClientName) + "\",\"clientVersion\":\"" + Narrow(kClientVersion) +
                        "\",\"clientScreen\":\"WATCH\",\"platform\":\"MOBILE\",";
        if (!visitor.empty()) c += "\"visitorData\":\"" + JsonEsc(visitor) + "\",";
        c += "\"deviceMake\":\"Apple\",\"deviceModel\":\"" + Narrow(kDeviceModel) + "\",\"osName\":\"visionOS\",\"osVersion\":\"" + Narrow(kOsVersion) +
             "\",\"hl\":\"" + JsonEsc(opt_.hl) + "\",\"gl\":\"" + JsonEsc(opt_.gl) + "\",\"utcOffsetMinutes\":0},"
             "\"request\":{\"internalExperimentFlags\":[],\"useSsl\":true},\"user\":{\"lockedSafetyMode\":false}}";
        return c;
    };
    // 1) visitorData (gecerli oynatici yaniti icin gerekli)
    std::wstring visitor;
    {
        std::string resp;
        int st = Http(L"POST", opt_.wwwBase + L"/youtubei/v1/visitor_id?prettyPrint=false", hdr, context(L"") + "}", 2u << 20, resp);
        if (st >= 200 && st < 300) {
            std::wstring j = FromUtf8(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            JsonValue v;
            size_t pos = j.find(L'{');
            if (pos != std::wstring::npos && ParseJson(j, pos, v))
                if (const JsonValue* vd = v.Path(L"responseContext.visitorData")) visitor = vd->AsStr();
        }
        if (cancel_) return false;
    }
    // 2) player
    std::wstring cpn = YtNonce(16);
    std::string body = context(visitor) + ",\"videoId\":\"" + JsonEsc(opt_.videoId) + "\",\"cpn\":\"" + Narrow(cpn) +
                       "\",\"contentCheckOk\":true,\"racyCheckOk\":true}";
    std::string resp;
    int st = Http(L"POST", opt_.apiBase + L"/youtubei/v1/player?prettyPrint=false&t=" + YtNonce(12) + L"&id=" + opt_.videoId, hdr, body, 16u << 20, resp);
    if (cancel_) return false;
    if (st < 200 || st >= 300) {
        info.error = st == 0 ? L"YouTube'a bağlanılamadı" : L"YouTube oynatıcı isteği başarısız (HTTP " + std::to_wstring(st) + L")";
        return false;
    }
    std::wstring j = FromUtf8(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
    if (!YtParsePlayer(j, info)) return false;
    // oynatma adreslerine istemci izleme numarasi (cpn) eklenir
    for (auto& f : info.formats) f.url += L"&cpn=" + cpn;
    return true;
}

bool YtStream::InitTrack(Track& t) {
    std::string data;
    long long end = t.fmt.indexEnd;
    std::wstring url = t.fmt.url + L"&range=0-" + std::to_wstring(end) + L"&rn=" + std::to_wstring(++rn_);
    std::wstring hdr = L"User-Agent: " + ua_ + L"\r\n";
    int st = Http(L"GET", url, hdr, "", size_t(end + 1) + 4096, data);
    if (st < 200 || st >= 300 || data.size() < size_t(t.fmt.indexEnd + 1)) {
        std::lock_guard<std::mutex> g(mu_);
        error_ = L"Video başlığı indirilemedi (HTTP " + std::to_wstring(st) + L")";
        return false;
    }
    bytes_ += data.size();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t initLen = t.fmt.initEnd >= 0 ? size_t(t.fmt.initEnd + 1) : size_t(t.fmt.indexStart);
    if (!t.parser.Append(p, std::min(initLen, data.size())) || !t.parser.haveInit || t.parser.tracks.empty()) {
        std::lock_guard<std::mutex> g(mu_);
        error_ = L"Video başlığı çözümlenemedi: " + std::wstring(t.parser.error.begin(), t.parser.error.end());
        return false;
    }
    t.parser.initChanged = false;
    t.cfg = t.parser.tracks[0];
    if (t.cfg.codec != Codec::H264 && t.cfg.codec != Codec::AAC) {
        std::lock_guard<std::mutex> g(mu_);
        error_ = L"Desteklenmeyen kodlayıcı";
        return false;
    }
    size_t is = size_t(std::max(0ll, t.fmt.indexStart));
    if (is >= data.size() || !FindSidx(p + is, data.size() - is, is, t.segs)) {
        std::lock_guard<std::mutex> g(mu_);
        error_ = L"Bölüm dizini (sidx) bulunamadı";
        return false;
    }
    t.have.assign(t.segs.size(), 0);
    t.tb = std::make_shared<TrackBuffer>(t.cfg.video);
    t.tb->SetConfig(t.cfg);
    return true;
}

// Bir izde siradaki eksik bolumleri indir (tek istek). didWork: bir sey indirildi.
bool YtStream::Pump(Track& t, double playhead, bool& didWork) {
    if (t.segs.empty()) return true;
    // gerideki ornekleri birak (bellek); bunlara geri donulurse yeniden indirilir
    double cut = playhead - opt_.behind;
    if (cut > 0) {
        bool any = false;
        for (size_t i = 0; i < t.segs.size(); i++) {
            if (t.have[i] && t.segs[i].start + t.segs[i].dur < cut) { t.have[i] = 0; any = true; }
        }
        if (any) t.tb->Remove(0, cut);
    }
    // oynatma noktasini iceren bolumden itibaren ilk eksik bolum
    size_t first = 0;
    while (first + 1 < t.segs.size() && t.segs[first].start + t.segs[first].dur <= playhead + 1e-3) first++;
    size_t i = first;
    while (i < t.segs.size() && t.have[i]) i++;
    if (i >= t.segs.size()) return true;
    if (t.segs[i].start > playhead + opt_.ahead) return true;   // yeterince tampon var
    // ardisik eksik bolumleri ~3 MB'a kadar birlestir
    size_t j = i;
    uint64_t total = t.segs[i].size;
    while (j + 1 < t.segs.size() && !t.have[j + 1] && total + t.segs[j + 1].size <= (3u << 20) &&
           t.segs[j + 1].start < playhead + opt_.ahead) {
        j++;
        total += t.segs[j].size;
    }
    uint64_t a = t.segs[i].offset, b = t.segs[j].offset + t.segs[j].size - 1;
    // onceki istekle ardisik degilse ayristiricinin yarim verisini at (arama)
    if (t.lastFetched == size_t(-1) || t.lastFetched + 1 != i) t.parser.ResetParserState();
    std::string data;
    std::wstring url = t.fmt.url + L"&range=" + std::to_wstring(a) + L"-" + std::to_wstring(b) + L"&rn=" + std::to_wstring(++rn_);
    int st = Http(L"GET", url, L"User-Agent: " + ua_ + L"\r\n", "", size_t(total) + 65536, data);
    if (cancel_) return false;
    if (st < 200 || st >= 300 || data.size() < total) {
        t.failures++;
        if (t.failures >= 6) {
            std::lock_guard<std::mutex> g(mu_);
            error_ = L"Video verisi indirilemedi (HTTP " + std::to_wstring(st) + L")";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300 * t.failures));
        return true;
    }
    t.failures = 0;
    bytes_ += data.size();
    if (!t.parser.Append(reinterpret_cast<const uint8_t*>(data.data()), size_t(total))) {
        std::string e = t.parser.error;
        t.parser = Mp4Parser();
        // baslangic yeniden gerekmez: yapilandirma izde; yeni ayristiriciya moov olmadan devam edilemez
        std::lock_guard<std::mutex> g(mu_);
        error_ = L"Video bölümü çözümlenemedi: " + std::wstring(e.begin(), e.end());
        return false;
    }
    std::vector<BufSample> group;
    double ts = t.cfg.timescale ? double(t.cfg.timescale) : 1.0;
    for (auto& s : t.parser.out) {
        BufSample bs;
        bs.pts = double(s.pts) / ts;
        bs.dts = double(s.dts) / ts;
        bs.dur = double(s.dur) / ts;
        bs.key = s.key;
        bs.data = s.data;
        group.push_back(std::move(bs));
    }
    t.parser.out.clear();
    if (!group.empty()) t.tb->Insert(group);
    for (size_t k = i; k <= j; k++) t.have[k] = 1;
    t.lastFetched = j;
    didWork = true;
    return true;
}

void YtStream::Run() {
    YtInfo info;
    if (!Resolve(info)) {
        if (!cancel_) {
            std::lock_guard<std::mutex> g(mu_);
            error_ = info.error.empty() ? L"Video bilgisi alınamadı" : info.error;
        }
        state_ = Failed;
        return;
    }
    const YtFormat* vf = nullptr;
    const YtFormat* af = nullptr;
    YtPick(info, opt_.maxHeight, &vf, &af);
    if (!vf && !af) {
        std::lock_guard<std::mutex> g(mu_);
        error_ = info.live ? L"Canlı yayınlar henüz desteklenmiyor" : L"Bu video için H.264/AAC biçimi yok (yalnızca VP9/AV1/Opus)";
        state_ = Failed;
        return;
    }
    for (const YtFormat* f : {vf, af}) {
        if (!f) continue;
        std::unique_ptr<Track> t(new Track());
        t->fmt = *f;
        if (!InitTrack(*t)) { state_ = Failed; return; }
        tracks_.push_back(std::move(t));
        if (cancel_) { state_ = Failed; return; }
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        title_ = info.title;
        duration_ = info.duration;
        for (auto& t : tracks_) {
            if (!t->segs.empty()) duration_ = std::max(duration_, t->segs.back().start + t->segs.back().dur);
            if (t->cfg.video) vtb_ = t->tb;
            else atb_ = t->tb;
        }
        if (vf) quality_ = std::to_wstring(vf->height) + L"p" + (vf->fps > 30 ? std::to_wstring(vf->fps) : L"");
    }
    state_ = Ready;
    uint64_t seenSeek = 0;
    while (!cancel_) {
        double ph;
        uint64_t sg;
        {
            std::lock_guard<std::mutex> g(mu_);
            ph = playhead_;
            sg = seekGen_;
        }
        if (sg != seenSeek) {
            seenSeek = sg;
            for (auto& t : tracks_) t->lastFetched = size_t(-1);
        }
        bool work = false;
        for (auto& t : tracks_) {
            if (!Pump(*t, ph, work)) {
                if (cancel_) return;
                state_ = Failed;
                return;
            }
        }
        // akis sonu: tum izlerde oynatma noktasindan sona kadar her bolum hazir
        bool all = !tracks_.empty();
        for (auto& t : tracks_) {
            size_t first = 0;
            while (first + 1 < t->segs.size() && t->segs[first].start + t->segs[first].dur <= ph + 1e-3) first++;
            for (size_t k = first; k < t->segs.size(); k++) if (!t->have[k]) { all = false; break; }
        }
        eos_ = all;
        if (!work) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(250), [&]() { return cancel_.load() || seekGen_ != seenSeek; });
        }
    }
}

} // namespace media
} // namespace mer
