// merOS Shield - MerEngine : yerleşim (layout) motoru
#pragma once
#include <string>
#include <vector>
#include "dom.h"
#include "style.h"

namespace mer {

struct FontMetrics {
    float baseline = 12;
    float height = 16;
    float space = 4;
};

// Platform bagimsiz metin olcum arayuzu (Windows'ta DirectWrite ile uygulanir)
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() {}
    virtual int Font(const Style& s) = 0;
    virtual float Width(int font, const std::wstring& text) = 0;
    virtual FontMetrics Metrics(int font) = 0;
};

struct DisplayItem {
    // Fill: dolu dikdortgen, Text: metin, HLine: cizgi, Image: resim, Stroke: cerceve,
    // BgImage: kutuya kirpilmis (dosenebilir) arka plan resmi, ClipPush/ClipPop: kirpma
    // Gradient: kutu degradesi (grad), Shadow: box-shadow (x/y/w/h golge kutusu, ix..ih asil kutu, blur)
    enum Kind : uint8_t { Fill, Text, HLine, Image, Stroke, BgImage, ClipPush, ClipPop, Gradient, Shadow };
    enum Role : uint8_t { None, TextInput, Button, Check };
    Kind kind = Fill;
    Role role = None;
    uint8_t repeat = 0;  // BgImage: 0 repeat, 1 no-repeat, 2 repeat-x, 3 repeat-y
    float x = 0, y = 0, w = 0, h = 0;
    float radius = 0;    // kose yuvarlakligi (Fill/Stroke)
    float sw = 1;        // Stroke kalinligi
    float ix = 0, iy = 0, iw = 0, ih = 0; // BgImage: ilk dosemenin konumu ve boyutu
    uint32_t color = 0;
    uint32_t tint = 0;   // Image/BgImage: maske olarak bu renkle boya (0 = yok)
    int font = -1;
    int link = -1;
    int node = -1;       // ilgili eleman (tiklama hedefi, form alani)
    int32_t zIndex = 0;  // katmanlama (konumlandirilmis ogeler)
    uint8_t cursor = 0;  // imlec: 0 varsayilan, 1 el, 2 metin, 3 yasak
    std::wstring text;   // metin; Image/BgImage icin resim anahtari (URL)
    float blur = 0;      // Shadow: bulaniklik yaricapi
    std::shared_ptr<const mer::Gradient> grad;   // Gradient
};

// Yuklenmis resimlerin boyutlarini yerlesime bildirir (platformdan bagimsiz).
class IImageProvider {
public:
    virtual ~IImageProvider() {}
    // img/svg dugumu icin yuklu resim varsa dogal boyutunu ve cizim anahtarini dondurur.
    virtual bool Lookup(const Node* img, float& w, float& h, std::wstring& key) = 0;
    // CSS arka plan/maske resmi (adres ile)
    virtual bool LookupUrl(const std::wstring& url, float& w, float& h) { (void)url; (void)w; (void)h; return false; }
};

struct PageLayout {
    std::vector<DisplayItem> items;
    std::vector<std::wstring> links; // ham href degerleri (cizim ogesi link indeksi)
    float height = 0;
    float width = 0;
    float viewW = 0;                 // yerlesimin hesaplandigi gorunur alan genisligi (CSS px)
    uint32_t background = 0xFFFFFFFF;
};

void LayoutPage(const Document& doc, const std::vector<Style>& styles, float viewportWidth,
                ITextMeasurer& tm, PageLayout& out, IImageProvider* images = nullptr, float viewportHeight = 0);

} // namespace mer
