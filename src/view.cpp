// merOS Shield - sayfa görünümü: DirectWrite ölçüm + Direct2D çizim + kaydırma/tıklama
// Hiz: yalnizca ekranda gorunen ogeler cizilir, kelime genislikleri onbellege alinir,
// cizim GPU hizlandirmali Direct2D ile yapilir.
#include "view.h"
#include "util.h"
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>

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
    std::lock_guard<std::recursive_mutex> lk(mu_);
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
    std::lock_guard<std::recursive_mutex> lk(mu_);
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
    std::lock_guard<std::recursive_mutex> lk(mu_);
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
    tm_ = std::make_shared<DWriteMeasurer>(dw_);
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
    if (images_) images_->ReleaseBitmaps();
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
    // eski sayfanin bitmapleri (Direct2D, yalnizca bu is parcacigi) burada birakilir;
    // depo sekmeyle paylasildigi icin sekme arka plandayken de yasar
    if (images_ && images_ != p.images) images_->ReleaseBitmaps();
    images_ = p.images;
    layout_ = p.layout ? p.layout : std::make_shared<PageLayout>();
    scroll_ = 0;
    target_ = 0;
    ResetTextState();
    findQuery_.clear();
    hoverLink_ = -1;
    focusNode_ = -1;
    laidOutW_ = -1;
    SetLayout(layout_, false);
    if (std::fabs(ViewW() - laidOutW_) > 0.5f) Relayout(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PageView::SetZoom(float z) {
    z = std::max(0.3f, std::min(z, 4.f));
    if (std::fabs(z - zoom_) < 0.001f) return;
    float rel = layout_->height > 0 ? scroll_ / layout_->height : 0;
    zoom_ = z;
    Relayout(true);
    ScrollTo(rel * layout_->height);
}

void PageView::Relayout(bool keepScroll) {
    (void)keepScroll;
    dpi_ = float(GetDpiForWindow(hwnd_));
    if (dpi_ < 48) dpi_ = 96;
    if (onRelayout) onRelayout();
}

void PageView::SetLayout(std::shared_ptr<const PageLayout> l, bool keepScroll) {
    if (!l) l = std::make_shared<PageLayout>();
    laidOutW_ = l->viewW > 0 ? l->viewW : -1;
    float keep = scroll_, keepTarget = target_;
    size_t oldCount = layout_->items.size();
    layout_ = std::move(l);
    // metin dizini yeniden kurulur; oge sayisi degistiyse secim gecersiz
    indexValid_ = false;
    if (layout_->items.size() != oldCount) { selA_ = TextPos(); selB_ = TextPos(); }
    if (!findQuery_.empty()) {
        int cur = findCur_;
        std::wstring q = findQuery_;
        Find(q, 2);   // eslesmeleri yeniden hesapla, konumu koru
        if (cur >= 0 && !matches_.empty()) findCur_ = std::min(cur, int(matches_.size()) - 1);
    }
    bool anim = animating_;
    ScrollTo(keepScroll ? keep : 0);
    if (keepScroll && anim) ScrollSmooth(keepTarget);
}

// ---------------- Metin dizini, bul, secim ----------------

void PageView::ResetTextState() {
    indexValid_ = false;
    textItems_.clear();
    itemOrder_.clear();
    stream_.clear();
    streamMap_.clear();
    matches_.clear();
    findCur_ = -1;
    selA_ = TextPos();
    selB_ = TextPos();
    selecting_ = mouseDown_ = false;
}

void PageView::BuildTextIndex() {
    if (indexValid_) return;
    indexValid_ = true;
    textItems_.clear();
    stream_.clear();
    streamMap_.clear();
    itemOrder_.assign(layout_->items.size(), -1);
    const DisplayItem* prev = nullptr;
    for (size_t i = 0; i < layout_->items.size(); i++) {
        const DisplayItem& d = layout_->items[i];
        if (d.kind != DisplayItem::Text || d.font < 0 || d.text.empty() || d.role == DisplayItem::TextInput) continue;
        if (stream_.size() > 4000000) break;
        if (prev) {
            bool sameLine = std::fabs((prev->y + prev->h / 2) - (d.y + d.h / 2)) < std::max(prev->h, d.h) * 0.5f;
            bool touching = sameLine && d.x <= prev->x + prev->w + 1.5f && d.x >= prev->x;
            wchar_t last = stream_.empty() ? L' ' : stream_.back();
            if (!touching && last != L' ' && last != L'\n' && d.text[0] != L' ') {
                stream_ += sameLine ? L' ' : L'\n';
                streamMap_.push_back({-1, 0});
            }
        }
        itemOrder_[i] = int(textItems_.size());
        textItems_.push_back(int(i));
        for (size_t c = 0; c < d.text.size(); c++) {
            stream_ += wchar_t(towlower(d.text[c]));
            streamMap_.push_back({int(i), int(c)});
        }
        prev = &d;
    }
}

float PageView::CharX(const DisplayItem& d, int ch) const {
    if (ch <= 0) return d.x;
    if (size_t(ch) >= d.text.size()) return d.x + tm_->Width(d.font, d.text);
    return d.x + tm_->Width(d.font, d.text.substr(0, size_t(ch)));
}

PageView::TextPos PageView::HitText(float x, float y) const {
    TextPos best;
    float bestD = 1e30f;
    for (int idx : textItems_) {
        const DisplayItem& d = layout_->items[size_t(idx)];
        float dy = y < d.y ? d.y - y : (y > d.y + d.h ? y - (d.y + d.h) : 0);
        float dx = x < d.x ? d.x - x : (x > d.x + d.w ? x - (d.x + d.w) : 0);
        float dist = dy * 8 + dx;          // satir uyumu daha onemli
        if (dist < bestD) { bestD = dist; best.item = idx; }
        if (dist == 0) break;
    }
    if (best.item < 0) return best;
    const DisplayItem& d = layout_->items[size_t(best.item)];
    if (x <= d.x) { best.ch = 0; return best; }
    if (x >= d.x + d.w) { best.ch = int(d.text.size()); return best; }
    // karakter konumu: onek genislikleriyle
    float prevX = d.x;
    for (size_t c = 1; c <= d.text.size() && c < 4000; c++) {
        float cx = CharX(d, int(c));
        if (x < (prevX + cx) / 2) { best.ch = int(c) - 1; return best; }
        prevX = cx;
    }
    best.ch = int(d.text.size());
    return best;
}

int PageView::Find(const std::wstring& query, int dir) {
    BuildTextIndex();
    std::wstring q;
    for (wchar_t c : query) q += wchar_t(towlower(c));
    bool same = (q == findQuery_) && dir != 0;
    findQuery_ = q;
    if (!same || dir == 2) {
        matches_.clear();
        if (!q.empty()) {
            size_t pos = 0;
            while ((pos = stream_.find(q, pos)) != std::wstring::npos && matches_.size() < 5000) {
                size_t e = pos + q.size() - 1;
                // eslesmenin ilk ve son gercek karakteri
                size_t s0 = pos, e0 = e;
                while (s0 < e && streamMap_[s0].first < 0) s0++;
                while (e0 > s0 && streamMap_[e0].first < 0) e0--;
                if (streamMap_[s0].first >= 0 && streamMap_[e0].first >= 0) {
                    Range r;
                    r.a = {streamMap_[s0].first, streamMap_[s0].second};
                    r.b = {streamMap_[e0].first, streamMap_[e0].second + 1};
                    matches_.push_back(r);
                }
                pos += std::max<size_t>(1, q.size());
            }
        }
        if (dir == 2) { InvalidateRect(hwnd_, nullptr, FALSE); return int(matches_.size()); }
        findCur_ = -1;
        // gorunur alanin ustunden sonraki ilk eslesme
        for (size_t i = 0; i < matches_.size(); i++) {
            if (layout_->items[size_t(matches_[i].a.item)].y >= scroll_ - 1) { findCur_ = int(i); break; }
        }
        if (findCur_ < 0 && !matches_.empty()) findCur_ = 0;
    } else if (!matches_.empty()) {
        int n = int(matches_.size());
        findCur_ = ((findCur_ < 0 ? 0 : findCur_ + dir) % n + n) % n;
    }
    if (findCur_ >= 0 && size_t(findCur_) < matches_.size()) {
        const DisplayItem& d = layout_->items[size_t(matches_[size_t(findCur_)].a.item)];
        float vh = ViewH();
        if (d.y < scroll_ + 20 || d.y + d.h > scroll_ + vh - 20) ScrollSmooth(d.y - vh * 0.35f);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return int(matches_.size());
}

void PageView::ClearFind() {
    findQuery_.clear();
    matches_.clear();
    findCur_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PageView::ClearSelection() {
    if (selA_.item < 0 && selB_.item < 0) return;
    selA_ = TextPos();
    selB_ = TextPos();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PageView::SelectAll() {
    BuildTextIndex();
    if (textItems_.empty()) return;
    selA_ = {textItems_.front(), 0};
    selB_ = {textItems_.back(), int(layout_->items[size_t(textItems_.back())].text.size())};
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring PageView::SelectedText() const {
    if (!HasSelection() || selA_.item >= int(itemOrder_.size()) || selB_.item >= int(itemOrder_.size())) return L"";
    TextPos a = selA_, b = selB_;
    int oa = itemOrder_[size_t(a.item)], ob = itemOrder_[size_t(b.item)];
    if (oa < 0 || ob < 0) return L"";
    if (oa > ob || (oa == ob && a.ch > b.ch)) { std::swap(a, b); std::swap(oa, ob); }
    std::wstring out;
    const DisplayItem* prev = nullptr;
    for (int o = oa; o <= ob && out.size() < 8000000; o++) {
        const DisplayItem& d = layout_->items[size_t(textItems_[size_t(o)])];
        int c0 = o == oa ? a.ch : 0;
        int c1 = o == ob ? b.ch : int(d.text.size());
        if (prev) {
            bool sameLine = std::fabs((prev->y + prev->h / 2) - (d.y + d.h / 2)) < std::max(prev->h, d.h) * 0.5f;
            bool touching = sameLine && d.x <= prev->x + prev->w + 1.5f && d.x >= prev->x;
            if (!sameLine) out += (d.y - (prev->y + prev->h) > prev->h * 0.6f) ? L"\r\n\r\n" : L"\r\n";
            else if (!touching && !out.empty() && out.back() != L' ' && d.text[0] != L' ') out += L' ';
        }
        if (c1 > c0) out += d.text.substr(size_t(c0), size_t(c1 - c0));
        prev = &d;
    }
    return out;
}

void PageView::DrawRange(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const Range& r, uint32_t color, float top, float bottom) {
    if (r.a.item < 0 || r.b.item < 0 || size_t(r.a.item) >= itemOrder_.size() || size_t(r.b.item) >= itemOrder_.size()) return;
    TextPos a = r.a, b = r.b;
    int oa = itemOrder_[size_t(a.item)], ob = itemOrder_[size_t(b.item)];
    if (oa < 0 || ob < 0) return;
    if (oa > ob || (oa == ob && a.ch > b.ch)) { std::swap(a, b); std::swap(oa, ob); }
    brush->SetColor(Col(color));
    for (int o = oa; o <= ob; o++) {
        const DisplayItem& d = layout_->items[size_t(textItems_[size_t(o)])];
        if (d.kind == DisplayItem::Shadow ? (d.y - 2 * d.blur > bottom || d.y + d.h + 2 * d.blur < top) : (d.y > bottom || d.y + d.h < top)) continue;
        float x0 = o == oa ? CharX(d, a.ch) : d.x;
        float x1 = o == ob ? CharX(d, b.ch) : d.x + d.w;
        // ayni satirdaki bir sonraki ogeye kadar bosluk da secili gorunsun
        if (o < ob) {
            const DisplayItem& n = layout_->items[size_t(textItems_[size_t(o + 1)])];
            if (std::fabs(n.y - d.y) < 2 && n.x > x1 && n.x - x1 < 30) x1 = n.x;
        }
        if (x1 > x0) rt->FillRectangle(D2D1::RectF(x0, d.y, x1, d.y + d.h), brush);
    }
}

void PageView::UpdateScrollBar() {
    SCROLLINFO si{sizeof(si)};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = int(std::ceil(layout_->height));
    si.nPage = UINT(std::max(1.f, ViewH()));
    si.nPos = int(scroll_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void PageView::ScrollSmooth(float y) {
    float maxS = std::max(0.f, layout_->height - ViewH());
    target_ = std::max(0.f, std::min(y, maxS));
    if (std::fabs(target_ - scroll_) < 1.f) { ScrollTo(target_); return; }
    if (!animating_) { animating_ = true; SetTimer(hwnd_, 77, 12, nullptr); }
}

void PageView::ScrollTo(float y) {
    float maxS = std::max(0.f, layout_->height - ViewH());
    scroll_ = std::max(0.f, std::min(y, maxS));
    target_ = scroll_;
    if (onScrolled) onScrolled(scroll_);
    if (animating_) { animating_ = false; KillTimer(hwnd_, 77); }
    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

const DisplayItem* PageView::Hit(int px, int py) const {
    float x = float(px) * 96.f / dpi_ / zoom_;
    float y = float(py) * 96.f / dpi_ / zoom_ + scroll_;
    for (size_t i = layout_->items.size(); i-- > 0;) {
        const DisplayItem& d = layout_->items[i];
        if (d.link < 0 && d.node < 0) continue;
        if (x >= d.x && x <= d.x + d.w && y >= d.y && y <= d.y + std::max(d.h, 2.f)) return &d;
    }
    return nullptr;
}

// Resim cizimi: ekranda onbellekli bitmap, ekran goruntusunde gecici bitmap
void PageView::DrawImage(ID2D1RenderTarget* rt, const std::wstring& key, uint32_t tint, const D2D1_RECT_F& dst0, bool snapshot) {
    if (!images_) return;
    D2D1_RECT_F dst = dst0;
    if (IsVideoKey(key)) {
        // <video>: siyah zemin + en-boy oranini koruyarak sigdir (object-fit: contain)
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b))) { rt->FillRectangle(dst0, b); b->Release(); }
        float iw = 0, ih = 0;
        if (!images_->LookupByKey(key, iw, ih) || iw <= 0 || ih <= 0) return;
        float bw = dst0.right - dst0.left, bh = dst0.bottom - dst0.top;
        float sc = std::min(bw / iw, bh / ih);
        float w = iw * sc, h = ih * sc;
        dst.left = dst0.left + (bw - w) / 2;
        dst.top = dst0.top + (bh - h) / 2;
        dst.right = dst.left + w;
        dst.bottom = dst.top + h;
    }
    if (!snapshot) {
        if (ID2D1Bitmap* bmp = images_->Bitmap(rt, key, tint))
            rt->DrawBitmap(bmp, dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    UINT w = 0, h = 0;
    std::vector<uint8_t> pxv;
    if (!images_->Pixels(key, tint, w, h, pxv) || pxv.empty()) return;
    const std::vector<uint8_t>* px = &pxv;
    ID2D1Bitmap* tmp = nullptr;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(w, h), px->data(), w * 4, props, &tmp))) {
        rt->DrawBitmap(tmp, dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        tmp->Release();
    }
}

void PageView::InvalidateImageKey(const std::wstring& key) {
    if (!hwnd_) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    bool any = false;
    for (const DisplayItem& d : layout_->items) {
        if (d.kind != DisplayItem::Image || d.text != key) continue;
        RECT r;
        r.left = LONG(std::floor(d.x * zoom_)) - 1;
        r.right = LONG(std::ceil((d.x + d.w) * zoom_)) + 1;
        r.top = LONG(std::floor((d.y - scroll_) * zoom_)) - 1;
        r.bottom = LONG(std::ceil((d.y + d.h - scroll_) * zoom_)) + 1;
        if (r.bottom < 0 || r.top > cr.bottom) continue;
        InvalidateRect(hwnd_, &r, FALSE);
        any = true;
    }
    (void)any;
}

// CSS degradesi (linear/radial, repeating) -> Direct2D firca
void PageView::DrawGradient(ID2D1RenderTarget* rt, const DisplayItem& d) {
    const Gradient& g = *d.grad;
    if (d.w < 0.5f || d.h < 0.5f || g.stops.size() < 2) return;
    float cx = d.x + d.w / 2, cy = d.y + d.h / 2;
    // degrade cizgisi uzunlugu
    float dx = 0, dy = 1, len = d.h;
    if (g.radial) {
        len = std::sqrt(d.w * d.w + d.h * d.h) / 2;   // farthest-corner (cember yaricapi olcusu)
    } else {
        if (g.corner) {
            dx = float(g.cornerX) * d.h; dy = float(g.cornerY) * d.w;
            float n = std::sqrt(dx * dx + dy * dy);
            if (n < 1e-4f) { dx = 0; dy = 1; } else { dx /= n; dy /= n; }
        } else {
            float a = g.angle * 3.14159265f / 180.f;
            dx = std::sin(a); dy = -std::cos(a);
        }
        len = std::fabs(d.w * dx) + std::fabs(d.h * dy);
    }
    if (len < 0.01f) return;
    // durak konumlari (0..1, cizgi boyunca)
    size_t n = g.stops.size();
    std::vector<float> pos(n, -1.f);
    for (size_t i = 0; i < n; i++) {
        const GradStop& st = g.stops[i];
        if (st.unit == 1) pos[i] = st.pos;
        else if (st.unit == 2) pos[i] = st.pos / len;
    }
    if (pos[0] < -0.5f && g.stops[0].unit == 0) pos[0] = 0;
    if (g.stops[n - 1].unit == 0) pos[n - 1] = 1;
    float mx = pos[0];
    for (size_t i = 1; i < n; i++) {
        if (g.stops[i].unit == 0 && i + 1 < n) continue;
        if (pos[i] < mx) pos[i] = mx;
        mx = pos[i];
    }
    for (size_t i = 1; i + 1 < n; i++) {
        if (g.stops[i].unit != 0) continue;
        size_t j = i;
        while (j < n && g.stops[j].unit == 0 && j + 1 < n) j++;
        float a = pos[i - 1], b = pos[j];
        for (size_t k = i; k < j; k++) pos[k] = a + (b - a) * float(k - i + 1) / float(j - i + 1);
        i = j - 1;
    }
    std::vector<D2D1_GRADIENT_STOP> ds;
    float span = pos[n - 1] - pos[0];
    if (g.repeating && span > 1e-3f) {
        // tekrarlayan: donguyu 0..1 araligina elle yay (EXTEND_MODE_WRAP her surucude guvenilir degil)
        float start = pos[0] - std::ceil(pos[0] / span) * span;
        for (float base = start; base < 1.f && ds.size() < 1020; base += span)
            for (size_t i = 0; i < n; i++) {
                D2D1_GRADIENT_STOP st;
                st.position = base + (pos[i] - pos[0]);
                st.color = Col(g.stops[i].color);
                ds.push_back(st);
            }
        // 0 ve 1'e kirp: sinir disindaki duraklari renk enterpolasyonuyla kes
        std::vector<D2D1_GRADIENT_STOP> cut;
        for (size_t i = 0; i < ds.size(); i++) {
            float q = ds[i].position;
            if (q < 0) {
                if (i + 1 < ds.size() && ds[i + 1].position > 0) {
                    float t = (0 - q) / std::max(1e-6f, ds[i + 1].position - q);
                    D2D1_GRADIENT_STOP st; st.position = 0;
                    st.color.r = ds[i].color.r + (ds[i + 1].color.r - ds[i].color.r) * t;
                    st.color.g = ds[i].color.g + (ds[i + 1].color.g - ds[i].color.g) * t;
                    st.color.b = ds[i].color.b + (ds[i + 1].color.b - ds[i].color.b) * t;
                    st.color.a = ds[i].color.a + (ds[i + 1].color.a - ds[i].color.a) * t;
                    cut.push_back(st);
                }
                continue;
            }
            cut.push_back(ds[i]);
            if (q > 1) break;
        }
        ds.swap(cut);
    } else {
        ds.resize(n);
        for (size_t i = 0; i < n; i++) { ds[i].position = pos[i]; ds[i].color = Col(g.stops[i].color); }
    }
    if (ds.empty()) return;
    for (auto& st : ds) st.position = std::max(0.f, std::min(1.f, st.position));
    const float p1 = 1;
    ID2D1GradientStopCollection* col = nullptr;
    if (FAILED(rt->CreateGradientStopCollection(ds.data(), UINT32(ds.size()), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col)) || !col) return;
    ID2D1Brush* br = nullptr;
    D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(float(d.color >> 24) / 255.f);
    if (g.radial) {
        // elips: farthest-corner, kutu oranini korur
        float rx = d.w / 2 * 1.41421356f, ry = d.h / 2 * 1.41421356f;
        if (g.circle) rx = ry = len;
        ID2D1RadialGradientBrush* rb = nullptr;
        if (SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx * p1, ry * p1), bp, col, &rb)))
            br = rb;
    } else {
        ID2D1LinearGradientBrush* lb = nullptr;
        float sx = cx - dx * len / 2, sy = cy - dy * len / 2;
        D2D1_POINT_2F a = D2D1::Point2F(sx, sy);
        D2D1_POINT_2F b = D2D1::Point2F(sx + dx * len, sy + dy * len);
        if (SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), bp, col, &lb)))
            br = lb;
    }
    if (br) {
        D2D1_RECT_F r = D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h);
        if (d.radius > 0.4f) rt->FillRoundedRectangle(D2D1::RoundedRect(r, d.radius, d.radius), br);
        else rt->FillRectangle(r, br);
        br->Release();
    }
    col->Release();
}

