// merOS Shield - sayfa görünümü: DirectWrite ölçüm + Direct2D çizim + kaydırma/tıklama
// Hiz: yalnizca ekranda gorunen ogeler cizilir, kelime genislikleri onbellege alinir,
// cizim GPU hizlandirmali Direct2D ile yapilir.
#include "view.h"
#include "util.h"
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace mer {

namespace {
const wchar_t* kViewClass = L"MerEngineView";
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}

// ---------------- DWriteMeasurer ----------------

DWriteMeasurer::DWriteMeasurer(IDWriteFactory* f) : f_(f) {
    f_->AddRef();
    f_->GetSystemFontCollection(&sys_, FALSE);
}

DWriteMeasurer::~DWriteMeasurer() {
    for (auto& e : fonts_) SafeRelease(e.fmt);
    SafeRelease(sys_);
    SafeRelease(f_);
}

bool DWriteMeasurer::Exists(const std::wstring& fam) {
    if (!sys_ || fam.empty()) return false;
    UINT32 idx = 0;
    BOOL ex = FALSE;
    return SUCCEEDED(sys_->FindFamilyName(fam.c_str(), &idx, &ex)) && ex;
}

std::wstring DWriteMeasurer::ResolveFamily(const std::wstring& list) {
    auto it = famCache_.find(list);
    if (it != famCache_.end()) return it->second;
    std::wstring result = defaultFamily_;
    if (result.empty()) {
        for (const wchar_t* f : {L"Segoe UI", L"Arial", L"Tahoma", L"Verdana"})
            if (Exists(f)) { result = f; break; }
        if (result.empty() && sys_ && sys_->GetFontFamilyCount() > 0) {
            IDWriteFontFamily* fam = nullptr;
            IDWriteLocalizedStrings* names = nullptr;
            if (SUCCEEDED(sys_->GetFontFamily(0, &fam)) && SUCCEEDED(fam->GetFamilyNames(&names))) {
                wchar_t buf[128] = {0};
                names->GetString(0, buf, 128);
                result = buf;
            }
            SafeRelease(names);
            SafeRelease(fam);
        }
        defaultFamily_ = result;
    }
    size_t st = 0;
    while (st <= list.size()) {
        size_t e = list.find(L',', st);
        if (e == std::wstring::npos) e = list.size();
        std::wstring f = Trim(list.substr(st, e - st));
        while (!f.empty() && (f[0] == L'"' || f[0] == L'\'')) f.erase(0, 1);
        while (!f.empty() && (f.back() == L'"' || f.back() == L'\'')) f.pop_back();
        std::wstring lf = ToLower(f);
        std::wstring cand;
        if (lf == L"sans-serif" || lf == L"system-ui" || lf == L"-apple-system" || lf == L"blinkmacsystemfont" ||
            lf == L"ui-sans-serif" || lf == L"helvetica" || lf == L"helvetica neue" || lf == L"arial" || lf == L"roboto" ||
            lf == L"inter" || lf == L"open sans" || lf == L"noto sans")
            cand = (lf == L"arial") ? L"Arial" : L"Segoe UI";
        else if (lf == L"serif" || lf == L"ui-serif" || lf == L"times") cand = L"Georgia";
        else if (lf == L"monospace" || lf == L"ui-monospace" || lf == L"courier" || lf == L"menlo" || lf == L"monaco" ||
                 lf == L"sfmono-regular" || lf == L"courier new" || lf == L"liberation mono")
            cand = L"Consolas";
        else if (lf == L"cursive") cand = L"Segoe Script";
        else if (lf == L"fantasy") cand = L"Impact";
        else cand = f;
        if (!cand.empty() && Exists(cand)) { result = cand; break; }
        st = e + 1;
    }
    if (famCache_.size() > 2000) famCache_.clear();
    famCache_[list] = result;
    return result;
}

int DWriteMeasurer::Font(const Style& s) {
    std::wstring fam = ResolveFamily(s.family);
    float size = std::max(4.f, std::min(std::round(s.fontSize * 2.f) / 2.f, 300.f));
    int weight = std::max(100, std::min(s.weight, 950));
    std::wstring key = fam + L"|" + std::to_wstring(int(size * 2)) + L"|" + std::to_wstring(weight) + (s.italic ? L"i" : L"n");
    auto it = keys_.find(key);
    if (it != keys_.end()) return it->second;

    IDWriteTextFormat* fmt = nullptr;
    HRESULT hr = f_->CreateTextFormat(fam.c_str(), nullptr, DWRITE_FONT_WEIGHT(weight),
                                      s.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"tr-tr", &fmt);
    if (FAILED(hr)) f_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"tr-tr", &fmt);
    if (fmt) fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Entry e{fmt, FontMetrics{size * 0.9f, size * 1.33f, size * 0.28f}};
    if (fmt) {
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(f_->CreateTextLayout(L"Hg", 2, fmt, 10000.f, 10000.f, &lay))) {
            DWRITE_LINE_METRICS lm{};
            UINT32 cnt = 0;
            if (SUCCEEDED(lay->GetLineMetrics(&lm, 1, &cnt)) && cnt > 0) { e.m.baseline = lm.baseline; e.m.height = lm.height; }
            lay->Release();
        }
    }
    fonts_.push_back(e);
    int id = int(fonts_.size()) - 1;
    keys_[key] = id;
    if (fmt) {
        float w2 = Width(id, L"aa"), w3 = Width(id, L"a a");
        if (w3 > w2) fonts_[size_t(id)].m.space = w3 - w2;
    }
    return id;
}

