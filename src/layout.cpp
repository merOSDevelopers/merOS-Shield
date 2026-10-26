// merOS Shield - MerEngine : yerleşim (layout) motoru
// Blok akisi, satir ici metin (kelime kaydirma, hizalama, taban cizgisi), float'lar,
// inline-block kutular, konumlandirma (relative/absolute/fixed), flexbox, grid,
// tablolar, liste isaretleri, ::before/::after sozde ogeleri.
#include "layout.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace mer {

namespace {

struct Piece {
    std::wstring text;
    int font = -1;
    uint32_t color = 0, bg = 0;
    int link = -1;
    bool underline = false, strike = false;
    bool spaceBefore = false, canBreak = false, brk = false;
    bool isImage = false;
    bool atom = false;            // inline-block kutu (items hazir)
    bool invisible = false;       // visibility:hidden metin (yer kaplar, cizilmez)
    DisplayItem::Role role = DisplayItem::None;
    int node = -1;
    float w = 0, lh = 0;
    float ascent = 0;             // atom/resim: taban cizgisi ustundeki yukseklik
    float shift = 0;              // sub/super kaydirmasi (+ asagi)
    uint8_t valign = 0;           // 0 taban, 1 orta
    int anchor = -1;              // absolute oge statik konum isareti (deferStore_ indeksi)
    size_t deferFrom = 0, deferTo = 0;  // atom icinde ertelenen absolute ogeler
    float imgH = 0;
    uint8_t styledFrame = 0;      // form denetimi: 0 varsayilan cerceve, 1 hic (Box ciziyor), 2 CSS cercevesi
    uint32_t frameColor = 0;
    float frameW = 0, frameRadius = 0;
    std::vector<DisplayItem> items;
    FontMetrics fm;
};

struct Placed { size_t idx; float x; };

float Collapse(float a, float b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a < 0 && b < 0) return std::min(a, b);
    return a + b;
}

inline float R(const Len& l, float base) { return ResolveLen(l, base); }

// Kotu niyetli/asiri buyuk sayfalara karsi toplam yerlesim isi siniri
constexpr size_t kMaxWork = 6000000;

struct BoxOpts {
    float forcedW = -1;   // kenarlik kutusu genisligi (flex/grid/float/atom)
    float forcedH = -1;   // kenarlik kutusu yuksekligi (stretch)
    bool bfc = false;     // yeni blok bicimlendirme baglami
    bool shrink = false;  // auto genislikte icerige gore daral
};

class Layouter {
public:
    Layouter(const Document& d, const std::vector<Style>& s, ITextMeasurer& tm, PageLayout& out, IImageProvider* img, float vh)
        : doc_(d), S_(s), tm_(tm), out_(out), img_(img), memo_(s.size(), int8_t(-1)),
          maxMemo_(s.size(), -1.f), minMemo_(s.size(), -1.f), vh_(vh) {}

    void Run(float vw) {
        out_.items.clear();
        out_.links.clear();
        out_.width = vw;
        vw_ = vw;
        if (vh_ <= 0) vh_ = std::max(400.f, vw * 0.6f);
        rootCB_ = CB{0, 0, vw, -1, {}};
        cbStack_.assign(1, &rootCB_);
        fixedCB_ = CB{0, 0, vw, vh_, {}};
        float h = BlockChildren(doc_.root.get(), 0, 0, vw, 0);
        h = std::max(h, FloatsBottom() );
        rootCB_.h = vh_;
        ProcessAbs(rootCB_, 0);
        ProcessAbs(fixedCB_, 0);
        float maxBottom = h;
        for (auto& it : out_.items) if (it.kind != DisplayItem::ClipPop && it.kind != DisplayItem::ClipPush) maxBottom = std::max(maxBottom, std::min(it.y + it.h, h + 20000.f));
        out_.height = std::max(h, maxBottom);
        if (hasZ_)
            std::stable_sort(out_.items.begin(), out_.items.end(), [](const DisplayItem& a, const DisplayItem& b) { return a.zIndex < b.zIndex; });
        // Sayfa arka plani: html, yoksa body
        uint32_t bg = 0;
        for (auto& k : doc_.root->kids) {
            if (k->kind != Node::Elem || k->name != L"html") continue;
            bg = St(k.get()).bg;
            if (!(bg >> 24))
                for (auto& b : k->kids)
                    if (b->kind == Node::Elem && b->name == L"body") bg = St(b.get()).bg;
        }
        if (bg >> 24) out_.background = 0xFF000000 | bg;
    }

private:
    const Document& doc_;
    const std::vector<Style>& S_;
    ITextMeasurer& tm_;
    PageLayout& out_;
    IImageProvider* img_;
    std::vector<int8_t> memo_;
    std::vector<float> maxMemo_, minMemo_;
    std::wstring marker_;
    const Style* markerStyle_ = nullptr;
    float curWidth_ = 800;
    float forceInputW_ = 0;
    bool inFlexItem_ = false;
    bool controlBoxed_ = false;   // form denetimi Box() icinden cizilyor (CSS kenarligi/arka plani Box'ta)
    float vw_ = 1280, vh_ = 800, curDefiniteH_ = -1.f;
    bool hasZ_ = false;
    size_t work_ = 0;             // kotu niyetli sayfalara karsi is siniri

    // ---- sozde ogeler (::before / ::after) ----
    std::deque<std::unique_ptr<Node>> fake_;
    std::vector<const Style*> fakeStyle_;
    std::unordered_map<const Node*, std::pair<const Node*, const Node*>> pseudoNodes_;

    // ---- float'lar (mutlak koordinat; gecerli blok bicimlendirme baglami) ----
    struct FloatBox { float x, y, w, h; uint8_t side; };
    std::vector<FloatBox> floats_;
    float lastFloatTop_ = -1e9f;

    // ---- absolute/fixed (kapsayan blok) ----
    struct Deferred { const Node* n; float sx, sy; size_t itemIndex; bool done; };
    struct CB { float x, y, w, h; std::vector<size_t> abs; };
    std::deque<Deferred> deferStore_;
    std::vector<CB*> cbStack_;
    CB rootCB_{0, 0, 0, 0, {}}, fixedCB_{0, 0, 0, 0, {}};

    static bool IsControl(const Node* n) {
        return n->kind == Node::Elem && (n->name == L"input" || n->name == L"textarea" || n->name == L"select");
    }
    static bool IsReplaced(const Node* n) {
        return n->kind == Node::Elem && (n->name == L"img" || n->name == L"svg" || n->name == L"video");
    }

    const Style& St(const Node* n) const {
        static const Style kNone = [] { Style s; s.display = Style::None; return s; }();
        if (n->id >= 0 && size_t(n->id) < S_.size()) return S_[size_t(n->id)];
        size_t f = size_t(n->id) - S_.size();
        if (n->id >= 0 && f < fakeStyle_.size()) return *fakeStyle_[f];
        return kNone;
    }
    // Sozde oge kimlikleri gercek elemana cevrilir (tiklama vb.)
    int RealId(const Node* n) const {
        while (n && ((n->id >= 0 && size_t(n->id) >= S_.size()) || n->kind == Node::Shadow)) n = n->kind == Node::Shadow ? n->host : n->parent;
        return n ? n->id : -1;
    }

    const Node* MakePseudo(const Node* host, const std::shared_ptr<Style>& st) {
        if (!st || fake_.size() > 50000) return nullptr;
        std::unique_ptr<Node> e(new Node());
        e->kind = Node::Elem;
        e->name = L"::pseudo";
        e->parent = const_cast<Node*>(host);
        e->id = int(S_.size() + fakeStyle_.size());
        fakeStyle_.push_back(st.get());
        if (st->content && !st->content->empty()) {
            std::unique_ptr<Node> t(new Node());
            t->kind = Node::Text;
            t->text = *st->content;
            t->parent = e.get();
            t->id = e->id;
            e->kids.push_back(std::move(t));
        }
        fake_.push_back(std::move(e));
        return fake_.back().get();
    }

    std::pair<const Node*, const Node*> Pseudos(const Node* n) {
        const Style& s = St(n);
        if (!s.pseudo) return {nullptr, nullptr};
        auto it = pseudoNodes_.find(n);
        if (it != pseudoNodes_.end()) return it->second;
        auto r = std::make_pair(MakePseudo(n, s.pseudo->before), MakePseudo(n, s.pseudo->after));
        pseudoNodes_[n] = r;
        return r;
    }

    // Cocuklari gez: ::before, cocuklar (display:contents acilir), ::after
    template <class F> void ForKids(const Node* n, F&& f, int depth = 0) {
        if (depth > 24) return;
        auto ps = n->kind == Node::Elem ? Pseudos(n) : std::pair<const Node*, const Node*>(nullptr, nullptr);
        if (ps.first) f(ps.first);
        auto one = [&](const Node* k) {
            if (k->kind == Node::Elem && St(k).display == Style::Contents && !IsReplaced(k)) { ForKids(k, f, depth + 1); return; }
            f(k);
        };
        if (HasFlatOverride(n)) {
            // shadow host / slot: cizim (flat) agaci
            std::vector<const Node*> flat;
            FlatKids(n, flat);
            for (const Node* k : flat) one(k);
        } else {
            for (auto& kp : n->kids) one(kp.get());
        }
        if (ps.second) f(ps.second);
    }

    static bool OutOfFlow(const Style& s) { return s.position == 2 || s.position == 3; }

    bool HasBlockDesc(const Node* n, size_t depth) {
        bool cacheable = n->id >= 0 && size_t(n->id) < memo_.size();
        if (cacheable && memo_[size_t(n->id)] >= 0) return memo_[size_t(n->id)] == 1;
        bool r = false;
        if (depth < kMaxDepth && !IsReplaced(n)) {
            ForKids(n, [&](const Node* k) {
                if (r || k->kind != Node::Elem) return;
                const Style& ks = St(k);
                if (ks.display == Style::None || OutOfFlow(ks)) return;
                if (ks.inlineBlock || IsReplaced(k) || IsControl(k)) return;
                if (ks.floatSide || ks.display != Style::Inline || HasBlockDesc(k, depth + 1)) r = true;
            });
        }
        if (cacheable) memo_[size_t(n->id)] = r ? 1 : 0;
        return r;
    }

    bool IsBlockLevel(const Node* n, size_t depth) {
        if (n->kind != Node::Elem) return false;
        const Style& s = St(n);
        if (s.display == Style::None) return false;
        if (s.floatSide) return true;
        if (s.inlineBlock) return false;
        if (s.display != Style::Inline) return true;
        if (IsReplaced(n) || IsControl(n)) return false;
        return HasBlockDesc(n, depth);
    }

    bool IsBfcRoot(const Node* n, const Style& s) const {
        return s.bfc || s.display == Style::Table || s.flex || s.gridCols || s.inlineBlock || OutOfFlow(s) || s.floatSide || IsControl(n) || IsReplaced(n);
    }

    size_t Reserve() {
        out_.items.emplace_back();
        out_.items.back().kind = DisplayItem::Fill;
        return out_.items.size() - 1;
    }

    void FillRect(float x, float y, float w, float h, uint32_t c, float radius = 0) {
        if (w <= 0 || h <= 0 || !(c >> 24)) return;
        DisplayItem d;
        d.kind = DisplayItem::Fill;
        d.x = x; d.y = y; d.w = w; d.h = h; d.color = c; d.radius = radius;
        out_.items.push_back(std::move(d));
    }

    void Borders(const Style& s, float x, float y, float w, float h, float radius) {
        bool uniform = s.bw[0] > 0 && s.bw[0] == s.bw[1] && s.bw[1] == s.bw[2] && s.bw[2] == s.bw[3] &&
                       s.bc[0] == s.bc[1] && s.bc[1] == s.bc[2] && s.bc[2] == s.bc[3];
        if (uniform && radius > 0) {
            if (!(s.bc[0] >> 24)) return;
            DisplayItem d;
            d.kind = DisplayItem::Stroke;
            d.x = x; d.y = y; d.w = w; d.h = h; d.color = s.bc[0]; d.radius = radius; d.sw = s.bw[0];
            out_.items.push_back(std::move(d));
            return;
        }
        if (s.bw[0] > 0) FillRect(x, y, w, s.bw[0], s.bc[0]);
        if (s.bw[2] > 0) FillRect(x, y + h - s.bw[2], w, s.bw[2], s.bc[2]);
        if (s.bw[3] > 0) FillRect(x, y, s.bw[3], h, s.bc[3]);
        if (s.bw[1] > 0) FillRect(x + w - s.bw[1], y, s.bw[1], h, s.bc[1]);
    }

    // Oge araligini (ve icinde ertelenmis absolute statik konumlarini) kaydir
    void Shift(size_t st, size_t en, float dx, float dy) {
        if (std::fabs(dx) < 0.01f && std::fabs(dy) < 0.01f) return;
        for (size_t i = st; i < en && i < out_.items.size(); i++) {
            DisplayItem& d = out_.items[i];
            d.x += dx; d.y += dy;
            d.ix += dx; d.iy += dy;
        }
        for (auto& d : deferStore_) {
            if (d.done || d.itemIndex < st || d.itemIndex > en) continue;
            d.sx += dx; d.sy += dy;
        }
    }

    // Belirli bir item araligini sil ve o aralikta ertelenen absolute ogeleri iptal et (yeniden yerlesim icin)
    void Discard(size_t st, size_t deferMark) {
        if (st < out_.items.size()) out_.items.resize(st);
        for (size_t i = deferMark; i < deferStore_.size(); i++) deferStore_[i].done = true;
    }

    float FloatsBottom() const {
        float b = 0;
        for (auto& f : floats_) b = std::max(b, f.y + f.h);
        return b;
    }

    // [x0, x1] araliginda y..y+h yuksekliginde kullanilabilir yatay aralik
    void FloatAvail(float y, float h, float x0, float x1, float& l, float& r) const {
        l = x0; r = x1;
        for (auto& f : floats_) {
            if (f.y >= y + std::max(h, 0.5f) || f.y + f.h <= y) continue;
            if (f.x + f.w <= x0 || f.x >= x1) continue;
            if (f.side == 1) l = std::max(l, f.x + f.w);
            else r = std::min(r, f.x);
        }
    }
    // y'den sonra biten ilk float alt kenari (yoksa -1)
    float NextFloatBottom(float y, float x0, float x1) const {
        float best = -1;
        for (auto& f : floats_) {
            float b = f.y + f.h;
            if (b <= y + 0.01f || f.y > y + 0.01f) continue;
            if (f.x + f.w <= x0 || f.x >= x1) continue;
            if (best < 0 || b < best) best = b;
        }
        return best;
    }
    float ClearY(float y, uint8_t side) const {
        for (auto& f : floats_) if ((side == 3 || side == f.side)) y = std::max(y, f.y + f.h);
        return y;
    }

    // ---------------- Absolute / fixed ----------------

    void Defer(const Node* n, float sx, float sy) {
        const Style& s = St(n);
        deferStore_.push_back({n, sx, sy, out_.items.size(), false});
        size_t idx = deferStore_.size() - 1;
        if (s.position == 3) fixedCB_.abs.push_back(idx);
        else cbStack_.back()->abs.push_back(idx);
    }

    void ProcessAbs(CB& cb, size_t depth) {
        for (size_t k = 0; k < cb.abs.size(); k++) {
            Deferred d = deferStore_[cb.abs[k]];
            if (d.done) continue;
            deferStore_[cb.abs[k]].done = true;
            LayoutAbs(d, cb, depth);
        }
        cb.abs.clear();
    }

