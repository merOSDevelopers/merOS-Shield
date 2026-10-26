// merOS Shield - MP4 / fMP4 ayristirici (bkz. mp4.h)
#include "mp4.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace mer {
namespace media {

namespace {

static constexpr uint32_t kMaxMp4Samples = 2000000;
static constexpr uint32_t kMaxTrunSamples = 500000;

struct Rd {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    Rd(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
    bool Need(size_t k) { if (i > n || k > n - i) { ok = false; return false; } return true; }
    uint8_t U8() { if (!Need(1)) return 0; return p[i++]; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = uint16_t((p[i] << 8) | p[i + 1]); i += 2; return v; }
    uint32_t U24() { if (!Need(3)) return 0; uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2]; i += 3; return v; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v = (uint32_t(p[i]) << 24) | (uint32_t(p[i + 1]) << 16) | (uint32_t(p[i + 2]) << 8) | p[i + 3]; i += 4; return v; }
    uint64_t U64() { uint64_t h = U32(); return (h << 32) | U32(); }
    void Skip(size_t k) { if (Need(k)) i += k; }
    const uint8_t* Cur() const { return p + i; }
    size_t Left() const { return i <= n ? n - i : 0; }
};

std::string FourCC(const uint8_t* p) { return std::string(reinterpret_cast<const char*>(p), 4); }

// Cocuk kutular: cb(type, body, bodyLen, absOffsetOfBox) ; false donerse durur
template <class F>
bool ForEachBox(const uint8_t* p, size_t n, F cb) {
    size_t i = 0;
    while (i <= n && n - i >= 8) {
        uint64_t size = (uint64_t(p[i]) << 24) | (uint64_t(p[i + 1]) << 16) | (uint64_t(p[i + 2]) << 8) | p[i + 3];
        std::string type = FourCC(p + i + 4);
        size_t hdr = 8;
        if (size == 1) {
            if (n - i < 16) return false;
            size = 0;
            for (int k = 0; k < 8; k++) size = (size << 8) | p[i + 8 + k];
            hdr = 16;
        } else if (size == 0) size = n - i;
        if (size < hdr || size > n - i) return false;
        if (!cb(type, p + i + hdr, size_t(size - hdr), i)) return true;
        i += size_t(size);
    }
    return true;
}

bool Printable4(const uint8_t* p) {
    for (int k = 0; k < 4; k++) { uint8_t c = p[k]; if (c < 0x20 || c > 0x7E) return false; }
    return true;
}

// Ilerlemeli dosya icin ornek tablolari
struct Tables {
    std::vector<std::pair<uint32_t, uint32_t>> stts;   // sayi, sure
    std::vector<std::pair<uint32_t, int32_t>> ctts;    // sayi, kayma
    std::vector<uint32_t> sizes;
    uint32_t constSize = 0, sampleCount = 0;
    struct Stsc { uint32_t first, perChunk; };
    std::vector<Stsc> stsc;
    std::vector<uint64_t> chunks;
    std::vector<uint32_t> sync;  // 1 tabanli
    bool hasSync = false;
    int64_t editOffset = 0;
};

uint32_t DescLen(Rd& r) {
    uint32_t len = 0;
    for (int k = 0; k < 4; k++) {
        uint8_t b = r.U8();
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

void ParseASC(TrackConfig& t) {
    if (t.asc.size() < 2) return;
    // bit okuyucu
    size_t bit = 0;
    auto bits = [&](int k) {
        uint32_t v = 0;
        for (int j = 0; j < k; j++) {
            size_t byte = bit >> 3;
            uint32_t b = byte < t.asc.size() ? (t.asc[byte] >> (7 - (bit & 7))) & 1 : 0;
            v = (v << 1) | b;
            bit++;
        }
        return v;
    };
    int ot = int(bits(5));
    if (ot == 31) ot = 32 + int(bits(6));
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint32_t fi = bits(4);
    int rate = fi == 15 ? int(bits(24)) : (fi < 13 ? kRates[fi] : 0);
    int ch = int(bits(4));
    t.aacObjectType = ot;
    if (rate > 0) t.sampleRate = rate;
    if (ch > 0) t.channels = ch == 7 ? 8 : ch;
}

void ParseEsds(TrackConfig& t, const uint8_t* p, size_t n) {
    Rd r(p, n);
    r.Skip(4); // version/flags
    while (r.ok && r.Left() > 2) {
        uint8_t tag = r.U8();
        uint32_t len = DescLen(r);
        if (tag == 0x03) {
            r.Skip(2);
            uint8_t fl = r.U8();
            if (fl & 0x80) r.Skip(2);
            if (fl & 0x40) { uint8_t ul = r.U8(); r.Skip(ul); }
            if (fl & 0x20) r.Skip(2);
            continue; // icindeki tanimlayicilara gir
        }
        if (tag == 0x04) {
            uint8_t oti = r.U8();
            r.Skip(12);
            if (oti != 0x40 && oti != 0x66 && oti != 0x67 && oti != 0x68) t.codec = Codec::Unsupported;  // AAC degil (orn. MP3 0x6B)
            continue;
        }
        if (tag == 0x05) {
            if (!r.Need(len)) return;
            t.asc.assign(r.Cur(), r.Cur() + len);
            ParseASC(t);
            return;
        }
        r.Skip(len);
    }
}

void ParseAvcC(TrackConfig& t, const uint8_t* p, size_t n) {
    Rd r(p, n);
    r.U8();
    t.profile = r.U8();
    uint8_t compat = r.U8();
    (void)compat;
    t.level = r.U8();
    t.nalLenSize = (r.U8() & 3) + 1;
    int nsps = r.U8() & 31;
    for (int k = 0; k < nsps && r.ok; k++) {
        uint16_t l = r.U16();
        if (!r.Need(l)) break;
        t.sps.emplace_back(r.Cur(), r.Cur() + l);
        r.Skip(l);
    }
    int npps = r.U8();
    for (int k = 0; k < npps && r.ok; k++) {
        uint16_t l = r.U16();
        if (!r.Need(l)) break;
        t.pps.emplace_back(r.Cur(), r.Cur() + l);
        r.Skip(l);
    }
}

void ParseStsd(TrackConfig& t, const uint8_t* p, size_t n) {
    Rd r(p, n);
    r.Skip(4);
    uint32_t cnt = r.U32();
    if (!r.ok || cnt == 0) return;
    ForEachBox(r.Cur(), r.Left(), [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        t.fourcc = type;
        if (type == "avc1" || type == "avc3") {
            t.codec = Codec::H264;
            Rd e(b, bl);
            e.Skip(24);
            int w = e.U16(), h = e.U16();
            if (w > 0 && h > 0) { t.width = w; t.height = h; }
            if (bl > 78) ForEachBox(b + 78, bl - 78, [&](const std::string& ct, const uint8_t* cb, size_t cl, size_t) {
                if (ct == "avcC") ParseAvcC(t, cb, cl);
                return true;
            });
        } else if (type == "mp4a") {
            t.codec = Codec::AAC;
            Rd e(b, bl);
            e.Skip(8);
            uint16_t ver = e.U16();
            e.Skip(6);
            t.channels = e.U16();
            e.Skip(6);
            t.sampleRate = int(e.U32() >> 16);
            size_t off = 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
            if (bl > off) ForEachBox(b + off, bl - off, [&](const std::string& ct, const uint8_t* cb, size_t cl, size_t) {
                if (ct == "esds") ParseEsds(t, cb, cl);
                return true;
            });
        } else {
            t.codec = Codec::Unsupported;   // encv/enca (DRM), vp09, av01, hvc1, Opus, ...
        }
        return false; // yalnizca ilk giris
    });
}

void ParseStbl(TrackConfig& t, const uint8_t* p, size_t n, Tables* tb) {
    ForEachBox(p, n, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type == "stsd") { ParseStsd(t, b, bl); return true; }
        if (!tb) return true;
        Rd r(b, bl);
        r.Skip(4);
        if (type == "stts") {
            uint32_t c = std::min<uint32_t>(r.U32(), kMaxMp4Samples);
            for (uint32_t k = 0; k < c && r.ok; k++) { uint32_t a = r.U32(), d = r.U32(); if (r.ok) tb->stts.push_back({a, d}); }
        } else if (type == "ctts") {
            uint32_t c = std::min<uint32_t>(r.U32(), kMaxMp4Samples);
            for (uint32_t k = 0; k < c && r.ok; k++) { uint32_t a = r.U32(); int32_t o = int32_t(r.U32()); if (r.ok) tb->ctts.push_back({a, o}); }
        } else if (type == "stsz") {
            tb->constSize = r.U32();
            uint32_t sc = r.U32();
            if (sc > kMaxMp4Samples) { tb->sampleCount = 0; r.ok = false; return true; }
            tb->sampleCount = sc;
            if (tb->constSize == 0) {
                if (uint64_t(tb->sampleCount) * 4 > r.Left()) { tb->sampleCount = 0; r.ok = false; return true; }
                tb->sizes.reserve(tb->sampleCount);
                for (uint32_t k = 0; k < tb->sampleCount; k++) tb->sizes.push_back(r.U32());
            }
        } else if (type == "stz2") {
            r.Skip(3);
            int fs = r.U8();
            uint32_t sc = r.U32();
            if (sc > kMaxMp4Samples) { tb->sampleCount = 0; r.ok = false; return true; }
            tb->sampleCount = sc;
            for (uint32_t k = 0; k < tb->sampleCount && r.ok; k++) {
                if (fs == 4) { uint8_t v = r.U8(); tb->sizes.push_back(v >> 4); if (++k < tb->sampleCount) tb->sizes.push_back(v & 15); }
                else if (fs == 8) tb->sizes.push_back(r.U8());
                else tb->sizes.push_back(r.U16());
            }
        } else if (type == "stsc") {
            uint32_t c = std::min<uint32_t>(r.U32(), kMaxMp4Samples);
            for (uint32_t k = 0; k < c && r.ok; k++) { uint32_t f = r.U32(), s = r.U32(); r.U32(); if (r.ok) tb->stsc.push_back({f, s}); }
        } else if (type == "stco") {
            uint32_t c = std::min<uint32_t>(r.U32(), kMaxMp4Samples);
            for (uint32_t k = 0; k < c && r.ok; k++) { uint32_t off = r.U32(); if (r.ok) tb->chunks.push_back(off); }
        } else if (type == "co64") {
            uint32_t c = std::min<uint32_t>(r.U32(), kMaxMp4Samples);
            for (uint32_t k = 0; k < c && r.ok; k++) { uint64_t off = r.U64(); if (r.ok) tb->chunks.push_back(off); }
        } else if (type == "stss") {
            tb->hasSync = true;
            uint32_t c = std::min<uint32_t>(r.U32(), kMaxMp4Samples);
            for (uint32_t k = 0; k < c && r.ok; k++) { uint32_t sn = r.U32(); if (r.ok) tb->sync.push_back(sn); }
        }
        return true;
    });
}

bool ParseTrak(const uint8_t* p, size_t n, TrackConfig& t, Tables* tb, bool& isAV) {
    std::string handler;
    int64_t editOffset = 0;
    ForEachBox(p, n, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type == "tkhd") {
            Rd r(b, bl);
            uint8_t v = r.U8();
            r.Skip(3);
            if (v == 1) { r.Skip(16); t.trackId = r.U32(); r.Skip(4); r.Skip(8); }
            else { r.Skip(8); t.trackId = r.U32(); r.Skip(4); r.Skip(4); }
            r.Skip(8 + 2 + 2 + 2 + 2 + 36);
            uint32_t w = r.U32(), h = r.U32();
            if (r.ok && (w >> 16) > 0 && (h >> 16) > 0) { t.width = int(w >> 16); t.height = int(h >> 16); }
        } else if (type == "edts") {
            ForEachBox(b, bl, [&](const std::string& et, const uint8_t* eb, size_t el, size_t) {
                if (et != "elst") return true;
                Rd r(eb, el);
                uint8_t v = r.U8();
                r.Skip(3);
                uint32_t c = std::min<uint32_t>(r.U32(), 1024);
                for (uint32_t k = 0; k < c && r.ok; k++) {
                    int64_t mt;
                    if (v == 1) { r.U64(); mt = int64_t(r.U64()); }
                    else { r.U32(); mt = int32_t(r.U32()); }
                    r.Skip(4);
                    if (mt >= 0) { editOffset = mt; break; }   // ilk bos olmayan duzenleme
                }
                return true;
            });
        } else if (type == "mdia") {
            ForEachBox(b, bl, [&](const std::string& mt, const uint8_t* mb, size_t ml, size_t) {
                if (mt == "mdhd") {
                    Rd r(mb, ml);
                    uint8_t v = r.U8();
                    r.Skip(3);
                    r.Skip(v == 1 ? 16 : 8);
                    t.timescale = r.U32();
                } else if (mt == "hdlr") {
                    if (ml >= 12) handler = FourCC(mb + 8);
                } else if (mt == "minf") {
                    ForEachBox(mb, ml, [&](const std::string& it, const uint8_t* ib, size_t il, size_t) {
                        if (it == "stbl") ParseStbl(t, ib, il, tb);
                        return true;
                    });
                }
                return true;
            });
        }
        return true;
    });
    isAV = handler == "vide" || handler == "soun";
    t.video = handler == "vide";
    if (tb) tb->editOffset = editOffset;
    t.defDur = 0;
    return t.timescale > 0;
}

} // namespace

