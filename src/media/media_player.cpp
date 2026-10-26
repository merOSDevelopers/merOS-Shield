// merOS Shield - medya oynatici (bkz. media_player.h)
// Cozuculer: Windows'un yerlesik H.264 ve AAC MFT'leri (Media Foundation). Ses: waveOut.
// Saat: ses varsa ses aygitinin calinan ornek sayisi (ana saat), yoksa sistem saati.
#include "media_player.h"
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <set>

namespace mer {
namespace media {

namespace {

const GUID kClsidH264Dec = {0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
const GUID kClsidAacDec = {0x32d186a7, 0x218f, 0x4c75, {0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99}};

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

double NowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

IMFSample* MakeSample(const uint8_t* p, size_t n, double pts, double dur, bool key) {
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    if (FAILED(MFCreateSample(&s))) return nullptr;
    if (FAILED(MFCreateMemoryBuffer(DWORD(n), &b))) { Rel(s); return nullptr; }
    BYTE* dst = nullptr;
    if (SUCCEEDED(b->Lock(&dst, nullptr, nullptr))) {
        memcpy(dst, p, n);
        b->Unlock();
    }
    b->SetCurrentLength(DWORD(n));
    s->AddBuffer(b);
    Rel(b);
    s->SetSampleTime(LONGLONG(pts * 1e7));
    s->SetSampleDuration(LONGLONG(dur * 1e7));
    if (key) s->SetUINT32(MFSampleExtension_CleanPoint, 1);
    return s;
}

// ---------------- H.264 ----------------

struct VFrame {
    double pts = 0, dur = 0;
    int w = 0, h = 0;
    bool bt709 = false;
    std::vector<uint8_t> nv12;   // w*h Y + w*(h/2) UV (sikistirilmis, adim = w)
};

class H264Dec {
public:
    ~H264Dec() { Close(); }
    bool Open(const TrackConfig& c, std::string& err) {
        Close();
        HRESULT hr = CoCreateInstance(kClsidH264Dec, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
        if (FAILED(hr) || !mft_) { err = "H.264 cozucu (MFT) acilamadi"; return false; }
        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (c.width > 0 && c.height > 0) MFSetAttributeSize(in, MF_MT_FRAME_SIZE, UINT32(c.width), UINT32(c.height));
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = mft_->SetInputType(0, in, 0);
        Rel(in);
        if (FAILED(hr)) { err = "H.264 giris turu reddedildi"; Close(); return false; }
        if (!PickOutput()) { err = "H.264 NV12 cikisi yok"; Close(); return false; }
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }
    void Close() {
        if (mft_) { mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); }
        Rel(mft_);
        pendingPts_.clear();
    }
    bool IsOpen() const { return mft_ != nullptr; }
    void Flush() { if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); pendingPts_.clear(); }
    void Drain() { if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0); }

    // 1: kabul edildi, 0: cozucu dolu (ayni ornegi sonra tekrar dene), -1: hata
    int Feed(const BufSample& s, const TrackConfig& cfg, std::deque<VFrame>& out) {
        if (!mft_ || !s.data) return -1;
        AvccToAnnexB(cfg, s.data->data(), s.data->size(), s.key, annexb_);
        if (annexb_.empty()) return 1;
        IMFSample* smp = MakeSample(annexb_.data(), annexb_.size(), s.pts, s.dur, s.key);
        if (!smp) return -1;
        HRESULT hr = mft_->ProcessInput(0, smp, 0);
        if (hr == MF_E_NOTACCEPTING) {
            Pull(out);
            hr = mft_->ProcessInput(0, smp, 0);
        }
        Rel(smp);
        if (SUCCEEDED(hr)) pendingPts_.insert(std::make_pair(s.pts, s.dur));
        Pull(out);
        if (hr == MF_E_NOTACCEPTING) return 0;
        return SUCCEEDED(hr) ? 1 : -1;
    }