    void LayoutAbs(const Deferred& d, const CB& cb, size_t depth) {
        if (depth > kMaxDepth || ++work_ > kMaxWork) return;
        const Node* n = d.n;
        const Style& s = St(n);
        float cbh = cb.h >= 0 ? cb.h : vh_;
        bool hasL = s.inset[3].Def(), hasR = s.inset[1].Def(), hasT = s.inset[0].Def(), hasB = s.inset[2].Def();
        float l = R(s.inset[3], cb.w), r = R(s.inset[1], cb.w), t = R(s.inset[0], cbh), b = R(s.inset[2], cbh);
        float ml = R(s.m[3], cb.w), mr = R(s.m[1], cb.w), mt = R(s.m[0], cb.w), mb = R(s.m[2], cb.w);
        float extra = R(s.p[1], cb.w) + R(s.p[3], cb.w) + s.bw[1] + s.bw[3];
        float bw;
        if (s.width.Def()) bw = R(s.width, cb.w) + (s.borderBox ? 0 : extra);
        else if (hasL && hasR) bw = cb.w - l - r - ml - mr;
        else {
            float avail = cb.w - (hasL ? l : 0) - (hasR ? r : 0) - ml - mr;
            if (IsReplaced(n)) {
                float iw, ih;
                std::wstring key;
                bw = ImageSize(n, s, cb.w, iw, ih, key) ? iw + extra : extra;
            } else bw = std::min(ItemMaxContent(n, depth), std::max(avail, 0.f));
        }
        if (s.maxWidth.Def()) bw = std::min(bw, R(s.maxWidth, cb.w) + (s.borderBox ? 0 : extra));
        if (s.minWidth.Def()) bw = std::max(bw, R(s.minWidth, cb.w) + (s.borderBox ? 0 : extra));
        bw = std::max(bw, 0.f);
        // yatay konum (margin auto: ortala)
        float x;
        if (hasL && hasR && s.m[3].u == Len::Auto && s.m[1].u == Len::Auto && s.width.Def()) x = cb.x + l + std::max(0.f, (cb.w - l - r - bw) / 2);
        else if (hasL) x = cb.x + l + ml;
        else if (hasR) x = cb.x + cb.w - r - mr - bw;
        else x = d.sx + ml;
        float y0 = hasT ? cb.y + t + mt : d.sy + mt;
        BoxOpts o;
        o.forcedW = bw;
        o.bfc = true;
        if (hasT && hasB && !s.height.Def() && cb.h >= 0) o.forcedH = std::max(0.f, cbh - t - b - mt - mb);
        size_t st = out_.items.size();
        float h = Box(n, x - ml, y0, bw + ml + mr, depth + 1, o);
        if (!hasT && hasB) {
            float ty = cb.y + cbh - b - mb - h;
            Shift(st, out_.items.size(), 0, ty - y0);
        } else if (hasT && hasB && s.height.Def() && s.m[0].u == Len::Auto && s.m[2].u == Len::Auto) {
            float ty = cb.y + t + std::max(0.f, (cbh - t - b - h) / 2);
            Shift(st, out_.items.size(), 0, ty - y0);
        }
    }

    // ---------------- Blok akisi ----------------

    float BlockChildren(const Node* n, float x, float y, float w, size_t depth, float gap = 0) {
        if (depth > kMaxDepth || ++work_ > kMaxWork) return 0;
        bool firstBlock = true;
        float cy = y, prevMB = 0;
        std::vector<const Node*> run;
        const Style& ns = St(n);
        auto flush = [&]() {
            if (run.empty()) return;
            float h = InlineRun(run, x, cy + prevMB, w, ns, depth);
            run.clear();
            if (h > 0) { cy += prevMB; prevMB = 0; cy += h; }
        };
        ForKids(n, [&](const Node* k) {
            if (k->kind == Node::Text) { run.push_back(k); return; }
            if (k->kind != Node::Elem) return;
            const Style& ks = St(k);
            if (ks.display == Style::None) return;
            if (OutOfFlow(ks)) {
                if (ks.display != Style::Inline || ks.inlineBlock) {
                    // blok duzeyi absolute: statik konum = simdiki satir
                    if (run.empty()) Defer(k, x, cy + prevMB);
                    else run.push_back(k);
                } else run.push_back(k);
                return;
            }
            if (ks.floatSide) {
                flush();
                PlaceFloat(k, x, cy + prevMB, w, depth + 1);
                return;
            }
            if (IsBlockLevel(k, depth)) {
                flush();
                float mt = R(ks.m[0], w), mb = R(ks.m[2], w);
                if (!firstBlock) cy += gap;
                firstBlock = false;
                if (ks.clear) {
                    float c = ClearY(cy + Collapse(prevMB, mt), ks.clear);
                    if (c > cy + Collapse(prevMB, mt) + 0.01f) { cy = c - mt; prevMB = 0; }
                }
                cy += Collapse(prevMB, mt);
                prevMB = 0;
                float bx = x, bwid = w;
                if (!floats_.empty() && IsBfcRoot(k, ks)) {
                    // BFC kokleri float'larin yanina yerlesir
                    float l, r;
                    FloatAvail(cy, 1, x, x + w, l, r);
                    bx = l; bwid = std::max(0.f, r - l);
                }
                cy += Box(k, bx, cy, bwid, depth + 1);
                prevMB = mb;
            } else {
                run.push_back(k);
            }
        });
        flush();
        cy += prevMB;
        return cy - y;
    }

    void PlaceFloat(const Node* n, float x, float y, float w, size_t depth) {
        const Style& s = St(n);
        float ml = R(s.m[3], w), mr = R(s.m[1], w), mt = R(s.m[0], w), mb = R(s.m[2], w);
        float extra = R(s.p[1], w) + R(s.p[3], w) + s.bw[1] + s.bw[3];
        float bw;
        if (s.width.Def()) bw = R(s.width, w) + (s.borderBox ? 0 : extra);
        else if (IsReplaced(n)) {
            float iw, ih;
            std::wstring key;
            bw = ImageSize(n, s, w, iw, ih, key) ? iw + extra : extra;
        } else if (s.display == Style::Table) {
            std::vector<float> cols;
            bw = std::min(PlanTable(n, w - ml - mr - extra, true, cols, depth), w - ml - mr - extra) + extra;
        } else bw = std::min(ItemMaxContent(n, depth), std::max(0.f, w - ml - mr));
        if (s.maxWidth.Def()) bw = std::min(bw, R(s.maxWidth, w) + (s.borderBox ? 0 : extra));
        if (s.minWidth.Def()) bw = std::max(bw, R(s.minWidth, w) + (s.borderBox ? 0 : extra));
        bw = std::max(0.f, bw);
        float outerW = bw + ml + mr;
        float fy = std::max(y, lastFloatTop_);
        if (s.clear) fy = ClearY(fy, s.clear);
        float l = x, r = x + w;
        for (int it = 0; it < 200; it++) {
            FloatAvail(fy, 1, x, x + w, l, r);
            if (r - l >= outerW - 0.5f) break;
            float nb = NextFloatBottom(fy, x, x + w);
            if (nb < 0) { l = x; r = x + w; break; }
            fy = nb;
        }
        float fx = s.floatSide == 1 ? l : std::max(l, r - outerW);
        BoxOpts o;
        o.forcedW = bw;
        o.bfc = true;
        float h = Box(n, fx, fy + mt, outerW, depth, o);
        floats_.push_back({fx, fy, outerW, mt + h + mb, s.floatSide});
        lastFloatTop_ = fy;
    }

    // Eleman kutusunu (x = kenar bosluk kutusu sol kenari, y = kenarlik ust kenari) yerlestirir;
    // kenarlik kutusu yuksekligini dondurur.
    float Box(const Node* n, float x, float y, float availW, size_t depth, const BoxOpts& o = BoxOpts()) {
        const Style& s = St(n);
        if (depth > kMaxDepth || ++work_ > kMaxWork) return 0;
        float ml = R(s.m[3], availW), mr = R(s.m[1], availW);
        float pl = R(s.p[3], availW), pr = R(s.p[1], availW);
        float pt = R(s.p[0], availW), pb = R(s.p[2], availW);
        float bl = s.bw[3], br = s.bw[1], bt = s.bw[0], bb = s.bw[2];
        float extra = pl + pr + bl + br;
        float vextra = pt + pb + bt + bb;

        float cw;
        bool constrained = false;
        float replW = -1, replH = -1;
        std::wstring replKey;
        if (IsReplaced(n)) {
            float iw, ih;
            if (ImageSize(n, s, std::max(0.f, availW - ml - mr - extra), iw, ih, replKey)) { replW = iw; replH = ih; }
        }
        if (o.forcedW >= 0) {
            cw = o.forcedW - extra;
            constrained = true;
            ml = std::max(0.f, ml);
        } else if (s.width.Def()) {
            cw = R(s.width, availW);
            if (s.borderBox) cw -= extra;
            constrained = true;
        } else if (IsReplaced(n)) {
            cw = replW >= 0 ? replW : 0;
            constrained = true;
        } else if (IsControl(n)) {
            cw = availW - ml - mr - extra;
        } else if (s.width.u == Len::Fit || o.shrink) {
            cw = std::min(ItemMaxContent(n, depth) - extra, availW - ml - mr - extra);
            constrained = true;
        } else if (s.display == Style::Table) {
            std::vector<float> cols;
            cw = std::min(PlanTable(n, availW - ml - mr - extra, true, cols, depth), availW - ml - mr - extra);
            constrained = true;
        } else {
            cw = availW - ml - mr - extra;
        }
        if (o.forcedW < 0) {
            if (s.maxWidth.Def()) {
                float mx = R(s.maxWidth, availW);
                if (s.borderBox) mx -= extra;
                if (cw > mx) { cw = mx; constrained = true; }
            }
            if (s.minWidth.Def()) {
                float mn = R(s.minWidth, availW);
                if (s.borderBox) mn -= extra;
                if (cw < mn) { cw = mn; constrained = true; }
            }
        }
        cw = std::max(0.f, cw);
        float outer = cw + extra;
        if (constrained && o.forcedW < 0) {
            bool la = s.m[3].u == Len::Auto, ra = s.m[1].u == Len::Auto;
            if (la && ra) ml = std::max(0.f, (availW - outer) / 2);
            else if (la) ml = std::max(0.f, availW - outer - mr);
        }
        float bx = x + ml;
        size_t itemStart = out_.items.size();
        float radius = s.radiusPct ? 1e6f : s.radius;
        bool paint = !s.hidden;
        uint32_t bgc = s.bg;
        if (s.opacity < 1 && (bgc >> 24)) bgc = (uint32_t(float(bgc >> 24) * s.opacity) << 24) | (bgc & 0xFFFFFF);
        // box-shadow (dis golgeler): arka plandan once cizilir
        size_t shadowFrom = out_.items.size(), shadowCount = 0;
        if (paint && s.shadows) for (auto& sh : *s.shadows) if (!sh.inset) { Reserve(); shadowCount++; }
        // saydam ama tiklanabilir kutular (dugme, imlec:pointer): gorunmez isabet alani
        int boxLink = (n->name == L"a" && n->Attr(L"href")) ? AddLink(*n->Attr(L"href")) : -1;
        bool hitBox = paint && !(bgc >> 24) && (s.cursor == 1 || IsControl(n) || boxLink >= 0);
        size_t bgIdx = (paint && ((bgc >> 24) || hitBox) && !s.mask) ? Reserve() : size_t(-1);
        size_t gradIdx = (paint && s.grad) ? Reserve() : size_t(-1);
        size_t bgImgIdx = (paint && s.bgImage && img_) ? Reserve() : size_t(-1);
        size_t maskIdx = (paint && s.mask && img_ && (bgc >> 24)) ? Reserve() : size_t(-1);
        bool clip = s.overflowHidden && !IsReplaced(n) && !IsControl(n);
        size_t clipIdx = clip ? Reserve() : size_t(-1);
        float cx = bx + bl + pl, cy = y + bt + pt;

        // Konumlandirilmis oge: absolute torunlar icin kapsayan blok
        bool positioned = s.position != 0;
        CB myCB{bx + bl, y + bt, cw + pl + pr, -1, {}};
        if (positioned) cbStack_.push_back(&myCB);

        // BFC: disaridaki float'lar ice etki etmez, icerdekiler disari tasmaz
        bool bfc = o.bfc || s.bfc || s.display == Style::Table || s.flex || s.gridCols || s.inlineBlock || IsControl(n);
        std::vector<FloatBox> savedFloats;
        float savedLastTop = lastFloatTop_;
        if (bfc) { savedFloats.swap(floats_); lastFloatTop_ = -1e9f; }

        // Kesin yukseklik (flex/grid hizalama ve yuzde yukseklikler icin)
        float definiteH = -1;
        if (o.forcedH >= 0) definiteH = std::max(0.f, o.forcedH - vextra);
        else if (s.height.u == Len::Px || (s.height.u == Len::Calc && s.height.p == 0)) definiteH = std::max(0.f, R(s.height, 0) - (s.borderBox ? vextra : 0));
        else if (s.height.Def() && ((curDefiniteH_ >= 0) || n->name == L"html" || n->name == L"body")) definiteH = std::max(0.f, R(s.height, curDefiniteH_ >= 0 ? curDefiniteH_ : vh_) - (s.borderBox ? vextra : 0));
        float minH = -1;
        if (s.minHeight.u == Len::Px || (s.minHeight.u == Len::Calc && s.minHeight.p == 0)) minH = std::max(0.f, R(s.minHeight, 0) - (s.borderBox ? vextra : 0));
        else if (s.minHeight.Def() && ((curDefiniteH_ >= 0) || n->name == L"html" || n->name == L"body")) minH = std::max(0.f, R(s.minHeight, curDefiniteH_ >= 0 ? curDefiniteH_ : vh_) - (s.borderBox ? vextra : 0));
        float savedDefiniteH = curDefiniteH_;
        curDefiniteH_ = definiteH >= 0 ? definiteH : ((n->name == L"html" || n->name == L"body") ? vh_ : -1.f);

        std::wstring savedMarker;
        const Style* savedMarkerStyle = markerStyle_;
        if (s.display == Style::ListItem) {
            savedMarker = marker_;
            marker_ = MarkerText(n, s);
            markerStyle_ = &s;
        }
        float ch;
        if (IsReplaced(n)) {
            if (replW >= 0) {
                float iw = replW, ih = replH;
                if (std::fabs(cw - iw) > 0.5f) {
                    // genislik disaridan verildi (flex/grid/float): oran korunarak yukseklik
                    if (!s.height.Def() && iw > 0) ih = ih * cw / iw;
                    iw = cw;
                }
                if (paint) EmitImage(cx, cy, iw, ih, replKey, -1, RealId(n), s);
                ch = ih;
            } else ch = 0;
        } else if (IsControl(n)) {
            // blok/flex ogesi olarak form denetimi: verilen genisligi doldurur
            float saved = forceInputW_;
            forceInputW_ = (constrained || s.flexGrow > 0 || inFlexItem_) ? cw : 0;
            std::vector<const Node*> one{n};
            bool savedBoxed = controlBoxed_;
            controlBoxed_ = true;
            ch = InlineRun(one, cx, cy, cw, s, depth);
            controlBoxed_ = savedBoxed;
            forceInputW_ = saved;
        } else if (s.display == Style::Table) ch = TableContent(n, cx, cy, cw, depth);
        else if (s.flex == 1) ch = FlexRow(n, cx, cy, cw, depth, definiteH);
        else if (s.flex == 2) ch = FlexCol(n, cx, cy, cw, depth, definiteH >= 0 ? definiteH : minH);
        else if (s.gridCols > 0) ch = Grid(n, cx, cy, cw, depth);
        else ch = BlockChildren(n, cx, cy, cw, depth);
        if (s.display == Style::ListItem) { marker_ = savedMarker; markerStyle_ = savedMarkerStyle; }
        curDefiniteH_ = savedDefiniteH;

        if (bfc) {
            float fb = FloatsBottom();
            if (fb > cy + ch) ch = fb - cy;
            floats_.swap(savedFloats);
            lastFloatTop_ = savedLastTop;
        }
        if (definiteH >= 0) ch = definiteH;
        else if (s.aspect > 0 && !s.height.Def()) ch = std::max(ch, (cw + pl + pr) / s.aspect - pt - pb);
        if (minH >= 0) ch = std::max(ch, minH);
        if (s.maxHeight.u == Len::Px) ch = std::min(ch, std::max(0.f, s.maxHeight.v - (s.borderBox ? vextra : 0)));

        float bh = bt + pt + ch + pb + bb;
        if (bgIdx != size_t(-1)) {
            DisplayItem& d = out_.items[bgIdx];
            d.x = bx; d.y = y; d.w = outer; d.h = bh; d.color = bgc; d.radius = std::min(radius, std::min(outer, bh) / 2);
            d.node = RealId(n);   // arka planli kutu tiklanabilir / getBoundingClientRect (yalnizca metni olmayan ogeler de)
        }
        if (shadowCount) {
            size_t k = shadowFrom;
            float rr = std::min(radius, std::min(outer, bh) / 2);
            for (auto& sh : *s.shadows) {
                if (sh.inset) continue;
                DisplayItem& d = out_.items[k++];
                d.kind = DisplayItem::Shadow;
                d.x = bx + sh.x - sh.spread; d.y = y + sh.y - sh.spread;
                d.w = outer + 2 * sh.spread; d.h = bh + 2 * sh.spread;
                d.ix = bx; d.iy = y; d.iw = outer; d.ih = bh;
                d.blur = sh.blur;
                d.tint = (((bgc >> 24) == 255 || s.grad) && !s.mask) ? 1u : 0u;   // opak arka plan: kutu ici dislanmasina gerek yok
                d.radius = std::max(0.f, rr + sh.spread);
                uint32_t c = sh.color;
                if (s.opacity < 1) c = (uint32_t(float(c >> 24) * s.opacity) << 24) | (c & 0xFFFFFF);
                d.color = c;
            }
        }
        if (gradIdx != size_t(-1)) {
            DisplayItem& d = out_.items[gradIdx];
            d.kind = DisplayItem::Gradient;
            d.x = bx; d.y = y; d.w = outer; d.h = bh;
            d.radius = std::min(radius, std::min(outer, bh) / 2);
            d.grad = s.grad;
            d.node = RealId(n);
            d.color = s.opacity < 1 ? (uint32_t(255 * s.opacity) << 24) : 0xFF000000;   // alfa: saydamlik carpani
        }
        if (bgImgIdx != size_t(-1)) PlaceBgImage(out_.items[bgImgIdx], *s.bgImage, bx, y, outer, bh, 0, RealId(n));
        if (maskIdx != size_t(-1)) PlaceBgImage(out_.items[maskIdx], *s.mask, bx, y, outer, bh, (bgc >> 24) ? bgc : 0, RealId(n));
        if (clipIdx != size_t(-1)) {
            DisplayItem& d = out_.items[clipIdx];
            d.kind = DisplayItem::ClipPush;
            d.x = bx; d.y = y; d.w = outer; d.h = bh;
            DisplayItem e;
            e.kind = DisplayItem::ClipPop;
            e.x = bx; e.y = y; e.w = outer; e.h = bh;
            out_.items.push_back(e);
        }
        if (paint) Borders(s, bx, y, outer, bh, std::min(radius, std::min(outer, bh) / 2));

        // Kendi absolute torunlarini yerlestir
        if (positioned) {
            myCB.h = bh - bt - bb;
            cbStack_.pop_back();
            ProcessAbs(myCB, depth);
        }
        // <button> icindeki ogeler: el imleci
        if (n->name == L"button")
            for (size_t i = itemStart; i < out_.items.size(); i++) if (out_.items[i].role == DisplayItem::None && out_.items[i].node >= 0) out_.items[i].role = DisplayItem::Button;
        if (boxLink >= 0)
            for (size_t i = itemStart; i < out_.items.size(); i++) if (out_.items[i].link < 0 && out_.items[i].kind != DisplayItem::ClipPush && out_.items[i].kind != DisplayItem::ClipPop) out_.items[i].link = boxLink;
        // relative/sticky kaydirma ve transform: translate
        float dx = 0, dy = 0;
        if (s.position == 1) {
            if (s.inset[3].Def()) dx += R(s.inset[3], availW);
            else if (s.inset[1].Def()) dx -= R(s.inset[1], availW);
            if (s.inset[0].Def()) dy += R(s.inset[0], vh_);
            else if (s.inset[2].Def()) dy -= R(s.inset[2], vh_);
        }
        if (s.tx.u != Len::Px || s.tx.v != 0) dx += R(s.tx, outer);
        if (s.ty.u != Len::Px || s.ty.v != 0) dy += R(s.ty, bh);
        if (dx != 0 || dy != 0) Shift(itemStart, out_.items.size(), dx, dy);
        // z-index (konumlandirilmis ogeler), imlec ve yari saydamlik alt agaca uygulanir
        if (s.hasZIndex && s.position != 0) {
            hasZ_ = true;
            for (size_t i = itemStart; i < out_.items.size(); i++) if (out_.items[i].zIndex == 0) out_.items[i].zIndex = s.zIndex;
        }
        if (s.cursor) for (size_t i = itemStart; i < out_.items.size(); i++) if (!out_.items[i].cursor) out_.items[i].cursor = s.cursor;
        if (s.opacity < 0.995f && s.opacity > 0.01f) {
            for (size_t i = itemStart; i < out_.items.size(); i++) {
                DisplayItem& d = out_.items[i];
                if (bgIdx != size_t(-1) && i == bgIdx) continue; // arka plan zaten saydamlastirildi
                if (i == gradIdx || (i >= shadowFrom && i < shadowFrom + shadowCount)) continue;   // kendi golge/degradesi zaten saydam
                uint32_t a = d.color >> 24;
                if (a) d.color = (uint32_t(float(a) * s.opacity) << 24) | (d.color & 0xFFFFFF);
            }
        }
        return bh;
    }