std::string TrackConfig::CodecString() const {
    char buf[32];
    if (codec == Codec::H264) { snprintf(buf, sizeof buf, "avc1.%02x%02x%02x", profile & 255, 0, level & 255); return buf; }
    if (codec == Codec::AAC) { snprintf(buf, sizeof buf, "mp4a.40.%d", aacObjectType ? aacObjectType : 2); return buf; }
    return fourcc;
}

const TrackConfig* Mp4Parser::Track(uint32_t id) const {
    for (auto& t : tracks) if (t.trackId == id) return &t;
    return nullptr;
}

bool Mp4Parser::ParseMoov(const uint8_t* p, size_t n) {
    std::vector<TrackConfig> nt;
    std::vector<std::pair<uint32_t, int64_t>> edits;
    bool tablesSeen = false;
    bool ok = ForEachBox(p, n, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type == "trak") {
            TrackConfig t;
            Tables tb;
            bool av = false;
            if (ParseTrak(b, bl, t, &tb, av) && av) {
                if (tb.sampleCount > 0) tablesSeen = true;
                edits.push_back({t.trackId, tb.editOffset});
                nt.push_back(std::move(t));
            }
        } else if (type == "mvex") {
            ForEachBox(b, bl, [&](const std::string& mt, const uint8_t* mb, size_t ml, size_t) {
                if (mt != "trex") return true;
                Rd r(mb, ml);
                r.Skip(4);
                uint32_t id = r.U32();
                r.U32();
                uint32_t d = r.U32(), s = r.U32(), f = r.U32();
                for (auto& t : nt) if (t.trackId == id) { t.defDur = d; t.defSize = s; t.defFlags = f; }
                return true;
            });
        }
        return true;
    });
    if (!ok) { error = "moov bozuk"; return false; }
    if (nt.empty()) { error = "moov icinde ses/video izi yok"; return false; }
    // trex mvex'ten once gelen trak'lara da uygulanmali: ikinci gecis
    ForEachBox(p, n, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type != "mvex") return true;
        ForEachBox(b, bl, [&](const std::string& mt, const uint8_t* mb, size_t ml, size_t) {
            if (mt != "trex") return true;
            Rd r(mb, ml);
            r.Skip(4);
            uint32_t id = r.U32();
            r.U32();
            uint32_t d = r.U32(), s = r.U32(), f = r.U32();
            for (auto& t : nt) if (t.trackId == id) { t.defDur = d; t.defSize = s; t.defFlags = f; }
            return true;
        });
        return true;
    });
    tracks = std::move(nt);
    edits_ = std::move(edits);
    haveInit = true;
    initChanged = true;
    progressive = tablesSeen;
    return true;
}