    void Pull(std::deque<VFrame>& out) {
        if (!mft_) return;
        for (int guard = 0; guard < 64; guard++) {
            MFT_OUTPUT_STREAM_INFO si = {};
            mft_->GetOutputStreamInfo(0, &si);
            MFT_OUTPUT_DATA_BUFFER odb = {};
            IMFSample* own = nullptr;
            if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                IMFMediaBuffer* b = nullptr;
                MFCreateSample(&own);
                MFCreateMemoryBuffer(si.cbSize ? si.cbSize : DWORD(outW_ * outH_ * 3 / 2 + 4096), &b);
                own->AddBuffer(b);
                Rel(b);
                odb.pSample = own;
            }
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) Rel(odb.pEvents);
            IMFSample* res = odb.pSample;      // MFT kendi ornegini verdiyse own'dan farklidir
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { if (res && res != own) res->Release(); Rel(own); break; }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (res && res != own) res->Release();
                Rel(own);
                if (!PickOutput()) break;
                continue;
            }
            if (FAILED(hr)) { if (res && res != own) res->Release(); Rel(own); break; }
            if (res) {
                Grab(res, out);
                if (res != own) res->Release();
            }
            Rel(own);
        }
    }

private:
    bool PickOutput() {
        for (DWORD i = 0;; i++) {
            IMFMediaType* t = nullptr;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
            GUID sub = {};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                HRESULT hr = mft_->SetOutputType(0, t, 0);
                if (SUCCEEDED(hr)) {
                    UINT32 w = 0, h = 0;
                    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
                    outW_ = int(w);
                    outH_ = int(h);
                    UINT32 stride = 0;
                    stride_ = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? int(int32_t(stride)) : int(w);
                    if (stride_ < int(w)) stride_ = int(w);
                    MFVideoArea area = {};
                    UINT32 sz = 0;
                    dispW_ = outW_;
                    dispH_ = outH_;
                    if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &sz)) && sz == sizeof(area)) {
                        if (area.Area.cx > 0 && area.Area.cy > 0) { dispW_ = std::min(outW_, int(area.Area.cx)); dispH_ = std::min(outH_, int(area.Area.cy)); }
                    }
                    Rel(t);
                    return true;
                }
            }
            Rel(t);
        }
        return false;
    }

    void Grab(IMFSample* s, std::deque<VFrame>& out) {
        LONGLONG t = 0, d = 0;
        s->GetSampleTime(&t);
        s->GetSampleDuration(&d);
        IMFMediaBuffer* b = nullptr;
        if (FAILED(s->ConvertToContiguousBuffer(&b)) || !b) return;
        BYTE* p = nullptr;
        LONG pitch = 0;
        IMF2DBuffer* b2 = nullptr;
        bool locked2d = false;
        DWORD len = 0;
        if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&b2))) && b2 && SUCCEEDED(b2->Lock2D(&p, &pitch))) locked2d = true;
        else if (SUCCEEDED(b->Lock(&p, nullptr, &len))) pitch = stride_;
        else p = nullptr;
        if (p && pitch > 0) {
            VFrame f;
            // Zaman damgasi: cozucu kareleri sunum sirasinda verir; beslenen en kucuk pts bu karenindir.
            // (Bazi MFT'ler -orn. Wine- giris zamanlarini korumaz; MS cozucusu korur, sonuc ayni.)
            if (!pendingPts_.empty()) {
                f.pts = pendingPts_.begin()->first;
                f.dur = pendingPts_.begin()->second;
                pendingPts_.erase(pendingPts_.begin());
            } else {
                f.pts = double(t) / 1e7;
                f.dur = double(d) / 1e7;
            }
            f.w = dispW_ & ~1;
            f.h = dispH_ & ~1;
            f.bt709 = f.h >= 720;
            size_t need = size_t(pitch) * outH_ * 3 / 2;
            if (locked2d || len >= need) {
                f.nv12.resize(size_t(f.w) * f.h * 3 / 2);
                for (int y = 0; y < f.h; y++) memcpy(&f.nv12[size_t(y) * f.w], p + size_t(y) * pitch, size_t(f.w));
                const uint8_t* uv = p + size_t(pitch) * outH_;
                uint8_t* duv = f.nv12.data() + size_t(f.w) * f.h;
                for (int y = 0; y < f.h / 2; y++) memcpy(duv + size_t(y) * f.w, uv + size_t(y) * pitch, size_t(f.w));
                // pts sirasina gore ekle (B-kareler cozucuden sirali cikar ama emin olalim)
                auto it = out.end();
                while (it != out.begin() && (it - 1)->pts > f.pts) --it;
                out.insert(it, std::move(f));
                count++;
            }
        }
        if (locked2d) b2->Unlock2D();
        else if (p) b->Unlock();
        Rel(b2);
        Rel(b);
    }