    void PlaceBgImage(DisplayItem& d, const BgImage& b, float x, float y, float w, float h, uint32_t tint, int node) {
        d.kind = DisplayItem::Fill;
        d.color = 0;
        float nw = 0, nh = 0;
        if (b.url.empty() || !img_ || !img_->LookupUrl(b.url, nw, nh) || nw <= 0 || nh <= 0 || w <= 0 || h <= 0) return;
        float tw = nw, th = nh;
        if (b.size == 1 || b.size == 2) {
            float sc = b.size == 1 ? std::max(w / nw, h / nh) : std::min(w / nw, h / nh);
            tw = nw * sc; th = nh * sc;
        } else if (b.size == 3) {
            bool aw = b.sw.Def(), ah = b.sh.Def();
            if (aw) tw = R(b.sw, w);
            if (ah) th = R(b.sh, h);
            if (aw && !ah) th = nh * tw / nw;
            else if (ah && !aw) tw = nw * th / nh;
        }
        if (tw < 0.5f || th < 0.5f) return;
        float px = b.px.u == Len::Pct ? (w - tw) * b.px.v / 100.f : R(b.px, w);
        float py = b.py.u == Len::Pct ? (h - th) * b.py.v / 100.f : R(b.py, h);
        d.kind = DisplayItem::BgImage;
        d.x = x; d.y = y; d.w = w; d.h = h;
        d.ix = x + px; d.iy = y + py; d.iw = tw; d.ih = th;
        d.repeat = b.repeat;
        d.text = b.url;
        d.tint = tint;
        d.node = node;
    }

    std::wstring MarkerText(const Node* n, const Style& s) {
        if (s.listType == 2) return L"";
        if (s.listType == 0) return L"•";
        if (s.listType == 3) return L"◦";
        if (s.listType == 4) return L"▪";
        int idx = 1;
        if (n->parent) {
            if (auto* st = n->parent->Attr(L"start")) idx = _wtoi(st->c_str());
            for (auto& k : n->parent->kids) {
                if (k.get() == n) break;
                if (k->kind == Node::Elem && k->name == L"li") idx++;
            }
        }
        if (auto* v = n->Attr(L"value")) idx = _wtoi(v->c_str());
        return std::to_wstring(idx) + L".";
    }

    // ---------------- Satir ici akis ----------------

    float LineH(const Style& s, const FontMetrics& fm) const {
        (void)fm;
        if (s.lineHeightPx > 0) return s.lineHeightPx;
        return s.fontSize * s.lineHeight;
    }

    int AddLink(const std::wstring& href) {
        std::wstring h = Trim(href);
        if (h.empty() || StartsWith(ToLower(h), L"javascript:")) return -1;
        out_.links.push_back(h);
        return int(out_.links.size()) - 1;
    }

    static void Transform(std::wstring& w, uint8_t tt, bool atWordStart) {
        if (tt == 1) for (auto& c : w) c = wchar_t(towupper(c));
        else if (tt == 2) for (auto& c : w) c = wchar_t(towlower(c));
        else if (tt == 3 && atWordStart && !w.empty()) w[0] = wchar_t(towupper(w[0]));
    }

    void TextPieces(const std::wstring& text, const Style& s, int link, std::vector<Piece>& pc, bool& space, int node) {
        if (pc.size() > 200000) return;
        int f = tm_.Font(s);
        FontMetrics fm = tm_.Metrics(f);
        float lh = LineH(s, fm);
        uint32_t bg = (s.display == Style::Inline && !s.inlineBlock) ? s.bg : 0;
        bool invisible = s.hidden || s.textHidden;
        float shift = s.vAlign == 6 ? s.fontSize * 0.25f : s.vAlign == 7 ? -s.fontSize * 0.4f : 0.f;
        auto emit = [&](std::wstring word, bool canBreakHere) {
            for (auto& c : word) if (c == 0xA0) c = L' ';
            Transform(word, s.textTransform, space || pc.empty());
            Piece p;
            p.text = std::move(word);
            p.font = f; p.fm = fm; p.lh = lh;
            p.color = s.color; p.bg = bg; p.link = link;
            p.underline = s.underline; p.strike = s.strike;
            p.spaceBefore = space;
            p.canBreak = canBreakHere && space && !s.nowrap;
            p.node = node;
            p.invisible = invisible;
            p.shift = shift;
            p.w = tm_.Width(f, p.text);
            pc.push_back(std::move(p));
            space = false;
        };
        if (s.pre) {
            std::wstring line;
            for (wchar_t c : text) {
                if (c == L'\n') {
                    if (!line.empty()) emit(line, true);
                    line.clear();
                    Piece b; b.brk = true; b.font = f; b.fm = fm; b.lh = lh;
                    pc.push_back(b);
                    space = false;
                } else if (c == L'\r') {
                } else if (c == L'\t') line += L"    ";
                else line += c;
            }
            if (!line.empty()) emit(line, true);
            return;
        }
        std::wstring word;
        for (wchar_t c : text) {
            if (IsWS(c)) {
                if (!word.empty()) { emit(word, true); word.clear(); }
                space = true;
            } else if (c == 0x200B || c == 0xAD) {
                // sifir genislikli bosluk / yumusak tire: kelimeyi bol ama bosluk ekleme
                if (!word.empty()) { emit(word, true); word.clear(); }
            } else {
                word += c;
                if (word.size() > 4000) { emit(word, true); word.clear(); }
            }
        }
        if (!word.empty()) emit(word, true);
    }

    // Kisa, arka planli/kenarlikli satir ici ogeler (rozet, etiket, buton gibi link) atom olarak cizilir
    bool InlineAsAtom(const Node* n, const Style& s) {
        if (s.inlineBlock) return true;
        if (s.display != Style::Inline) return false;
        bool deco = (s.bg >> 24) || s.bw[0] > 0 || s.bw[1] > 0 || s.bw[2] > 0 || s.bw[3] > 0 || s.bgImage || s.mask;
        if (!deco) return false;
        bool pad = R(s.p[1], 0) > 0 || R(s.p[3], 0) > 0 || s.bw[1] > 0 || s.bw[3] > 0 || s.width.Def() || s.mask || s.bgImage;
        if (!pad) return false;
        size_t len = 0;
        bool ok = true;
        std::vector<const Node*> st{n};
        int count = 0;
        while (!st.empty() && ok) {
            const Node* c = st.back();
            st.pop_back();
            if (++count > 40) { ok = false; break; }
            if (c->kind == Node::Text) { len += c->text.size(); if (len > 60) ok = false; continue; }
            if (c != n && c->kind == Node::Elem && St(c).display != Style::Inline && St(c).display != Style::None) ok = false;
            for (auto& k : c->kids) st.push_back(k.get());
        }
        return ok;
    }