int64_t Mp4Parser::EditOffset(uint32_t id) const {
    for (auto& e : edits_) if (e.first == id) return e.second;
    return 0;
}

bool Mp4Parser::ParseMoof(const uint8_t* p, size_t n, uint64_t moofStart) {
    uint64_t prevEnd = moofStart;
    int trafIndex = 0;
    bool ok = ForEachBox(p, n, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type != "traf") return true;
        uint32_t tfFlags = 0, trackId = 0, dDur = 0, dSize = 0, dFlags = 0;
        bool haveDur = false, haveSize = false, haveFlags = false;
        uint64_t base = moofStart;
        bool haveTfdt = false;
        int64_t decodeTime = 0;
        // once tfhd ve tfdt
        ForEachBox(b, bl, [&](const std::string& tt, const uint8_t* tb, size_t tl, size_t) {
            Rd r(tb, tl);
            if (tt == "tfhd") {
                uint32_t vf = r.U32();
                tfFlags = vf & 0xFFFFFF;
                trackId = r.U32();
                if (tfFlags & 0x1) base = r.U64();
                else if (!(tfFlags & 0x20000) && trafIndex > 0) base = prevEnd;
                if (tfFlags & 0x2) r.U32();
                if (tfFlags & 0x8) { dDur = r.U32(); haveDur = true; }
                if (tfFlags & 0x10) { dSize = r.U32(); haveSize = true; }
                if (tfFlags & 0x20) { dFlags = r.U32(); haveFlags = true; }
            } else if (tt == "tfdt") {
                uint8_t v = r.U8();
                r.Skip(3);
                decodeTime = v == 1 ? int64_t(r.U64()) : int64_t(r.U32());
                haveTfdt = r.ok;
            }
            return true;
        });
        const TrackConfig* tc = Track(trackId);
        if (!tc) { trafIndex++; return true; }   // bilinmeyen iz: atla
        if (!haveDur) dDur = tc->defDur;
        if (!haveSize) dSize = tc->defSize;
        if (!haveFlags) dFlags = tc->defFlags;
        int64_t dts = haveTfdt ? decodeTime : nextDts_[trackId];
        uint64_t off = base;
        bool firstTrun = true;
        ForEachBox(b, bl, [&](const std::string& tt, const uint8_t* tb, size_t tl, size_t) {
            if (tt != "trun") return true;
            Rd r(tb, tl);
            uint32_t vf = r.U32();
            uint8_t ver = uint8_t(vf >> 24);
            uint32_t fl = vf & 0xFFFFFF;
            uint32_t cnt = std::min<uint32_t>(r.U32(), kMaxTrunSamples);
            if (fl & 0x1) off = base + int64_t(int32_t(r.U32()));
            else if (firstTrun) off = base;
            firstTrun = false;
            uint32_t firstFlags = 0;
            bool hasFirst = (fl & 0x4) != 0;
            if (hasFirst) firstFlags = r.U32();
            for (uint32_t k = 0; k < cnt && r.ok && pending_.size() < kMaxTrunSamples; k++) {
                uint32_t dur = (fl & 0x100) ? r.U32() : dDur;
                uint32_t size = (fl & 0x200) ? r.U32() : dSize;
                uint32_t sf = (fl & 0x400) ? r.U32() : (k == 0 && hasFirst ? firstFlags : dFlags);
                if (k == 0 && hasFirst && (fl & 0x400)) sf = firstFlags;
                int32_t cts = 0;
                if (fl & 0x800) { uint32_t c = r.U32(); cts = ver == 0 ? int32_t(c) : int32_t(c); }
                if (!r.ok) break;
                Pending pe;
                pe.trackId = trackId;
                pe.dts = dts;
                pe.cts = cts;
                pe.dur = dur;
                pe.size = size;
                pe.key = tc->video ? !((sf >> 16) & 1) : true;
                pe.absOff = off;
                pending_.push_back(pe);
                off += size;
                dts += dur;
            }
            return true;
        });
        nextDts_[trackId] = dts;
        prevEnd = off;
        trafIndex++;
        return true;
    });
    if (!ok) { error = "moof bozuk"; return false; }
    return true;
}