float DWriteMeasurer::Width(int font, const std::wstring& text) {
    if (font < 0 || size_t(font) >= fonts_.size() || !fonts_[size_t(font)].fmt || text.empty()) return 0;
    std::wstring key = std::to_wstring(font);
    key += L'\x1';
    key += text;
    auto it = widths_.find(key);
    if (it != widths_.end()) return it->second;
    float w = 0;
    IDWriteTextLayout* lay = nullptr;
    if (SUCCEEDED(f_->CreateTextLayout(text.c_str(), UINT32(text.size()), fonts_[size_t(font)].fmt, 100000.f, 10000.f, &lay))) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(lay->GetMetrics(&tm))) w = tm.widthIncludingTrailingWhitespace;
        lay->Release();
    }
    if (widths_.size() > 400000) widths_.clear();
    widths_.emplace(std::move(key), w);
    return w;
}

FontMetrics DWriteMeasurer::Metrics(int font) {
    if (font < 0 || size_t(font) >= fonts_.size()) return FontMetrics();
    return fonts_[size_t(font)].m;
}

// ---------------- PageView ----------------

bool PageView::Register(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kViewClass;
    return RegisterClassExW(&wc) != 0;
}

HWND PageView::Create(HWND parent, HINSTANCE inst) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw_));
    if (!d2d_ || !dw_) return nullptr;
    tm_.reset(new DWriteMeasurer(dw_));
    hwnd_ = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS | WS_TABSTOP,
                            0, 0, 100, 100, parent, nullptr, inst, this);
    return hwnd_;
}

LRESULT CALLBACK PageView::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    PageView* self = reinterpret_cast<PageView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = reinterpret_cast<PageView*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
        self->hwnd_ = h;
    }
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

float PageView::ViewW() const {
    RECT rc; GetClientRect(hwnd_, &rc);
    return float(rc.right) * 96.f / dpi_ / zoom_;
}
float PageView::ViewH() const {
    RECT rc; GetClientRect(hwnd_, &rc);
    return float(rc.bottom) * 96.f / dpi_ / zoom_;
}

D2D1_COLOR_F PageView::Col(uint32_t c) const {
    return D2D1::ColorF(float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, float(c & 255) / 255.f, float(c >> 24) / 255.f);
}

void PageView::ReleaseTarget() {
    if (page_.images) page_.images->ReleaseBitmaps();
    SafeRelease(brush_);
    SafeRelease(rt_);
}