    void Collect(const Node* n, int link, std::vector<Piece>& pc, bool& space, size_t depth) {
        if (depth > kMaxDepth || pc.size() > 200000) return;
        if (n->kind == Node::Text) {
            TextPieces(n->text, St(n->parent), link, pc, space, RealId(n->parent));
            return;
        }
        if (n->kind != Node::Elem) return;
        const Style& s = St(n);
        if (s.display == Style::None) return;
        const std::wstring& t = n->name;
        if (OutOfFlow(s)) {
            // statik konum: bu noktadaki satir konumu (satir yerlestirilince guncellenir)
            Defer(n, 0, 0);
            Piece a;
            a.anchor = int(deferStore_.size() - 1);
            a.w = 0;
            a.fm = tm_.Metrics(tm_.Font(s));
            a.lh = 0;
            pc.push_back(std::move(a));
            return;
        }
        if (t == L"br") {
            int f = tm_.Font(s);
            Piece b; b.brk = true; b.font = f; b.fm = tm_.Metrics(f); b.lh = LineH(s, b.fm);
            pc.push_back(b);
            space = false;
            return;
        }
        if (IsReplaced(n)) {
            float iw, ih;
            std::wstring key;
            if (ImageSize(n, s, curWidth_, iw, ih, key)) {
                Piece p;
                p.isImage = true;
                p.text = key;
                p.w = iw;
                p.imgH = ih;
                p.fm = FontMetrics{ih, ih, tm_.Metrics(tm_.Font(s)).space};
                p.ascent = ih;
                p.lh = ih;
                p.link = link;
                p.node = n->id;
                p.spaceBefore = space;
                p.canBreak = true;
                p.invisible = s.hidden;
                p.valign = s.vAlign == 1 ? 1 : 0;
                pc.push_back(std::move(p));
                space = false;
                return;
            }
            const std::wstring* alt = n->Attr(L"alt");
            if (t == L"img" && alt && !Trim(*alt).empty()) {
                Style a = s;
                a.italic = true;
                a.color = 0xFF6B7280;
                a.display = Style::Inline;
                a.inlineBlock = false;
                TextPieces(L"[" + Trim(*alt) + L"]", a, link, pc, space, n->id);
            }
            return;
        }
        if (t == L"input" || t == L"textarea") {
            std::wstring type = (t == L"input" && n->Attr(L"type")) ? ToLower(*n->Attr(L"type")) : L"text";
            if (type == L"hidden" || type == L"file" || type == L"image") return;
            std::wstring label;
            Style a = s;
            a.display = Style::Inline;
            a.bg = 0xFFFFFFFF;
            a.underline = false;
            DisplayItem::Role role = DisplayItem::TextInput;
            float minW = 0;
            bool checked = n->dirtyChecked ? n->checked : (n->Attr(L"checked") != nullptr);
            std::wstring val = n->dirtyValue ? n->value : (n->Attr(L"value") ? *n->Attr(L"value") : L"");
            if (t == L"textarea" && !n->dirtyValue) val = TextContent(n);
            if (type == L"checkbox") { label = checked ? L"☑" : L"☐"; role = DisplayItem::Check; a.bg = 0; }
            else if (type == L"radio") { label = checked ? L"◉" : L"○"; role = DisplayItem::Check; a.bg = 0; }
            else if (type == L"submit" || type == L"button" || type == L"reset") {
                label = !val.empty() ? val : (type == L"reset" ? L"Sıfırla" : L"Gönder");
                label = L" " + label + L" ";
                a.bg = 0xFFE4E8EF;
                role = DisplayItem::Button;
            } else {
                if (type == L"password") label = std::wstring(val.size(), L'•');
                else label = val;
                for (auto& c : label) if (c == L'\n' || c == L'\r') c = L' ';
                if (label.empty() && n->Attr(L"placeholder")) { label = *n->Attr(L"placeholder"); a.color = 0xFF8A8F99; }
                label = L" " + label;
                minW = 180;
                if (auto* sz = n->Attr(L"size")) { int c = _wtoi(sz->c_str()); if (c > 0 && c < 200) minW = float(c) * a.fontSize * 0.55f; }
                if (s.width.Def()) minW = std::max(20.f, R(s.width, curWidth_) - (s.borderBox ? 0.f : 0.f));
            }
            Piece p;
            p.font = tm_.Font(a); p.fm = tm_.Metrics(p.font); p.lh = LineH(a, p.fm) + 6;
            p.text = label; p.color = a.color; p.bg = a.bg; p.link = link;
            p.spaceBefore = space; p.canBreak = space;
            p.role = role;
            p.node = n->id;
            p.invisible = s.hidden;
            p.w = std::max(tm_.Width(p.font, p.text) + (role == DisplayItem::TextInput ? 6.f : 0.f), minW);
            if (forceInputW_ > 0 && (role == DisplayItem::TextInput || role == DisplayItem::Button)) p.w = std::max(forceInputW_ - 2, 20.f);
            if (role == DisplayItem::TextInput && p.w > curWidth_ && curWidth_ > 40) p.w = curWidth_ - 8;
            // Yazar CSS'i kenarlik/arka plan verdiyse varsayilan cerceve cizilmez
            bool styled = s.bw[0] > 0 || s.bw[1] > 0 || s.bw[2] > 0 || s.bw[3] > 0 || (s.bg >> 24) || s.radius > 0;
            if (styled) {
                if (controlBoxed_) { p.styledFrame = 1; }
                else { p.styledFrame = 2; p.frameColor = s.bc[0]; p.frameW = s.bw[0]; p.frameRadius = s.radius; if (s.bg >> 24) p.bg = s.bg; }
            }
            pc.push_back(std::move(p));
            space = false;
            return;
        }
        if (t == L"select") {
            std::wstring first;
            for (auto& k : n->kids) {
                if (k->kind == Node::Elem && k->name == L"option") {
                    for (auto& tx : k->kids) if (tx->kind == Node::Text) first = Trim(tx->text);
                    break;
                }
            }
            for (auto& k : n->kids)
                if (k->kind == Node::Elem && k->name == L"option" && k->Attr(L"selected"))
                    for (auto& tx : k->kids) if (tx->kind == Node::Text) first = Trim(tx->text);
            Style a = s;
            a.display = Style::Inline;
            a.inlineBlock = false;
            a.bg = 0xFFEEF0F4;
            TextPieces(first + L" ▾", a, link, pc, space, n->id);
            return;
        }
        int l = link;
        if (t == L"a") {
            if (auto* h = n->Attr(L"href")) { int nl = AddLink(*h); if (nl >= 0) l = nl; }
        }
        if (InlineAsAtom(n, s) || s.floatSide) {
            AtomPiece(n, s, l, pc, space, depth);
            return;
        }
        ForKids(n, [&](const Node* k) { Collect(k, l, pc, space, depth + 1); });
    }

    // inline-block: kutuyu ayri yerlestir, satira tek parca olarak koy
    void AtomPiece(const Node* n, const Style& s, int link, std::vector<Piece>& pc, bool& space, size_t depth) {
        float base = curWidth_;
        float ml = R(s.m[3], base), mr = R(s.m[1], base), mt = R(s.m[0], base), mb = R(s.m[2], base);
        float extra = R(s.p[1], base) + R(s.p[3], base) + s.bw[1] + s.bw[3];
        float bw;
        if (s.width.Def()) bw = R(s.width, base) + (s.borderBox ? 0 : extra);
        else if (s.display == Style::Table) {
            std::vector<float> cols;
            bw = std::min(PlanTable(n, base - ml - mr - extra, true, cols, depth), base - ml - mr - extra) + extra;
        } else bw = std::min(ItemMaxContent(n, depth), std::max(0.f, base - ml - mr));
        if (s.maxWidth.Def()) bw = std::min(bw, R(s.maxWidth, base) + (s.borderBox ? 0 : extra));
        if (s.minWidth.Def()) bw = std::max(bw, R(s.minWidth, base) + (s.borderBox ? 0 : extra));
        bw = std::max(bw, 0.f);
        size_t st = out_.items.size();
        size_t dmark = deferStore_.size();
        std::wstring savedMarker = marker_;
        marker_.clear();
        float savedCur = curWidth_;
        BoxOpts o;
        o.forcedW = bw;
        o.bfc = true;
        float h = Box(n, 0, mt, bw + ml + mr, depth + 1, o);
        curWidth_ = savedCur;
        marker_ = savedMarker;
        Piece p;
        p.atom = true;
        p.items.assign(out_.items.begin() + long(st), out_.items.end());
        out_.items.resize(st);
        p.w = bw + ml + mr;
        float total = mt + h + mb;
        // taban cizgisi: son metin satirinin tabani; metin yoksa alt kenar
        float baseLine = -1;
        for (auto& d : p.items) {
            if (d.kind != DisplayItem::Text || d.font < 0) continue;
            float b = d.y + tm_.Metrics(d.font).baseline;
            if (b > baseLine) baseLine = b;
        }
        if (baseLine < 0 || s.overflowHidden) baseLine = total;
        p.ascent = std::min(baseLine, total);
        p.imgH = total;
        int f = tm_.Font(s);
        p.fm = tm_.Metrics(f);
        p.lh = total;
        p.link = link;
        p.node = RealId(n);
        p.spaceBefore = space;
        p.canBreak = true;
        p.valign = (s.vAlign == 1 || s.vAlign == 2 || s.vAlign == 3) ? 1 : 0;
        p.deferFrom = dmark;
        p.deferTo = deferStore_.size();
        // atomdan sonraki metin icin bosluk: atomun kendisi kelime gibi davranir
        pc.push_back(std::move(p));
        space = false;
    }

    void SplitLong(std::vector<Piece>& pc, float w) {
        if (w < 30) return;
        bool any = false;
        for (auto& p : pc) if (!p.brk && !p.isImage && !p.atom && p.role == DisplayItem::None && p.w > w && p.text.size() >= 2) { any = true; break; }
        if (!any) return;
        std::vector<Piece> out;
        bool changed = false;
        for (auto& p : pc) {
            if (p.brk || p.isImage || p.atom || p.role != DisplayItem::None || p.w <= w || p.text.size() < 2 || out.size() > 200000) { out.push_back(std::move(p)); continue; }
            changed = true;
            size_t len = p.text.size(), st = 0;
            bool first = true;
            while (st < len) {
                size_t cnt = std::max<size_t>(1, size_t(float(len) * w / p.w));
                cnt = std::min(cnt, len - st);
                float cw = tm_.Width(p.font, p.text.substr(st, cnt));
                while (cnt > 1 && cw > w) { cnt = std::max<size_t>(1, cnt * 9 / 10); cw = tm_.Width(p.font, p.text.substr(st, cnt)); }
                Piece q = p;
                q.text = p.text.substr(st, cnt);
                q.w = cw;
                if (!first) { q.spaceBefore = false; q.canBreak = true; }
                out.push_back(std::move(q));
                st += cnt;
                first = false;
            }
        }
        if (changed) pc.swap(out);
    }

    float InlineRun(const std::vector<const Node*>& run, float x, float y, float w, const Style& cs, size_t depth) {
        std::vector<Piece> pc;
        bool space = false;
        float savedW = curWidth_;
        curWidth_ = w;
        for (const Node* n : run) Collect(n, -1, pc, space, depth + 1);
        curWidth_ = savedW;
        if (pc.empty()) return 0;
        bool onlyAnchors = true;
        for (auto& p : pc) if (p.anchor < 0) { onlyAnchors = false; break; }
        if (onlyAnchors) {
            for (auto& p : pc) { Deferred& d = deferStore_[size_t(p.anchor)]; d.sx = x; d.sy = y; d.itemIndex = out_.items.size(); }
            return 0;
        }
        SplitLong(pc, w);

        float lineY = y;
        float curX = 0;
        float lx = x, lw = w;      // gecerli satirin float'lara gore sol kenari ve genisligi
        bool lineOpen = false;
        std::vector<Placed> line;

        auto openLine = [&](float estH) {
            float l = x, r = x + w;
            if (!floats_.empty()) {
                for (int it = 0; it < 100; it++) {
                    FloatAvail(lineY, estH, x, x + w, l, r);
                    if (r - l >= std::min(40.f, w) || it > 98) break;
                    float nb = NextFloatBottom(lineY, x, x + w);
                    if (nb < 0) break;
                    lineY = nb;
                }
            }
            lx = l;
            lw = std::max(0.f, r - l);
            lineOpen = true;
        };

        // text-overflow: ellipsis (tek satir, nowrap) ve -webkit-line-clamp: N
        const int clampN = cs.lineClamp > 0 ? int(cs.lineClamp) : 0;
        const bool ellOne = cs.ellipsis && cs.nowrap && cs.overflowHidden;
        int linesDone = 0;
        bool stop = false;
        // satiri 'limit' genisligine sigacak sekilde kisaltip sonuna "…" ekler
        auto truncateLine = [&](float limit) {
            while (!line.empty()) {
                Placed& last = line.back();
                Piece& p = pc[last.idx];
                if (p.anchor >= 0) { line.pop_back(); continue; }
                bool isText = !p.atom && !p.isImage && p.role == DisplayItem::None && !p.text.empty();
                int f = isText ? p.font : tm_.Font(cs);
                float ew = tm_.Width(f, L"\x2026");
                if (isText) {
                    if (last.x + p.w + ew <= limit + 0.5f) { p.text += L"\x2026"; p.w += ew; curX = last.x + p.w; return; }
                    size_t lo = 0, hi = p.text.size();
                    while (lo < hi) {
                        size_t mid = (lo + hi + 1) / 2;
                        if (last.x + tm_.Width(p.font, p.text.substr(0, mid)) + ew <= limit + 0.5f) lo = mid; else hi = mid - 1;
                    }
                    if (lo > 0) {
                        std::wstring t = p.text.substr(0, lo);
                        while (!t.empty() && t.back() == L' ') t.pop_back();
                        p.text = t + L"\x2026";
                        p.w = tm_.Width(p.font, p.text);
                        curX = last.x + p.w;
                        return;
                    }
                }
                line.pop_back();
            }
            curX = 0;
        };
        auto flushLine = [&](int brkIdx, bool more = false) {
            if (!stop && !line.empty()) {
                if (clampN && linesDone + 1 == clampN && more) { truncateLine(lw); stop = true; }
                else if (ellOne && curX > lw + 0.5f) { truncateLine(lw); stop = true; }
            }
            if (!line.empty()) linesDone++;
            if (line.empty()) {
                if (brkIdx >= 0) lineY += pc[size_t(brkIdx)].lh;
                lineOpen = false;
                return;
            }
            float above = 0, below = 0;
            float parentBase = 0;
            for (auto& pl : line) {
                const Piece& p = pc[pl.idx];
                if (p.anchor >= 0) continue;
                if (p.atom || p.isImage) continue;
                float half = (p.lh - p.fm.height) / 2;
                above = std::max(above, p.fm.baseline + half - p.shift);
                below = std::max(below, p.fm.height - p.fm.baseline + half + p.shift);
                parentBase = std::max(parentBase, p.fm.baseline);
            }
            {
                // satir kutusu "destegi" (strut): kapsayicinin yazi tipi
                FontMetrics cfm = tm_.Metrics(tm_.Font(cs));
                float chalf = (LineH(cs, cfm) - cfm.height) / 2;
                above = std::max(above, cfm.baseline + chalf);
                below = std::max(below, cfm.height - cfm.baseline + chalf);
                if (parentBase <= 0) parentBase = cfm.baseline;
            }
            float mid = parentBase * 0.32f;
            for (auto& pl : line) {
                const Piece& p = pc[pl.idx];
                if (!p.atom && !p.isImage) continue;
                float h = p.imgH;
                if (p.valign == 1) {
                    above = std::max(above, h / 2 + mid);
                    below = std::max(below, h / 2 - mid);
                } else {
                    above = std::max(above, p.ascent);
                    below = std::max(below, h - p.ascent);
                }
            }
            float base = lineY + above;
            float off = 0;
            if (cs.align == 1) off = std::max(0.f, (lw - curX) / 2);
            else if (cs.align == 2) off = std::max(0.f, lw - curX);

            if (!marker_.empty() && markerStyle_) {
                int mf = tm_.Font(*markerStyle_);
                FontMetrics mfm = tm_.Metrics(mf);
                float mw = tm_.Width(mf, marker_);
                DisplayItem d;
                d.kind = DisplayItem::Text;
                d.x = x - mw - 8; d.y = base - mfm.baseline; d.w = mw; d.h = mfm.height;
                d.color = markerStyle_->color; d.font = mf; d.text = marker_;
                if (!markerStyle_->hidden) out_.items.push_back(std::move(d));
                marker_.clear();
            }

            float prevEnd = -1;
            int prevLink = -2;
            for (auto& pl : line) {
                const Piece& p = pc[pl.idx];
                float px = lx + off + pl.x;
                if (p.anchor >= 0) {
                    Deferred& d = deferStore_[size_t(p.anchor)];
                    d.sx = px; d.sy = lineY; d.itemIndex = out_.items.size();
                    continue;
                }
                if (p.atom) {
                    float top = p.valign == 1 ? base - mid - p.imgH / 2 : base - p.ascent;
                    size_t st = out_.items.size();
                    for (auto d : p.items) { d.x += px; d.y += top; d.ix += px; d.iy += top; out_.items.push_back(std::move(d)); }
                    for (size_t k = p.deferFrom; k < p.deferTo && k < deferStore_.size(); k++) {
                        Deferred& d = deferStore_[k];
                        if (d.done) continue;
                        d.sx += px; d.sy += top; d.itemIndex = st;
                    }
                    if (p.link >= 0)
                        for (size_t i = st; i < out_.items.size(); i++) if (out_.items[i].link < 0 && out_.items[i].kind != DisplayItem::ClipPush && out_.items[i].kind != DisplayItem::ClipPop) out_.items[i].link = p.link;
                    prevEnd = -1;
                    prevLink = -2;
                    continue;
                }
                float ty = base - p.fm.baseline + p.shift;
                if (p.isImage) {
                    float top = p.valign == 1 ? base - mid - p.imgH / 2 : base - p.ascent;
                    if (!p.invisible) EmitImage(px, top, p.w, p.imgH, p.text, p.link, p.node, St0());
                    prevEnd = -1;
                    prevLink = -2;
                    continue;
                }
                if (p.invisible) { prevEnd = -1; prevLink = -2; continue; }
                if ((p.role == DisplayItem::TextInput || p.role == DisplayItem::Button) && p.styledFrame != 1) {
                    float bh = p.fm.height + 4;
                    FillRect(px, ty - 2, p.w, bh, p.bg ? p.bg : 0xFFFFFFFF, p.styledFrame == 2 ? p.frameRadius : 0);
                    DisplayItem b;
                    b.kind = DisplayItem::Stroke;
                    b.role = p.role;
                    b.x = px; b.y = ty - 2; b.w = p.w; b.h = bh;
                    b.color = p.role == DisplayItem::Button ? 0xFFA9B1BF : 0xFF9AA3B2;
                    if (p.styledFrame == 2) { b.color = p.frameW > 0 ? p.frameColor : 0; b.radius = p.frameRadius; }
                    b.node = p.node;
                    if (b.color >> 24) out_.items.push_back(std::move(b));
                } else if (p.styledFrame == 1) {
                    // odak cercevesi icin gorunmez isaret
                    DisplayItem b;
                    b.kind = DisplayItem::Stroke;
                    b.role = p.role;
                    b.x = px; b.y = ty - 2; b.w = p.w; b.h = p.fm.height + 4;
                    b.color = 0;
                    b.node = p.node;
                    out_.items.push_back(std::move(b));
                } else if (p.bg >> 24) {
                    float sp = (prevLink == p.link && prevEnd >= 0 && p.spaceBefore) ? px - prevEnd : 0;
                    FillRect(px - 2 - sp, ty, p.w + 4 + sp, p.fm.height, p.bg);
                }
                DisplayItem d;
                d.kind = DisplayItem::Text;
                d.role = p.role;
                d.x = px; d.y = ty; d.w = p.w; d.h = p.fm.height;
                d.color = p.color; d.font = p.font; d.link = p.link; d.text = p.text; d.node = p.node;
                out_.items.push_back(std::move(d));
                float thick = std::max(1.f, p.fm.height / 18.f);
                if (p.underline) {
                    float ux = (prevLink == p.link && prevEnd >= 0 && p.spaceBefore) ? prevEnd : px;
                    DisplayItem u;
                    u.kind = DisplayItem::HLine;
                    u.x = ux; u.y = base + p.shift + thick + 0.5f; u.w = px + p.w - ux; u.h = thick;
                    u.color = p.color; u.link = p.link; u.node = p.node;
                    out_.items.push_back(std::move(u));
                }
                prevEnd = px + p.w;
                prevLink = p.link;
                if (p.strike) {
                    DisplayItem u;
                    u.kind = DisplayItem::HLine;
                    u.x = px; u.y = base + p.shift - p.fm.baseline * 0.32f; u.w = p.w; u.h = thick;
                    u.color = p.color;
                    out_.items.push_back(std::move(u));
                }
            }
            lineY += above + below;
            line.clear();
            curX = 0;
            lineOpen = false;
        };

        size_t i = 0;
        while (i < pc.size()) {
            if (stop) break;
            if (pc[i].brk) {
                if (!lineOpen) openLine(pc[i].lh);
                flushLine(int(i), i + 1 < pc.size());
                i++;
                continue;
            }
            size_t j = i + 1;
            while (j < pc.size() && !pc[j].brk && !pc[j].canBreak) j++;
            float gw = 0;
            for (size_t k = i; k < j; k++) gw += pc[k].w + ((k > i && pc[k].spaceBefore) ? pc[k].fm.space : 0);
            if (!lineOpen) openLine(std::max(pc[i].lh, pc[i].imgH));
            float sw = (!line.empty() && pc[i].spaceBefore) ? pc[i].fm.space : 0;
            if (!line.empty() && curX + sw + gw > lw + 0.5f) {
                flushLine(-1, true);
                if (stop) break;
                openLine(std::max(pc[i].lh, pc[i].imgH));
                sw = 0;
            }
            if (line.empty() && gw > lw + 0.5f && lw < w - 0.5f && !floats_.empty()) {
                // float'lar yuzunden dar satir: float bitimine in
                float nb = NextFloatBottom(lineY, x, x + w);
                if (nb > lineY) { lineY = nb; openLine(pc[i].lh); }
            }
            float px = curX + sw;
            for (size_t k = i; k < j; k++) {
                if (k > i && pc[k].spaceBefore) px += pc[k].fm.space;
                line.push_back({k, px});
                px += pc[k].w;
            }
            curX = px;
            i = j;
        }
        flushLine(-1);
        return lineY - y;
    }