void Mp4Parser::TakeFromMdat(const uint8_t* p, size_t n, uint64_t dataAbs) {
    std::vector<Pending> keep;
    for (auto& pe : pending_) {
        if (pe.absOff >= dataAbs && pe.size <= n && (pe.absOff - dataAbs) <= n - pe.size) {
            Sample s;
            s.trackId = pe.trackId;
            int64_t eo = EditOffset(pe.trackId);
            s.dts = pe.dts - eo;
            s.pts = pe.dts + pe.cts - eo;
            s.dur = pe.dur;
            s.key = pe.key;
            const uint8_t* src = p + static_cast<size_t>(pe.absOff - dataAbs);
            s.data = std::make_shared<std::vector<uint8_t>>(src, src + pe.size);
            out.push_back(std::move(s));
        } else if (pe.absOff >= dataAbs && (pe.absOff - dataAbs) > n - std::min<uint64_t>(pe.size, n)) {
            if (keep.size() < kMaxTrunSamples) keep.push_back(pe);   // sonraki mdat'ta
        }
    }
    pending_.swap(keep);
}

bool Mp4Parser::ParseBox(const uint8_t* p, size_t n, uint64_t absStart, const std::string& type, size_t hdr) {
    if (type == "moov") return ParseMoov(p, n);
    if (type == "moof") {
        if (!haveInit) { error = "baslatma bolumunden once medya bolumu"; return false; }
        pending_.clear();
        return ParseMoof(p, n, absStart);
    }
    if (type == "mdat") {
        TakeFromMdat(p, n, absStart + hdr);
        return true;
    }
    return true;   // ftyp, styp, sidx, emsg, free, skip, prft, uuid, meta ...
}