bool PageView::EnsureTarget() {
    dpi_ = float(GetDpiForWindow(hwnd_));
    if (dpi_ < 48) dpi_ = 96;
    if (rt_) { rt_->SetDpi(dpi_, dpi_); return true; }
    RECT rc; GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = dpi_;
    if (FAILED(d2d_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(UINT32(rc.right), UINT32(rc.bottom))), &rt_)))
        return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    return brush_ != nullptr;
}

void PageView::SetPage(const ViewPage& p) {
    // eski sayfanin bitmapleri eski depoya aitti; depo sayfayla birlikte yok edilir
    page_ = p;
    scroll_ = 0;
    hoverLink_ = -1;
    focusNode_ = -1;
    laidOutW_ = -1;
    Relayout(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PageView::SetZoom(float z) {
    z = std::max(0.3f, std::min(z, 4.f));
    if (std::fabs(z - zoom_) < 0.001f) return;
    float rel = layout_.height > 0 ? scroll_ / layout_.height : 0;
    zoom_ = z;
    Relayout(true);
    ScrollTo(rel * layout_.height);
}

void PageView::Relayout(bool keepScroll) {
    dpi_ = float(GetDpiForWindow(hwnd_));
    if (dpi_ < 48) dpi_ = 96;
    float vw = ViewW();
    if (!page_.doc || !page_.styles) { layout_ = PageLayout(); UpdateScrollBar(); return; }
    laidOutW_ = vw;
    float keep = scroll_;
    LayoutPage(*page_.doc, *page_.styles, std::max(200.f, vw), *tm_, layout_, page_.images);
    ScrollTo(keepScroll ? keep : 0);
}

void PageView::UpdateScrollBar() {
    SCROLLINFO si{sizeof(si)};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = int(std::ceil(layout_.height));
    si.nPage = UINT(std::max(1.f, ViewH()));
    si.nPos = int(scroll_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void PageView::ScrollTo(float y) {
    float maxS = std::max(0.f, layout_.height - ViewH());
    scroll_ = std::max(0.f, std::min(y, maxS));
    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

const DisplayItem* PageView::Hit(int px, int py) const {
    float x = float(px) * 96.f / dpi_ / zoom_;
    float y = float(py) * 96.f / dpi_ / zoom_ + scroll_;
    for (size_t i = layout_.items.size(); i-- > 0;) {
        const DisplayItem& d = layout_.items[i];
        if (d.link < 0 && d.node < 0) continue;
        if (x >= d.x && x <= d.x + d.w && y >= d.y && y <= d.y + std::max(d.h, 2.f)) return &d;
    }
    return nullptr;
}

void PageView::DrawItems(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float top, float bottom, bool snapshot) {
    // ENABLE_COLOR_FONT (4): renkli emoji. Desteklenmeyen surucude otomatik kapatilir.
    const D2D1_DRAW_TEXT_OPTIONS textOpts = (colorFont_ && !snapshot) ? D2D1_DRAW_TEXT_OPTIONS(4) : D2D1_DRAW_TEXT_OPTIONS_NONE;
    const DisplayItem* focusText = nullptr;
    for (const DisplayItem& d : layout_.items) {
        if (d.y > bottom || d.y + d.h < top) continue;
        uint32_t c = d.color;
        if (!snapshot && d.link >= 0 && d.link == hoverLink_ && d.kind != DisplayItem::Fill && d.kind != DisplayItem::Image) c = 0xFFC2410C;
        brush->SetColor(Col(c));
        switch (d.kind) {
        case DisplayItem::Fill:
        case DisplayItem::HLine:
            if (d.radius > 0) {
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), d.radius, d.radius);
                rt->FillRoundedRectangle(rr, brush);
            } else {
                rt->FillRectangle(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), brush);
            }
            break;
        case DisplayItem::Stroke:
            if (!snapshot && d.node >= 0 && d.node == focusNode_) {
                brush->SetColor(Col(0xFF2563EB));
                if (d.radius > 0) {
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), d.radius, d.radius);
                    rt->DrawRoundedRectangle(rr, brush, 2.f);
                } else {
                    rt->DrawRectangle(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), brush, 2.f);
                }
            } else {
                if (d.radius > 0) {
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(d.x + 0.5f, d.y + 0.5f, d.x + d.w - 0.5f, d.y + d.h - 0.5f), d.radius, d.radius);
                    rt->DrawRoundedRectangle(rr, brush, 1.f);
                } else {
                    rt->DrawRectangle(D2D1::RectF(d.x + 0.5f, d.y + 0.5f, d.x + d.w - 0.5f, d.y + d.h - 0.5f), brush, 1.f);
                }
            }
            break;
        case DisplayItem::Image:
            if (page_.images) {
                ID2D1Bitmap* bmp = nullptr;
                ID2D1Bitmap* tmp = nullptr;
                if (!snapshot) {
                    bmp = page_.images->Bitmap(rt, d.text);
                } else {
                    // ekran goruntusu: gecici bitmap (onbellek ekran hedefine ait)
                    auto it = page_.images->map.find(d.text);
                    if (it != page_.images->map.end() && it->second.state == 1 && !it->second.px.empty()) {
                        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(it->second.w, it->second.h), it->second.px.data(), it->second.w * 4, props, &tmp))) {
                            bmp = tmp;
                        }
                    }
                }
                if (bmp) {
                    D2D1_RECT_F dstRect = D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h);
                    if (d.srcW > 0 && d.srcH > 0) {
                        D2D1_RECT_F srcRect = D2D1::RectF(d.srcX, d.srcY, d.srcX + d.srcW, d.srcY + d.srcH);
                        rt->DrawBitmap(bmp, dstRect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRect);
                    } else {
                        rt->DrawBitmap(bmp, dstRect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                    if (tmp) tmp->Release();
                }
            }
            break;
        case DisplayItem::Text:
            if (d.font >= 0 && tm_->Format(d.font)) {
                if (d.role == DisplayItem::TextInput) {
                    // metni kutuya kirp
                    rt->PushAxisAlignedClip(D2D1::RectF(d.x, d.y - 2, d.x + d.w, d.y + d.h + 2), D2D1_ANTIALIAS_MODE_ALIASED);
                    rt->DrawText(d.text.c_str(), UINT32(d.text.size()), tm_->Format(d.font), D2D1::RectF(d.x, d.y, d.x + 100000, d.y + d.h), brush, textOpts);
                    rt->PopAxisAlignedClip();
                    if (!snapshot && d.node == focusNode_) focusText = &d;
                } else {
                    rt->DrawText(d.text.c_str(), UINT32(d.text.size()), tm_->Format(d.font),
                                 D2D1::RectF(d.x, d.y, d.x + d.w + 4, d.y + d.h), brush, textOpts);
                }
            }
            break;
        }
    }
    if (focusText) {
        // imlec: odakli alandaki metnin sonunda (yer tutucu gosteriliyorsa basta)
        float cw = tm_->Width(focusText->font, focusText->text);
        if (focusText->color == 0xFF8A8F99) cw = tm_->Width(focusText->font, L" ");
        float cx = std::min(focusText->x + cw + 1, focusText->x + focusText->w - 2);
        brush->SetColor(Col(0xFF111827));
        rt->FillRectangle(D2D1::RectF(cx, focusText->y + 1, cx + 1.5f, focusText->y + focusText->h - 1), brush);
    }
}