    static const Style& St0() {
        static const Style s;
        return s;
    }

    // ---------------- Resimler ----------------

    void EmitImage(float x, float y, float w, float h, const std::wstring& key, int link, int node, const Style& s) {
        if (key.empty()) return; // yuklenmemis/basarisiz: yer kaplar ama gri kutu cizilmez
        DisplayItem d;
        d.kind = DisplayItem::Image;
        d.x = x; d.y = y; d.w = w; d.h = h;
        d.color = 0xFFFFFFFF;
        d.radius = s.radiusPct ? std::min(w, h) / 2 : std::min(s.radius, std::min(w, h) / 2);
        d.text = key; d.link = link; d.node = node;
        out_.items.push_back(std::move(d));
    }

    // Resim boyutu: CSS/oznitelik boyutu, yoksa dogal boyut; en-boy orani korunur.
    bool ImageSize(const Node* n, const Style& s, float availW, float& w, float& h, std::wstring& key) {
        float nw = 0, nh = 0;
        bool loaded = img_ && img_->Lookup(n, nw, nh, key) && nw > 0 && nh > 0;
        if (!loaded) key.clear();
        bool svg = n->name == L"svg";
        if (svg && !loaded) {
            // yuklenmemis satir ici SVG: viewBox oranindan dogal boyut
            if (auto* vb = n->Attr(L"viewbox")) {
                auto t = SplitWS(*vb);
                std::vector<std::wstring> parts;
                for (auto& x : t) { std::wstring y = x; for (auto& c : y) if (c == L',') c = L' '; for (auto& z : SplitWS(y)) parts.push_back(z); }
                if (parts.size() == 4) { nw = float(_wtof(parts[2].c_str())); nh = float(_wtof(parts[3].c_str())); }
            }
            if (nw <= 0 || nh <= 0) { nw = s.fontSize; nh = s.fontSize; }
        }
        bool hasRatio = loaded || svg;
        bool intrinsic = availW > 50000.f;
        float cw = (s.width.Def() && !(intrinsic && (s.width.u == Len::Pct || (s.width.u == Len::Calc && s.width.p != 0)))) ? R(s.width, availW) : -1;
        float ch = (s.height.u == Len::Px || (s.height.u == Len::Calc && s.height.p == 0)) ? R(s.height, 0) : -1;
        if (cw >= 0 && s.borderBox) cw -= R(s.p[1], intrinsic ? 0 : availW) + R(s.p[3], intrinsic ? 0 : availW) + s.bw[1] + s.bw[3];
        if (cw >= 0 && ch >= 0) { w = cw; h = ch; }
        else if (cw >= 0) { w = cw; h = hasRatio ? cw * nh / nw : -1; }
        else if (ch >= 0) { h = ch; w = hasRatio ? ch * nw / nh : -1; }
        else if (hasRatio) { w = nw; h = nh; }
        else return false;
        if (w < 0 || h < 0) return false;
        if (s.maxWidth.Def() && !(intrinsic && (s.maxWidth.u == Len::Pct || (s.maxWidth.u == Len::Calc && s.maxWidth.p != 0)))) {
            float mx = R(s.maxWidth, availW);
            if (w > mx && mx > 0) { h = h * mx / w; w = mx; }
        }
        if (s.maxHeight.u == Len::Px && h > s.maxHeight.v && s.maxHeight.v > 0) { w = w * s.maxHeight.v / h; h = s.maxHeight.v; }
        if (s.minWidth.Def() && !(intrinsic && (s.minWidth.u == Len::Pct || (s.minWidth.u == Len::Calc && s.minWidth.p != 0)))) w = std::max(w, R(s.minWidth, availW));
        if (s.minHeight.u == Len::Px) h = std::max(h, s.minHeight.v);
        if (!loaded && !svg && (w < 2 || h < 2)) return false;
        w = std::min(w, 20000.f);
        h = std::min(h, 20000.f);
        return true;
    }

    // ---------------- Icerik genislikleri ----------------

    float ExtraH(const Style& ks) const { return R(ks.p[1], 0) + R(ks.p[3], 0) + ks.bw[1] + ks.bw[3]; }

    // Ogenin max-content kenarlik kutusu genisligi
    float ItemMaxContent(const Node* k, size_t depth) {
        bool cacheable = k->id >= 0 && size_t(k->id) < maxMemo_.size();
        if (cacheable && maxMemo_[size_t(k->id)] >= 0) return maxMemo_[size_t(k->id)];
        float r = ItemMaxContentImpl(k, depth);
        if (cacheable) maxMemo_[size_t(k->id)] = r;
        return r;
    }

    float ItemMaxContentImpl(const Node* k, size_t depth) {
        const Style& ks = St(k);
        float extra = ExtraH(ks);
        if (depth > kMaxDepth || ++work_ > kMaxWork) return extra;
        if (IsReplaced(k)) {
            float iw, ih;
            std::wstring key;
            return ImageSize(k, ks, 100000.f, iw, ih, key) ? iw + extra : extra;
        }
        if (ks.width.u == Len::Px || (ks.width.u == Len::Calc && ks.width.p == 0)) return R(ks.width, 0) + (ks.borderBox ? 0 : extra);
        if (IsControl(k)) return ControlWidth(k, depth) + extra + 1;
        float r;
        if (ks.flex == 1 && depth < kMaxDepth) {
            // flex satiri: cocuklarin dogal genislikleri + bosluklar
            float sum = 0;
            int n = 0;
            ForKids(k, [&](const Node* c) {
                if (c->kind == Node::Text) {
                    float b = 0, cur = 0; bool sp = false;
                    MaxContentText(c, St(k), b, cur, sp);
                    if (std::max(b, cur) > 0) { sum += std::max(b, cur); n++; }
                    return;
                }
                if (c->kind != Node::Elem) return;
                const Style& cs = St(c);
                if (cs.display == Style::None || OutOfFlow(cs)) return;
                sum += ItemMaxContent(c, depth + 1) + R(cs.m[1], 0) + R(cs.m[3], 0);
                n++;
            });
            if (n > 1) sum += ks.gapCol * float(n - 1);
            r = sum + extra + 1;
        } else if (ks.gridCols && ks.grid && !ks.grid->cols.empty()) {
            // grid: sabit izler + icerik
            float sum = 0;
            int cols = int(ks.grid->cols.size());
            std::vector<float> colMax(size_t(cols), 0);
            int i = 0;
            ForKids(k, [&](const Node* c) {
                if (c->kind != Node::Elem || St(c).display == Style::None || OutOfFlow(St(c))) return;
                colMax[size_t(i % cols)] = std::max(colMax[size_t(i % cols)], ItemMaxContent(c, depth + 1));
                i++;
            });
            for (int c = 0; c < cols; c++) {
                const Track& t = ks.grid->cols[size_t(c)];
                sum += t.mx.k == 0 ? t.mx.v : colMax[size_t(c)];
            }
            r = sum + ks.gapCol * float(std::max(0, cols - 1)) + extra + 1;
        } else {
            float best = 0, cur = 0;
            bool sp = false;
            MaxContent(k, best, cur, sp, depth + 1);
            r = std::max(best, cur) + extra + 1;
        }
        if (ks.maxWidth.u == Len::Px) r = std::min(r, ks.maxWidth.v + (ks.borderBox ? 0 : extra));
        if (ks.minWidth.u == Len::Px) r = std::max(r, ks.minWidth.v + (ks.borderBox ? 0 : extra));
        return r;
    }

    // En uzun kirilamaz parca (min-content) + kenarlar
    float MinContent(const Node* k, size_t depth) {
        bool cacheable = k->id >= 0 && size_t(k->id) < minMemo_.size();
        if (cacheable && minMemo_[size_t(k->id)] >= 0) return minMemo_[size_t(k->id)];
        const Style& ks = St(k);
        float extra = ExtraH(ks);
        float r = extra;
        if (depth > kMaxDepth || ++work_ > kMaxWork) return r;
        if (IsReplaced(k)) {
            float iw, ih;
            std::wstring key;
            r = ImageSize(k, ks, 100000.f, iw, ih, key) ? ((ks.maxWidth.u == Len::Pct || ks.width.u == Len::Pct) ? 0.f : iw) + extra : extra;
        } else if (ks.width.u == Len::Px || (ks.width.u == Len::Calc && ks.width.p == 0)) r = R(ks.width, 0) + (ks.borderBox ? 0 : extra);
        else if (IsControl(k)) r = std::min(ControlWidth(k, depth), 60.f) + extra;
        else if (ks.nowrap) r = ItemMaxContent(k, depth);
        else {
            float best = 0;
            ForKids(k, [&](const Node* c) {
                if (c->kind == Node::Text) {
                    const Style& s = St(k);
                    int f = tm_.Font(s);
                    std::wstring word;
                    for (wchar_t ch : c->text) {
                        if (IsWS(ch)) { if (!word.empty()) { best = std::max(best, tm_.Width(f, word)); word.clear(); } }
                        else word += ch;
                    }
                    if (!word.empty()) best = std::max(best, tm_.Width(f, word));
                    return;
                }
                if (c->kind != Node::Elem) return;
                const Style& cs = St(c);
                if (cs.display == Style::None || OutOfFlow(cs)) return;
                best = std::max(best, MinContent(c, depth + 1) + std::max(0.f, R(cs.m[1], 0)) + std::max(0.f, R(cs.m[3], 0)));
            });
            r = best + extra;
        }
        if (ks.minWidth.u == Len::Px) r = std::max(r, ks.minWidth.v + (ks.borderBox ? 0 : extra));
        if (cacheable) minMemo_[size_t(k->id)] = r;
        return r;
    }

    // Form denetiminin dogal genisligi (flex/tablo olcumu icin)
    float ControlWidth(const Node* n, size_t depth) {
        std::vector<Piece> pc;
        bool sp = false;
        float saved = forceInputW_;
        forceInputW_ = 0;
        Collect(n, -1, pc, sp, depth + 1);
        forceInputW_ = saved;
        float w = 0;
        for (auto& p : pc) w += p.w;
        return w;
    }

