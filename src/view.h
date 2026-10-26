// merOS Shield - sayfa görünümü: DirectWrite ölçüm + Direct2D çizim + kaydırma/tıklama/klavye
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "images.h"
#include "layout.h"

namespace mer {

// Metin olcumu: tum sekme is parcaciklari (yerlesim) ve arayuz (cizim) tek ornegi paylasir;
// yazi tipi tablosu ve genislik onbellegi kilitle korunur.
class DWriteMeasurer : public ITextMeasurer {
public:
    explicit DWriteMeasurer(IDWriteFactory* f);
    ~DWriteMeasurer() override;
    int Font(const Style& s) override;
    float Width(int font, const std::wstring& text) override;
    FontMetrics Metrics(int font) override;
    IDWriteTextFormat* Format(int font) const {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        return font >= 0 && size_t(font) < fonts_.size() ? fonts_[size_t(font)].fmt : nullptr;
    }

private:
    std::wstring ResolveFamily(const std::wstring& cssList);
    bool Exists(const std::wstring& fam);
    struct Entry { IDWriteTextFormat* fmt; FontMetrics m; };
    mutable std::recursive_mutex mu_;
    IDWriteFactory* f_;
    IDWriteFontCollection* sys_ = nullptr;
    std::vector<Entry> fonts_;
    std::unordered_map<std::wstring, int> keys_;
    std::unordered_map<std::wstring, std::wstring> famCache_;
    std::unordered_map<std::wstring, float> widths_;
    std::wstring defaultFamily_;
};

// Gorunumun cizdigi sayfa: sekme is parcaciginin yayimladigi degismez yerlesim + paylasilan resim deposu.
// Gorunum DOM'a hic dokunmaz; boylece donan bir sekme arayuzu kilitleyemez.
struct ViewPage {
    std::shared_ptr<ImageStore> images;
    std::shared_ptr<const PageLayout> layout;
};

class PageView {
public:
    static bool Register(HINSTANCE inst);
    HWND Create(HWND parent, HINSTANCE inst);
    HWND Hwnd() const { return hwnd_; }

    void SetPage(const ViewPage& p);         // yeni sayfa: kaydirma basa
    void SetLayout(std::shared_ptr<const PageLayout> l, bool keepScroll = true);   // sekme yeni yerlesim yayimladi
    void Relayout(bool keepScroll = true);   // boyut/yakinlastirma degisti: sekmeden yeni yerlesim iste (onRelayout)
    std::shared_ptr<DWriteMeasurer> Measurer() const { return tm_; }
    float LaidOutW() const { return laidOutW_; }
    void InvalidateImageKey(const std::wstring& key);   // yalnizca o resmi (video karesi) yeniden ciz
    void SetZoom(float z);
    float Zoom() const { return zoom_; }
    float ScrollY() const { return scroll_; }
    void ScrollTo(float y);                  // aninda
    void ScrollSmooth(float y);              // yumusak (animasyonlu) kaydirma
    void ScrollBy(float dy) { ScrollSmooth(target_ + dy); }
    const PageLayout& Layout() const { return *layout_; }
    std::shared_ptr<const PageLayout> LayoutPtr() const { return layout_; }
    void SetFocusNode(int nodeId) { focusNode_ = nodeId; InvalidateRect(hwnd_, nullptr, FALSE); }
    int FocusNode() const { return focusNode_; }
    float ViewW() const;
    float ViewH() const;
    bool SaveSnapshot(const std::wstring& pngPath); // tam sayfa PNG (en fazla 16384 px yukseklik)

    // Sayfada bul: dir 0 = yeni arama (gorunur alandan itibaren ilk eslesme), 1 sonraki, -1 onceki.
    // Eslesme sayisini dondurur; FindCurrent() 0 tabanli gecerli eslesme.
    int Find(const std::wstring& query, int dir);
    int FindCurrent() const { return findCur_; }
    void ClearFind();
    // Metin secimi (fareyle surukleme, cift tik = kelime, Ctrl+A)
    bool HasSelection() const { return selA_.item >= 0 && selB_.item >= 0 && !(selA_.item == selB_.item && selA_.ch == selB_.ch); }
    std::wstring SelectedText() const;
    void SelectAll();
    void ClearSelection();

    // Cerceve geri cagrilari
    std::function<void(int nodeId, int link, bool ctrl, float x, float y)> onClick;   // x, y: belge koordinati (CSS px)
    std::function<void(const std::wstring& href)> onHover;
    std::function<void(wchar_t ch)> onChar;           // odakli form alanina yazim
    std::function<bool(UINT vk)> onKey;               // odakli form alaninda ozel tuslar
    std::function<void(int link)> onMiddleClick;      // orta tik (baglanti yeni sekmede)
    std::function<void(int link, int nodeId, POINT screen)> onContext; // sag tik menusu
    std::function<void()> onRelayout;                 // gorunur alan degisti: yeni yerlesim gerekli
    std::function<void(float y)> onScrolled;          // kaydirma konumu (getBoundingClientRect icin)

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    bool EnsureTarget();
    void ReleaseTarget();
    void Paint();
    void DrawGradient(ID2D1RenderTarget* rt, const DisplayItem& d);
    void DrawShadow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const DisplayItem& d);
    void DrawShadowShape(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const DisplayItem& d);
    void DrawItems(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float top, float bottom, bool snapshot);
    void DrawImage(ID2D1RenderTarget* rt, const std::wstring& key, uint32_t tint, const D2D1_RECT_F& dst, bool snapshot);
    void UpdateScrollBar();
    const DisplayItem* Hit(int px, int py) const;
    D2D1_COLOR_F Col(uint32_t argb) const;
    struct TextPos { int item = -1; int ch = 0; };
    struct Range { TextPos a, b; };
    void BuildTextIndex();
    TextPos HitText(float x, float y) const;     // belge koordinati -> en yakin metin konumu
    float CharX(const DisplayItem& d, int ch) const;
    void DrawRange(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const Range& r, uint32_t color, float top, float bottom);
    void ResetTextState();

    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::shared_ptr<DWriteMeasurer> tm_;
    std::shared_ptr<ImageStore> images_;
    std::shared_ptr<const PageLayout> layout_ = std::make_shared<PageLayout>();
    float scroll_ = 0;
    float zoom_ = 1.f;
    float dpi_ = 96.f;
    int hoverLink_ = -1;
    int hoverRole_ = 0;
    int hoverCursor_ = 0;
    int focusNode_ = -1;
    float laidOutW_ = -1;
    bool colorFont_ = true;
    // yumusak kaydirma
    float target_ = 0;
    bool animating_ = false;
    // metin dizini: tum Text ogeleri tek akista (bul ve secim icin)
    std::vector<int> textItems_;                 // akistaki sira -> oge indeksi
    std::vector<int> itemOrder_;                 // oge indeksi -> akistaki sira (-1 metin degil)
    std::wstring stream_;                        // birlesik kucuk harf metin
    std::vector<std::pair<int, int>> streamMap_; // akis karakteri -> (oge, karakter)
    bool indexValid_ = false;
    // bul
    std::wstring findQuery_;
    std::vector<Range> matches_;
    int findCur_ = -1;
    // secim
    TextPos selA_, selB_;
    bool mouseDown_ = false, selecting_ = false, suppressClick_ = false;
    float downX_ = 0, downY_ = 0;
};

} // namespace mer
