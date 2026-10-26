// merOS Shield - medya oynatici: H.264/AAC cozme (Media Foundation MFT), ses cikisi (waveOut),
// A/V esleme ve kare sunumu. Kendi is parcaciginda calisir; JS tarafi yalnizca komut verir ve
// durum (Status) okur. Kareler FrameSink ile arayuze (UI is parcacigina) iletilir.
#pragma once
#include "track_buffer.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mer {
namespace media {

// w, h: piksel; px: BGRA (onceden carpilmis alfa = 255). Oynatici is parcacigindan cagrilir.
using FrameSink = std::function<void(int w, int h, std::shared_ptr<std::vector<uint8_t>> px)>;

struct PlayerStatus {
    double time = 0;          // gecerli sunum zamani (s)
    bool playing = false;     // oynatma istendi ve duraklatilmadi
    bool waiting = false;     // veri bekleniyor (tampon bos)
    bool seeking = false;
    bool ended = false;
    int videoW = 0, videoH = 0;
    uint32_t framesDecoded = 0, framesDropped = 0, framesShown = 0;
    uint32_t seekSerial = 0;  // tamamlanan arama sayaci
    bool decodeError = false;
    std::string error;
    bool audioOut = false;    // ses aygiti acik
};

class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    void SetTracks(std::shared_ptr<TrackBuffer> video, std::shared_ptr<TrackBuffer> audio);
    void SetSink(FrameSink sink);
    void Play();
    void Pause();
    void Seek(double t);
    void SetVolume(double v, bool muted);
    void SetRate(double r);
    void SetEndOfStream(bool eos, double duration);
    void Wake();               // yeni veri eklendi
    PlayerStatus Status();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// NV12 -> BGRA (sinirli aralik, BT.601 / BT.709)
void Nv12ToBgra(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, bool bt709, uint8_t* out);

} // namespace media
} // namespace mer
