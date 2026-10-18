// merOS Shield - resim deposu (WIC ile güvenli çözme, Direct2D bitmap önbelleği)
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "dom.h"
#include "layout.h"

namespace mer {

struct ImageEntry {
    int state = 0;                 // 0 bekliyor, 1 hazir, 2 basarisiz/engellendi
    UINT w = 0, h = 0;
    std::vector<uint8_t> px;       // BGRA, onceden carpilmis alfa
    ID2D1Bitmap* bmp = nullptr;
};

class ImageStore : public IImageProvider {
public:
    ~ImageStore() { ReleaseBitmaps(); }
    std::wstring base;             // sayfanin taban adresi
    std::unordered_map<std::wstring, ImageEntry> map;

    bool Lookup(const Node* img, float& w, float& h, std::wstring& key) override;
    bool LookupByKey(const std::wstring& key, float& w, float& h) override;
    static std::wstring SrcOf(const Node* img); // src / srcset / data-src secimi (ham)
    std::wstring UrlOf(const Node* img) const;  // cozulmus mutlak adres
    ID2D1Bitmap* Bitmap(ID2D1RenderTarget* rt, const std::wstring& key);
    void ReleaseBitmaps();
};

// Resim baytlarini cozer (PNG, JPEG, GIF, BMP, ICO, TIFF, WebP*). Buyuk resimler kucultulur.
// *WebP, Windows 11'deki WebP uzantisi kuruluysa calisir.
bool DecodeImage(const std::string& bytes, UINT& w, UINT& h, std::vector<uint8_t>& px);
// data: URL'den baytlari cikarir (yalnizca base64 ve image/*)
bool DecodeDataUrl(const std::wstring& url, std::string& out);

} // namespace mer