// box-shadow: bulaniklik, ust uste binen genisleyen yuvarlak dikdortgenlerle yaklasik.
// Katman ofsetleri Gauss dagilimina (sigma = blur/2) gore secilir, boylece kenar gecisi
// CSS'teki gibi yumusak olur. Saydam kutularda kutunun ici dort serit kirpmasiyla dislanir.
static float InvNormal(float p) {
    // Acklam yaklasimi (kisaltilmis): standart normal dagilimin ters CDF'i
    if (p <= 0) return -4; if (p >= 1) return 4;
    float q = p < 0.5f ? p : 1 - p;
    float t = std::sqrt(-2.f * std::log(q));
    float x = t - (2.515517f + 0.802853f * t + 0.010328f * t * t) / (1.f + 1.432788f * t + 0.189269f * t * t + 0.001308f * t * t * t);
    return p < 0.5f ? -x : x;
}

void PageView::DrawShadowShape(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const DisplayItem& d) {
    float A = float(d.color >> 24) / 255.f;
    uint32_t rgb = d.color & 0xFFFFFF;
    if (d.blur < 0.5f) {
        brush->SetColor(Col(d.color));
        D2D1_RECT_F r = D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h);
        if (d.radius > 0.4f) rt->FillRoundedRectangle(D2D1::RoundedRect(r, d.radius, d.radius), brush);
        else rt->FillRectangle(r, brush);
        return;
    }
    int K = int(std::min(20.f, std::max(4.f, d.blur / 1.2f)));
    float sigma = d.blur / 2;
    for (int i = 0; i < K; i++) {
        // dis katmanlar once: ilk i+1 katmanin toplami (i+1)/K * A olacak sekilde alfa (dogrusal birikim)
        float T0 = A * float(i) / float(K), T1 = A * float(i + 1) / float(K);
        float a = (T1 - T0) / std::max(1e-4f, 1.f - T0);
        brush->SetColor(Col((uint32_t(std::min(255.f, a * 255.f + 0.5f)) << 24) | rgb));
        // i. katman: kenari -e kadar (dis -> ic); kapsam orani Gauss CDF'ini izler
        float e = -sigma * InvNormal((float(i) + 0.5f) / float(K));
        float x0 = d.x - e, y0 = d.y - e, x1 = d.x + d.w + e, y1 = d.y + d.h + e;
        if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) continue;
        float rr = std::max(0.f, d.radius + e);
        rr = std::min(rr, std::min(x1 - x0, y1 - y0) / 2);
        if (rr > 0.4f) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), rr, rr), brush);
        else rt->FillRectangle(D2D1::RectF(x0, y0, x1, y1), brush);
    }
}

