// merOS Shield - YouTube yerel oynatici akisi
// YouTube'un 10 MB'lik masaustu JavaScript'ini calistirmadan videoyu kendi oynaticimizda izlemek icin:
//  1) InnerTube "player" istegi (visionOS istemcisi: dogrudan adresler, imza/n cozmesi gerekmez)
//  2) H.264 (avc1) + AAC (mp4a) DASH bicimlerinin secimi (gorunur alana gore cozunurluk)
//  3) sidx dizininden bolumlerin bulunmasi, aralik (range=) istekleriyle oynatma noktasinin
//     onunde ~60 sn tamponlama, aramada dogru bolumden devam
// Ag erisimi enjekte edilir (HttpFn): Windows'ta WinHTTP, testlerde yerel dosyalar.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "track_buffer.h"

namespace mer {
namespace media {

struct YtFormat {
    int itag = 0;
    bool video = false;
    std::wstring url;
    std::string mime;              // "video/mp4"
    std::string codecs;            // "avc1.64001F"
    int width = 0, height = 0, fps = 0;
    long long bitrate = 0, contentLength = 0;
    long long initStart = -1, initEnd = -1, indexStart = -1, indexEnd = -1;
    double durationMs = 0;
    std::string audioTrackId;      // cok dilli videolar: "en.4"
    bool audioDefault = true;
};

struct YtInfo {
    bool ok = false;
    std::wstring status;           // playabilityStatus.status (OK, LOGIN_REQUIRED, UNPLAYABLE, ...)
    std::wstring error;            // kullaniciya gosterilecek neden
    std::wstring title, author;
    double duration = 0;           // sn
    bool live = false;
    std::vector<YtFormat> formats;
};

// InnerTube "player" yanitini ayristirir (yalnizca dogrudan "url" tasiyan bicimler)
bool YtParsePlayer(const std::wstring& json, YtInfo& out);
// Oynatilabilir en iyi video (H.264, yukseklik <= maxHeight) ve ses (AAC) bicimini secer
bool YtPick(const YtInfo& info, int maxHeight, const YtFormat** video, const YtFormat** audio);

// sidx (Segment Index) kutusu: bolumlerin dosya icindeki konumu ve zamani
struct SidxRef { uint64_t offset = 0, size = 0; double start = 0, dur = 0; };
// p: kutunun tamami (boyut + "sidx" dahil), boxAbs: kutunun dosyadaki konumu
bool ParseSidx(const uint8_t* p, size_t n, uint64_t boxAbs, std::vector<SidxRef>& out);
// [p, p+n) icinde "sidx" kutusunu bulur ve ayristirir (p dosyanin dataAbs konumundan baslar)
bool FindSidx(const uint8_t* p, size_t n, uint64_t dataAbs, std::vector<SidxRef>& out);

// HTTP erisimi: method, url, ek basliklar (CRLF), govde, en fazla bayt, iptal bayragi -> durum kodu (0: ag hatasi)
using HttpFn = std::function<int(const std::wstring& method, const std::wstring& url, const std::wstring& headers,
                                 const std::string& body, size_t maxBytes, const std::atomic<bool>* cancel, std::string& out)>;

struct YtOptions {
    std::wstring videoId;
    int maxHeight = 720;           // gorunur alana gore (1080'e kadar)
    std::wstring hl = L"tr", gl = L"TR";
    std::wstring apiBase = L"https://youtubei.googleapis.com";   // test icin degistirilebilir
    std::wstring wwwBase = L"https://www.youtube.com";
    double ahead = 60;             // oynatma noktasinin onunde tamponlanacak sure (sn)
    double behind = 30;            // geride tutulacak sure (bellek)
};

// Bir videonun akisi: kendi is parcaciginda cozumler ve indirir; JS is parcacigi yoklar.
class YtStream : public std::enable_shared_from_this<YtStream> {
public:
    YtStream(const YtOptions& o, HttpFn http);
    ~YtStream();
    void Start();                  // is parcacigi kendine shared_ptr tutar (sahip birakinca da guvenli)

    enum State { Resolving, Ready, Failed };
    State GetState() const { return state_.load(); }
    std::wstring Error() const { std::lock_guard<std::mutex> g(mu_); return error_; }
    std::wstring Title() const { std::lock_guard<std::mutex> g(mu_); return title_; }
    std::wstring Quality() const { std::lock_guard<std::mutex> g(mu_); return quality_; }
    double Duration() const { std::lock_guard<std::mutex> g(mu_); return duration_; }
    std::shared_ptr<TrackBuffer> Video() const { std::lock_guard<std::mutex> g(mu_); return vtb_; }
    std::shared_ptr<TrackBuffer> Audio() const { std::lock_guard<std::mutex> g(mu_); return atb_; }
    bool Eos() const { return eos_.load(); }
    uint64_t BytesLoaded() const { return bytes_.load(); }

    void SetPlayhead(double t);    // her medya tikinde
    void Seek(double t);           // arama: o noktanin bolumunden indir
    void Cancel();

private:
    struct Track;
    void Run();
    bool Resolve(YtInfo& info);
    bool InitTrack(Track& t);
    bool Pump(Track& t, double playhead, bool& didWork);
    int Http(const std::wstring& method, const std::wstring& url, const std::wstring& headers, const std::string& body,
             size_t maxBytes, std::string& out);

    YtOptions opt_;
    HttpFn http_;
    std::atomic<bool> cancel_{false};
    std::atomic<State> state_{Resolving};
    std::atomic<bool> eos_{false};
    std::atomic<uint64_t> bytes_{0};
    mutable std::mutex mu_;
    std::condition_variable cv_;
    double playhead_ = 0;
    uint64_t seekGen_ = 0;
    double seekTo_ = 0;
    std::wstring error_, title_, quality_;
    double duration_ = 0;
    std::shared_ptr<TrackBuffer> vtb_, atb_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::wstring ua_;
    int rn_ = 0;
};

// Rastgele "cpn" / "t" (InnerTube istemci izleme numarasi)
std::wstring YtNonce(int len);

} // namespace media
} // namespace mer
