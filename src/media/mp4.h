// merOS Shield - MP4 / parcali MP4 (fMP4) ayristirici
// MSE (MediaSource Extensions) icin: baslatma bolumu (ftyp+moov) ve medya bolumleri (moof+mdat)
// parca parca eklenir; tamamlanan kutular islenir, ornekler (sample) cikarilir.
// Ilerlemeli (parcasiz) MP4 de desteklenir: moov icindeki ornek tablolari (stts/ctts/stsz/stsc/stco/stss).
// Platformdan bagimsizdir (Windows gerektirmez) ve Linux'ta ffprobe ile karsilastirilarak test edilir.
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mer {
namespace media {

enum class Codec { None, H264, AAC, Unsupported };

struct TrackConfig {
    uint32_t trackId = 0;
    bool video = false;
    Codec codec = Codec::None;
    std::string fourcc;              // "avc1", "mp4a", ...
    uint32_t timescale = 0;
    // video
    int width = 0, height = 0;       // tkhd / sample entry
    int nalLenSize = 4;
    std::vector<std::vector<uint8_t>> sps, pps;
    int profile = 0, level = 0;
    // ses
    int sampleRate = 0, channels = 0;
    int aacObjectType = 0;
    std::vector<uint8_t> asc;        // AudioSpecificConfig
    // trex varsayilanlari
    uint32_t defDur = 0, defSize = 0, defFlags = 0;
    std::string CodecString() const; // "avc1.64001f", "mp4a.40.2"
};

struct Sample {
    uint32_t trackId = 0;
    int64_t dts = 0, pts = 0;        // track timescale birimi
    uint32_t dur = 0;
    bool key = false;
    std::shared_ptr<std::vector<uint8_t>> data;
};

class Mp4Parser {
public:
    // Bayt ekle; tamamlanan kutulari isler. false: bicim hatasi (error doldurulur).
    bool Append(const uint8_t* p, size_t n);
    // Yarim kalan veriyi at (SourceBuffer.abort / resetParserState)
    void ResetParserState();

    bool haveInit = false;           // moov goruldu
    bool initChanged = false;        // son Append'de yeni moov geldi (cagiran sifirlar)
    std::vector<TrackConfig> tracks;
    std::vector<Sample> out;         // cikarilan ornekler (cagiran bosaltir)
    std::string error;
    bool progressive = false;        // moov ornek tablolari tasiyor (parcasiz dosya)

    const TrackConfig* Track(uint32_t id) const;

    // Parcasiz dosya: butun dosya bellekteyken ornekleri tablolardan uret
    static bool ParseFile(const uint8_t* p, size_t n, std::vector<TrackConfig>& tracks, std::vector<Sample>& samples, std::string& err);

private:
    std::vector<uint8_t> buf_;       // islenmemis baytlar
    uint64_t bufPos_ = 0;            // buf_[0]'in akistaki konumu
    // son moof'un ornek tanimlari (mdat bekleniyor)
    struct Pending { uint32_t trackId; int64_t dts; int32_t cts; uint32_t dur, size; bool key; uint64_t absOff; };
    std::vector<Pending> pending_;
    std::vector<std::pair<uint32_t, int64_t>> edits_;   // iz -> elst medya kaymasi (B-kare gecikmesi)
    std::map<uint32_t, int64_t> nextDts_;               // tfdt yoksa devam
    int64_t EditOffset(uint32_t id) const;
    bool ParseBox(const uint8_t* p, size_t n, uint64_t absStart, const std::string& type, size_t hdr);
    bool ParseMoov(const uint8_t* p, size_t n);
    bool ParseMoof(const uint8_t* p, size_t n, uint64_t moofStart);
    void TakeFromMdat(const uint8_t* p, size_t n, uint64_t dataAbs);
};

// H.264: AVCC (uzunluk onekli) -> Annex B (baslangic kodlu); anahtar karede SPS/PPS eklenir
void AvccToAnnexB(const TrackConfig& t, const uint8_t* p, size_t n, bool key, std::vector<uint8_t>& out);
// AAC ADTS basligi olmadan ham cerceve; MF icin HEAACWAVEINFO ek verisi (12 bayt + ASC)
std::vector<uint8_t> AacUserData(const TrackConfig& t);

// MIME/codec dizgesi destekleniyor mu ("video/mp4; codecs=\"avc1.4d401f\"")
// 2: kesin (probably), 1: belki (maybe), 0: hayir
int SupportLevel(const std::wstring& mime);

} // namespace media
} // namespace mer
