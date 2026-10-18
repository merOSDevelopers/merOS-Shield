// merOS Shield - sayfa görünümü: DirectWrite ölçüm + Direct2D çizim + kaydırma/tıklama/klavye
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "images.h"
#include "layout.h"

namespace mer {

class DWriteMeasurer : public ITextMeasurer {
public:
    explicit DWriteMeasurer(IDWriteFactory* f);
    ~DWriteMeasurer() override;
    int Font(const Style& s) override;
    float Width(int font, const std::wstring& text) override;
    FontMetrics Metrics(int font) override;
    IDWriteTextFormat* Format(int font) const { return fonts_[size_t(font)].fmt; }

private:
    std::wstring ResolveFamily(const std::wstring& cssList);
    bool Exists(const std::wstring& fam);
    struct Entry { IDWriteTextFormat* fmt; FontMetrics m; };
    IDWriteFactory* f_;
    IDWriteFontCollection* sys_ = nullptr;
    std::vector<Entry> fonts_;
    std::unordered_map<std::wstring, int> keys_;
    std::unordered_map<std::wstring, std::wstring> famCache_;
    std::unordered_map<std::wstring, float> widths_;
    std::wstring defaultFamily_;
};

// Gorunumun cizdigi sayfa (sahibi cerceve)
struct ViewPage {
    Document* doc = nullptr;
    const std::vector<Style>* styles = nullptr;
    ImageStore* images = nullptr;
};

class PageView {
public:
    static bool Register(HINSTANCE inst);
    HWND Create(HWND parent, HINSTANCE inst);
    HWND Hwnd() const { return hwnd_; }

    void SetPage(const ViewPage& p);         // yeni sayfa: kaydirma basa
    void Relayout(bool keepScroll = true);   // stil/DOM/resim degisti
    void SetZoom(float z);
    float Zoom() const { return zoom_; }
    float ScrollY() const { return scroll_; }
    void ScrollTo(float y);
    const PageLayout& Layout() const { return layout_; }
    void SetFocusNode(int nodeId) { focusNode_ = nodeId; InvalidateRect(hwnd_, nullptr, FALSE); }
    int FocusNode() const { return focusNode_; }
    float ViewW() const;
    float ViewH() const;
    bool SaveSnapshot(const std::wstring& pngPath); // tam sayfa PNG (en fazla 16384 px yukseklik)

    // Cerceve geri cagrilari
    std::function<void(int nodeId, int link, bool ctrl)> onClick;
    std::function<void(const std::wstring& href)> onHover;
    std::function<void(wchar_t ch)> onChar;           // odakli form alanina yazim
    std::function<bool(UINT vk)> onKey;               // odakli form alaninda ozel tuslar

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    bool EnsureTarget();
    void ReleaseTarget();
    void Paint();
    void DrawItems(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float top, float bottom, bool snapshot);
    void UpdateScrollBar();
    const DisplayItem* Hit(int px, int py) const;
    D2D1_COLOR_F Col(uint32_t argb) const;

    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::unique_ptr<DWriteMeasurer> tm_;
    ViewPage page_;
    PageLayout layout_;
    float scroll_ = 0;
    float zoom_ = 1.f;
    float dpi_ = 96.f;
    int hoverLink_ = -1;
    int hoverRole_ = 0;
    int focusNode_ = -1;
    float laidOutW_ = -1;
    bool colorFont_ = true;
};

} // namespace mer