void PageView::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (EnsureTarget()) {
        rt_->BeginDraw();
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        rt_->Clear(Col(layout_.background));
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll_) * D2D1::Matrix3x2F::Scale(zoom_, zoom_));
        DrawItems(rt_, brush_, scroll_, scroll_ + ViewH(), false);
        HRESULT hr = rt_->EndDraw();
        if (hr == HRESULT(D2DERR_RECREATE_TARGET)) {
            ReleaseTarget();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (FAILED(hr) && colorFont_) {
            colorFont_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    EndPaint(hwnd_, &ps);
}

bool PageView::SaveSnapshot(const std::wstring& path) {
    if (!d2d_) return false;
    UINT W = UINT(std::max(200.f, laidOutW_ > 0 ? laidOutW_ : ViewW()));
    UINT H = UINT(std::min(16384.f, std::max(100.f, layout_.height)));
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) break;
        if (FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) break;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(d2d_->CreateWicBitmapRenderTarget(bmp, props, &rt))) break;
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br))) break;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->BeginDraw();
        rt->Clear(Col(layout_.background));
        DrawItems(rt, br, 0, float(H), true);
        if (FAILED(rt->EndDraw())) break;
        if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr))) break;
        if (FAILED(fr->WriteSource(bmp, nullptr)) || FAILED(fr->Commit()) || FAILED(enc->Commit())) break;
        ok = true;
    } while (false);
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(br);
    SafeRelease(rt);
    SafeRelease(bmp);
    SafeRelease(wic);
    return ok;
}