public:
    uint32_t count = 0;   // cozulen kare sayisi
private:
    IMFTransform* mft_ = nullptr;
    int outW_ = 0, outH_ = 0, stride_ = 0, dispW_ = 0, dispH_ = 0;
    std::vector<uint8_t> annexb_;
    std::multiset<std::pair<double, double>> pendingPts_;
};

// ---------------- AAC ----------------

class AacDec {
public:
    ~AacDec() { Close(); }
    bool Open(const TrackConfig& c, std::string& err) {
        Close();
        HRESULT hr = CoCreateInstance(kClsidAacDec, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
        if (FAILED(hr) || !mft_) { err = "AAC cozucu (MFT) acilamadi"; return false; }
        int rate = c.sampleRate > 0 ? c.sampleRate : 44100;
        int ch = c.channels > 0 ? c.channels : 2;
        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(rate));
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(ch));
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        in->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        std::vector<uint8_t> ud = AacUserData(c);
        in->SetBlob(MF_MT_USER_DATA, ud.data(), UINT32(ud.size()));
        hr = mft_->SetInputType(0, in, 0);
        Rel(in);
        if (FAILED(hr)) { err = "AAC giris turu reddedildi"; Close(); return false; }
        bool picked = false;
        for (DWORD i = 0; !picked; i++) {
            IMFMediaType* t = nullptr;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
            GUID sub = {};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 bits = 0;
            t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if (sub == MFAudioFormat_PCM && bits == 16 && SUCCEEDED(mft_->SetOutputType(0, t, 0))) {
                UINT32 r = 0, n = 0;
                t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
                t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &n);
                outRate = int(r ? r : UINT32(rate));
                outCh = int(n ? n : UINT32(ch));
                picked = true;
            }
            Rel(t);
        }
        if (!picked) { err = "AAC PCM cikisi yok"; Close(); return false; }
        asc_ = c.asc;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }
    void Close() { Rel(mft_); }
    bool IsOpen() const { return mft_ != nullptr; }
    bool Matches(const TrackConfig& c) const { return mft_ && c.asc == asc_; }
    void Flush() { if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }
    // Bir AAC cercevesi -> 16 bit PCM (araya eklenmis kanallar)
    bool Decode(const BufSample& s, std::vector<int16_t>& pcm) {
        if (!mft_ || !s.data) return false;
        IMFSample* smp = MakeSample(s.data->data(), s.data->size(), s.pts, s.dur, true);
        if (!smp) return false;
        HRESULT hr = mft_->ProcessInput(0, smp, 0);
        if (hr == MF_E_NOTACCEPTING) { Pull(pcm); hr = mft_->ProcessInput(0, smp, 0); }
        Rel(smp);
        Pull(pcm);
        return SUCCEEDED(hr);
    }
    int outRate = 0, outCh = 0;

private:
    void Pull(std::vector<int16_t>& pcm) {
        for (int guard = 0; guard < 16; guard++) {
            MFT_OUTPUT_STREAM_INFO si = {};
            mft_->GetOutputStreamInfo(0, &si);
            MFT_OUTPUT_DATA_BUFFER odb = {};
            IMFSample* own = nullptr;
            if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                IMFMediaBuffer* b = nullptr;
                MFCreateSample(&own);
                MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 65536, &b);
                own->AddBuffer(b);
                Rel(b);
                odb.pSample = own;
            }
            DWORD st = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &odb, &st);
            if (odb.pEvents) Rel(odb.pEvents);
            IMFSample* res = odb.pSample;
            if (FAILED(hr)) { if (res && res != own) res->Release(); Rel(own); break; }
            if (res) {
                IMFMediaBuffer* b = nullptr;
                if (SUCCEEDED(res->ConvertToContiguousBuffer(&b)) && b) {
                    BYTE* p = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                        size_t n = len / 2;
                        size_t o = pcm.size();
                        pcm.resize(o + n);
                        memcpy(pcm.data() + o, p, n * 2);
                        b->Unlock();
                    }
                    Rel(b);
                }
                if (res != own) res->Release();
            }
            Rel(own);
        }
    }
    IMFTransform* mft_ = nullptr;
    std::vector<uint8_t> asc_;
};