bool Mp4Parser::Append(const uint8_t* p, size_t n) {
    if (!error.empty()) return false;
    buf_.insert(buf_.end(), p, p + n);
    size_t i = 0;
    while (buf_.size() - i >= 8) {
        const uint8_t* b = buf_.data() + i;
        uint64_t size = (uint64_t(b[0]) << 24) | (uint64_t(b[1]) << 16) | (uint64_t(b[2]) << 8) | b[3];
        if (!Printable4(b + 4)) { error = "gecersiz kutu (MP4 degil)"; buf_.clear(); return false; }
        std::string type = FourCC(b + 4);
        size_t hdr = 8;
        if (size == 1) {
            if (buf_.size() - i < 16) break;
            size = 0;
            for (int k = 0; k < 8; k++) size = (size << 8) | b[8 + k];
            hdr = 16;
        } else if (size == 0) {
            error = "boyutsuz kutu desteklenmiyor";
            return false;
        }
        if (size < hdr || size > (1ull << 31)) { error = "kutu boyutu gecersiz"; buf_.clear(); return false; }
        if (buf_.size() - i < size) break;   // kutu henuz tamamlanmadi
        if (!ParseBox(b + hdr, size_t(size - hdr), bufPos_ + i, type, hdr)) { buf_.clear(); return false; }
        i += size_t(size);
    }
    if (i > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + i);
        bufPos_ += i;
    }
    return true;
}