void PageView::DrawShadow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const DisplayItem& d) {
    float A = float(d.color >> 24) / 255.f;
    if (A <= 0.003f || d.w <= 0 || d.h <= 0) return;
    if (d.tint) { DrawShadowShape(rt, brush, d); return; }
    // saydam kutu: golge kutunun altinda gorunmemeli -> kutu disindaki 4 seride kirparak ciz
    const float B = 1e5f;
    float l = d.ix, t = d.iy, r = d.ix + d.iw, b = d.iy + d.ih;
    D2D1_RECT_F strips[4] = {
        D2D1::RectF(-B, -B, B, t), D2D1::RectF(-B, b, B, B),
        D2D1::RectF(-B, t, l, b), D2D1::RectF(r, t, B, b) };
    for (auto& c : strips) {
        if (c.right <= c.left || c.bottom <= c.top) continue;
        rt->PushAxisAlignedClip(c, D2D1_ANTIALIAS_MODE_ALIASED);
        DrawShadowShape(rt, brush, d);
        rt->PopAxisAlignedClip();
    }
}

void PageView::DrawItems(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float top, float bottom, bool snapshot) {
    // ENABLE_COLOR_FONT (4): renkli emoji. Desteklenmeyen surucude otomatik kapatilir.
    const D2D1_DRAW_TEXT_OPTIONS textOpts = (colorFont_ && !snapshot) ? D2D1_DRAW_TEXT_OPTIONS(4) : D2D1_DRAW_TEXT_OPTIONS_NONE;
    const DisplayItem* focusText = nullptr;
    int clipDepth = 0;
    for (const DisplayItem& d : layout_->items) {
        // kirpma ogeleri gorunur alan disinda olsa bile islenmeli (yigin dengesi)
        if (d.kind == DisplayItem::ClipPush) {
            rt->PushAxisAlignedClip(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), D2D1_ANTIALIAS_MODE_ALIASED);
            clipDepth++;
            continue;
        }
        if (d.kind == DisplayItem::ClipPop) {
            if (clipDepth > 0) { rt->PopAxisAlignedClip(); clipDepth--; }
            continue;
        }
        if (d.y > bottom || d.y + d.h < top) continue;
        uint32_t c = d.color;
        if (!snapshot && d.link >= 0 && d.link == hoverLink_ && d.kind != DisplayItem::Fill && d.kind != DisplayItem::Image && d.kind != DisplayItem::BgImage) c = 0xFFC2410C;
        brush->SetColor(Col(c));
        switch (d.kind) {
        case DisplayItem::Fill:
        case DisplayItem::HLine:
            if (!(c >> 24)) break;   // gorunmez isabet alani
            if (d.radius > 0.4f)
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), d.radius, d.radius), brush);
            else
                rt->FillRectangle(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), brush);
            break;
        case DisplayItem::Stroke:
            if (d.role == DisplayItem::None) {
                float h2 = d.sw / 2;
                D2D1_RECT_F r = D2D1::RectF(d.x + h2, d.y + h2, d.x + d.w - h2, d.y + d.h - h2);
                if (d.radius > 0.4f) rt->DrawRoundedRectangle(D2D1::RoundedRect(r, std::max(0.f, d.radius - h2), std::max(0.f, d.radius - h2)), brush, d.sw);
                else rt->DrawRectangle(r, brush, d.sw);
            } else if (!snapshot && d.node >= 0 && d.node == focusNode_) { brush->SetColor(Col(0xFF2563EB)); rt->DrawRectangle(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), brush, 2.f); }
            else rt->DrawRectangle(D2D1::RectF(d.x + 0.5f, d.y + 0.5f, d.x + d.w - 0.5f, d.y + d.h - 0.5f), brush, 1.f);
            break;
        case DisplayItem::Image:
            if (d.radius > 0.4f) {
                // yuvarlak resim (avatar): yuvarlatilmis geometriye kirp
                ID2D1RoundedRectangleGeometry* g = nullptr;
                ID2D1Layer* layer = nullptr;
                if (d2d_ && SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), d.radius, d.radius), &g)) &&
                    SUCCEEDED(rt->CreateLayer(nullptr, &layer))) {
                    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), layer);
                    DrawImage(rt, d.text, d.tint, D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), snapshot);
                    rt->PopLayer();
                } else {
                    DrawImage(rt, d.text, d.tint, D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), snapshot);
                }
                if (layer) layer->Release();
                if (g) g->Release();
            } else {
                DrawImage(rt, d.text, d.tint, D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), snapshot);
            }
            break;
        case DisplayItem::BgImage: {
            if (d.iw < 0.5f || d.ih < 0.5f) break;
            rt->PushAxisAlignedClip(D2D1::RectF(d.x, d.y, d.x + d.w, d.y + d.h), D2D1_ANTIALIAS_MODE_ALIASED);
            bool rx = d.repeat == 0 || d.repeat == 2, ry = d.repeat == 0 || d.repeat == 3;
            float x0 = d.ix, y0 = d.iy;
            if (rx) while (x0 > d.x) x0 -= d.iw;
            if (ry) while (y0 > d.y) y0 -= d.ih;
            float visTop = std::max(d.y, top), visBot = std::min(d.y + d.h, bottom);
            if (ry && y0 + d.ih < visTop) y0 += std::floor((visTop - y0) / d.ih) * d.ih;
            int tiles = 0;
            for (float yy = y0; (ry ? yy < visBot : yy == y0) && tiles < 4000; yy += d.ih) {
                for (float xx = x0; (rx ? xx < d.x + d.w : xx == x0) && tiles < 4000; xx += d.iw) {
                    DrawImage(rt, d.text, d.tint, D2D1::RectF(xx, yy, xx + d.iw, yy + d.ih), snapshot);
                    tiles++;
                    if (!rx) break;
                }
                if (!ry) break;
            }
            rt->PopAxisAlignedClip();
            break;
        }
        case DisplayItem::Gradient:
            if (d.grad) DrawGradient(rt, d);
            break;
        case DisplayItem::Shadow:
            DrawShadow(rt, brush, d);
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
        default:
            break;
        }
    }
    while (clipDepth-- > 0) rt->PopAxisAlignedClip();
    if (!snapshot && (!matches_.empty() || HasSelection())) {
        BuildTextIndex();
        for (size_t i = 0; i < matches_.size(); i++)
            DrawRange(rt, brush, matches_[i], int(i) == findCur_ ? 0x99F97316 : 0x77FACC15, top, bottom);
        if (HasSelection()) DrawRange(rt, brush, Range{selA_, selB_}, 0x553B82F6, top, bottom);
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
        rt_->Clear(Col(layout_->background));
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
    UINT H = UINT(std::min(16384.f, std::max(100.f, layout_->height)));
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
        rt->Clear(Col(layout_->background));
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
    case WM_TIMER:
        if (w == 77) {
            float maxS = std::max(0.f, layout_->height - ViewH());
            target_ = std::max(0.f, std::min(target_, maxS));
            float d = target_ - scroll_;
            if (std::fabs(d) < 0.6f) {
                float t = target_;
                ScrollTo(t);
            } else {
                scroll_ += d * 0.28f;
                if (onScrolled) onScrolled(scroll_);
                UpdateScrollBar();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;
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
        case SB_BOTTOM: y = layout_->height; break;
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
        if (lines == WHEEL_PAGESCROLL) lines = UINT(ViewH() / 38.f);
        ScrollBy(-float(delta) / WHEEL_DELTA * float(lines) * 38.f);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (focusNode_ >= 0 && onKey && onKey(UINT(w))) return 0;
        if (focusNode_ >= 0 && (w == VK_LEFT || w == VK_RIGHT || w == VK_BACK || w == VK_SPACE || w == VK_HOME || w == VK_END)) return 0;
        switch (w) {
        case VK_DOWN: ScrollBy(48); return 0;
        case VK_UP: ScrollBy(-48); return 0;
        case VK_NEXT: case VK_SPACE: ScrollBy(ViewH() * 0.88f * ((GetKeyState(VK_SHIFT) < 0 && w == VK_SPACE) ? -1 : 1)); return 0;
        case VK_PRIOR: ScrollBy(-ViewH() * 0.88f); return 0;
        case VK_HOME: ScrollSmooth(0); return 0;
        case VK_END: ScrollSmooth(layout_->height); return 0;
        case 'A': if (GetKeyState(VK_CONTROL) < 0) { SelectAll(); return 0; } break;
        case 'C':
        case VK_INSERT:
            if (GetKeyState(VK_CONTROL) < 0 && HasSelection()) {
                std::wstring t = SelectedText();
                if (!t.empty() && OpenClipboard(hwnd_)) {
                    EmptyClipboard();
                    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
                        if (void* mem = GlobalLock(g)) {
                            memcpy(mem, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
                            GlobalUnlock(g);
                            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
                        } else GlobalFree(g);
                    }
                    CloseClipboard();
                }
                return 0;
            }
            break;
        }
        break;
    case WM_CHAR:
        if (focusNode_ >= 0 && onChar) { onChar(wchar_t(w)); return 0; }
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        SetCapture(hwnd_);
        mouseDown_ = true;
        selecting_ = false;
        downX_ = float(GET_X_LPARAM(l));
        downY_ = float(GET_Y_LPARAM(l));
        if (GetKeyState(VK_SHIFT) < 0 && selA_.item >= 0) {
            BuildTextIndex();
            selB_ = HitText(downX_ * 96.f / dpi_ / zoom_, downY_ * 96.f / dpi_ / zoom_ + scroll_);
            selecting_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        // cift tik: kelimeyi sec
        BuildTextIndex();
        TextPos p = HitText(float(GET_X_LPARAM(l)) * 96.f / dpi_ / zoom_, float(GET_Y_LPARAM(l)) * 96.f / dpi_ / zoom_ + scroll_);
        if (p.item >= 0 && focusNode_ < 0) {
            const std::wstring& t = layout_->items[size_t(p.item)].text;
            int a = std::min(p.ch, int(t.size())), b = a;
            while (a > 0 && iswalnum(t[size_t(a - 1)])) a--;
            while (b < int(t.size()) && iswalnum(t[size_t(b)])) b++;
            if (b > a) { selA_ = {p.item, a}; selB_ = {p.item, b}; suppressClick_ = true; InvalidateRect(hwnd_, nullptr, FALSE); }
        }
        SetCapture(hwnd_);
        mouseDown_ = true;
        return 0;
    }
    case WM_MBUTTONDOWN:
        SetFocus(hwnd_);
        return 0;
    case WM_MBUTTONUP: {
        const DisplayItem* d = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (d && d->link >= 0 && onMiddleClick) onMiddleClick(d->link);
        return 0;
    }
    case WM_RBUTTONUP: {
        const DisplayItem* d = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(hwnd_, &pt);
        if (onContext) onContext(d ? d->link : -1, d ? d->node : -1, pt);
        return 0;
    }
    case WM_CAPTURECHANGED:
        mouseDown_ = false;
        return 0;
    case WM_LBUTTONUP: {
        bool wasDown = mouseDown_;
        mouseDown_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (selecting_) { selecting_ = false; return 0; }
        if (suppressClick_) { suppressClick_ = false; return 0; }
        if (!wasDown) return 0;
        ClearSelection();
        const DisplayItem* d = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        float docX = float(GET_X_LPARAM(l)) * 96.f / dpi_ / zoom_, docY = float(GET_Y_LPARAM(l)) * 96.f / dpi_ / zoom_ + scroll_;
        if (onClick) onClick(d ? d->node : -1, d ? d->link : -1, (w & MK_CONTROL) != 0, docX, docY);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (mouseDown_ && (w & MK_LBUTTON) && focusNode_ < 0) {
            float mx = float(GET_X_LPARAM(l)), my = float(GET_Y_LPARAM(l));
            if (!selecting_ && (std::fabs(mx - downX_) > 4 || std::fabs(my - downY_) > 4)) {
                BuildTextIndex();
                selA_ = HitText(downX_ * 96.f / dpi_ / zoom_, downY_ * 96.f / dpi_ / zoom_ + scroll_);
                selecting_ = selA_.item >= 0;
            }
            if (selecting_) {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (my < 0) ScrollTo(scroll_ - 24);
                else if (my > float(rc.bottom)) ScrollTo(scroll_ + 24);
                selB_ = HitText(mx * 96.f / dpi_ / zoom_, my * 96.f / dpi_ / zoom_ + scroll_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        const DisplayItem* d = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        int lk = d ? d->link : -1;
        hoverRole_ = d ? int(d->role) : 0;
        hoverCursor_ = d ? int(d->cursor) : 0;
        if (lk != hoverLink_) {
            hoverLink_ = lk;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onHover) onHover(lk >= 0 && size_t(lk) < layout_->links.size() ? layout_->links[size_t(lk)] : L"");
            TRACKMOUSEEVENT te{sizeof(te), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&te);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        hoverCursor_ = 0;
        if (hoverLink_ >= 0) {
            hoverLink_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onHover) onHover(L"");
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            LPCWSTR cur = IDC_ARROW;
            if (selecting_) { SetCursor(LoadCursor(nullptr, IDC_IBEAM)); return TRUE; }
            if (hoverLink_ >= 0 || hoverRole_ == DisplayItem::Button || hoverRole_ == DisplayItem::Check || hoverCursor_ == 1) cur = IDC_HAND;
            else if (hoverRole_ == DisplayItem::TextInput || hoverCursor_ == 2) cur = IDC_IBEAM;
            else if (hoverCursor_ == 3) cur = IDC_NO;
            SetCursor(LoadCursor(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (animating_) KillTimer(hwnd_, 77);
        layout_ = std::make_shared<PageLayout>();
        ReleaseTarget();
        images_.reset();
        tm_.reset();
        SafeRelease(dw_);
        SafeRelease(d2d_);
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

} // namespace mer
