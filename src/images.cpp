// merOS Shield - resim deposu (WIC ile güvenli çözme, Direct2D bitmap önbelleği)
#include "images.h"
#include <wincodec.h>
#include <algorithm>
#include "net.h"
#include "svg.h"
#include "url.h"
#include "util.h"

namespace mer {

namespace {
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr UINT kMaxDim = 4096;
}

std::wstring ImageStore::SrcOf(const Node* n) {
    const std::wstring* src = n->Attr(L"src");
    const std::wstring* lazy = n->Attr(L"data-src");
    if (!lazy) lazy = n->Attr(L"data-lazy-src");
    if (!lazy) lazy = n->Attr(L"data-original");
    std::wstring s = src ? Trim(*src) : L"";
    // tembel yukleme yer tutuculari (1x1 gif/svg) yerine gercek adresi kullan
    if (lazy && !Trim(*lazy).empty() && (s.empty() || StartsWith(ToLower(s), L"data:"))) return Trim(*lazy);
    if (s.empty()) {
        const std::wstring* set = n->Attr(L"srcset");
        if (!set) set = n->Attr(L"data-srcset");
        if (set) {
            std::wstring first = Trim(*set);
            size_t c = first.find(L',');
            if (c != std::wstring::npos) first = first.substr(0, c);
            size_t sp = first.find(L' ');
            if (sp != std::wstring::npos) first = first.substr(0, sp);
            return Trim(first);
        }
    }
    return s;
}

std::wstring ImageStore::UrlOf(const Node* n) const {
    std::wstring s = SrcOf(n);
    if (s.empty()) return L"";
    if (StartsWith(ToLower(s), L"data:")) return s;
    return ResolveUrl(base, s);
}

bool ImageStore::Lookup(const Node* img, float& w, float& h, std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (img->name == L"video") {
        // video her zaman kutu uretir: kare yoksa siyah, varsayilan 300x150 (HTML)
        key = VideoKey(img);
        auto it = map.find(key);
        if (it != map.end() && it->second.state == 1 && it->second.w > 0) {
            w = it->second.natW > 0 ? it->second.natW : float(it->second.w);
            h = it->second.natH > 0 ? it->second.natH : float(it->second.h);
        } else {
            w = 300;
            h = 150;
        }
        return true;
    }
    std::wstring u;
    if (img->name == L"svg") {
        auto k = svgKeys.find(img);
        if (k == svgKeys.end()) return false;
        u = k->second;
    } else u = UrlOf(img);
    if (u.empty()) return false;
    auto it = map.find(u);
    if (it == map.end() || it->second.state != 1) return false;
    w = it->second.natW > 0 ? it->second.natW : float(it->second.w);
    h = it->second.natH > 0 ? it->second.natH : float(it->second.h);
    key = u;
    return true;
}

bool ImageStore::LookupUrl(const std::wstring& url, float& w, float& h) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = map.find(url);
    if (it == map.end() || it->second.state != 1) return false;
    w = it->second.natW > 0 ? it->second.natW : float(it->second.w);
    h = it->second.natH > 0 ? it->second.natH : float(it->second.h);
    return true;
}

namespace {
// Maske: kaynak alfasini kullanarak tek renkle boya
void TintPixels(const std::vector<uint8_t>& src, uint32_t tint, std::vector<uint8_t>& dst) {
    dst.resize(src.size());
    uint32_t ta = tint >> 24, tr = (tint >> 16) & 255, tg = (tint >> 8) & 255, tb = tint & 255;
    for (size_t i = 0; i + 3 < src.size(); i += 4) {
        uint32_t a = src[i + 3] * ta / 255;
        dst[i] = uint8_t(tb * a / 255);
        dst[i + 1] = uint8_t(tg * a / 255);
        dst[i + 2] = uint8_t(tr * a / 255);
        dst[i + 3] = uint8_t(a);
    }
}
std::wstring TintKey(const std::wstring& key, uint32_t tint) {
    wchar_t buf[16];
    swprintf(buf, 16, L"#%08X", tint);
    return key + buf;
}
}

bool ImageStore::Pixels(const std::wstring& key, uint32_t tint, UINT& w, UINT& h, std::vector<uint8_t>& out) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = map.find(key);
    if (it == map.end() || it->second.state != 1 || it->second.px.empty()) return false;
    w = it->second.w;
    h = it->second.h;
    if (!tint) { out = it->second.px; return true; }
    std::wstring tk = TintKey(key, tint);
    auto t = tintedPx_.find(tk);
    if (t == tintedPx_.end()) {
        if (tintedPx_.size() > 400) tintedPx_.clear();
        TintPixels(it->second.px, tint, tintedPx_[tk]);
        t = tintedPx_.find(tk);
    }
    out = t->second;
    return true;
}

void ImageStore::SetResult(const std::wstring& key, bool ok, UINT w, UINT h, float natW, float natH, std::vector<uint8_t>& px) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    ImageEntry& e = map[key];
    if (ok) {
        // ayni anahtar icin eski bitmap (cok nadir: yeniden yukleme) arayuzde birakilir; burada yalnizca pikseller
        e.state = 1; e.w = w; e.h = h; e.natW = natW; e.natH = natH; e.px.swap(px);
    } else e.state = 2;
}