void Mp4Parser::ResetParserState() {
    bufPos_ += buf_.size();
    buf_.clear();
    pending_.clear();
    error.clear();
}

bool Mp4Parser::ParseFile(const uint8_t* p, size_t n, std::vector<TrackConfig>& tracks, std::vector<Sample>& samples, std::string& err) {
    const uint8_t* moov = nullptr;
    size_t moovLen = 0;
    bool fragmented = false;
    ForEachBox(p, n, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type == "moov") { moov = b; moovLen = bl; }
        if (type == "moof") fragmented = true;
        return true;
    });
    if (!moov) { err = "moov bulunamadi"; return false; }
    if (fragmented) {
        Mp4Parser ps;
        if (!ps.Append(p, n)) { err = ps.error; return false; }
        tracks = ps.tracks;
        samples = std::move(ps.out);
        return true;
    }
    tracks.clear();
    samples.clear();
    bool ok = ForEachBox(moov, moovLen, [&](const std::string& type, const uint8_t* b, size_t bl, size_t) {
        if (type != "trak") return true;
        TrackConfig t;
        Tables tb;
        bool av = false;
        if (!ParseTrak(b, bl, t, &tb, av) || !av) return true;
        uint32_t count = std::min<uint32_t>(tb.sampleCount, kMaxMp4Samples);
        if (count == 0 || tb.chunks.empty() || tb.stsc.empty()) { tracks.push_back(t); return true; }
        // ornek basina dts/sure
        std::vector<int64_t> dts(count);
        std::vector<uint32_t> durs(count, 0);
        {
            size_t s = 0;
            int64_t d = 0;
            for (auto& e : tb.stts) for (uint32_t k = 0; k < e.first && s < count; k++) { dts[s] = d; durs[s] = e.second; d += e.second; s++; }
            for (; s < count; s++) dts[s] = d;
        }
        std::vector<int32_t> cts(count, 0);
        {
            size_t s = 0;
            for (auto& e : tb.ctts) for (uint32_t k = 0; k < e.first && s < count; k++) cts[s++] = e.second;
        }
        std::vector<bool> key(count, !tb.hasSync || !t.video);
        if (tb.hasSync && t.video) for (uint32_t sn : tb.sync) if (sn >= 1 && sn <= count) key[sn - 1] = true;
        // konumlar: stsc + stco
        size_t s = 0;
        for (size_t c = 0; c < tb.chunks.size() && s < count; c++) {
            uint32_t chunkNo = uint32_t(c + 1);
            uint32_t per = 0;
            for (auto& e : tb.stsc) if (e.first <= chunkNo) per = e.perChunk; else break;
            uint64_t off = tb.chunks[c];
            for (uint32_t k = 0; k < per && s < count; k++, s++) {
                uint32_t sz = tb.constSize ? tb.constSize : (s < tb.sizes.size() ? tb.sizes[s] : 0);
                if (off > n || sz > n - off) { off += sz; continue; }
                Sample sm;
                sm.trackId = t.trackId;
                sm.dts = dts[s] - tb.editOffset;
                sm.pts = dts[s] + cts[s] - tb.editOffset;
                sm.dur = durs[s];
                sm.key = key[s];
                sm.data = std::make_shared<std::vector<uint8_t>>(p + off, p + off + sz);
                samples.push_back(std::move(sm));
                off += sz;
            }
        }
        tracks.push_back(t);
        return true;
    });
    if (!ok) { err = "moov bozuk"; return false; }
    if (tracks.empty()) { err = "ses/video izi yok"; return false; }
    // dts sirasina gore (izler karisik)
    std::stable_sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.trackId < b.trackId; });
    return true;
}