LRESULT PageView::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_SIZE: {
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        float vw = ViewW();
        if (std::fabs(vw - laidOutW_) > 0.5f) Relayout(true);
        else ScrollTo(scroll_);
        return 0;
    }
    case WM_DPICHANGED_AFTERPARENT:
        Relayout(true);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        float y = scroll_;
        switch (LOWORD(w)) {
        case SB_LINEUP: y -= 40; break;
        case SB_LINEDOWN: y += 40; break;
        case SB_PAGEUP: y -= ViewH() * 0.9f; break;
        case SB_PAGEDOWN: y += ViewH() * 0.9f; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: y = float(si.nTrackPos); break;
        case SB_TOP: y = 0; break;
        case SB_BOTTOM: y = layout_.height; break;
        }
        ScrollTo(y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
            SetZoom(zoom_ * (delta > 0 ? 1.1f : 1.f / 1.1f));
            return 0;
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        ScrollTo(scroll_ - float(delta) / WHEEL_DELTA * float(lines) * 38.f);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (focusNode_ >= 0 && onKey && onKey(UINT(w))) return 0;
        if (focusNode_ >= 0 && (w == VK_LEFT || w == VK_RIGHT || w == VK_BACK || w == VK_SPACE || w == VK_HOME || w == VK_END)) return 0;
        switch (w) {
        case VK_DOWN: ScrollTo(scroll_ + 40); return 0;
        case VK_UP: ScrollTo(scroll_ - 40); return 0;
        case VK_NEXT: case VK_SPACE: ScrollTo(scroll_ + ViewH() * 0.9f * ((GetKeyState(VK_SHIFT) < 0 && w == VK_SPACE) ? -1 : 1)); return 0;
        case VK_PRIOR: ScrollTo(scroll_ - ViewH() * 0.9f); return 0;
        case VK_HOME: ScrollTo(0); return 0;
        case VK_END: ScrollTo(layout_.height); return 0;
        }
        break;
    case WM_CHAR:
        if (focusNode_ >= 0 && onChar) { onChar(wchar_t(w)); return 0; }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetFocus(hwnd_);
        return 0;
    case WM_LBUTTONUP: {
        const DisplayItem* d = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (onClick) onClick(d ? d->node : -1, d ? d->link : -1, (w & MK_CONTROL) != 0);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const DisplayItem* d = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        int lk = d ? d->link : -1;
        hoverRole_ = d ? int(d->role) : 0;
        if (lk != hoverLink_) {
            hoverLink_ = lk;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onHover) onHover(lk >= 0 && size_t(lk) < layout_.links.size() ? layout_.links[size_t(lk)] : L"");
            TRACKMOUSEEVENT te{sizeof(te), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&te);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hoverLink_ >= 0) {
            hoverLink_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onHover) onHover(L"");
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            LPCWSTR cur = IDC_ARROW;
            if (hoverLink_ >= 0 || hoverRole_ == DisplayItem::Button || hoverRole_ == DisplayItem::Check) cur = IDC_HAND;
            else if (hoverRole_ == DisplayItem::TextInput) cur = IDC_IBEAM;
            SetCursor(LoadCursor(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        layout_ = PageLayout();
        ReleaseTarget();
        tm_.reset();
        SafeRelease(dw_);
        SafeRelease(d2d_);
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

} // namespace mer