ID2D1Bitmap* ImageStore::Bitmap(ID2D1RenderTarget* rt, const std::wstring& key, uint32_t tint) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = map.find(key);
    if (it == map.end() || it->second.state != 1) return nullptr;
    ImageEntry& e = it->second;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (tint) {
        std::wstring tk = TintKey(key, tint);
        auto t = tinted_.find(tk);
        if (t != tinted_.end()) return t->second;
        if (e.px.empty() || tinted_.size() > 600) return nullptr;
        std::vector<uint8_t> px;
        TintPixels(e.px, tint, px);
        ID2D1Bitmap* b = nullptr;
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(e.w, e.h), px.data(), e.w * 4, props, &b))) b = nullptr;
        tinted_[tk] = b;
        return b;
    }
    if (e.bmp) return e.bmp;
    if (e.px.empty()) return nullptr;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(e.w, e.h), e.px.data(), e.w * 4, props, &e.bmp))) { e.bmp = nullptr; e.state = 2; }
    return e.bmp;
}

bool ImageStore::UpdateVideo(const std::wstring& key, UINT w, UINT h, const std::vector<uint8_t>& px, bool keepPixels) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    ImageEntry& e = map[key];
    bool sizeChanged = e.state != 1 || e.w != w || e.h != h;
    if (!sizeChanged && e.bmp && px.size() >= size_t(w) * h * 4) {
        // ekrandaki bitmap'i yerinde guncelle; pikselleri yalnizca ekran goruntusu icin sakla
        if (SUCCEEDED(e.bmp->CopyFromMemory(nullptr, px.data(), w * 4))) {
            if (keepPixels) e.px = px;
            return false;
        }
    }
    SafeRelease(e.bmp);
    e.state = 1;
    e.w = w;
    e.h = h;
    e.natW = float(w);
    e.natH = float(h);
    e.px = px;
    return sizeChanged;
}

void ImageStore::ReleaseBitmaps() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    for (auto& kv : map) SafeRelease(kv.second.bmp);
    for (auto& kv : tinted_) SafeRelease(kv.second);
    tinted_.clear();
}

bool DecodeImage(const std::string& bytes, UINT& w, UINT& h, std::vector<uint8_t>& px, float* natW, float* natH) {
    if (bytes.empty() || bytes.size() > 32u * 1024 * 1024) return false;
    if (LooksLikeSvg(bytes)) {
        // SVG: kendi ciziciyle 2x keskinlikte piksellere
        std::wstring markup = DecodeBody(bytes, L"image/svg+xml; charset=utf-8");
        uint32_t W = 0, H = 0;
        float nw = 0, nh = 0;
        if (!RasterizeSvg(markup, 2.f, 0xFF000000, W, H, nw, nh, px)) return false;
        w = W; h = H;
        if (natW) *natW = nw;
        if (natH) *natH = nh;
        return true;
    }
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    do {
        if (FAILED(f->CreateStream(&st))) break;
        if (FAILED(st->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), DWORD(bytes.size())))) break;
        if (FAILED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        if (fw == 0 || fh == 0 || fw > 60000 || fh > 60000) break;
        IWICBitmapSource* src = frame;
        UINT tw = fw, th = fh;
        if (fw > kMaxDim || fh > kMaxDim) {
            double s = std::min(double(kMaxDim) / fw, double(kMaxDim) / fh);
            tw = std::max(1u, UINT(fw * s));
            th = std::max(1u, UINT(fh * s));
            if (FAILED(f->CreateBitmapScaler(&scaler))) break;
            if (FAILED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant))) break;
            src = scaler;
        }
        if (FAILED(f->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) break;
        px.resize(size_t(tw) * th * 4);
        if (FAILED(conv->CopyPixels(nullptr, tw * 4, UINT(px.size()), px.data()))) { px.clear(); break; }
        w = tw;
        h = th;
        ok = true;
    } while (false);
    SafeRelease(conv);
    SafeRelease(scaler);
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(st);
    SafeRelease(f);
    return ok;
}

bool DecodeDataUrl(const std::wstring& url, std::string& out) {
    std::wstring l = ToLower(url.substr(0, std::min<size_t>(url.size(), 64)));
    if (!StartsWith(l, L"data:image/")) return false;
    size_t comma = url.find(L',');
    if (comma == std::wstring::npos) return false;
    std::wstring meta = ToLower(url.substr(0, comma));
    if (meta.find(L";base64") == std::wstring::npos) {
        // yuzde-kodlu (genellikle SVG): %XX coz, digerlerini UTF-8 yaz
        auto hexv = [](wchar_t c) -> int { if (c >= L'0' && c <= L'9') return c - L'0'; if (c >= L'a' && c <= L'f') return c - L'a' + 10; if (c >= L'A' && c <= L'F') return c - L'A' + 10; return -1; };
        for (size_t i = comma + 1; i < url.size(); i++) {
            wchar_t c = url[i];
            if (c == L'%' && i + 2 < url.size() && hexv(url[i + 1]) >= 0 && hexv(url[i + 2]) >= 0) {
                out += char(hexv(url[i + 1]) * 16 + hexv(url[i + 2]));
                i += 2;
            } else {
                out += ToUtf8(std::wstring(1, c));
            }
            if (out.size() > 8u * 1024 * 1024) return false;
        }
        return !out.empty();
    }
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = comma + 1; i < url.size(); i++) {
        wchar_t c = url[i];
        int v;
        if (c >= L'A' && c <= L'Z') v = c - L'A';
        else if (c >= L'a' && c <= L'z') v = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') v = c - L'0' + 52;
        else if (c == L'+' || c == L'-') v = 62;
        else if (c == L'/' || c == L'_') v = 63;
        else continue;
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += char((buf >> bits) & 0xFF); }
        if (out.size() > 8u * 1024 * 1024) return false;
    }
    return !out.empty();
}

} // namespace mer