void AvccToAnnexB(const TrackConfig& t, const uint8_t* p, size_t n, bool key, std::vector<uint8_t>& out) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out.clear();
    out.reserve(n + 64);
    if (key) {
        for (auto& s : t.sps) { out.insert(out.end(), sc, sc + 4); out.insert(out.end(), s.begin(), s.end()); }
        for (auto& s : t.pps) { out.insert(out.end(), sc, sc + 4); out.insert(out.end(), s.begin(), s.end()); }
    }
    size_t i = 0;
    int L = t.nalLenSize;
    if (L < 1 || L > 4) return;
    while (i <= n && n - i >= size_t(L)) {
        uint32_t len = 0;
        for (int k = 0; k < L; k++) len = (len << 8) | p[i + k];
        i += size_t(L);
        if (len == 0 || len > n - i) break;
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), p + i, p + i + len);
        i += len;
    }
}

std::vector<uint8_t> AacUserData(const TrackConfig& t) {
    // HEAACWAVEINFO'nun WAVEFORMATEX sonrasi kismi: wPayloadType(0=ham), wAudioProfileLevelIndication(0xFE),
    // wStructType(0), wReserved1, dwReserved2; ardindan AudioSpecificConfig
    std::vector<uint8_t> u(12, 0);
    u[2] = 0xFE;
    u.insert(u.end(), t.asc.begin(), t.asc.end());
    return u;
}

int SupportLevel(const std::wstring& mime0) {
    std::wstring m;
    for (wchar_t c : mime0) m += wchar_t(towlower(c));
    size_t semi = m.find(L';');
    std::wstring type = m.substr(0, semi);
    while (!type.empty() && iswspace(type.back())) type.pop_back();
    while (!type.empty() && iswspace(type.front())) type.erase(type.begin());
    bool isMp4 = type == L"video/mp4" || type == L"audio/mp4" || type == L"audio/x-m4a" || type == L"audio/aac";
    if (!isMp4) return 0;
    if (semi == std::wstring::npos) return 1;
    size_t cp = m.find(L"codecs", semi);
    if (cp == std::wstring::npos) return 1;
    size_t eq = m.find(L'=', cp);
    if (eq == std::wstring::npos) return 1;
    std::wstring list = m.substr(eq + 1);
    std::wstring cur;
    std::vector<std::wstring> codecs;
    for (wchar_t c : list) {
        if (c == L'"' || c == L'\'' || iswspace(c)) continue;
        if (c == L',' || c == L';') { if (!cur.empty()) codecs.push_back(cur); cur.clear(); if (c == L';') break; continue; }
        cur += c;
    }
    if (!cur.empty()) codecs.push_back(cur);
    if (codecs.empty()) return 1;
    for (auto& c : codecs) {
        if (c.compare(0, 5, L"avc1.") == 0 || c.compare(0, 5, L"avc3.") == 0 || c == L"avc1" || c == L"avc3") {
            if (type.compare(0, 5, L"audio") == 0) return 0;
            continue;
        }
        if (c == L"mp4a.40.2" || c == L"mp4a.40.5" || c == L"mp4a.40.29" || c == L"mp4a.40.02" || c == L"mp4a.40" || c == L"mp4a.67") continue;
        return 0;   // vp09, av01, hev1, opus, ec-3, ...
    }
    return 2;
}

} // namespace media
} // namespace mer
