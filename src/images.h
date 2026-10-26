// merOS Shield - resim deposu (WIC ile güvenli çözme, SVG çizimi, Direct2D bitmap önbelleği)
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "dom.h"
#include "layout.h"

namespace mer {

struct ImageEntry {
    int state = 0;                 // 0 bekliyor, 1 hazir, 2 basarisiz/engellendi
    UINT w = 0, h = 0;             // piksel boyutu
    float natW = 0, natH = 0;      // dogal (CSS piksel) boyut; SVG'de piksel boyutundan kucuk olabilir
    std::vector<uint8_t> px;       // BGRA, onceden carpilmis alfa
    ID2D1Bitmap* bmp = nullptr;
};

// Is parcacigi guvenligi: depo, sekmenin is parcacigi (yerlesim, yeni resimler) ile arayuz is
// parcacigi (cizim, video kareleri) arasinda paylasilir. Tum erisimler Mu() kilidiyle yapilir;
// kilit yalnizca kisa sureli tutulur (asla betik calisirken). Direct2D bitmapleri yalnizca arayuz
// is parcacigida olusturulur ve birakilir.
class ImageStore : public IImageProvider {
public:
    ~ImageStore() { ReleaseBitmaps(); }
    std::recursive_mutex& Mu() const { return mu_; }
    std::wstring base;             // sayfanin taban adresi
    std::unordered_map<std::wstring, ImageEntry> map;
    std::unordered_map<const Node*, std::wstring> svgKeys;     // satir ici <svg> -> anahtar
    std::unordered_map<std::wstring, std::wstring> svgSource;  // anahtar -> SVG metni (cizilmeyi bekleyen)
    std::unordered_map<std::wstring, uint32_t> svgColor;       // anahtar -> currentColor

    bool Lookup(const Node* img, float& w, float& h, std::wstring& key) override;
    bool LookupUrl(const std::wstring& url, float& w, float& h) override;
    bool LookupByKey(const std::wstring& key, float& w, float& h) { return LookupUrl(key, w, h); }
    static std::wstring SrcOf(const Node* img); // src / srcset / data-src secimi (ham)
    std::wstring UrlOf(const Node* img) const;  // cozulmus mutlak adres
    ID2D1Bitmap* Bitmap(ID2D1RenderTarget* rt, const std::wstring& key, uint32_t tint = 0);
    // Ekran goruntusu icin: (gerekirse boyanmis) piksellerin kopyasi
    bool Pixels(const std::wstring& key, uint32_t tint, UINT& w, UINT& h, std::vector<uint8_t>& out);
    // Resim sonucu (sekme is parcacigi): hazir ya da basarisiz olarak isaretle
    void SetResult(const std::wstring& key, bool ok, UINT w, UINT h, float natW, float natH, std::vector<uint8_t>& px);
    void ReleaseBitmaps();
    // <video> karesi: ayni boyutta ise mevcut bitmap'e kopyalanir. Dogal boyut degistiyse true.
    bool UpdateVideo(const std::wstring& key, UINT w, UINT h, const std::vector<uint8_t>& px, bool keepPixels = false);

private:
    mutable std::recursive_mutex mu_;
    std::unordered_map<std::wstring, ID2D1Bitmap*> tinted_;
    std::unordered_map<std::wstring, std::vector<uint8_t>> tintedPx_;
};

// Resim baytlarini cozer (PNG, JPEG, GIF, BMP, ICO, TIFF, WebP*, SVG). Buyuk resimler kucultulur.
// *WebP, Windows 11'deki WebP uzantisi kuruluysa calisir.
bool DecodeImage(const std::string& bytes, UINT& w, UINT& h, std::vector<uint8_t>& px, float* natW = nullptr, float* natH = nullptr);
// data: URL'den baytlari cikarir (base64 ya da yuzde-kodlu; yalnizca image/*)
bool DecodeDataUrl(const std::wstring& url, std::string& out);

} // namespace mer