// ---------------- waveOut ----------------

class WaveOut {
public:
    ~WaveOut() { Close(); }
    bool Open(int rate, int ch) {
        Close();
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = WORD(ch);
        wf.nSamplesPerSec = DWORD(rate);
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = WORD(ch * 2);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        if (waveOutOpen(&h_, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { h_ = nullptr; return false; }
        rate_ = rate;
        ch_ = ch;
        waveOutPause(h_);
        paused_ = true;
        return true;
    }
    void Close() {
        if (!h_) return;
        waveOutReset(h_);
        Reap(true);
        waveOutClose(h_);
        h_ = nullptr;
    }
    bool IsOpen() const { return h_ != nullptr; }
    int Rate() const { return rate_; }
    int Channels() const { return ch_; }
    void Write(const int16_t* p, size_t samples) {
        if (!h_ || samples == 0) return;
        Reap(false);
        Buf* b = new Buf();
        b->data.assign(p, p + samples);
        b->hdr.lpData = reinterpret_cast<LPSTR>(b->data.data());
        b->hdr.dwBufferLength = DWORD(samples * 2);
        if (waveOutPrepareHeader(h_, &b->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { delete b; return; }
        if (waveOutWrite(h_, &b->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { waveOutUnprepareHeader(h_, &b->hdr, sizeof(WAVEHDR)); delete b; return; }
        queued_.push_back(b);
        written_ += samples / size_t(ch_);
    }
    // aygita yazilan ama henuz calinmamis cerceve sayisi
    double QueuedSec() {
        if (!h_) return 0;
        return double(written_) / rate_ - Played();
    }
    double Played() {
        if (!h_) return 0;
        MMTIME mt = {};
        mt.wType = TIME_SAMPLES;
        if (waveOutGetPosition(h_, &mt, sizeof mt) != MMSYSERR_NOERROR) return 0;
        if (mt.wType == TIME_SAMPLES) return double(mt.u.sample) / rate_;
        if (mt.wType == TIME_BYTES) return double(mt.u.cb) / (rate_ * ch_ * 2);
        return 0;
    }
    void Pause() { if (h_ && !paused_) { waveOutPause(h_); paused_ = true; } }
    void Resume() { if (h_ && paused_) { waveOutRestart(h_); paused_ = false; } }
    void Reset() {
        if (!h_) return;
        waveOutReset(h_);   // konum 0'a doner, tamponlar geri verilir
        Reap(true);
        written_ = 0;
        waveOutPause(h_);
        paused_ = true;
    }
    void SetVolume(double v) {
        if (!h_) return;
        DWORD x = DWORD(std::max(0.0, std::min(1.0, v)) * 0xFFFF);
        waveOutSetVolume(h_, (x << 16) | x);
    }

private:
    struct Buf { WAVEHDR hdr = {}; std::vector<int16_t> data; };
    void Reap(bool all) {
        while (!queued_.empty()) {
            Buf* b = queued_.front();
            if (!all && !(b->hdr.dwFlags & WHDR_DONE)) break;
            waveOutUnprepareHeader(h_, &b->hdr, sizeof(WAVEHDR));
            delete b;
            queued_.pop_front();
        }
    }
    HWAVEOUT h_ = nullptr;
    int rate_ = 44100, ch_ = 2;
    bool paused_ = true;
    size_t written_ = 0;
    std::deque<Buf*> queued_;
};

} // namespace

void Nv12ToBgra(const uint8_t* yp, int yPitch, const uint8_t* uvp, int uvPitch, int w, int h, bool bt709, uint8_t* out) {
    // 16.16 sabit nokta katsayilar (sinirli aralik)
    const int cy = 76309;                       // 1.164
    const int crv = bt709 ? 117504 : 104597;    // 1.793 / 1.596
    const int cgu = bt709 ? 13954 : 25675;      // 0.213 / 0.392
    const int cgv = bt709 ? 34903 : 53279;      // 0.533 / 0.813
    const int cbu = bt709 ? 138453 : 132201;    // 2.112 / 2.017
    for (int y = 0; y < h; y++) {
        const uint8_t* yr = yp + size_t(y) * yPitch;
        const uint8_t* uvr = uvp + size_t(y / 2) * uvPitch;
        uint8_t* o = out + size_t(y) * w * 4;
        for (int x = 0; x < w; x += 2) {
            int u = uvr[x] - 128, v = uvr[x + 1] - 128;
            int rr = crv * v + 32768, gg = -cgu * u - cgv * v + 32768, bb = cbu * u + 32768;
            for (int k = 0; k < 2 && x + k < w; k++) {
                int yy = (yr[x + k] - 16) * cy;
                int r = (yy + rr) >> 16, g = (yy + gg) >> 16, b = (yy + bb) >> 16;
                o[0] = uint8_t(b < 0 ? 0 : b > 255 ? 255 : b);
                o[1] = uint8_t(g < 0 ? 0 : g > 255 ? 255 : g);
                o[2] = uint8_t(r < 0 ? 0 : r > 255 ? 255 : r);
                o[3] = 255;
                o += 4;
            }
        }
    }
}

struct MediaPlayer::Impl {
    std::mutex mu;
    std::condition_variable cv;
    std::thread th;
    bool quit = false;

    // komutlar (mu altinda)
    std::shared_ptr<TrackBuffer> vbuf, abuf;
    bool tracksChanged = false;
    bool wantPlay = false;
    bool seekReq = false;
    double seekTarget = 0;
    double volume = 1;
    bool muted = false;
    double rate = 1;
    bool eos = false;
    double duration = NAN;
    FrameSink sink;
    PlayerStatus st;

    void Run();
};

MediaPlayer::MediaPlayer() : d_(new Impl()) {
    d_->th = std::thread([this] { d_->Run(); });
}

MediaPlayer::~MediaPlayer() {
    {
        std::lock_guard<std::mutex> g(d_->mu);
        d_->quit = true;
    }
    d_->cv.notify_all();
    if (d_->th.joinable()) d_->th.join();
}

void MediaPlayer::SetTracks(std::shared_ptr<TrackBuffer> v, std::shared_ptr<TrackBuffer> a) {
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->vbuf == v && d_->abuf == a) return;
    d_->vbuf = v;
    d_->abuf = a;
    d_->tracksChanged = true;
    d_->cv.notify_all();
}
void MediaPlayer::SetSink(FrameSink s) { std::lock_guard<std::mutex> g(d_->mu); d_->sink = std::move(s); }
void MediaPlayer::Play() { std::lock_guard<std::mutex> g(d_->mu); d_->wantPlay = true; d_->st.ended = false; d_->cv.notify_all(); }
void MediaPlayer::Pause() { std::lock_guard<std::mutex> g(d_->mu); d_->wantPlay = false; d_->cv.notify_all(); }
void MediaPlayer::Seek(double t) {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->seekReq = true;
    d_->seekTarget = std::max(0.0, t);
    d_->st.seeking = true;
    d_->st.ended = false;
    d_->st.time = d_->seekTarget;
    d_->cv.notify_all();
}
void MediaPlayer::SetVolume(double v, bool m) { std::lock_guard<std::mutex> g(d_->mu); d_->volume = v; d_->muted = m; d_->cv.notify_all(); }
void MediaPlayer::SetRate(double r) { std::lock_guard<std::mutex> g(d_->mu); d_->rate = r > 0 ? r : 1; d_->cv.notify_all(); }
void MediaPlayer::SetEndOfStream(bool e, double dur) { std::lock_guard<std::mutex> g(d_->mu); d_->eos = e; d_->duration = dur; d_->cv.notify_all(); }
void MediaPlayer::Wake() { d_->cv.notify_all(); }
PlayerStatus MediaPlayer::Status() { std::lock_guard<std::mutex> g(d_->mu); return d_->st; }

void MediaPlayer::Impl::Run() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mfOk = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    timeBeginPeriod(1);

    H264Dec vdec;
    AacDec adec;
    WaveOut wo;
    std::deque<VFrame> frames;
    std::vector<int16_t> pcm;
    std::shared_ptr<TrackBuffer> vb, ab;

    // konum durumu
    double vNext = 0, aNext = 0;       // sonraki cozulecek ornegin dts'si
    bool vIncl = true, aIncl = true;
    bool vNeedKey = true;
    bool vDrained = false;
    double presentMin = 0;             // aramadan sonra bundan onceki kareler gosterilmez
    bool seeking = true, seekPending = true;
    double seekTo = 0;
    bool playing = false;
    // saat
    bool audioActive = false;          // ses aygiti saat kaynagi
    bool audioBaseSet = false;
    double audioBase = 0;
    double sysBase = 0, sysStart = NowSec();
    bool sysRunning = false;
    double curTime = 0;
    uint32_t decoded = 0, dropped = 0, shown = 0, seekSerial = 0;
    std::string err;
    bool vErr = false, aErr = false;
    int vw = 0, vh = 0;
    double volume = 1, rate = 1;
    bool muted = false, eos = false;
    double duration = NAN;
    FrameSink sink;
    bool endedFlag = false;

    auto clockNow = [&]() -> double {
        if (audioActive && audioBaseSet) return audioBase + wo.Played();
        if (sysRunning) return sysBase + (NowSec() - sysStart) * rate;
        return sysBase;
    };
    auto freezeSys = [&](double t) { sysBase = t; sysStart = NowSec(); sysRunning = false; };
    auto startSys = [&](double t) { sysBase = t; sysStart = NowSec(); sysRunning = true; };

    while (true) {
        bool tracksChangedL = false, seekReqL = false;
        double seekTargetL = 0;
        bool wantPlay = false;
        {
            std::unique_lock<std::mutex> g(mu);
            if (quit) break;
            if (tracksChanged) { tracksChangedL = true; tracksChanged = false; vb = vbuf; ab = abuf; }
            if (seekReq) { seekReqL = true; seekTargetL = seekTarget; seekReq = false; }
            wantPlay = this->wantPlay;
            volume = this->volume;
            muted = this->muted;
            rate = this->rate;
            eos = this->eos;
            duration = this->duration;
            sink = this->sink;
        }
        if (tracksChangedL && !seekReqL) { seekReqL = true; seekTargetL = curTime; }
        if (seekReqL) {
            // Flush yerine cozuculeri yeniden ac: bazi MFT'ler (orn. Wine) flush'ta ic kuyrugu bosaltmiyor
            vdec.Close();
            adec.Close();
            frames.clear();
            wo.Reset();
            audioBaseSet = false;
            seeking = true;
            seekPending = true;
            seekTo = seekTargetL;
            curTime = seekTo;
            freezeSys(seekTo);
            vDrained = false;
            endedFlag = false;
        }
        bool hasV = vb && vb->Config().codec == Codec::H264;
        bool hasA = ab && ab->Config().codec == Codec::AAC;

        // arama noktasi: veri gelene kadar bekle
        if (seekPending) {
            bool ok = true;
            double kd = 0, kp = 0;
            if (hasV) {
                if (vb->SeekPoint(seekTo, kd, kp)) { vNext = kd; vIncl = true; vNeedKey = true; }
                else ok = false;
            }
            if (hasA && ok) {
                double ad = 0, ap = 0;
                if (ab->SeekPoint(seekTo, ad, ap)) { aNext = ad; aIncl = true; }
                else if (!hasV) ok = false;
                else { aNext = seekTo; aIncl = true; }
            }
            if (!hasV && !hasA) ok = false;
            if (ok) { seekPending = false; presentMin = seekTo; }
        }

        // cozuculeri ac
        if (hasV && !vdec.IsOpen() && !vErr && mfOk) {
            if (!vdec.Open(vb->Config(), err)) vErr = true;
        }
        if (hasA && !aErr && mfOk) {
            TrackConfig ac = ab->Config();
            if (!adec.Matches(ac)) {
                if (!adec.Open(ac, err)) aErr = true;
                else if (!wo.IsOpen() || wo.Rate() != adec.outRate || wo.Channels() != adec.outCh) {
                    wo.Open(adec.outRate, adec.outCh);
                    audioBaseSet = false;
                }
            }
        }

        if (!seekPending) {
            // ---- video: birkac kare ileriyi coz ----
            if (hasV && vdec.IsOpen()) {
                TrackConfig vc = vb->Config();
                vdec.Pull(frames);   // cozucude bekleyen kareler
                int fed = 0;
                while (frames.size() < 5 && fed < 8) {
                    BufSample s;
                    if (!vb->Next(vNext, vIncl, s)) {
                        if (eos && !vDrained) { vdec.Drain(); vdec.Pull(frames); vDrained = true; }
                        break;
                    }
                    // cozucu ici gecikme olsa da saatten en fazla ~1 sn ileriyi besle
                    if (!vNeedKey && !seeking && s.pts > clockNow() + 1.0) break;
                    if (vNeedKey && !s.key) { vNext = s.dts; vIncl = false; continue; }
                    int fr = vdec.Feed(s, vc, frames);
                    if (fr < 0) { vErr = true; err = "H.264 cozme hatasi"; break; }
                    if (fr == 0) break;      // cozucu dolu: ayni ornek sonraki turda
                    vNext = s.dts;
                    vIncl = false;
                    vNeedKey = false;
                    fed++;
                    // aramadan onceki kareleri at (son birini tut: hedef iki kare arasindaysa)
                    while (frames.size() >= 2 && frames[1].pts <= presentMin + 1e-3) { frames.pop_front(); }
                }
                decoded = vdec.count;
                while (frames.size() >= 2 && frames[1].pts <= presentMin + 1e-3) frames.pop_front();
                if (!frames.empty() && (frames.front().w != vw || frames.front().h != vh)) { vw = frames.front().w; vh = frames.front().h; }
            }

            // ---- ses ----
            audioActive = hasA && adec.IsOpen() && wo.IsOpen() && std::fabs(rate - 1.0) < 1e-6;
            if (audioActive) {
                wo.SetVolume(muted ? 0 : volume);
                int guard = 0;
                while (wo.QueuedSec() < 0.4 && guard++ < 64) {
                    BufSample s;
                    if (!ab->Next(aNext, aIncl, s)) break;
                    aNext = s.dts;
                    aIncl = false;
                    if (s.pts + s.dur < seekTo - 0.05 && !audioBaseSet) continue;   // hedefin gerisinde
                    pcm.clear();
                    adec.Decode(s, pcm);
                    if (!pcm.empty()) {
                        if (!audioBaseSet) { audioBase = s.pts; audioBaseSet = true; }
                        wo.Write(pcm.data(), pcm.size());
                    }
                }
            } else if (hasA) {
                // ses kapali (hiz != 1 ya da aygit yok): ornekleri saatle birlikte ilerlet
                BufSample s;
                while (ab->Next(aNext, aIncl, s) && s.pts < curTime) { aNext = s.dts; aIncl = false; }
            }

            // ---- arama tamamlandi mi ----
            if (seeking) {
                bool vReady = !hasV || vErr || !frames.empty() || vDrained;
                bool aReady = !audioActive || audioBaseSet || eos;
                if (vReady && aReady) {
                    seeking = false;
                    seekSerial++;
                    if (!frames.empty() && sink) {
                        VFrame& f = frames.front();
                        auto px = std::make_shared<std::vector<uint8_t>>(size_t(f.w) * f.h * 4);
                        Nv12ToBgra(f.nv12.data(), f.w, f.nv12.data() + size_t(f.w) * f.h, f.w, f.w, f.h, f.bt709, px->data());
                        sink(f.w, f.h, px);
                        shown++;
                    }
                }
            }

            // ---- oynat / duraklat ----
            bool shouldPlay = wantPlay && !seeking && !endedFlag;
            if (shouldPlay != playing) {
                if (shouldPlay) {
                    if (audioActive) wo.Resume();
                    startSys(curTime);
                } else {
                    curTime = clockNow();
                    if (audioActive) wo.Pause();
                    freezeSys(curTime);
                }
                playing = shouldPlay;
            }

            // ---- saat ve bekleme ----
            bool waiting = false;
            if (playing) {
                double t = clockNow();
                BufSample tmp;
                bool vMore = hasV && !vErr && (!frames.empty() || vb->Next(vNext, vIncl, tmp) || (eos && !vDrained));
                bool aMore = audioActive && (wo.QueuedSec() > 0.02 || ab->Next(aNext, aIncl, tmp));
                if (audioActive) {
                    if (!aMore && !eos) waiting = true;
                } else if (hasV && !vErr) {
                    // sistem saati: kare yoksa saati dondur
                    if (frames.empty() && !vMore && !eos) {
                        if (sysRunning) freezeSys(t);
                        waiting = true;
                    } else if (!sysRunning) startSys(t);
                }
                t = clockNow();
                if (!audioActive && !std::isnan(duration) && duration > 0 && t > duration) t = duration;
                curTime = t;
                // kare sunumu
                if (hasV && !frames.empty() && frames.front().pts <= t + 0.008) {
                    int popped = 0;
                    VFrame pick;
                    while (!frames.empty() && frames.front().pts <= t + 0.008) { pick = std::move(frames.front()); frames.pop_front(); popped++; }
                    if (popped > 1) dropped += uint32_t(popped - 1);
                    if (sink && pick.w > 0) {
                        auto px = std::make_shared<std::vector<uint8_t>>(size_t(pick.w) * pick.h * 4);
                        Nv12ToBgra(pick.nv12.data(), pick.w, pick.nv12.data() + size_t(pick.w) * pick.h, pick.w, pick.w, pick.h, pick.bt709, px->data());
                        sink(pick.w, pick.h, px);
                    }
                    shown++;
                }
                // son
                if (eos) {
                    double endT = !std::isnan(duration) && duration > 0 ? duration : 0;
                    if (endT <= 0) {
                        if (hasV) endT = std::max(endT, vb->LastEnd());
                        if (hasA) endT = std::max(endT, ab->LastEnd());
                    }
                    BufSample nx;
                    bool vFedAll = !hasV || vErr || !vb->Next(vNext, vIncl, nx);
                    bool aDone = !audioActive || wo.QueuedSec() <= 0.01;
                    if (vFedAll && aDone && t >= endT - 0.05) {
                        endedFlag = true;
                        playing = false;
                        if (!std::isnan(duration)) curTime = duration;
                        if (audioActive) wo.Pause();
                        freezeSys(curTime);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> g(mu);
                st.time = seeking ? seekTo : curTime;
                st.playing = playing;
                st.waiting = waiting;
                st.seeking = seeking || seekPending;
                st.ended = endedFlag;
                st.videoW = vw;
                st.videoH = vh;
                st.framesDecoded = decoded;
                st.framesDropped = dropped;
                st.framesShown = shown;
                st.seekSerial = seekSerial;
                st.decodeError = vErr && aErr ? true : (vErr && !hasA) || (aErr && !hasV);
                st.error = err;
                st.audioOut = audioActive;
                if (endedFlag) this->wantPlay = false;
            }
        } else {
            std::lock_guard<std::mutex> g(mu);
            st.seeking = true;
            st.time = seekTo;
            st.waiting = wantPlay;
        }

        // ---- uyu ----
        double sleep = 0.05;
        if (playing) {
            sleep = 0.01;
            if (!frames.empty()) {
                double dt = frames.front().pts - clockNow();
                sleep = std::max(0.001, std::min(0.01, dt));
            }
        } else if (seekPending || seeking) sleep = 0.01;
        std::unique_lock<std::mutex> g(mu);
        if (quit) break;
        cv.wait_for(g, std::chrono::microseconds(int64_t(sleep * 1e6)));
    }

    vdec.Close();
    adec.Close();
    wo.Close();
    timeEndPeriod(1);
    if (mfOk) MFShutdown();
    if (SUCCEEDED(coHr)) CoUninitialize();
}

} // namespace media
} // namespace mer
