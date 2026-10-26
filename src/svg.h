// merOS Shield - MerEngine : SVG çizici
// SVG belgesi kendi ayristiricimizla okunur; sekiller (path, rect, circle, ellipse, line,
// polyline, polygon), gruplar, donusumler, <use>/<symbol>, <style> sinif kurallari,
// currentColor ve degradeler (yaklasik renk) desteklenir. Cizim platform katmaninda
// (Windows: Direct2D yol geometrisi) yapilir; geometri uretimi platformdan bagimsizdir.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mer {

// Platformdan bagimsiz yol parcasi (cihaz koordinatlari): M=0, L=1, C=2 (kubik), Z=3
struct SvgSeg {
    uint8_t kind = 0;
    float x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
};

class ISvgPainter {
public:
    virtual ~ISvgPainter() {}
    virtual void Fill(const std::vector<SvgSeg>& path, bool evenOdd, uint32_t argb) = 0;
    virtual void Stroke(const std::vector<SvgSeg>& path, float width, uint32_t argb, int cap, int join) = 0;
};

// SVG'yi ayristirir; dogal boyutu (px) dondurur. Basarisizsa false.
bool SvgNaturalSize(const std::wstring& markup, float& w, float& h);

// SVG'yi verilen hedef boyutta (piksel) cizer. currentColor: CSS 'color' degeri.
bool RenderSvg(const std::wstring& markup, float targetW, float targetH, uint32_t currentColor, ISvgPainter& painter);

// Windows: SVG'yi BGRA (onceden carpilmis alfa) piksellere cizer.
// scale: dogal boyuta gore carpan (keskinlik icin 2). natW/natH: dogal (CSS piksel) boyut.
bool RasterizeSvg(const std::wstring& markup, float scale, uint32_t currentColor,
                  uint32_t& outW, uint32_t& outH, float& natW, float& natH, std::vector<uint8_t>& px);

// Baytlar SVG gibi gorunuyor mu (ilk 4 KB'ta <svg)?
bool LooksLikeSvg(const std::string& bytes);

} // namespace mer