    void MaxContentText(const Node* k, const Style& s, float& best, float& cur, bool& sp) {
        int f = tm_.Font(s);
        FontMetrics fm = tm_.Metrics(f);
        std::wstring word;
        auto addWord = [&]() {
            if (word.empty()) return;
            if (sp && cur > 0) cur += fm.space;
            cur += tm_.Width(f, word);
            word.clear();
            sp = false;
        };
        for (wchar_t c : k->text) {
            if (c == L'\n' && s.pre) { addWord(); best = std::max(best, cur); cur = 0; }
            else if (IsWS(c) && !s.pre) { addWord(); sp = true; }
            else word += c;
            if (word.size() > 4000) addWord();
        }
        addWord();
    }

    void MaxContent(const Node* n, float& best, float& cur, bool& sp, size_t depth) {
        if (depth > kMaxDepth || ++work_ > kMaxWork) return;
        ForKids(n, [&](const Node* k) {
            if (k->kind == Node::Text) { MaxContentText(k, St(n), best, cur, sp); return; }
            if (k->kind != Node::Elem) return;
            const Style& ks = St(k);
            if (ks.display == Style::None || OutOfFlow(ks)) return;
            if (k->name == L"br") { best = std::max(best, cur); cur = 0; sp = false; return; }
            if (IsReplaced(k)) {
                float iw, ih;
                std::wstring key;
                if (ImageSize(k, ks, 100000.f, iw, ih, key)) { if (sp && cur > 0) cur += 4; cur += iw; sp = false; }
                return;
            }
            if (IsControl(k)) { cur += ControlWidth(k, depth); return; }
            if (ks.floatSide) { cur += ItemMaxContent(k, depth + 1) + R(ks.m[1], 0) + R(ks.m[3], 0); return; }
            if (InlineAsAtom(k, ks)) {
                if (sp && cur > 0) cur += 4;
                cur += ItemMaxContent(k, depth + 1) + std::max(0.f, R(ks.m[1], 0)) + std::max(0.f, R(ks.m[3], 0));
                sp = false;
                return;
            }
            if (ks.display != Style::Inline) {
                best = std::max(best, cur);
                cur = 0;
                float sub = ItemMaxContent(k, depth + 1) + std::max(0.f, R(ks.m[1], 0)) + std::max(0.f, R(ks.m[3], 0));
                best = std::max(best, sub);
            } else {
                MaxContent(k, best, cur, sp, depth + 1);
            }
        });
    }

    // ---------------- Flexbox ----------------

    struct FlexItem {
        const Node* n = nullptr;          // eleman ya da (anonim ise) nullptr
        std::vector<const Node*> run;     // anonim metin ogesi
        float basis = 0, floor = 0, maxW = 1e9f, grow = 0, shrink = 1;
        float ml = 0, mr = 0, mt = 0, mb = 0;
        bool mlAuto = false, mrAuto = false;
        int order = 0;
        float width = 0;
    };

    void CollectFlexItems(const Node* n, float x, float y, float w, size_t depth, std::vector<FlexItem>& items) {
        std::vector<const Node*> textRun;
        auto flushText = [&]() {
            bool any = false;
            for (auto* t : textRun) if (t->kind == Node::Text) { for (wchar_t c : t->text) if (!IsWS(c)) { any = true; break; } }
            else any = true;
            if (any) {
                FlexItem it;
                it.run = textRun;
                float best = 0, cur = 0;
                bool sp = false;
                for (auto* t : textRun) {
                    if (t->kind == Node::Text) MaxContentText(t, St(t->parent), best, cur, sp);
                    else { float b2 = 0, c2 = 0; bool s2 = false; MaxContent(t, b2, c2, s2, depth + 1); cur += std::max(b2, c2); }
                }
                it.basis = std::max(best, cur) + 1;
                it.floor = std::min(it.basis, 60.f);
                it.shrink = 1;
                items.push_back(std::move(it));
            }
            textRun.clear();
        };
        ForKids(n, [&](const Node* k) {
            if (k->kind == Node::Text) { textRun.push_back(k); return; }
            if (k->kind != Node::Elem) return;
            const Style& ks = St(k);
            if (ks.display == Style::None) return;
            if (OutOfFlow(ks)) { Defer(k, x, y); return; }
            flushText();
            FlexItem it;
            it.n = k;
            it.ml = R(ks.m[3], w); it.mr = R(ks.m[1], w); it.mt = R(ks.m[0], w); it.mb = R(ks.m[2], w);
            it.mlAuto = ks.m[3].u == Len::Auto; it.mrAuto = ks.m[1].u == Len::Auto;
            float extra = R(ks.p[1], w) + R(ks.p[3], w) + ks.bw[1] + ks.bw[3];
            float basis;
            if (ks.flexBasis.Def() && !(ks.flexBasis.u == Len::Px && ks.flexBasis.v == 0 && ks.flexGrow == 0)) {
                basis = R(ks.flexBasis, w) + (ks.borderBox ? 0 : extra);
                if (ks.flexBasis.u == Len::Px && ks.flexBasis.v == 0) basis = extra;
            } else if (ks.width.Def()) {
                basis = R(ks.width, w) + (ks.borderBox ? 0 : extra);
            } else {
                basis = ItemMaxContent(k, depth);
            }
            if (ks.maxWidth.Def()) { it.maxW = R(ks.maxWidth, w) + (ks.borderBox ? 0 : extra); basis = std::min(basis, it.maxW); }
            float minW = -1;
            if (ks.minWidth.Def()) minW = R(ks.minWidth, w) + (ks.borderBox ? 0 : extra);
            if (minW >= 0) basis = std::max(basis, minW);
            it.basis = std::max(0.f, basis);
            it.grow = ks.flexGrow;
            it.shrink = ks.flexShrink;
            // otomatik en kucuk boyut: min-content (overflow gizliyse 0)
            if (minW >= 0) it.floor = minW;
            else if (ks.overflowHidden || ks.bfc) it.floor = extra;
            else {
                it.floor = MinContent(k, depth);
                if (ks.width.Def()) it.floor = std::min(it.floor, R(ks.width, w) + (ks.borderBox ? 0 : extra));
                if (w > 0 && it.shrink > 0) it.floor = std::min(it.floor, std::max(extra, w));
            }
            it.order = ks.order;
            items.push_back(std::move(it));
        });
        flushText();
        std::stable_sort(items.begin(), items.end(), [](const FlexItem& a, const FlexItem& b) { return a.order < b.order; });
        if (St(n).flexReverse) std::reverse(items.begin(), items.end());
    }

    // Esnek ogeyi yerlestir (x: kenar bosluk kutusu sol kenari)
    float LayoutFlexItem(const FlexItem& it, float x, float y, float w, size_t depth, float forcedH) {
        if (!it.n) {
            const Node* parent = it.run.empty() ? nullptr : it.run[0]->parent;
            const Style& ps = parent ? St(parent) : St0();
            Style a = ps;
            a.align = 0;
            return InlineRun(it.run, x + it.ml, y, w, a, depth + 1);
        }
        BoxOpts o;
        o.forcedW = w;
        o.forcedH = forcedH;
        o.bfc = true;
        bool savedFlex = inFlexItem_;
        inFlexItem_ = true;
        float h = Box(it.n, x, y, w + it.ml + it.mr, depth + 1, o);
        inFlexItem_ = savedFlex;
        return h;
    }

    bool NeedsStretch(const FlexItem& it) const {
        if (!it.n) return false;
        const Style& ks = St(it.n);
        if (ks.height.Def()) return false;
        return (ks.bg >> 24) || ks.bw[0] > 0 || ks.bw[2] > 0 || ks.bgImage || ks.flex || ks.gridCols || ks.radius > 0 || ks.bw[1] > 0;
    }

    float FlexRow(const Node* n, float x, float y, float w, size_t depth, float crossH) {
        if (depth > kMaxDepth) return 0;
        const Style& cs = St(n);
        std::vector<FlexItem> items;
        CollectFlexItems(n, x, y, w, depth, items);
        const float gap = cs.gapCol, rowGap = cs.gapRow;
        float cy = y;
        size_t i = 0;
        bool firstLine = true;
        size_t lineCount = 0;
        while (i < items.size()) {
            size_t j = i;
            float sum = 0;
            while (j < items.size()) {
                float add = items[j].basis + items[j].ml + items[j].mr + (j > i ? gap : 0);
                if (cs.flexWrap && j > i && sum + add > w + 0.5f) break;
                sum += add;
                j++;
            }
            lineCount++;
            // Esneme / daralma
            float free = w - sum;
            for (size_t k = i; k < j; k++) items[k].width = items[k].basis;
            if (free > 0) {
                float growSum = 0;
                for (size_t k = i; k < j; k++) growSum += items[k].grow;
                for (int pass = 0; pass < 3 && free > 0.5f && growSum > 0; pass++) {
                    float used = 0, nextGrow = 0;
                    for (size_t k = i; k < j; k++) {
                        if (items[k].grow <= 0) continue;
                        float add = free * items[k].grow / growSum;
                        float nw = std::min(items[k].width + add, items[k].maxW);
                        used += nw - items[k].width;
                        items[k].width = nw;
                        if (nw < items[k].maxW) nextGrow += items[k].grow;
                    }
                    free -= used;
                    growSum = nextGrow;
                }
            } else if (free < 0) {
                for (int pass = 0; pass < 4 && free < -0.5f; pass++) {
                    float wsum = 0;
                    for (size_t k = i; k < j; k++) if (items[k].width > items[k].floor + 0.01f) wsum += items[k].shrink * items[k].basis;
                    if (wsum <= 0) break;
                    float taken = 0;
                    for (size_t k = i; k < j; k++) {
                        if (items[k].width <= items[k].floor + 0.01f) continue;
                        float cut = -free * items[k].shrink * items[k].basis / wsum;
                        float nw = std::max(items[k].floor, items[k].width - cut);
                        taken += items[k].width - nw;
                        items[k].width = nw;
                    }
                    free += taken;
                    if (taken < 0.01f) break;
                }
            }
            float used = 0;
            for (size_t k = i; k < j; k++) used += items[k].width + items[k].ml + items[k].mr + (k > i ? gap : 0);
            float rem = std::max(0.f, w - used), start = 0, between = gap;
            size_t cnt = j - i;
            // otomatik kenar bosluklari bos alani yutar
            int autoCount = 0;
            for (size_t k = i; k < j; k++) autoCount += (items[k].mlAuto ? 1 : 0) + (items[k].mrAuto ? 1 : 0);
            float autoShare = (autoCount > 0 && rem > 0.5f) ? rem / float(autoCount) : 0;
            if (autoCount == 0 && rem > 0.5f) {
                if (cs.justify == 1) start = rem / 2;
                else if (cs.justify == 2) start = rem;
                else if (cs.justify == 3 && cnt > 1) between = gap + rem / float(cnt - 1);
                else if (cs.justify == 4) { between = gap + rem / float(cnt); start = rem / float(cnt) / 2; }
                else if (cs.justify == 5) { between = gap + rem / float(cnt + 1); start = rem / float(cnt + 1); }
            }
            if (!firstLine) cy += rowGap;
            firstLine = false;
            struct Span { size_t start, end; float h; float x; size_t k; };
            std::vector<Span> placed;
            const size_t lineStart = out_.items.size();
            const size_t lineDefer = deferStore_.size();
            auto alignOf = [&](const FlexItem& it) -> uint8_t {
                uint8_t al = cs.alignItems;
                if (it.n && St(it.n).alignSelf) { uint8_t as = St(it.n).alignSelf; al = as == 1 ? 0 : as == 2 ? 1 : as == 3 ? 2 : 3; }
                return al;
            };
            auto placeLine = [&](float stretchH) {
                placed.clear();
                float lh = 0, cx = x + start;
                for (size_t k = i; k < j; k++) {
                    FlexItem& it = items[k];
                    if (it.mlAuto) cx += autoShare;
                    size_t st = out_.items.size();
                    float fh = -1;
                    if (stretchH >= 0 && alignOf(it) == 0 && NeedsStretch(it)) fh = std::max(0.f, stretchH - it.mt - it.mb);
                    float h = LayoutFlexItem(it, cx, cy + it.mt, it.width, depth, fh) + it.mt + it.mb;
                    placed.push_back({st, out_.items.size(), h, cx, k});
                    lh = std::max(lh, h);
                    cx += it.width + it.ml + it.mr + between;
                    if (it.mrAuto) cx += autoShare;
                }
                return lh;
            };
            float lineH = placeLine(-1);
            bool singleLine = !cs.flexWrap || lineCount == 1;
            if (crossH >= 0 && singleLine && (!cs.flexWrap || j == items.size())) lineH = std::max(lineH, crossH);
            bool needStretch = false;
            for (auto& pl : placed) if (alignOf(items[pl.k]) == 0 && lineH - pl.h > 0.5f && NeedsStretch(items[pl.k])) needStretch = true;
            if (needStretch) {
                Discard(lineStart, lineDefer);
                float lh2 = placeLine(lineH);
                lineH = std::max(lineH, lh2);
            }
            for (auto& pl : placed) {
                uint8_t al = alignOf(items[pl.k]);
                if (al == 1 || al == 2) {
                    float off = al == 1 ? (lineH - pl.h) / 2 : lineH - pl.h;
                    if (off > 0.5f) Shift(pl.start, pl.end, 0, off);
                }
            }
            cy += lineH;
            i = j;
        }
        float h = cy - y;
        if (crossH >= 0) h = std::max(h, crossH);
        return h;
    }

