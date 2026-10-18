// merOS Shield - resim deposu (WIC ile güvenli çözme, Direct2D bitmap önbelleği)
#include "images.h"
#include <wincodec.h>
#include <algorithm>
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
    std::wstring u = UrlOf(img);
    if (u.empty()) return false;
    auto it = map.find(u);
    if (it == map.end() || it->second.state != 1) return false;
    w = float(it->second.w);
    h = float(it->second.h);
    key = u;
    return true;
}

bool ImageStore::LookupByKey(const std::wstring& key, float& w, float& h) {
    if (key.empty()) return false;
    auto it = map.find(key);
    if (it == map.end() || it->second.state != 1) return false;
    w = float(it->second.w);
    h = float(it->second.h);
    return true;
}

ID2D1Bitmap* ImageStore::Bitmap(ID2D1RenderTarget* rt, const std::wstring& key) {
    auto it = map.find(key);
    if (it == map.end() || it->second.state != 1) return nullptr;
    ImageEntry& e = it->second;
    if (e.bmp) return e.bmp;
    if (e.px.empty()) return nullptr;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(e.w, e.h), e.px.data(), e.w * 4, props, &e.bmp))) { e.bmp = nullptr; e.state = 2; }
    return e.bmp;
}

void ImageStore::ReleaseBitmaps() {
    for (auto& kv : map) SafeRelease(kv.second.bmp);
}

bool DecodeImage(const std::string& bytes, UINT& w, UINT& h, std::vector<uint8_t>& px) {
    if (bytes.empty() || bytes.size() > 32u * 1024 * 1024) return false;
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
    if (meta.find(L"svg") != std::wstring::npos) return false; // SVG henuz cizilmiyor
    if (meta.find(L";base64") == std::wstring::npos) return false;
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
