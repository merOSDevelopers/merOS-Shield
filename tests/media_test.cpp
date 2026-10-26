// Medya hatti testi: fMP4 dosyalarini TrackBuffer'a yukler, MediaPlayer ile oynatir.
// kullanim: media_test video.mp4 [ses.mp4]
#include "media/media_player.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <atomic>
using namespace mer::media;

static std::vector<uint8_t> ReadAll(const char* p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); }

static std::shared_ptr<TrackBuffer> Load(const char* path, bool& isVideo) {
    std::vector<uint8_t> d = ReadAll(path);
    Mp4Parser p;
    if (!p.Append(d.data(), d.size())) { printf("ayristirma hatasi: %s\n", p.error.c_str()); return nullptr; }
    if (p.tracks.empty()) return nullptr;
    const TrackConfig& t = p.tracks[0];
    isVideo = t.video;
    auto tb = std::make_shared<TrackBuffer>(t.video);
    tb->SetConfig(t);
    std::vector<BufSample> g;
    for (auto& s : p.out) { BufSample b; b.pts = double(s.pts) / t.timescale; b.dts = double(s.dts) / t.timescale; b.dur = double(s.dur) / t.timescale; b.key = s.key; b.data = s.data; g.push_back(b); }
    tb->Insert(g);
    auto r = tb->Buffered();
    printf("%s: %s %zu ornek, tampon", path, t.CodecString().c_str(), g.size());
    for (auto& x : r) printf(" [%.3f,%.3f]", x.first, x.second);
    printf("\n");
    return tb;
}

int main(int argc, char** argv) {
    std::shared_ptr<TrackBuffer> v, a;
    for (int i = 1; i < argc; i++) { bool isV = false; auto t = Load(argv[i], isV); if (!t) return 1; (isV ? v : a) = t; }
    std::atomic<int> frames{0};
    std::atomic<uint32_t> sum{0};
    int fw = 0, fh = 0;
    {
        MediaPlayer mp;
        mp.SetSink([&](int w, int h, std::shared_ptr<std::vector<uint8_t>> px) {
            frames++;
            fw = w; fh = h;
            if (frames == 30) {
                FILE* f = fopen("frame30.ppm", "wb");
                fprintf(f, "P6 %d %d 255\n", w, h);
                for (size_t i = 0; i + 3 < px->size(); i += 4) { fputc((*px)[i + 2], f); fputc((*px)[i + 1], f); fputc((*px)[i], f); }
                fclose(f);
            }
        });
        mp.SetTracks(v, a);
        mp.SetEndOfStream(true, 10.0);
        mp.Seek(0);
        Sleep(300);
        PlayerStatus s0 = mp.Status();
        printf("arama sonrasi: t=%.3f seeking=%d seekSerial=%u kare=%d %dx%d hata='%s'\n", s0.time, s0.seeking, s0.seekSerial, frames.load(), s0.videoW, s0.videoH, s0.error.c_str());
        mp.Play();
        for (int i = 0; i < 6; i++) {
            Sleep(500);
            PlayerStatus s = mp.Status();
            printf("t=%.3f playing=%d waiting=%d ses=%d kare=%d cozulen=%u atlanan=%u\n", s.time, s.playing, s.waiting, s.audioOut, frames.load(), s.framesDecoded, s.framesDropped);
        }
        mp.Pause();
        Sleep(200);
        double tp = mp.Status().time;
        Sleep(400);
        printf("duraklatildi: %.3f -> %.3f\n", tp, mp.Status().time);
        mp.Seek(7.5);
        Sleep(400);
        PlayerStatus s2 = mp.Status();
        printf("7.5'e arama: t=%.3f seeking=%d serial=%u\n", s2.time, s2.seeking, s2.seekSerial);
        mp.Play();
        for (int i = 0; i < 7; i++) { Sleep(500); PlayerStatus s = mp.Status(); printf("  t=%.3f kare=%d cozulen=%u atlanan=%u ended=%d\n", s.time, frames.load(), s.framesDecoded, s.framesDropped, s.ended); }
        PlayerStatus s3 = mp.Status();
        printf("son: t=%.3f ended=%d playing=%d kare=%d\n", s3.time, s3.ended, s3.playing, frames.load());
    }
    printf("TAMAM %dx%d\n", fw, fh);
    return 0;
}