    // flex-direction: column
    float FlexCol(const Node* n, float x, float y, float w, size_t depth, float targetH) {
        if (depth > kMaxDepth) return 0;
        const Style& cs = St(n);
        std::vector<FlexItem> items;
        CollectFlexItems(n, x, y, w, depth, items);
        struct Rec { size_t start, end; float h; float bx, bw; };
        std::vector<Rec> recs(items.size());
        // yatay boyut/konum (dikey yerlesimden bagimsiz)
        float growSum = 0;
        for (size_t k = 0; k < items.size(); k++) {
            FlexItem& it = items[k];
            uint8_t al = cs.alignItems;
            const Style* ks = it.n ? &St(it.n) : nullptr;
            if (ks && ks->alignSelf) { uint8_t as = ks->alignSelf; al = as == 1 ? 0 : as == 2 ? 1 : as == 3 ? 2 : 3; }
            float avail = std::max(0.f, w - it.ml - it.mr);
            float bw = avail;
            if (ks) {
                float extra = R(ks->p[1], w) + R(ks->p[3], w) + ks->bw[1] + ks->bw[3];
                if (ks->width.Def()) bw = R(ks->width, w) + (ks->borderBox ? 0 : extra);
                else if (al != 0 || ks->width.u == Len::Fit || it.mlAuto || it.mrAuto) bw = std::min(ItemMaxContent(it.n, depth), avail);
                if (ks->maxWidth.Def()) bw = std::min(bw, R(ks->maxWidth, w) + (ks->borderBox ? 0 : extra));
                if (ks->minWidth.Def()) bw = std::max(bw, R(ks->minWidth, w) + (ks->borderBox ? 0 : extra));
            } else if (al != 0) bw = std::min(it.basis, avail);
            float bx = x + it.ml;
            float rem = avail - bw;
            if (it.mlAuto && it.mrAuto) bx += std::max(0.f, rem / 2);
            else if (it.mlAuto) bx += std::max(0.f, rem);
            else if (al == 1) bx += std::max(0.f, rem / 2);
            else if (al == 2) bx += std::max(0.f, rem);
            recs[k].bx = bx;
            recs[k].bw = bw;
            growSum += it.grow;
        }
        const size_t colStart = out_.items.size();
        const size_t colDefer = deferStore_.size();
        auto placeAll = [&](const std::vector<float>* extraH) {
            float cy = y;
            for (size_t k = 0; k < items.size(); k++) {
                FlexItem& it = items[k];
                if (k > 0) cy += cs.gapRow;
                cy += it.mt;
                recs[k].start = out_.items.size();
                float h;
                if (!it.n) h = LayoutFlexItem(it, recs[k].bx - it.ml, cy, recs[k].bw, depth, -1);
                else {
                    BoxOpts o;
                    o.forcedW = recs[k].bw;
                    o.bfc = true;
                    if (extraH && (*extraH)[k] > 0) o.forcedH = recs[k].h + (*extraH)[k];
                    bool savedFlex = inFlexItem_;
                    inFlexItem_ = true;
                    h = Box(it.n, recs[k].bx - it.ml, cy, recs[k].bw + it.ml + it.mr, depth + 1, o);
                    inFlexItem_ = savedFlex;
                    const Style& ks = St(it.n);
                    if (ks.flexBasis.u == Len::Px && ks.flexBasis.v > h) h = ks.flexBasis.v;
                }
                recs[k].end = out_.items.size();
                if (!extraH) recs[k].h = h;
                cy += h + it.mb;
            }
            return cy - y;
        };
        float total = placeAll(nullptr);
        if (targetH > total + 0.5f && !items.empty()) {
            float free = targetH - total;
            if (growSum > 0) {
                std::vector<float> extraH(items.size(), 0);
                for (size_t k = 0; k < items.size(); k++) if (items[k].grow > 0 && items[k].n) extraH[k] = free * items[k].grow / growSum;
                Discard(colStart, colDefer);
                total = placeAll(&extraH);
            } else if (cs.justify == 1 || cs.justify == 2) {
                float off = cs.justify == 1 ? free / 2 : free;
                Shift(colStart, out_.items.size(), 0, off);
            } else if (cs.justify == 3 && recs.size() > 1) {
                float step = free / float(recs.size() - 1);
                for (size_t k = 1; k < recs.size(); k++) Shift(recs[k].start, recs[k].end, 0, step * float(k));
            }
            total = std::max(total, targetH);
        }
        return total;
    }

    // ---------------- Grid ----------------

    struct GridItem {
        const Node* n = nullptr;
        std::vector<const Node*> run;
        int c0 = 0, c1 = 1, r0 = 0, r1 = 1;   // [c0, c1) sutunlar, [r0, r1) satirlar
        bool placed = false;
        float h = 0;
        size_t start = 0, end = 0, defer = 0;
        float x = 0, y = 0, w = 0;
    };

    float Grid(const Node* n, float x, float y, float w, size_t depth) {
        if (depth > kMaxDepth) return 0;
        const Style& cs = St(n);
        static const GridSpec kEmpty;
        const GridSpec& g = cs.grid ? *cs.grid : kEmpty;
        const float gapC = cs.gapCol, gapR = cs.gapRow;

        std::vector<GridItem> items;
        {
            std::vector<const Node*> textRun;
            auto flushText = [&]() {
                bool any = false;
                for (auto* t : textRun) for (wchar_t c : t->text) if (!IsWS(c)) { any = true; break; }
                if (any) { GridItem it; it.run = textRun; items.push_back(std::move(it)); }
                textRun.clear();
            };
            ForKids(n, [&](const Node* k) {
                if (k->kind == Node::Text) { textRun.push_back(k); return; }
                if (k->kind != Node::Elem) return;
                const Style& ks = St(k);
                if (ks.display == Style::None) return;
                if (OutOfFlow(ks)) { Defer(k, x, y); return; }
                flushText();
                GridItem it;
                it.n = k;
                items.push_back(std::move(it));
            });
            flushText();
            std::stable_sort(items.begin(), items.end(), [this](const GridItem& a, const GridItem& b) {
                return (a.n ? St(a.n).order : 0) < (b.n ? St(b.n).order : 0);
            });
        }

        // Sutun izleri
        std::vector<Track> cols = g.cols;
        if (!g.autoRepeat.empty()) {
            float repMin = 0;
            for (auto& t : g.autoRepeat) {
                float a = t.mn.k == 0 ? t.mn.v : t.mn.k == 1 ? t.mn.v * w / 100.f : 0;
                float b = t.mx.k == 0 ? t.mx.v : t.mx.k == 1 ? t.mx.v * w / 100.f : 0;
                repMin += std::max(std::max(a, b), 1.f);
            }
            float fixedOther = 0;
            for (auto& t : g.cols) fixedOther += t.mx.k == 0 ? t.mx.v : 0;
            int count = std::max(1, int((w - fixedOther + gapC) / (repMin + gapC)));
            count = std::min(count, 100);
            if (g.autoFit) count = std::max(1, std::min(count, int(items.size())));
            std::vector<Track> rep;
            for (int k = 0; k < count; k++) rep.insert(rep.end(), g.autoRepeat.begin(), g.autoRepeat.end());
            size_t at = size_t(std::max(0, std::min(g.autoRepeatAt, int(cols.size()))));
            cols.insert(cols.begin() + long(at), rep.begin(), rep.end());
        }
        int nCols = int(cols.size());
        for (auto& row : g.areas) nCols = std::max(nCols, int(row.size()));
        // Ozel alan adlari
        auto areaRect = [&](const std::wstring& name, int& c0, int& c1, int& r0, int& r1) {
            bool found = false;
            for (size_t r = 0; r < g.areas.size(); r++)
                for (size_t c = 0; c < g.areas[r].size(); c++)
                    if (g.areas[r][c] == name) {
                        if (!found) { c0 = int(c); c1 = int(c) + 1; r0 = int(r); r1 = int(r) + 1; found = true; }
                        else { c0 = std::min(c0, int(c)); c1 = std::max(c1, int(c) + 1); r0 = std::min(r0, int(r)); r1 = std::max(r1, int(r) + 1); }
                    }
            return found;
        };
        // acik cizgi numaralarindan en buyugu
        for (auto& it : items) {
            if (!it.n) continue;
            const Style& ks = St(it.n);
            if (ks.gcs > 0) nCols = std::max(nCols, int(ks.gcs) - 1 + int(ks.gspanC));
            if (ks.gce > 1) nCols = std::max(nCols, int(ks.gce) - 1);
        }
        if (nCols <= 0) nCols = 1;
        nCols = std::min(nCols, 200);

        // Yerlestirme
        std::vector<std::vector<char>> occ;
        auto ensureRows = [&](int r) { while (int(occ.size()) < r) occ.push_back(std::vector<char>(size_t(nCols), 0)); };
        auto fits = [&](int r0, int c0, int rs, int csp) {
            ensureRows(r0 + rs);
            for (int r = r0; r < r0 + rs; r++) for (int c = c0; c < c0 + csp; c++) if (c >= nCols || occ[size_t(r)][size_t(c)]) return false;
            return true;
        };
        auto mark = [&](GridItem& it) {
            ensureRows(it.r1);
            for (int r = it.r0; r < it.r1; r++) for (int c = it.c0; c < std::min(it.c1, nCols); c++) occ[size_t(r)][size_t(c)] = 1;
            it.placed = true;
        };
        auto lineToIdx = [&](int line, int count) { return line > 0 ? line - 1 : count + 1 + line; };
        // 1) acik konumlular
        for (auto& it : items) {
            if (!it.n) continue;
            const Style& ks = St(it.n);
            int c0, c1, r0, r1;
            if (!ks.gridArea.empty() && areaRect(ks.gridArea, c0, c1, r0, r1)) {
                it.c0 = c0; it.c1 = c1; it.r0 = r0; it.r1 = r1;
                mark(it);
                continue;
            }
            bool hasC = ks.gcs != 0 || ks.gce != 0, hasR = ks.grs != 0 || ks.gre != 0;
            int spanC = std::min(int(ks.gspanC), nCols), spanR = ks.gspanR;
            if (hasC) {
                int a = ks.gcs ? lineToIdx(ks.gcs, nCols) : -1;
                int b = ks.gce ? lineToIdx(ks.gce, nCols) : -1;
                if (a >= 0 && b > a) { c0 = a; c1 = b; }
                else if (a >= 0) { c0 = a; c1 = a + spanC; }
                else if (b > 0) { c1 = b; c0 = std::max(0, b - spanC); }
                else { c0 = 0; c1 = spanC; }
                c0 = std::max(0, std::min(c0, nCols - 1));
                c1 = std::max(c0 + 1, std::min(c1, nCols));
                it.c0 = c0; it.c1 = c1;
            }
            if (hasR) {
                int rowsCount = std::max(int(g.rows.size()), int(g.areas.size()));
                int a = ks.grs ? lineToIdx(ks.grs, rowsCount) : -1;
                int b = ks.gre ? lineToIdx(ks.gre, rowsCount) : -1;
                if (a >= 0 && b > a) { r0 = a; r1 = b; }
                else if (a >= 0) { r0 = a; r1 = a + spanR; }
                else if (b > 0) { r1 = b; r0 = std::max(0, b - spanR); }
                else { r0 = 0; r1 = spanR; }
                r0 = std::max(0, std::min(r0, 500)); r1 = std::max(r0 + 1, std::min(r1, r0 + 100));
                it.r0 = r0; it.r1 = r1;
            }
            if (hasC && hasR) mark(it);
            else if (hasC) {
                int r = 0;
                while (!fits(r, it.c0, spanR, it.c1 - it.c0) && r < 2000) r++;
                it.r0 = r; it.r1 = r + spanR;
                mark(it);
            }
        }
        // 2) otomatik yerlestirme (satir oncelikli)
        int curR = 0, curC = 0;
        for (auto& it : items) {
            if (it.placed) continue;
            int spanC = 1, spanR = 1;
            int fixedRow = -1;
            if (it.n) {
                const Style& ks = St(it.n);
                spanC = std::max(1, std::min(int(ks.gspanC), nCols));
                spanR = std::max(1, std::min(int(ks.gspanR), 100));
                if (ks.grs > 0) fixedRow = ks.grs - 1;
            }
            if (fixedRow >= 0) {
                int c = 0;
                while (!fits(fixedRow, c, spanR, spanC) && c < nCols) c++;
                if (c >= nCols) c = 0;
                it.r0 = fixedRow; it.r1 = fixedRow + spanR; it.c0 = c; it.c1 = c + spanC;
                mark(it);
                continue;
            }
            int guard = 0;
            while (!fits(curR, curC, spanR, spanC) && guard++ < 100000) {
                curC++;
                if (curC + spanC > nCols) { curC = 0; curR++; }
            }
            it.r0 = curR; it.r1 = curR + spanR; it.c0 = curC; it.c1 = curC + spanC;
            mark(it);
            curC += spanC;
            if (curC >= nCols) { curC = 0; curR++; }
        }
        int nRows = std::max(int(occ.size()), std::max(int(g.rows.size()), int(g.areas.size())));
        nRows = std::max(nRows, 1);

        // Sutun genislikleri
        std::vector<float> colW(size_t(nCols), 0), colMax(size_t(nCols), 0);
        std::vector<char> isFr(size_t(nCols), 0);
        std::vector<float> fr(size_t(nCols), 0);
        std::vector<float> contentMin(size_t(nCols), 0), contentMax(size_t(nCols), 0);
        for (auto& it : items) {
            if (it.c1 - it.c0 != 1) continue;
            size_t c = size_t(it.c0);
            if (c >= size_t(nCols)) continue;
            if (it.n) {
                bool needMax = c < cols.size() ? (cols[c].mx.k >= 3 || cols[c].mn.k >= 3) : true;
                bool needMin = c < cols.size() ? (cols[c].mn.k >= 3) : true;
                if (needMax) contentMax[c] = std::max(contentMax[c], ItemMaxContent(it.n, depth) + R(St(it.n).m[1], w) + R(St(it.n).m[3], w));
                if (needMin) contentMin[c] = std::max(contentMin[c], MinContent(it.n, depth) + R(St(it.n).m[1], w) + R(St(it.n).m[3], w));
            } else {
                float best = 0, cur = 0;
                bool sp = false;
                for (auto* t : it.run) MaxContentText(t, St(t->parent), best, cur, sp);
                contentMax[c] = std::max(contentMax[c], std::max(best, cur) + 1);
                contentMin[c] = std::max(contentMin[c], std::min(std::max(best, cur), 80.f));
            }
        }
        auto resolveTS = [&](const TrackSize& t, size_t c, bool isMin) -> float {
            switch (t.k) {
            case 0: return t.v;
            case 1: return t.v * w / 100.f;
            case 4: return contentMin[c];
            case 5: return contentMax[c];
            case 3: return isMin ? contentMin[c] : contentMax[c];
            default: return 0;
            }
        };
        float fixedSum = 0, frSum = 0;
        for (int c = 0; c < nCols; c++) {
            size_t ci = size_t(c);
            Track t = ci < cols.size() ? cols[ci] : Track{{3, 0}, {3, 0}};
            float mn = resolveTS(t.mn, ci, true);
            if (t.mx.k == 2) { isFr[ci] = 1; fr[ci] = t.mx.v; frSum += t.mx.v; colW[ci] = mn; colMax[ci] = 1e9f; }
            else {
                float mx = resolveTS(t.mx, ci, false);
                colW[ci] = mn;
                colMax[ci] = std::max(mn, mx);
            }
            fixedSum += colW[ci];
        }
        float gapsW = gapC * float(nCols - 1);
        float free = w - gapsW - fixedSum;
        // auto/max-content izleri once buyur
        if (free > 0) {
            float want = 0;
            for (int c = 0; c < nCols; c++) if (!isFr[size_t(c)]) want += colMax[size_t(c)] - colW[size_t(c)];
            if (want > 0) {
                float give = frSum > 0 ? std::min(free, want) : std::min(free, want);
                for (int c = 0; c < nCols; c++) {
                    size_t ci = size_t(c);
                    if (isFr[ci]) continue;
                    float add = give * (colMax[ci] - colW[ci]) / want;
                    colW[ci] += add;
                }
                free -= give;
            }
        }
        // fr izleri kalan alani paylasir (en az kendi minimumu kadar)
        if (frSum > 0) {
            float nonFr = 0;
            for (int c = 0; c < nCols; c++) if (!isFr[size_t(c)]) nonFr += colW[size_t(c)];
            float left = std::max(0.f, w - gapsW - nonFr);
            std::vector<char> frozen(size_t(nCols), 0);
            float active = frSum;
            for (int pass = 0; pass < nCols + 1; pass++) {
                float unit = active > 0 ? left / active : 0;
                bool changed = false;
                for (int c = 0; c < nCols; c++) {
                    size_t ci = size_t(c);
                    if (!isFr[ci] || frozen[ci]) continue;
                    if (unit * fr[ci] < colW[ci] - 0.01f) { frozen[ci] = 1; left -= colW[ci]; active -= fr[ci]; changed = true; }
                }
                if (!changed) {
                    for (int c = 0; c < nCols; c++) {
                        size_t ci = size_t(c);
                        if (isFr[ci] && !frozen[ci]) colW[ci] = std::max(colW[ci], unit * fr[ci]);
                    }
                    break;
                }
                left = std::max(0.f, left);
            }
        } else if (free < 0) {
            // tasma: auto izleri min-content'e kadar daralt
            float shrinkable = 0;
            for (int c = 0; c < nCols; c++) shrinkable += std::max(0.f, colW[size_t(c)] - contentMin[size_t(c)]);
            if (shrinkable > 0) {
                float cut = std::min(-free, shrinkable);
                for (int c = 0; c < nCols; c++) {
                    size_t ci = size_t(c);
                    float can = std::max(0.f, colW[ci] - contentMin[ci]);
                    colW[ci] -= cut * can / shrinkable;
                }
            }
        }
        std::vector<float> colX(size_t(nCols) + 1, x);
        for (int c = 0; c < nCols; c++) colX[size_t(c) + 1] = colX[size_t(c)] + colW[size_t(c)] + gapC;
        // justify-content (izlerin toplami kapsayicidan dar ise)
        float totalW = colX[size_t(nCols)] - x - gapC;
        if (totalW < w - 0.5f && (cs.justify == 1 || cs.justify == 2)) {
            float off = cs.justify == 1 ? (w - totalW) / 2 : w - totalW;
            for (auto& cx : colX) cx += off;
        }

        // Ogeleri yerlestir (once y=0 ile olc, sonra satir konumuna kaydir)
        std::vector<float> rowH(size_t(nRows), 0);
        for (int r = 0; r < nRows; r++) {
            size_t ri = size_t(r);
            if (ri < g.rows.size()) {
                const Track& t = g.rows[ri];
                if (t.mn.k == 0) rowH[ri] = t.mn.v;
                if (t.mx.k == 0 && t.mn.k == 0) rowH[ri] = std::max(rowH[ri], t.mx.v);
            } else if (g.hasAutoRow && g.autoRow.mn.k == 0) rowH[ri] = g.autoRow.mn.v;
        }
        for (auto& it : items) {
            it.x = colX[size_t(it.c0)];
            it.w = std::max(0.f, colX[size_t(std::min(it.c1, nCols))] - gapC - it.x);
            it.start = out_.items.size();
            it.defer = deferStore_.size();
            it.h = LayoutGridItem(it, cs, 0, depth, -1);
            it.end = out_.items.size();
            if (it.r1 - it.r0 == 1) rowH[size_t(it.r0)] = std::max(rowH[size_t(it.r0)], it.h);
        }
        // satir yayilan ogeler: son satiri buyut
        for (auto& it : items) {
            if (it.r1 - it.r0 <= 1) continue;
            float span = gapR * float(it.r1 - it.r0 - 1);
            for (int r = it.r0; r < it.r1 && r < nRows; r++) span += rowH[size_t(r)];
            if (it.h > span) rowH[size_t(std::min(it.r1, nRows) - 1)] += it.h - span;
        }
        std::vector<float> rowY(size_t(nRows) + 1, y);
        for (int r = 0; r < nRows; r++) rowY[size_t(r) + 1] = rowY[size_t(r)] + rowH[size_t(r)] + (r + 1 < nRows ? gapR : 0);
        auto alignOf = [&](const GridItem& it) -> uint8_t {
            uint8_t al = cs.alignItems;
            if (it.n && St(it.n).alignSelf) { uint8_t as = St(it.n).alignSelf; al = as == 1 ? 0 : as == 2 ? 1 : as == 3 ? 2 : 3; }
            return al;
        };
        auto areaOf = [&](const GridItem& it, float& ry, float& areaH) {
            ry = rowY[size_t(it.r0)];
            areaH = rowY[size_t(std::min(it.r1, nRows))] - ry - (it.r1 < nRows ? gapR : 0);
        };
        bool needStretch = false;
        for (auto& it : items) {
            float ry, areaH;
            areaOf(it, ry, areaH);
            if (alignOf(it) == 0 && areaH - it.h > 0.5f && it.n && NeedsStretchNode(it.n)) needStretch = true;
        }
        if (needStretch && !items.empty()) {
            // ikinci gecis: tum ogeleri son konumlarinda (gerekirse uzatarak) yeniden yerlestir
            Discard(items.front().start, items.front().defer);
            for (auto& it : items) {
                float ry, areaH;
                areaOf(it, ry, areaH);
                uint8_t al = alignOf(it);
                bool st = al == 0 && areaH - it.h > 0.5f && it.n && NeedsStretchNode(it.n);
                float off = al == 1 ? (areaH - it.h) / 2 : al == 2 ? areaH - it.h : 0;
                it.start = out_.items.size();
                LayoutGridItem(it, cs, ry + (st ? 0 : std::max(0.f, off)), depth, st ? areaH : -1);
                it.end = out_.items.size();
            }
        } else {
            for (auto& it : items) {
                float ry, areaH;
                areaOf(it, ry, areaH);
                uint8_t al = alignOf(it);
                float off = al == 1 ? (areaH - it.h) / 2 : al == 2 ? areaH - it.h : 0;
                Shift(it.start, it.end, 0, ry + std::max(0.f, off));
            }
        }
        return rowY[size_t(nRows)] - y;
    }

