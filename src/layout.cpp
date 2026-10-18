// merOS Shield - MerEngine : yerleşim (layout) motoru
// Blok akisi, satir ici metin (kelime kaydirma, hizalama, taban cizgisi),
// liste isaretleri ve otomatik genislikli tablolar.
#include "layout.h"
#include "url.h"
#include "util.h"
#include <algorithm>
#include <cmath>

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
    DisplayItem::Role role = DisplayItem::None;
    int node = -1;
    float w = 0, lh = 0;
    FontMetrics fm;
};

struct Placed { size_t idx; float x; };

float Collapse(float a, float b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a < 0 && b < 0) return std::min(a, b);
    return a + b;
}

float R(const Len& l, float base) {
    if (l.u == Len::Px) return l.v;
    if (l.u == Len::Pct) return l.v * base / 100.f;
    return 0;
}

class Layouter {
public:
    Layouter(const Document& d, const std::vector<Style>& s, ITextMeasurer& tm, PageLayout& out, IImageProvider* img)
        : doc_(d), S_(s), tm_(tm), out_(out), img_(img), memo_(s.size(), int8_t(-1)) {}

    struct ContainingBlock {
        float x = 0, y = 0, w = 800, h = 600;
    };

    void Run(float vw) {
        out_.items.clear();
        out_.links.clear();
        out_.width = vw;
        ContainingBlock cb{0, 0, vw, 600};
        out_.height = BlockChildren(doc_.root.get(), 0, 0, vw, 0, cb);
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
        std::stable_sort(out_.items.begin(), out_.items.end(), [](const DisplayItem& a, const DisplayItem& b) {
            return a.zIndex < b.zIndex;
        });
    }

private:
    const Document& doc_;
    const std::vector<Style>& S_;
    ITextMeasurer& tm_;
    PageLayout& out_;
    IImageProvider* img_;
    std::vector<int8_t> memo_;
    std::wstring marker_;
    const Style* markerStyle_ = nullptr;
    float curWidth_ = 800;
    float forceInputW_ = 0;
    bool inFlexItem_ = false;

