// merOS Shield - YouTube yerel oynatici akisi testleri (platformdan bagimsiz)
// Kullanim: yt_stream_test <fikstur klasoru>
//   player.json : gercek bir InnerTube visionOS "player" yaniti (NewPipeExtractor mock'u)
//   yt_v.mp4 / yt_a.mp4 : sidx'li DASH H.264 / AAC dosyalari (ffmpeg -movflags dash+global_sidx)
#include "../src/media/yt_stream.h"
#include "../src/util.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <thread>

using namespace mer;
using namespace mer::media;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::fflush(stdout); fails++; } } while (0)

static std::string ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static std::wstring W(const std::string& s) { return FromUtf8(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    // 1) gercek visionOS yaniti
    {
        YtInfo info;
        bool ok = YtParsePlayer(W(ReadFile(dir + "/player.json")), info);
        CHECK(ok);
        CHECK(info.status == L"OK");
        CHECK(info.formats.size() == 8);
        CHECK(info.duration == 721);
        CHECK(info.title.find(L"CopyrightX") != std::wstring::npos);
        const YtFormat *v = nullptr, *a = nullptr;
        YtPick(info, 720, &v, &a);
        CHECK(v && v->itag == 136 && v->height == 720 && v->indexEnd == 2270 && v->initEnd == 738);
        CHECK(a && a->itag == 140 && a->codecs == "mp4a.40.2");
        YtPick(info, 480, &v, &a);
        CHECK(v && v->itag == 134);
        YtPick(info, 1080, &v, &a);
        CHECK(v && v->itag == 137);
        YtPick(info, 100, &v, &a);
        CHECK(v && v->itag == 160);   // hicbiri sigmazsa en kucuk
        // hata yaniti
        YtInfo bad;
        CHECK(!YtParsePlayer(L"{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm your age\"}}", bad));
        CHECK(bad.status == L"LOGIN_REQUIRED" && bad.error == L"Sign in to confirm your age");
    }
    // 2) sidx
    std::string vbytes = ReadFile(dir + "/yt_v.mp4"), abytes = ReadFile(dir + "/yt_a.mp4");
    CHECK(!vbytes.empty() && !abytes.empty());
    std::vector<SidxRef> segs;
    CHECK(FindSidx(reinterpret_cast<const uint8_t*>(vbytes.data()), vbytes.size(), 0, segs));
    CHECK(segs.size() == 20);
    if (!segs.empty()) {
        CHECK(std::fabs(segs[0].start) < 0.01 && std::fabs(segs[0].dur - 2.0) < 0.01);
        CHECK(segs.back().offset + segs.back().size <= vbytes.size());
        CHECK(vbytes.compare(size_t(segs[0].offset) + 4, 4, "moof") == 0);
        CHECK(vbytes.compare(size_t(segs[7].offset) + 4, 4, "moof") == 0);
    }
    // 3) tam akis: sahte ag (visitor_id + player + aralikli GET)
    auto boxEnd = [](const std::string& d, const char* type, long long& start, long long& end) {
        size_t i = 0;
        while (i + 8 <= d.size()) {
            uint32_t sz = (uint8_t(d[i]) << 24) | (uint8_t(d[i + 1]) << 16) | (uint8_t(d[i + 2]) << 8) | uint8_t(d[i + 3]);
            if (d.compare(i + 4, 4, type) == 0) { start = (long long)i; end = (long long)(i + sz - 1); return; }
            i += sz;
        }
    };
    long long vMoovS = 0, vMoovE = 0, vSidxS = 0, vSidxE = 0, aMoovS = 0, aMoovE = 0, aSidxS = 0, aSidxE = 0;
    boxEnd(vbytes, "moov", vMoovS, vMoovE);
    boxEnd(vbytes, "sidx", vSidxS, vSidxE);
    boxEnd(abytes, "moov", aMoovS, aMoovE);
    boxEnd(abytes, "sidx", aSidxS, aSidxE);
    char pj[4096];
    std::snprintf(pj, sizeof pj,
                  "{\"playabilityStatus\":{\"status\":\"OK\"},\"videoDetails\":{\"title\":\"Deneme\",\"lengthSeconds\":\"40\"},"
                  "\"streamingData\":{\"adaptiveFormats\":["
                  "{\"itag\":134,\"url\":\"http://test/v?id=1\",\"mimeType\":\"video/mp4; codecs=\\\"avc1.4D401E\\\"\",\"width\":256,\"height\":144,"
                  "\"initRange\":{\"start\":\"0\",\"end\":\"%lld\"},\"indexRange\":{\"start\":\"%lld\",\"end\":\"%lld\"},\"contentLength\":\"%zu\"},"
                  "{\"itag\":243,\"url\":\"http://test/vp9\",\"mimeType\":\"video/webm; codecs=\\\"vp9\\\"\",\"width\":256,\"height\":144,"
                  "\"initRange\":{\"start\":\"0\",\"end\":\"10\"},\"indexRange\":{\"start\":\"11\",\"end\":\"20\"}},"
                  "{\"itag\":140,\"url\":\"http://test/a?id=1\",\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":130000,"
                  "\"initRange\":{\"start\":\"0\",\"end\":\"%lld\"},\"indexRange\":{\"start\":\"%lld\",\"end\":\"%lld\"},\"contentLength\":\"%zu\"}]}}",
                  vMoovE, vSidxS, vSidxE, vbytes.size(), aMoovE, aSidxS, aSidxE, abytes.size());
    std::string playerJson = pj;
    std::mutex logMu;
    std::vector<std::wstring> requests;
    int getCount = 0;
    std::string lastPlayerBody, lastUa;
    HttpFn http = [&](const std::wstring& method, const std::wstring& url, const std::wstring& headers, const std::string& body,
                      size_t maxBytes, const std::atomic<bool>*, std::string& out) -> int {
        std::lock_guard<std::mutex> g(logMu);
        requests.push_back(method + L" " + url);
        size_t ua = headers.find(L"User-Agent: ");
        if (ua != std::wstring::npos) lastUa = ToUtf8(headers.substr(ua + 12, headers.find(L"\r\n", ua) - ua - 12));
        if (url.find(L"/youtubei/v1/visitor_id") != std::wstring::npos) { out = "{\"responseContext\":{\"visitorData\":\"ZIYARETCI%3D\"}}"; return 200; }
        if (url.find(L"/youtubei/v1/player") != std::wstring::npos) { lastPlayerBody = body; out = playerJson; return 200; }
        const std::string* file = url.find(L"http://test/v?") == 0 ? &vbytes : url.find(L"http://test/a?") == 0 ? &abytes : nullptr;
        if (!file) return 404;
        size_t r = url.find(L"&range=");
        if (r == std::wstring::npos) return 400;
        long long a = std::wcstoll(url.c_str() + r + 7, nullptr, 10);
        size_t dash = url.find(L'-', r + 7);
        long long b = std::wcstoll(url.c_str() + dash + 1, nullptr, 10);
        if (a < 0 || b < a || size_t(b) >= file->size()) b = (long long)file->size() - 1;
        if (size_t(b - a + 1) > maxBytes) return 413;
        out = file->substr(size_t(a), size_t(b - a + 1));
        getCount++;
        return 200;
    };
    YtOptions o;
    o.videoId = L"abcDEF12345";
    o.maxHeight = 720;
    o.ahead = 10;
    o.behind = 6;
    auto st = std::make_shared<YtStream>(o, http);
    st->Start();
    for (int i = 0; i < 200 && st->GetState() == YtStream::Resolving; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(st->GetState() == YtStream::Ready);
    if (st->GetState() != YtStream::Ready) std::printf("hata: %ls\n", st->Error().c_str());
    CHECK(st->Video() && st->Audio());
    CHECK(std::fabs(st->Duration() - 40) < 0.2);
    CHECK(st->Quality() == L"144p");
    CHECK(lastPlayerBody.find("\"visitorData\":\"ZIYARETCI%3D\"") != std::string::npos);
    CHECK(lastPlayerBody.find("\"videoId\":\"abcDEF12345\"") != std::string::npos && lastPlayerBody.find("\"clientName\":\"VISIONOS\"") != std::string::npos);
    CHECK(lastUa.find("com.google.visionos.youtube/") == 0);
    auto waitFor = [&](std::function<bool()> c) {
        for (int i = 0; i < 300; i++) { if (c()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return false;
    };
    // oynatma basi: ~10 sn tamponlanir, fazlasi degil
    bool buffered = waitFor([&]() { return st->Video() && st->Video()->EndFrom(0.2) >= 9.9 && st->Audio()->EndFrom(0.2) >= 9.9; });
    if (!buffered && st->Video()) std::printf("tampon: v=%.3f a=%.3f vlast=%.3f alast=%.3f\n", st->Video()->EndFrom(0), st->Audio()->EndFrom(0), st->Video()->LastEnd(), st->Audio()->LastEnd());
    CHECK(buffered);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(st->Video()->LastEnd() <= 14.1);
    CHECK(!st->Eos());
    TrackConfig vc = st->Video()->Config();
    CHECK(vc.width == 256 && vc.height == 144 && vc.codec == Codec::H264);
    // arama: 30. saniye
    st->Seek(30);
    CHECK(waitFor([&]() { return st->Video()->Covers(30.5) && st->Audio()->Covers(30.5); }));
    CHECK(waitFor([&]() { return st->Eos(); }));       // 30 + 10 >= 40: sona kadar inmis
    CHECK(st->Video()->LastEnd() > 39.5);
    // ilerleyen oynatma: gerideki ornekler birakilir
    st->SetPlayhead(38);
    CHECK(waitFor([&]() { return !st->Video()->Covers(1.0); }));
    // geri arama: yeniden indirilir
    st->Seek(2);
    CHECK(waitFor([&]() { return st->Video()->Covers(2.5) && st->Video()->Covers(9.0); }));
    CHECK(!st->Eos() || st->Video()->Covers(39));
    st->Cancel();
    // indirilen ornek sayisi mantikli: 24 fps
    BufSample s;
    CHECK(st->Video()->Next(2.0, true, s) && s.data && !s.data->empty());
    std::printf("istek: %d GET, %llu bayt\n", getCount, (unsigned long long)st->BytesLoaded());
    // 4) oynatilamaz video
    {
        HttpFn h2 = [&](const std::wstring&, const std::wstring& url, const std::wstring&, const std::string&, size_t, const std::atomic<bool>*, std::string& out) -> int {
            if (url.find(L"visitor_id") != std::wstring::npos) return 500;
            out = "{\"playabilityStatus\":{\"status\":\"UNPLAYABLE\",\"errorScreen\":{\"playerErrorMessageRenderer\":{\"reason\":{\"simpleText\":\"Video kullanilamiyor\"}}}}}";
            return 200;
        };
        auto s2 = std::make_shared<YtStream>(o, h2);
        s2->Start();
        for (int i = 0; i < 200 && s2->GetState() == YtStream::Resolving; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(s2->GetState() == YtStream::Failed && s2->Error() == L"Video kullanilamiyor");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf(fails ? "%d HATA\n" : "TUM YT AKIS TESTLERI GECTI\n", fails);
    return fails ? 1 : 0;
}
