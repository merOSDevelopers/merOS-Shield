// merOS Shield - MerEngine : yerleşim (layout) motoru ve görüntü listesi
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dom.h"
#include "style.h"

namespace mer {

struct FontMetrics {
    float baseline = 12;  // yazi kutusu ustunden taban cizgisine
    float height = 16;    // yazi kutusu yuksekligi
    float space = 4;      // bosluk genisligi
};

// Metin olcumu soyut: Windows'ta DirectWrite, testlerde sahte olcucu.
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() {}
    virtual int Font(const Style& s) = 0;
    virtual float Width(int font, const std::wstring& text) = 0;
    virtual FontMetrics Metrics(int font) = 0;
};

struct DisplayItem {
    enum Kind : uint8_t { Fill, Text, HLine, Image, Stroke };
    enum Role : uint8_t { None, TextInput, Button, Check };
    Kind kind = Fill;
    Role role = None;
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0;
    int font = -1;
    int link = -1;
    int node = -1;       // ilgili eleman (tiklama hedefi, form alani)
    std::wstring text;   // metin; Image icin resim anahtari (URL)

    // CSS sprite kesme ve konumlandirma
    float srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    float radius = 0;    // border-radius
    int32_t zIndex = 0;  // katmanlama siralamasi
    bool isBackground = false; // arka plan resmi
};

// Yuklenmis resimlerin boyutlarini yerlesime bildirir (platformdan bagimsiz).
class IImageProvider {
public:
    virtual ~IImageProvider() {}
    // img dugumu icin yuklu resim varsa dogal boyutunu ve cizim anahtarini dondurur.
    virtual bool Lookup(const Node* img, float& w, float& h, std::wstring& key) = 0;
    // URL anahtarina gore boyut dondurur (arka plan resmi ve CSS sprite'lar icin)
    virtual bool LookupByKey(const std::wstring& key, float& w, float& h) { return false; }
};

struct PageLayout {
    std::vector<DisplayItem> items;
    std::vector<std::wstring> links; // ham href degerleri (cizim ogesi link indeksi)
    float height = 0;
    float width = 0;
    uint32_t background = 0xFFFFFFFF;
};

void LayoutPage(const Document& doc, const std::vector<Style>& styles, float viewportWidth,
                ITextMeasurer& tm, PageLayout& out, IImageProvider* images = nullptr);

} // namespace mer