    static bool IsControl(const Node* n) {
        return n->kind == Node::Elem && (n->name == L"input" || n->name == L"textarea" || n->name == L"select");
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

    const Style& St(const Node* n) const {
        static const Style kNone = [] { Style s; s.display = Style::None; return s; }();
        return (n->id >= 0 && size_t(n->id) < S_.size()) ? S_[size_t(n->id)] : kNone;
    }

    bool HasBlockDesc(const Node* n, size_t depth) {
        if (n->id < 0 || size_t(n->id) >= memo_.size()) return false;
        if (memo_[n->id] >= 0) return memo_[n->id] == 1;
        bool r = false;
        if (depth < kMaxDepth) {
            for (auto& k : n->kids) {
                if (k->kind != Node::Elem) continue;
                const Style& ks = St(k.get());
                if (ks.display == Style::None) continue;
                if (ks.display != Style::Inline || HasBlockDesc(k.get(), depth + 1)) { r = true; break; }
            }
        }
        memo_[n->id] = r ? 1 : 0;
        return r;
    }

    bool IsBlockLevel(const Node* n, size_t depth) {
        if (n->kind != Node::Elem) return false;
        const Style& s = St(n);
        if (s.display == Style::None) return false;
        if (s.display != Style::Inline) return true;
        return HasBlockDesc(n, depth);
    }

    size_t Reserve() {
        out_.items.emplace_back();
        out_.items.back().kind = DisplayItem::Fill;
        return out_.items.size() - 1;
    }

    void FillRect(float x, float y, float w, float h, uint32_t c) {
        if (w <= 0 || h <= 0 || !(c >> 24)) return;
        DisplayItem d;
        d.kind = DisplayItem::Fill;
        d.x = x; d.y = y; d.w = w; d.h = h; d.color = c;
        out_.items.push_back(std::move(d));
    }

    void Borders(const Style& s, float x, float y, float w, float h) {
        if (s.borderRadius > 0 && s.bw[0] > 0) {
            DisplayItem d;
            d.kind = DisplayItem::Stroke;
            d.x = x + s.bw[0] * 0.5f; d.y = y + s.bw[0] * 0.5f;
            d.w = std::max(0.f, w - s.bw[0]); d.h = std::max(0.f, h - s.bw[0]);
            d.color = s.bc[0];
            d.radius = s.borderRadius;
            out_.items.push_back(std::move(d));
            return;
        }
        if (s.bw[0] > 0) FillRect(x, y, w, s.bw[0], s.bc[0]);
        if (s.bw[2] > 0) FillRect(x, y + h - s.bw[2], w, s.bw[2], s.bc[2]);
        if (s.bw[3] > 0) FillRect(x, y, s.bw[3], h, s.bc[3]);
        if (s.bw[1] > 0) FillRect(x + w - s.bw[1], y, s.bw[1], h, s.bc[1]);
    }

    // ---------------- Blok akisi ----------------

    float BlockChildren(const Node* n, float x, float y, float w, size_t depth, const ContainingBlock& cb = ContainingBlock(), float gap = 0) {
        if (depth > kMaxDepth) return 0;
        bool firstBlock = true;
        float cy = y, prevMB = 0;
        std::vector<const Node*> run;
        auto flush = [&]() {
            if (run.empty()) return;
            float h = InlineRun(run, x, cy, w, St(n), depth);
            run.clear();
            if (h > 0) { cy += prevMB; prevMB = 0; cy += h; }
        };

        float floatX = x;
        float floatRowH = 0;
        bool floatActive = false;
        auto clearFloats = [&]() {
            if (floatActive) {
                cy = std::max(cy, cy + floatRowH);
                floatX = x;
                floatRowH = 0;
                floatActive = false;
            }
        };

        for (auto& kp : n->kids) {
            const Node* k = kp.get();
            if (k->kind == Node::Text) { run.push_back(k); continue; }
            if (k->kind != Node::Elem) continue;
            const Style& ks = St(k);
            if (ks.display == Style::None) continue;

            if (ks.position >= 2) {
                float ml = R(ks.m[3], cb.w), mr = R(ks.m[1], cb.w);
                float mt = R(ks.m[0], cb.w), mb = R(ks.m[2], cb.w);
                float itemW = (ks.width.u == Len::Px) ? ks.width.v : (ks.width.u == Len::Pct ? R(ks.width, cb.w) : cb.w);
                float absX = x + ml;
                if (ks.left.u != Len::Auto) absX = cb.x + R(ks.left, cb.w) + ml;
                else if (ks.right.u != Len::Auto) absX = cb.x + cb.w - R(ks.right, cb.w) - itemW - mr;

                float absY = cy + mt;
                if (ks.top.u != Len::Auto) absY = cb.y + R(ks.top, cb.h) + mt;
                else if (ks.bottom.u != Len::Auto) absY = cb.y + std::max(cb.h, 600.f) - R(ks.bottom, cb.h) - mb;

                Box(k, absX, absY, cb.w, depth + 1, cb);
                continue;
            }

            if (IsBlockLevel(k, depth)) {
                flush();
                if (ks.clear != 0) {
                    clearFloats();
                }
                float mt = R(ks.m[0], w), mb = R(ks.m[2], w);

                if (ks.floatSide == 1) {
                    float ml = R(ks.m[3], w), mr = R(ks.m[1], w);
                    float itemW = (ks.width.u == Len::Px) ? ks.width.v : (ks.width.u == Len::Pct ? R(ks.width, w) : 0);
                    if (itemW <= 0) itemW = ItemMaxContent(k, depth + 1);
                    float outerW = itemW + ml + mr;

                    if (floatActive && floatX + outerW > x + w + 0.5f) {
                        cy += floatRowH;
                        floatX = x;
                        floatRowH = 0;
                    }
                    float h = Box(k, floatX, cy, w, depth + 1, cb);
                    floatX += outerW;
                    floatRowH = std::max(floatRowH, h + mt + mb);
                    floatActive = true;
                    continue;
                }

                if (floatActive) {
                    clearFloats();
                }

                if (!firstBlock) cy += gap;
                firstBlock = false;
                cy += Collapse(prevMB, mt);
                prevMB = 0;
                cy += Box(k, x, cy, w, depth + 1, cb);
                prevMB = mb;
            } else {
                run.push_back(k);
            }
        }
        flush();
        if (floatActive) {
            clearFloats();
        }
        cy += prevMB;
        return cy - y;
    }

    // Eleman kutusunu (y = kenarlik ust kenari) yerlestirir; kenarlik kutusu yuksekligini dondurur.
    float Box(const Node* n, float x, float y, float availW, size_t depth, const ContainingBlock& cb = ContainingBlock()) {
        const Style& s = St(n);
        size_t itemStart = out_.items.size();
        float ml = R(s.m[3], availW), mr = R(s.m[1], availW);
        float pl = R(s.p[3], availW), pr = R(s.p[1], availW);
        float pt = R(s.p[0], availW), pb = R(s.p[2], availW);
        float bl = s.bw[3], br = s.bw[1], bt = s.bw[0], bb = s.bw[2];
        float extra = pl + pr + bl + br;

        bool autoW = (s.width.u == Len::Auto || s.width.u == Len::None);
        float cw;
        bool constrained = false;
        if (!autoW) {
            cw = R(s.width, availW);
            if (s.borderBox) cw -= extra;
            constrained = true;
        } else if (s.position >= 2) {
            cw = std::min(availW - ml - mr - extra, ItemMaxContent(n, depth + 1));
            constrained = true;
        } else if (s.display == Style::Table) {
            std::vector<float> cols;
            cw = std::min(PlanTable(n, availW - ml - mr - extra, true, cols, depth), availW - ml - mr - extra);
            constrained = true;
        } else {
            cw = availW - ml - mr - extra;
        }
        if (s.maxWidth.u == Len::Px || s.maxWidth.u == Len::Pct) {
            float mx = R(s.maxWidth, availW);
            if (s.borderBox) mx -= extra;
            if (cw > mx) { cw = mx; constrained = true; }
        }
        cw = std::max(0.f, cw);
        float outer = cw + extra;
        if (constrained && s.position < 2) {
            bool la = s.m[3].u == Len::Auto, ra = s.m[1].u == Len::Auto;
            if (la && ra) ml = std::max(0.f, (availW - outer) / 2);
            else if (la) ml = std::max(0.f, availW - outer - mr);
        }
        float bx = x + ml;
        size_t bgIdx = (s.bg >> 24) ? Reserve() : size_t(-1);
        size_t bgImgIdx = (!s.bgImage.empty()) ? Reserve() : size_t(-1);
        float cx = bx + bl + pl, cy = y + bt + pt;

        ContainingBlock childCB = cb;
        if (s.position != 0) {
            childCB.x = bx;
            childCB.y = y;
            childCB.w = outer;
            childCB.h = (s.height.u == Len::Px) ? s.height.v : 0;
        }

        std::wstring savedMarker;
        if (s.display == Style::ListItem) {
            savedMarker = marker_;
            marker_ = MarkerText(n, s);
            markerStyle_ = &s;
        }
        float ch;
        if (n->name == L"img" || n->name == L"video") {
            float iw, ih;
            std::wstring key;
            if (ImageSize(n, s, availW, iw, ih, key)) {
                EmitImage(cx, cy, iw, ih, key, -1, n->id);
                ch = ih;
            } else if (n->name == L"video") {
                float vw = 300.f, vh = 150.f;
                if (auto* wAttr = n->Attr(L"width")) { float v = float(_wtof(wAttr->c_str())); if (v > 0) vw = v; }
                if (auto* hAttr = n->Attr(L"height")) { float v = float(_wtof(hAttr->c_str())); if (v > 0) vh = v; }
                if (s.width.u == Len::Px && s.width.v > 0) vw = s.width.v;
                if (s.height.u == Len::Px && s.height.v > 0) vh = s.height.v;
                FillRect(cx, cy, vw, vh, 0xFF181818);
                DisplayItem d;
                d.kind = DisplayItem::Text;
                d.x = cx + vw / 2 - 12;
                d.y = cy + vh / 2 - 10;
                d.w = 24;
                d.h = 20;
                d.color = 0xFFFFFFFF;
                d.font = tm_.Font(s);
                d.text = L"\u25B6";
                d.node = n->id;
                out_.items.push_back(std::move(d));
                ch = vh;
            } else ch = 0;
        } else if (IsControl(n)) {
            // blok/flex ogesi olarak form denetimi: verilen genisligi doldurur
            float saved = forceInputW_;
            forceInputW_ = (constrained || s.flexGrow > 0 || inFlexItem_) ? cw : 0;
            std::vector<const Node*> one{n};
            ch = InlineRun(one, cx, cy, cw, s, depth);
            forceInputW_ = saved;
        } else if (s.display == Style::Table) ch = TableContent(n, cx, cy, cw, depth);
        else if (s.flex == 1) ch = FlexRow(n, cx, cy, cw, depth);
        else if (s.gridCols > 0) ch = Grid(n, cx, cy, cw, depth);
        else ch = BlockChildren(n, cx, cy, cw, depth, childCB, s.flex == 2 ? s.gapRow : 0.f);
        if (s.display == Style::ListItem) marker_ = savedMarker.empty() ? std::wstring() : savedMarker;
        if (s.height.u == Len::Px) ch = std::max(ch, s.height.v - (s.borderBox ? pt + pb + bt + bb : 0));

        float bh = bt + pt + ch + pb + bb;
        if (bgIdx != size_t(-1)) {
            DisplayItem& d = out_.items[bgIdx];
            d.x = bx; d.y = y; d.w = outer; d.h = bh; d.color = s.bg;
            if (s.borderRadius > 0) d.radius = s.borderRadius;
        }
        if (bgImgIdx != size_t(-1)) {
            DisplayItem& d = out_.items[bgImgIdx];
            d.kind = DisplayItem::Image;
            d.isBackground = true;
            d.x = bx + bl; d.y = y + bt; d.w = std::max(0.f, outer - bl - br); d.h = std::max(0.f, bh - bt - bb);
            d.text = StartsWith(ToLower(s.bgImage), L"data:") ? s.bgImage : ResolveUrl(doc_.baseHref, s.bgImage);
            if (s.borderRadius > 0) d.radius = s.borderRadius;

            float nw = 0, nh = 0;
            bool hasDim = img_ && img_->LookupByKey(d.text, nw, nh);
            float sx = 0, sy = 0;
            if (s.bgPosX.u == Len::Px) {
                if (s.bgPosX.v < 0) sx = -s.bgPosX.v;
                else { d.x += s.bgPosX.v; d.w = std::max(0.f, d.w - s.bgPosX.v); }
            } else if (s.bgPosX.u == Len::Pct && hasDim && nw > d.w) {
                sx = (nw - d.w) * s.bgPosX.v / 100.f;
            }
            if (s.bgPosY.u == Len::Px) {
                if (s.bgPosY.v < 0) sy = -s.bgPosY.v;
                else { d.y += s.bgPosY.v; d.h = std::max(0.f, d.h - s.bgPosY.v); }
            } else if (s.bgPosY.u == Len::Pct && hasDim && nh > d.h) {
                sy = (nh - d.h) * s.bgPosY.v / 100.f;
            }
            d.srcX = sx;
            d.srcY = sy;
            d.srcW = d.w;
            d.srcH = d.h;
        }
        Borders(s, bx, y, outer, bh);

        // position: relative offset
        if (s.position == 1) {
            float dx = (s.left.u != Len::Auto) ? R(s.left, availW) : ((s.right.u != Len::Auto) ? -R(s.right, availW) : 0);
            float dy = (s.top.u != Len::Auto) ? R(s.top, availW) : ((s.bottom.u != Len::Auto) ? -R(s.bottom, availW) : 0);
            if (dx != 0 || dy != 0) {
                for (size_t idx = itemStart; idx < out_.items.size(); idx++) {
                    out_.items[idx].x += dx;
                    out_.items[idx].y += dy;
                }
            }
        }
        // z-index propagation
        if (s.hasZIndex) {
            for (size_t idx = itemStart; idx < out_.items.size(); idx++) {
                if (out_.items[idx].zIndex == 0) out_.items[idx].zIndex = s.zIndex;
            }
        }
        return bh;
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
        if (s.textIndentOffscreen || pc.size() > 200000) return;
        int f = tm_.Font(s);
        FontMetrics fm = tm_.Metrics(f);
        float lh = LineH(s, fm);
        uint32_t bg = (s.display == Style::Inline) ? s.bg : 0;
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
            }
        }
        if (!word.empty()) emit(word, true);
    }

    void Collect(const Node* n, int link, std::vector<Piece>& pc, bool& space, size_t depth) {
        if (depth > kMaxDepth) return;
        if (n->kind == Node::Text) {
            TextPieces(n->text, St(n->parent), link, pc, space, n->parent ? n->parent->id : -1);
            return;
        }
        if (n->kind != Node::Elem) return;
        const Style& s = St(n);
        if (s.display == Style::None) return;
        const std::wstring& t = n->name;
        if (t == L"br") {
            int f = tm_.Font(s);
            Piece b; b.brk = true; b.font = f; b.fm = tm_.Metrics(f); b.lh = LineH(s, b.fm);
            pc.push_back(b);
            space = false;
            return;
        }
        if (t == L"img" || t == L"video") {
            float iw, ih;
            std::wstring key;
            if (ImageSize(n, s, curWidth_, iw, ih, key)) {
                Piece p;
                p.isImage = true;
                p.text = key;
                p.w = iw;
                p.fm = FontMetrics{ih, ih, tm_.Metrics(tm_.Font(s)).space};
                p.lh = ih;
                p.link = link;
                p.node = n->id;
                p.spaceBefore = space;
                p.canBreak = true;
                pc.push_back(std::move(p));
                space = false;
                return;
            }
            if (t == L"video") {
                float vw = 300.f, vh = 150.f;
                if (auto* wAttr = n->Attr(L"width")) { float v = float(_wtof(wAttr->c_str())); if (v > 0) vw = v; }
                if (auto* hAttr = n->Attr(L"height")) { float v = float(_wtof(hAttr->c_str())); if (v > 0) vh = v; }
                if (s.width.u == Len::Px && s.width.v > 0) vw = s.width.v;
                if (s.height.u == Len::Px && s.height.v > 0) vh = s.height.v;
                Piece p;
                p.isImage = false;
                p.w = vw;
                p.fm = FontMetrics{vh, vh, tm_.Metrics(tm_.Font(s)).space};
                p.lh = vh;
                p.link = link;
                p.node = n->id;
                p.bg = 0xFF181818;
                p.color = 0xFFFFFFFF;
                p.text = L"  \u25B6  ";
                p.spaceBefore = space;
                p.canBreak = true;
                pc.push_back(std::move(p));
                space = false;
                return;
            }
            const std::wstring* alt = n->Attr(L"alt");
            if (alt && !Trim(*alt).empty()) {
                Style a = s;
                a.italic = true;
                a.color = 0xFF6B7280;
                a.display = Style::Inline;
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
            if (type == L"checkbox") { label = checked ? L"\u2611" : L"\u2610"; role = DisplayItem::Check; a.bg = 0; }
            else if (type == L"radio") { label = checked ? L"\u25C9" : L"\u25CB"; role = DisplayItem::Check; a.bg = 0; }
            else if (type == L"submit" || type == L"button" || type == L"reset") {
                label = !val.empty() ? val : (type == L"reset" ? L"S\u0131f\u0131rla" : L"G\u00F6nder");
                label = L" " + label + L" ";
                a.bg = 0xFFE4E8EF;
                role = DisplayItem::Button;
            } else {
                if (type == L"password") label = std::wstring(val.size(), L'\u2022');
                else label = val;
                for (auto& c : label) if (c == L'\n' || c == L'\r') c = L' ';
                if (label.empty() && n->Attr(L"placeholder")) { label = *n->Attr(L"placeholder"); a.color = 0xFF8A8F99; }
                label = L" " + label;
                minW = 180;
                if (auto* sz = n->Attr(L"size")) { int c = _wtoi(sz->c_str()); if (c > 0 && c < 200) minW = float(c) * a.fontSize * 0.55f; }
                if (s.width.u == Len::Px) minW = s.width.v;
            }
            Piece p;
            p.font = tm_.Font(a); p.fm = tm_.Metrics(p.font); p.lh = LineH(a, p.fm) + 6;
            p.text = label; p.color = a.color; p.bg = a.bg; p.link = link;
            p.spaceBefore = space; p.canBreak = space;
            p.role = role;
            p.node = n->id;
            p.w = std::max(tm_.Width(p.font, p.text) + (role == DisplayItem::TextInput ? 6.f : 0.f), minW);
            if (forceInputW_ > 0 && (role == DisplayItem::TextInput || role == DisplayItem::Button)) p.w = std::max(forceInputW_ - 2, 20.f);
            if (role == DisplayItem::TextInput && p.w > curWidth_ && curWidth_ > 40) p.w = curWidth_ - 8;
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
            Style a = s;
            a.display = Style::Inline;
            a.bg = 0xFFEEF0F4;
            TextPieces(first + L" \u25BE", a, link, pc, space, n->id);
            return;
        }
        int l = link;
        if (t == L"a") {
            if (auto* h = n->Attr(L"href")) { int nl = AddLink(*h); if (nl >= 0) l = nl; }
        }
        for (auto& k : n->kids) Collect(k.get(), l, pc, space, depth + 1);
    }

    void SplitLong(std::vector<Piece>& pc, float w) {
        if (w < 30) return;
        bool any = false;
        for (auto& p : pc) if (!p.brk && !p.isImage && p.role == DisplayItem::None && p.w > w && p.text.size() >= 2) { any = true; break; }
        if (!any) return;
        std::vector<Piece> out;
        bool changed = false;
        for (auto& p : pc) {
            if (p.brk || p.isImage || p.role != DisplayItem::None || p.w <= w || p.text.size() < 2 || out.size() > 200000) { out.push_back(std::move(p)); continue; }
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
        SplitLong(pc, w);

        float lineY = y;
        float curX = 0;
        std::vector<Placed> line;

        auto flushLine = [&](int brkIdx) {
            if (line.empty()) {
                if (brkIdx >= 0) lineY += pc[size_t(brkIdx)].lh;
                return;
            }
            float above = 0, below = 0;
            for (auto& pl : line) {
                const Piece& p = pc[pl.idx];
                float half = (p.lh - p.fm.height) / 2;
                above = std::max(above, p.fm.baseline + half);
                below = std::max(below, p.fm.height - p.fm.baseline + half);
            }
            float base = lineY + above;
            float off = 0;
            if (cs.align == 1) off = std::max(0.f, (w - curX) / 2);
            else if (cs.align == 2) off = std::max(0.f, w - curX);

            if (!marker_.empty() && markerStyle_) {
                int mf = tm_.Font(*markerStyle_);
                FontMetrics mfm = tm_.Metrics(mf);
                float mw = tm_.Width(mf, marker_);
                DisplayItem d;
                d.kind = DisplayItem::Text;
                d.x = x - mw - 8; d.y = base - mfm.baseline; d.w = mw; d.h = mfm.height;
                d.color = markerStyle_->color; d.font = mf; d.text = marker_;
                out_.items.push_back(std::move(d));
                marker_.clear();
            }

            float prevEnd = -1;
            int prevLink = -2;
            for (auto& pl : line) {
                const Piece& p = pc[pl.idx];
                float px = x + off + pl.x;
                float ty = base - p.fm.baseline;
                if (p.isImage) {
                    EmitImage(px, ty, p.w, p.fm.height, p.text, p.link, p.node);
                    prevEnd = -1;
                    prevLink = -2;
                    continue;
                }
                if (p.role == DisplayItem::TextInput || p.role == DisplayItem::Button) {
                    float bh = p.fm.height + 4;
                    FillRect(px, ty - 2, p.w, bh, p.bg ? p.bg : 0xFFFFFFFF);
                    DisplayItem b;
                    b.kind = DisplayItem::Stroke;
                    b.role = p.role;
                    b.x = px; b.y = ty - 2; b.w = p.w; b.h = bh;
                    b.color = p.role == DisplayItem::Button ? 0xFFA9B1BF : 0xFF9AA3B2;
                    b.node = p.node;
                    out_.items.push_back(std::move(b));
                } else if (p.bg >> 24) FillRect(px - 2, ty, p.w + 4, p.fm.height, p.bg);
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
                    u.x = ux; u.y = base + thick + 0.5f; u.w = px + p.w - ux; u.h = thick;
                    u.color = p.color; u.link = p.link; u.node = p.node;
                    out_.items.push_back(std::move(u));
                    prevEnd = px + p.w;
                    prevLink = p.link;
                } else {
                    prevEnd = -1;
                    prevLink = -2;
                }
                if (p.strike) {
                    DisplayItem u;
                    u.kind = DisplayItem::HLine;
                    u.x = px; u.y = base - p.fm.baseline * 0.32f; u.w = p.w; u.h = thick;
                    u.color = p.color;
                    out_.items.push_back(std::move(u));
                }
            }
            lineY += above + below;
            line.clear();
            curX = 0;
        };

        size_t i = 0;
        while (i < pc.size()) {
            if (pc[i].brk) { flushLine(int(i)); i++; continue; }
            size_t j = i + 1;
            while (j < pc.size() && !pc[j].brk && !pc[j].canBreak) j++;
            float gw = 0;
            for (size_t k = i; k < j; k++) gw += pc[k].w + ((k > i && pc[k].spaceBefore) ? pc[k].fm.space : 0);
            float sw = (!line.empty() && pc[i].spaceBefore) ? pc[i].fm.space : 0;
            if (!line.empty() && curX + sw + gw > w + 0.5f) { flushLine(-1); sw = 0; }
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

    // ---------------- Resimler ----------------

    void EmitImage(float x, float y, float w, float h, const std::wstring& key, int link, int node) {
        DisplayItem d;
        d.kind = key.empty() ? DisplayItem::Fill : DisplayItem::Image;
        d.x = x; d.y = y; d.w = w; d.h = h;
        d.color = key.empty() ? 0xFFE9ECF2 : 0xFFFFFFFF;
        d.text = key; d.link = link; d.node = node;
        out_.items.push_back(std::move(d));
    }

    // Resim boyutu: CSS/oznitelik boyutu, yoksa dogal boyut; en-boy orani korunur.
    bool ImageSize(const Node* n, const Style& s, float availW, float& w, float& h, std::wstring& key) {
        float nw = 0, nh = 0;
        bool loaded = img_ && img_->Lookup(n, nw, nh, key) && nw > 0 && nh > 0;
        if (!loaded) key.clear();
        float cw = (s.width.u == Len::Px || s.width.u == Len::Pct) ? R(s.width, availW) : -1;
        float ch = (s.height.u == Len::Px) ? s.height.v : -1;
        if (cw >= 0 && ch >= 0) { w = cw; h = ch; }
        else if (cw >= 0) { w = cw; h = loaded ? cw * nh / nw : -1; }
        else if (ch >= 0) { h = ch; w = loaded ? ch * nw / nh : -1; }
        else if (loaded) { w = nw; h = nh; }
        else return false;
        if (w < 0 || h < 0) return false;
        if (s.maxWidth.u == Len::Px || s.maxWidth.u == Len::Pct) {
            float mx = R(s.maxWidth, availW);
            if (w > mx && mx > 0) { h = h * mx / w; w = mx; }
        }
        if (w > availW && availW > 0 && availW < 99999.f) { h = h * availW / w; w = availW; }
        if (!loaded && (w < 2 || h < 2)) return false;
        w = std::min(w, 20000.f);
        h = std::min(h, 20000.f);
        return true;
    }

    // ---------------- Flexbox ve Grid ----------------

    float ItemMaxContent(const Node* k, size_t depth) {
        const Style& ks = St(k);
        float extra = R(ks.p[1], 0) + R(ks.p[3], 0) + ks.bw[1] + ks.bw[3];
        if (k->name == L"img" || k->name == L"video") {
            float iw, ih;
            std::wstring key;
            return ImageSize(k, ks, 100000.f, iw, ih, key) ? iw + extra : (k->name == L"video" ? 300.f + extra : extra);
        }
        if (ks.width.u == Len::Px) return ks.width.v + (ks.borderBox ? 0 : extra);
        if (IsControl(k)) return ControlWidth(k, depth) + extra + 1;
        if (ks.flex == 1 && depth < kMaxDepth) {
            // ic ice flex satiri: cocuklarin dogal genislikleri + bosluklar
            float sum = 0;
            int n = 0;
            for (auto& kp : k->kids) {
                const Node* c = kp.get();
                if (c->kind != Node::Elem || St(c).display == Style::None) continue;
                const Style& cs = St(c);
                sum += ItemMaxContent(c, depth + 1) + R(cs.m[1], 0) + R(cs.m[3], 0);
                n++;
            }
            if (n > 1) sum += ks.gapCol * float(n - 1);
            return sum + extra + 1;
        }
        float best = 0, cur = 0;
        bool sp = false;
        MaxContent(k, best, cur, sp, depth + 1);
        return std::max(best, cur) + extra + 1;
    }

    struct FlexItem { const Node* n; float outer, grow, mt, mb; };

    float FlexRow(const Node* n, float x, float y, float w, size_t depth) {
        if (depth > kMaxDepth) return 0;
        const Style& cs = St(n);
        std::vector<FlexItem> items;
        for (auto& kp : n->kids) {
            const Node* k = kp.get();
            if (k->kind != Node::Elem) continue; // flex kapsayicisinda serbest metin nadirdir
            const Style& ks = St(k);
            if (ks.display == Style::None) continue;
            float ml = R(ks.m[3], w), mr = R(ks.m[1], w);
            float extra = R(ks.p[1], w) + R(ks.p[3], w) + ks.bw[1] + ks.bw[3];
            float basis;
            if (ks.flexBasis.u == Len::Pct || (ks.flexBasis.u == Len::Px && !(ks.flexBasis.v == 0 && ks.flexGrow == 0))) {
                basis = R(ks.flexBasis, w);
            } else if (ks.width.u == Len::Px || ks.width.u == Len::Pct) {
                basis = R(ks.width, w) + (ks.borderBox ? 0 : extra);
            } else {
                basis = ItemMaxContent(k, depth);
            }
            if (ks.maxWidth.u == Len::Px || ks.maxWidth.u == Len::Pct) basis = std::min(basis, R(ks.maxWidth, w));
            basis = std::min(std::max(basis, 0.f), w);
            items.push_back({k, basis + ml + mr, ks.flexGrow, R(ks.m[0], w), R(ks.m[2], w)});
        }
        std::stable_sort(items.begin(), items.end(), [this](const FlexItem& a, const FlexItem& b) { return St(a.n).order < St(b.n).order; });
        const float gap = cs.gapCol, rowGap = cs.gapRow;
        float cy = y;
        size_t i = 0;
        bool firstLine = true;
        while (i < items.size()) {
            size_t j = i;
            float sum = 0;
            while (j < items.size()) {
                float add = items[j].outer + (j > i ? gap : 0);
                if (cs.flexWrap && j > i && sum + add > w + 0.5f) break;
                sum += add;
                j++;
            }
            float free = w - sum, growSum = 0, tot = 0;
            for (size_t k = i; k < j; k++) { growSum += items[k].grow; tot += items[k].outer; }
            std::vector<float> widths;
            for (size_t k = i; k < j; k++) {
                float ow = items[k].outer;
                if (free > 0 && growSum > 0) ow += free * items[k].grow / growSum;
                else if (free < 0 && tot > 0) ow = std::max(ow + free * items[k].outer / tot, std::min(ow, 24.f));
                widths.push_back(ow);
            }
            float used = 0;
            for (size_t k = 0; k < widths.size(); k++) used += widths[k] + (k ? gap : 0);
            float rem = std::max(0.f, w - used), start = 0, between = gap;
            size_t cnt = j - i;
            if (rem > 0.5f && growSum == 0) {
                if (cs.justify == 1) start = rem / 2;
                else if (cs.justify == 2) start = rem;
                else if (cs.justify == 3 && cnt > 1) between = gap + rem / float(cnt - 1);
                else if (cs.justify == 4) { between = gap + rem / float(cnt); start = rem / float(cnt) / 2; }
            }
            if (!firstLine) cy += rowGap;
            firstLine = false;
            struct Span { size_t start, end; float h; };
            std::vector<Span> placed;
            float lineH = 0, cx = x + start;
            for (size_t k = i; k < j; k++) {
                const FlexItem& it = items[k];
                size_t st = out_.items.size();
                bool savedFlex = inFlexItem_;
                inFlexItem_ = true;
                float h = Box(it.n, cx, cy + it.mt, widths[k - i], depth + 1) + it.mt + it.mb;
                inFlexItem_ = savedFlex;
                placed.push_back({st, out_.items.size(), h});
                lineH = std::max(lineH, h);
                cx += widths[k - i] + between;
            }
            if (cs.alignItems == 1 || cs.alignItems == 2) {
                for (auto& pl : placed) {
                    float off = cs.alignItems == 1 ? (lineH - pl.h) / 2 : lineH - pl.h;
                    if (off > 0.5f) for (size_t q = pl.start; q < pl.end; q++) out_.items[q].y += off;
                }
            }
            cy += lineH;
            i = j;
        }
        return cy - y;
    }

    float Grid(const Node* n, float x, float y, float w, size_t depth) {
        const Style& cs = St(n);
        int cols = std::max(1, cs.gridCols);
        // Dar alanda sutun sayisini azalt (duyarli tasarimlar icin)
        while (cols > 1 && (w - cs.gapCol * float(cols - 1)) / float(cols) < 140.f) cols--;
        float colW = (w - cs.gapCol * float(cols - 1)) / float(cols);
        float cy = y, rowH = 0;
        int c = 0;
        bool firstRow = true;
        for (auto& kp : n->kids) {
            const Node* k = kp.get();
            if (k->kind != Node::Elem) continue;
            const Style& ks = St(k);
            if (ks.display == Style::None) continue;
            if (c == 0) { if (!firstRow) cy += cs.gapRow; firstRow = false; rowH = 0; }
            float mt = R(ks.m[0], colW), mb = R(ks.m[2], colW);
            float h = Box(k, x + float(c) * (colW + cs.gapCol), cy + mt, colW, depth + 1) + mt + mb;
            rowH = std::max(rowH, h);
            if (++c == cols) { c = 0; cy += rowH; }
        }
        if (c != 0) cy += rowH;
        return cy - y;
    }

    // ---------------- Tablolar ----------------

    void CollectRows(const Node* n, std::vector<const Node*>& rows, std::vector<const Node*>& caps, size_t depth) {
        if (depth > 8) return;
        for (auto& k : n->kids) {
            if (k->kind != Node::Elem) continue;
            const Style& ks = St(k.get());
            if (ks.display == Style::None) continue;
            if (ks.display == Style::Row) rows.push_back(k.get());
            else if (k->name == L"caption") caps.push_back(k.get());
            else if (ks.display == Style::Block) CollectRows(k.get(), rows, caps, depth + 1);
        }
    }

    static bool IsCell(const Node* k, const Style& ks) {
        return k->kind == Node::Elem && ks.display != Style::None && (ks.display == Style::Cell || k->name == L"td" || k->name == L"th");
    }

    void MaxContent(const Node* n, float& best, float& cur, bool& sp, size_t depth) {
        if (depth > kMaxDepth) return;
        for (auto& kp : n->kids) {
            const Node* k = kp.get();
            if (k->kind == Node::Text) {
                const Style& s = St(n);
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
                    else if (IsWS(c) && !s.nowrap) { addWord(); sp = true; }
                    else word += c;
                }
                addWord();
                continue;
            }
            if (k->kind != Node::Elem) continue;
            const Style& ks = St(k);
            if (ks.display == Style::None) continue;
            if (k->name == L"br") { best = std::max(best, cur); cur = 0; sp = false; continue; }
            if (k->name == L"img") {
                float iw, ih;
                std::wstring key;
                if (ImageSize(k, ks, 100000.f, iw, ih, key)) cur += iw;
                continue;
            }
            if (IsControl(k)) { cur += ControlWidth(k, depth); continue; }
            if (ks.display != Style::Inline) {
                best = std::max(best, cur);
                cur = 0;
                float b2 = 0, c2 = 0;
                bool s2 = false;
                MaxContent(k, b2, c2, s2, depth + 1);
                float sub = std::max(b2, c2) + R(ks.p[1], 0) + R(ks.p[3], 0) + ks.bw[1] + ks.bw[3] + R(ks.m[1], 0) + R(ks.m[3], 0);
                if (ks.width.u == Len::Px) sub = ks.width.v;
                best = std::max(best, sub);
            } else {
                MaxContent(k, best, cur, sp, depth + 1);
            }
        }
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
        float avail = std::max(0.f, w - sp * (cols + 1));
        for (auto* r : rows) {
            int c = 0;
            for (auto& kp : r->kids) {
                const Node* k = kp.get();
                const Style& ks = St(k);
                if (!IsCell(k, ks)) continue;
                if (c >= cols) break;
                int span = std::min(ks.colspan, cols - c);
                if (span == 1) {
                    if (ks.width.u == Len::Px || ks.width.u == Len::Pct)
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
                        colW[size_t(c)] = pref[size_t(c)] + (prefSum > 0 ? extra * pref[size_t(c)] / prefSum : extra / autoN);
            } else {
                for (int c = 0; c < cols; c++)
                    if (fixedW[size_t(c)] < 0)
                        colW[size_t(c)] = prefSum > 0 ? remain * pref[size_t(c)] / prefSum : remain / autoN;
            }
        } else if (!shrink && fixedSum > 0 && fixedSum < avail) {
            float f = avail / fixedSum;
            for (auto& c : colW) c *= f;
        }
        float total = sp * (cols + 1);
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
                float h = BlockChildren(k, cx + pl, cy + pt, std::max(0.f, cw - pl - pr), depth + 1) + pt + pb;
                if (ks.height.u == Len::Px) h = std::max(h, ks.height.v);
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
                if (off > 0.5f) for (size_t k = rc.start; k < rc.end; k++) out_.items[k].y += off;
                if (rc.bg != size_t(-1)) {
                    DisplayItem& d = out_.items[rc.bg];
                    d.x = rc.x; d.y = cy; d.w = rc.w; d.h = rowH; d.color = rc.st->bg;
                }
                Borders(*rc.st, rc.x, cy, rc.w, rowH);
            }
            cy += rowH + sp;
        }
        return cy - y;
    }
};

} // namespace

void LayoutPage(const Document& doc, const std::vector<Style>& styles, float viewportWidth, ITextMeasurer& tm, PageLayout& out, IImageProvider* images) {
    out.background = 0xFFFFFFFF;
    Layouter l(doc, styles, tm, out, images);
    l.Run(viewportWidth);
}

} // namespace mer