    bool NeedsStretchNode(const Node* n) const {
        const Style& ks = St(n);
        if (ks.height.Def()) return false;
        return (ks.bg >> 24) || ks.bw[0] > 0 || ks.bw[2] > 0 || ks.bgImage || ks.flex || ks.gridCols || ks.radius > 0 || ks.bw[1] > 0;
    }

    float LayoutGridItem(GridItem& it, const Style& cs, float y, size_t depth, float forcedH) {
        if (!it.n) {
            Style a = St(it.run[0]->parent);
            return InlineRun(it.run, it.x, y, it.w, a, depth + 1);
        }
        const Style& ks = St(it.n);
        float ml = R(ks.m[3], it.w), mr = R(ks.m[1], it.w), mt = R(ks.m[0], it.w), mb = R(ks.m[2], it.w);
        float bw = std::max(0.f, it.w - ml - mr);
        float bx = it.x;
        uint8_t ji = cs.justifyItems;
        if (!ks.width.Def() && (ji == 1 || ji == 2 || ji == 3 || ks.m[3].u == Len::Auto || ks.m[1].u == Len::Auto)) {
            float sw = std::min(ItemMaxContent(it.n, depth), bw);
            float rem = bw - sw;
            if (ks.m[3].u == Len::Auto && ks.m[1].u == Len::Auto) bx += rem / 2;
            else if (ks.m[3].u == Len::Auto) bx += rem;
            else if (ji == 1) bx += rem / 2;
            else if (ji == 2) bx += rem;
            bw = sw;
        } else if (ks.width.Def()) {
            float extra = R(ks.p[1], it.w) + R(ks.p[3], it.w) + ks.bw[1] + ks.bw[3];
            bw = std::min(bw, R(ks.width, it.w) + (ks.borderBox ? 0 : extra));
            if (ji == 1) bx += (it.w - ml - mr - bw) / 2;
        }
        BoxOpts o;
        o.forcedW = bw;
        o.forcedH = forcedH >= 0 ? std::max(0.f, forcedH - mt - mb) : -1;
        o.bfc = true;
        bool savedFlex = inFlexItem_;
        inFlexItem_ = true;
        float h = Box(it.n, bx, y + mt, bw + ml + mr, depth + 1, o);
        inFlexItem_ = savedFlex;
        return h + mt + mb;
    }

    // ---------------- Tablolar ----------------

    void CollectRows(const Node* n, std::vector<const Node*>& rows, std::vector<const Node*>& caps, size_t depth) {
        if (depth > 8) return;
        ForKids(n, [&](const Node* k) {
            if (k->kind != Node::Elem) return;
            const Style& ks = St(k);
            if (ks.display == Style::None) return;
            if (ks.display == Style::Row) rows.push_back(k);
            else if (k->name == L"caption") caps.push_back(k);
            else if (ks.display == Style::Block) CollectRows(k, rows, caps, depth + 1);
        });
    }

    static bool IsCell(const Node* k, const Style& ks) {
        return k->kind == Node::Elem && ks.display != Style::None && (ks.display == Style::Cell || k->name == L"td" || k->name == L"th");
    }

    float CellPref(const Node* cell, size_t depth) {
        const Style& cs = St(cell);
        float best = 0, cur = 0;
        bool sp = false;
        MaxContent(cell, best, cur, sp, depth);
        return std::min(std::max(best, cur), 4000.f) + R(cs.p[1], 0) + R(cs.p[3], 0) + cs.bw[1] + cs.bw[3] + 1;
    }

    // Sutun genisliklerini hesaplar; toplam tablo genisligini (bosluklar dahil) dondurur.
    float PlanTable(const Node* t, float w, bool shrink, std::vector<float>& colW, size_t depth) {
        std::vector<const Node*> rows, caps;
        CollectRows(t, rows, caps, 0);
        const float sp = St(t).cellSpacing;
        int cols = 0;
        for (auto* r : rows) {
            int c = 0;
            for (auto& k : r->kids) if (IsCell(k.get(), St(k.get()))) c += St(k.get()).colspan;
            cols = std::max(cols, c);
        }
        cols = std::min(cols, 64);
        colW.assign(size_t(cols), 0);
        if (cols == 0) return 0;
        std::vector<float> fixedW(size_t(cols), -1), pref(size_t(cols), 0);
        float avail = std::max(0.f, w - sp * float(cols + 1));
        for (auto* r : rows) {
            int c = 0;
            for (auto& kp : r->kids) {
                const Node* k = kp.get();
                const Style& ks = St(k);
                if (!IsCell(k, ks)) continue;
                if (c >= cols) break;
                int span = std::min(ks.colspan, cols - c);
                if (span == 1) {
                    if (ks.width.Def())
                        fixedW[size_t(c)] = std::max(fixedW[size_t(c)], R(ks.width, avail));
                    pref[size_t(c)] = std::max(pref[size_t(c)], CellPref(k, depth + 1));
                }
                c += span;
            }
        }
        float fixedSum = 0, prefSum = 0;
        int autoN = 0;
        for (int c = 0; c < cols; c++) {
            if (fixedW[size_t(c)] >= 0) fixedSum += fixedW[size_t(c)];
            else { autoN++; prefSum += pref[size_t(c)]; }
        }
        float remain = avail - fixedSum;
        if (remain < 0) {
            float f = fixedSum > 0 ? avail / fixedSum : 0;
            for (int c = 0; c < cols; c++) if (fixedW[size_t(c)] >= 0) colW[size_t(c)] = fixedW[size_t(c)] * f;
            remain = 0;
        } else {
            for (int c = 0; c < cols; c++) if (fixedW[size_t(c)] >= 0) colW[size_t(c)] = fixedW[size_t(c)];
        }
        if (autoN > 0) {
            if (prefSum <= remain) {
                float extra = shrink ? 0 : remain - prefSum;
                for (int c = 0; c < cols; c++)
                    if (fixedW[size_t(c)] < 0)
                        colW[size_t(c)] = pref[size_t(c)] + (prefSum > 0 ? extra * pref[size_t(c)] / prefSum : extra / float(autoN));
            } else {
                for (int c = 0; c < cols; c++)
                    if (fixedW[size_t(c)] < 0)
                        colW[size_t(c)] = prefSum > 0 ? remain * pref[size_t(c)] / prefSum : remain / float(autoN);
            }
        } else if (!shrink && fixedSum > 0 && fixedSum < avail) {
            float f = avail / fixedSum;
            for (auto& c : colW) c *= f;
        }
        float total = sp * float(cols + 1);
        for (float c : colW) total += c;
        return total;
    }

    float TableContent(const Node* t, float x, float y, float w, size_t depth) {
        std::vector<const Node*> rows, caps;
        CollectRows(t, rows, caps, 0);
        float cy = y;
        for (auto* c : caps) cy += Box(c, x, cy, w, depth + 1);
        std::vector<float> colW;
        PlanTable(t, w, false, colW, depth);
        const int cols = int(colW.size());
        if (cols == 0) return cy - y;
        const float sp = St(t).cellSpacing;
        cy += sp;

        struct CellRec { size_t bg, start, end; float x, w, h; const Style* st; };
        for (auto* r : rows) {
            const Style& rs = St(r);
            size_t rowBg = (rs.bg >> 24) ? Reserve() : size_t(-1);
            std::vector<CellRec> recs;
            float rowH = 0, cx = x + sp;
            int c = 0;
            for (auto& kp : r->kids) {
                const Node* k = kp.get();
                const Style& ks = St(k);
                if (!IsCell(k, ks)) continue;
                if (c >= cols) break;
                int span = std::min(ks.colspan, cols - c);
                float cw = sp * float(span - 1);
                for (int s = 0; s < span; s++) cw += colW[size_t(c + s)];
                size_t bg = (ks.bg >> 24) ? Reserve() : size_t(-1);
                float pl = R(ks.p[3], cw) + ks.bw[3], pr = R(ks.p[1], cw) + ks.bw[1];
                float pt = R(ks.p[0], cw) + ks.bw[0], pb = R(ks.p[2], cw) + ks.bw[2];
                size_t start = out_.items.size();
                std::vector<FloatBox> saved;
                saved.swap(floats_);
                float savedTop = lastFloatTop_;
                lastFloatTop_ = -1e9f;
                bool positioned = ks.position != 0;
                CB cellCB{cx, cy, cw, -1, {}};
                if (positioned) cbStack_.push_back(&cellCB);
                float inner = BlockChildren(k, cx + pl, cy + pt, std::max(0.f, cw - pl - pr), depth + 1);
                float fb = FloatsBottom();
                if (fb > cy + pt + inner) inner = fb - cy - pt;
                floats_.swap(saved);
                lastFloatTop_ = savedTop;
                float h = inner + pt + pb;
                if (ks.height.u == Len::Px) h = std::max(h, ks.height.v);
                if (positioned) { cellCB.h = h; cbStack_.pop_back(); ProcessAbs(cellCB, depth); }
                recs.push_back({bg, start, out_.items.size(), cx, cw, h, &ks});
                rowH = std::max(rowH, h);
                cx += cw + sp;
                c += span;
            }
            if (rs.height.u == Len::Px) rowH = std::max(rowH, rs.height.v);
            if (rowBg != size_t(-1)) {
                DisplayItem& d = out_.items[rowBg];
                d.x = x; d.y = cy - sp / 2; d.w = w; d.h = rowH + sp; d.color = rs.bg;
            }
            for (auto& rc : recs) {
                float off = 0;
                uint8_t va = rc.st->valign;
                if (va == 1) off = (rowH - rc.h) / 2;
                else if (va == 2) off = rowH - rc.h;
                if (off > 0.5f) Shift(rc.start, rc.end, 0, off);
                if (rc.bg != size_t(-1)) {
                    DisplayItem& d = out_.items[rc.bg];
                    d.x = rc.x; d.y = cy; d.w = rc.w; d.h = rowH; d.color = rc.st->bg;
                }
                Borders(*rc.st, rc.x, cy, rc.w, rowH, 0);
            }
            cy += rowH + sp;
        }
        return cy - y;
    }
};

} // namespace

void LayoutPage(const Document& doc, const std::vector<Style>& styles, float viewportWidth, ITextMeasurer& tm, PageLayout& out, IImageProvider* images, float viewportHeight) {
    out.background = 0xFFFFFFFF;
    Layouter l(doc, styles, tm, out, images, viewportHeight);
    l.Run(viewportWidth);
}

} // namespace mer
