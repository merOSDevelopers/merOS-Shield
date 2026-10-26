// merOS Shield - MerEngine : SVG çizici (bkz. svg.h)
#include "svg.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <unordered_map>
#include "css.h"
#include "dom.h"
#include "style.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#endif

namespace mer {

namespace {

constexpr int kMaxSvgNodes = 30000;
constexpr int kMaxSvgDepth = 64;
constexpr size_t kMaxSegs = 2000000;

// ---------------- XML ayristirma ----------------

struct SvgDoc {
    std::unique_ptr<Node> root;
    const Node* svg = nullptr;
    std::unordered_map<std::wstring, const Node*> ids;
    StyleSheet sheet;
    bool hasSheet = false;
    int count = 0;
};

bool NameChar(wchar_t c) { return iswalnum(c) || c == L'-' || c == L':' || c == L'_' || c == L'.'; }

void ParseXml(const std::wstring& s, SvgDoc& d) {
    d.root.reset(new Node());
    d.root->kind = Node::Doc;
    std::vector<Node*> stack{d.root.get()};
    size_t i = 0, n = s.size();
    std::wstring styleText;
    while (i < n && d.count < kMaxSvgNodes) {
        if (s[i] != L'<') {
            size_t e = s.find(L'<', i);
            if (e == std::wstring::npos) e = n;
            Node* cur = stack.back();
            if (cur->name == L"style") styleText += DecodeEntities(s.substr(i, e - i));
            i = e;
            continue;
        }
        if (s.compare(i, 4, L"<!--") == 0) { size_t e = s.find(L"-->", i + 4); i = e == std::wstring::npos ? n : e + 3; continue; }
        if (s.compare(i, 9, L"<![CDATA[") == 0) {
            size_t e = s.find(L"]]>", i + 9);
            if (e == std::wstring::npos) e = n;
            if (stack.back()->name == L"style") styleText += s.substr(i + 9, e - i - 9);
            i = e == n ? n : e + 3;
            continue;
        }
        if (s.compare(i, 2, L"<!") == 0) {
            // DOCTYPE (ic altkume dahil)
            size_t e = i + 2;
            int br = 0;
            while (e < n && !(s[e] == L'>' && br == 0)) { if (s[e] == L'[') br++; else if (s[e] == L']') br--; e++; }
            i = e + 1;
            continue;
        }
        if (s.compare(i, 2, L"<?") == 0) { size_t e = s.find(L"?>", i); i = e == std::wstring::npos ? n : e + 2; continue; }
        if (i + 1 < n && s[i + 1] == L'/') {
            size_t j = i + 2;
            std::wstring nm;
            while (j < n && NameChar(s[j])) nm += wchar_t(towlower(s[j++]));
            size_t e = s.find(L'>', j);
            i = e == std::wstring::npos ? n : e + 1;
            for (size_t k = stack.size(); k-- > 1;) {
                if (stack[k]->name == nm) { stack.resize(k); break; }
            }
            continue;
        }
        size_t j = i + 1;
        std::wstring nm;
        while (j < n && NameChar(s[j])) nm += wchar_t(towlower(s[j++]));
        if (nm.empty()) { i++; continue; }
        // svg: ad alani onekini at (svg:path -> path)
        size_t colon = nm.find(L':');
        if (colon != std::wstring::npos && nm.compare(0, colon, L"svg") == 0) nm = nm.substr(colon + 1);
        std::unique_ptr<Node> el(new Node());
        el->kind = Node::Elem;
        el->name = nm;
        bool selfClose = false;
        while (j < n) {
            while (j < n && IsWS(s[j])) j++;
            if (j >= n) break;
            if (s[j] == L'>') { j++; break; }
            if (s[j] == L'/' && j + 1 < n && s[j + 1] == L'>') { selfClose = true; j += 2; break; }
            std::wstring an;
            while (j < n && !IsWS(s[j]) && s[j] != L'=' && s[j] != L'>' && s[j] != L'/') an += wchar_t(towlower(s[j++]));
            while (j < n && IsWS(s[j])) j++;
            std::wstring av;
            if (j < n && s[j] == L'=') {
                j++;
                while (j < n && IsWS(s[j])) j++;
                if (j < n && (s[j] == L'"' || s[j] == L'\'')) {
                    wchar_t q = s[j++];
                    size_t e = s.find(q, j);
                    if (e == std::wstring::npos) e = n;
                    av = DecodeEntities(s.substr(j, e - j));
                    j = e < n ? e + 1 : n;
                } else {
                    size_t st = j;
                    while (j < n && !IsWS(s[j]) && s[j] != L'>') j++;
                    av = s.substr(st, j - st);
                }
            }
            if (an.empty()) { if (j < n && s[j] != L'>') j++; continue; }
            if (el->attrs.size() < 128) el->attrs.emplace_back(an, av);
        }
        i = j;
        if (auto* c = el->Attr(L"class")) for (auto& w : SplitWS(*c)) el->classes.push_back(w);
        if (auto* id = el->Attr(L"id")) el->idAttr = *id;
        Node* parent = stack.back();
        el->parent = parent;
        el->id = d.count++;
        Node* raw = el.get();
        parent->kids.push_back(std::move(el));
        if (!raw->idAttr.empty() && !d.ids.count(raw->idAttr)) d.ids[raw->idAttr] = raw;
        if (!d.svg && raw->name == L"svg") d.svg = raw;
        if (!selfClose && stack.size() < size_t(kMaxSvgDepth)) stack.push_back(raw);
    }
    if (!styleText.empty()) {
        d.sheet.Add(styleText);
        d.sheet.BuildIndex();
        d.hasSheet = !d.sheet.rules.empty();
    }
}

// ---------------- Sayilar, uzunluklar, donusumler ----------------

struct Mat {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Mat operator*(const Mat& o) const { // this * o  (once o, sonra this)
        Mat r;
        r.a = a * o.a + c * o.b; r.b = b * o.a + d * o.b;
        r.c = a * o.c + c * o.d; r.d = b * o.c + d * o.d;
        r.e = a * o.e + c * o.f + e; r.f = b * o.e + d * o.f + f;
        return r;
    }
    void Apply(float x, float y, float& ox, float& oy) const { ox = a * x + c * y + e; oy = b * x + d * y + f; }
    float Scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

bool ReadNum(const std::wstring& s, size_t& i, float& v) {
    while (i < s.size() && (IsWS(s[i]) || s[i] == L',')) i++;
    if (i >= s.size()) return false;
    const wchar_t* b = s.c_str() + i;
    wchar_t* e = nullptr;
    double d = wcstod(b, &e);
    if (e == b) return false;
    i += size_t(e - b);
    v = float(d);
    return std::isfinite(v);
}

// Uzunluk (px, em, %, ...). pctBase: yuzde icin taban
float ParseLength(const std::wstring* sp, float def, float pctBase = 100.f) {
    if (!sp) return def;
    std::wstring s = Trim(*sp);
    if (s.empty()) return def;
    const wchar_t* b = s.c_str();
    wchar_t* e = nullptr;
    double v = wcstod(b, &e);
    if (e == b || !std::isfinite(v)) return def;
    std::wstring u = ToLower(Trim(std::wstring(e)));
    if (u.empty() || u == L"px") return float(v);
    if (u == L"%") return float(v) * pctBase / 100.f;
    if (u == L"em" || u == L"rem") return float(v) * 16.f;
    if (u == L"ex") return float(v) * 8.f;
    if (u == L"pt") return float(v) * 4.f / 3.f;
    if (u == L"pc") return float(v) * 16.f;
    if (u == L"mm") return float(v) * 3.7795f;
    if (u == L"cm") return float(v) * 37.795f;
    if (u == L"in") return float(v) * 96.f;
    return float(v);
}

Mat ParseTransform(const std::wstring& t) {
    Mat m;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (IsWS(t[i]) || t[i] == L',')) i++;
        size_t st = i;
        while (i < t.size() && iswalpha(t[i])) i++;
        std::wstring fn = ToLower(t.substr(st, i - st));
        while (i < t.size() && IsWS(t[i])) i++;
        if (fn.empty() || i >= t.size() || t[i] != L'(') break;
        i++;
        std::vector<float> a;
        float v;
        while (a.size() < 6 && ReadNum(t, i, v)) a.push_back(v);
        while (i < t.size() && t[i] != L')') i++;
        if (i < t.size()) i++;
        Mat x;
        if (fn == L"matrix" && a.size() == 6) { x.a = a[0]; x.b = a[1]; x.c = a[2]; x.d = a[3]; x.e = a[4]; x.f = a[5]; }
        else if (fn == L"translate" && !a.empty()) { x.e = a[0]; x.f = a.size() > 1 ? a[1] : 0; }
        else if (fn == L"scale" && !a.empty()) { x.a = a[0]; x.d = a.size() > 1 ? a[1] : a[0]; }
        else if (fn == L"rotate" && !a.empty()) {
            float r = a[0] * 3.14159265f / 180.f;
            Mat rot;
            rot.a = std::cos(r); rot.b = std::sin(r); rot.c = -std::sin(r); rot.d = std::cos(r);
            if (a.size() >= 3) {
                Mat t1, t2;
                t1.e = a[1]; t1.f = a[2];
                t2.e = -a[1]; t2.f = -a[2];
                x = t1 * rot * t2;
            } else x = rot;
        } else if (fn == L"skewx" && !a.empty()) { x.c = std::tan(a[0] * 3.14159265f / 180.f); }
        else if (fn == L"skewy" && !a.empty()) { x.b = std::tan(a[0] * 3.14159265f / 180.f); }
        m = m * x;
    }
    return m;
}

// ---------------- Yol olusturucu ----------------

class PathBuilder {
public:
    explicit PathBuilder(const Mat& m) : m_(m) {}
    std::vector<SvgSeg> segs;
    bool open = false;
    void MoveTo(float x, float y) { Push(0, x, y); sx_ = cx_ = x; sy_ = cy_ = y; open = true; }
    void LineTo(float x, float y) { if (!open) MoveTo(cx_, cy_); Push(1, x, y); cx_ = x; cy_ = y; }
    void CubicTo(float x1, float y1, float x2, float y2, float x, float y) {
        if (!open) MoveTo(cx_, cy_);
        if (segs.size() >= kMaxSegs) return;
        SvgSeg s;
        s.kind = 2;
        m_.Apply(x1, y1, s.x[0], s.y[0]);
        m_.Apply(x2, y2, s.x[1], s.y[1]);
        m_.Apply(x, y, s.x[2], s.y[2]);
        segs.push_back(s);
        cx_ = x; cy_ = y;
    }
    void QuadTo(float x1, float y1, float x, float y) {
        CubicTo(cx_ + 2.f / 3.f * (x1 - cx_), cy_ + 2.f / 3.f * (y1 - cy_), x + 2.f / 3.f * (x1 - x), y + 2.f / 3.f * (y1 - y), x, y);
    }
    void Close() { if (open) { SvgSeg s; s.kind = 3; segs.push_back(s); cx_ = sx_; cy_ = sy_; open = false; } }
    float cx() const { return cx_; }
    float cy() const { return cy_; }

    // SVG eliptik yay -> kubik egriler (SVG 1.1 F.6.5)
    void ArcTo(float rx, float ry, float rot, bool large, bool sweep, float x, float y) {
        float x0 = cx_, y0 = cy_;
        if (std::fabs(x - x0) < 1e-6f && std::fabs(y - y0) < 1e-6f) return;
        rx = std::fabs(rx); ry = std::fabs(ry);
        if (rx < 1e-6f || ry < 1e-6f) { LineTo(x, y); return; }
        float phi = rot * 3.14159265f / 180.f;
        float cp = std::cos(phi), sp = std::sin(phi);
        float dx = (x0 - x) / 2, dy = (y0 - y) / 2;
        float x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
        float lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
        if (lam > 1) { float s = std::sqrt(lam); rx *= s; ry *= s; }
        float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
        float co = den > 0 ? std::sqrt(std::max(0.f, num / den)) : 0;
        if (large == sweep) co = -co;
        float cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
        float ccx = cp * cxp - sp * cyp + (x0 + x) / 2, ccy = sp * cxp + cp * cyp + (y0 + y) / 2;
        auto ang = [](float ux, float uy, float vx, float vy) {
            float d = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
            if (d <= 0) return 0.f;
            float c = std::max(-1.f, std::min(1.f, (ux * vx + uy * vy) / d));
            float a = std::acos(c);
            return (ux * vy - uy * vx) < 0 ? -a : a;
        };
        float th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        float dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!sweep && dth > 0) dth -= 2 * 3.14159265f;
        else if (sweep && dth < 0) dth += 2 * 3.14159265f;
        int nseg = std::max(1, int(std::ceil(std::fabs(dth) / (3.14159265f / 2) - 0.001f)));
        float d = dth / float(nseg);
        float t = 4.f / 3.f * std::tan(d / 4);
        float a = th1;
        for (int k = 0; k < nseg; k++) {
            float c1 = std::cos(a), s1 = std::sin(a), c2 = std::cos(a + d), s2 = std::sin(a + d);
            float p1x = c1 - t * s1, p1y = s1 + t * c1, p2x = c2 + t * s2, p2y = s2 - t * c2;
            auto map = [&](float ux, float uy, float& ox, float& oy) {
                ox = ccx + rx * ux * cp - ry * uy * sp;
                oy = ccy + rx * ux * sp + ry * uy * cp;
            };
            float ax, ay, bx2, by2, ex, ey;
            map(p1x, p1y, ax, ay);
            map(p2x, p2y, bx2, by2);
            map(c2, s2, ex, ey);
            if (k == nseg - 1) { ex = x; ey = y; }
            CubicTo(ax, ay, bx2, by2, ex, ey);
            a += d;
        }
    }

private:
    Mat m_;
    float sx_ = 0, sy_ = 0, cx_ = 0, cy_ = 0;
    void Push(uint8_t k, float x, float y) {
        if (segs.size() >= kMaxSegs) return;
        SvgSeg s;
        s.kind = k;
        m_.Apply(x, y, s.x[0], s.y[0]);
        segs.push_back(s);
    }
};

void ParsePathData(const std::wstring& d, PathBuilder& pb) {
    size_t i = 0;
    wchar_t cmd = 0;
    float lcx = 0, lcy = 0;     // son kontrol noktasi (S/T icin)
    wchar_t prev = 0;
    int guard = 0;
    while (i < d.size() && guard++ < 2000000) {
        while (i < d.size() && (IsWS(d[i]) || d[i] == L',')) i++;
        if (i >= d.size()) break;
        if (iswalpha(d[i])) { cmd = d[i++]; }
        else if (!cmd) break;
        bool rel = iswlower(cmd) != 0;
        wchar_t C = wchar_t(towupper(cmd));
        float ox = rel ? pb.cx() : 0, oy = rel ? pb.cy() : 0;
        float a[7];
        auto need = [&](int n) { for (int k = 0; k < n; k++) if (!ReadNum(d, i, a[k])) return false; return true; };
        if (C == L'Z') { pb.Close(); prev = C; cmd = 0; continue; }
        bool ok = true;
        switch (C) {
        case L'M':
            if (!need(2)) { ok = false; break; }
            pb.MoveTo(ox + a[0], oy + a[1]);
            cmd = rel ? L'l' : L'L';
            break;
        case L'L': if (!need(2)) { ok = false; break; } pb.LineTo(ox + a[0], oy + a[1]); break;
        case L'H': if (!need(1)) { ok = false; break; } pb.LineTo(ox + a[0], pb.cy()); break;
        case L'V': if (!need(1)) { ok = false; break; } pb.LineTo(pb.cx(), oy + a[0]); break;
        case L'C':
            if (!need(6)) { ok = false; break; }
            pb.CubicTo(ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]);
            lcx = ox + a[2]; lcy = oy + a[3];
            break;
        case L'S': {
            if (!need(4)) { ok = false; break; }
            float x1 = pb.cx(), y1 = pb.cy();
            if (prev == L'C' || prev == L'S') { x1 = 2 * pb.cx() - lcx; y1 = 2 * pb.cy() - lcy; }
            pb.CubicTo(x1, y1, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
            lcx = ox + a[0]; lcy = oy + a[1];
            break;
        }
        case L'Q':
            if (!need(4)) { ok = false; break; }
            pb.QuadTo(ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
            lcx = ox + a[0]; lcy = oy + a[1];
            break;
        case L'T': {
            if (!need(2)) { ok = false; break; }
            float x1 = pb.cx(), y1 = pb.cy();
            if (prev == L'Q' || prev == L'T') { x1 = 2 * pb.cx() - lcx; y1 = 2 * pb.cy() - lcy; }
            pb.QuadTo(x1, y1, ox + a[0], oy + a[1]);
            lcx = x1; lcy = y1;
            break;
        }
        case L'A': {
            // bayraklar bosluksuz yazilabilir: "a1 1 0 011 1"
            if (!ReadNum(d, i, a[0]) || !ReadNum(d, i, a[1]) || !ReadNum(d, i, a[2])) { ok = false; break; }
            float fl[2];
            for (int k = 0; k < 2; k++) {
                while (i < d.size() && (IsWS(d[i]) || d[i] == L',')) i++;
                if (i >= d.size() || (d[i] != L'0' && d[i] != L'1')) { ok = false; break; }
                fl[k] = float(d[i] - L'0');
                i++;
            }
            if (!ok || !ReadNum(d, i, a[5]) || !ReadNum(d, i, a[6])) { ok = false; break; }
            pb.ArcTo(a[0], a[1], a[2], fl[0] != 0, fl[1] != 0, ox + a[5], oy + a[6]);
            break;
        }
        default: ok = false;
        }
        if (!ok) break;
        prev = C;
    }
}

// ---------------- Stil ----------------

struct Paint { uint8_t kind = 1; uint32_t color = 0xFF000000; };  // 0 yok, 1 renk, 2 currentColor

struct SvgStyle {
    Paint fill, stroke{0, 0};
    float strokeWidth = 1, fillOpacity = 1, strokeOpacity = 1;
    bool evenOdd = false;
    int cap = 0, join = 0;
    uint32_t color = 0xFF000000;
    bool hidden = false;
    // kalitimsiz
    float opacity = 1;
    bool display = true;
};

class Renderer {
public:
    Renderer(SvgDoc& d, ISvgPainter& p) : d_(d), p_(p) {}

    void Render(const Mat& root, float vpW, float vpH, uint32_t currentColor) {
        SvgStyle st;
        st.color = currentColor;
        Walk(d_.svg, root, st, 1.f, 0, vpW, vpH, true);
    }

private:
    SvgDoc& d_;
    ISvgPainter& p_;
    int work_ = 0;

    // url(#id) -> degrade renkleri ortalamasi
    bool GradientColor(const std::wstring& id, uint32_t& out, int depth = 0) {
        auto it = d_.ids.find(id);
        if (it == d_.ids.end() || depth > 4) return false;
        const Node* g = it->second;
        std::vector<uint32_t> cs;
        for (auto& k : g->kids) {
            if (k->kind != Node::Elem || k->name != L"stop") continue;
            uint32_t c = 0xFF000000;
            float op = 1;
            if (auto* sc = k->Attr(L"stop-color")) ParseColor(*sc, c);
            if (auto* so = k->Attr(L"stop-opacity")) op = float(_wtof(so->c_str()));
            if (auto* sa = k->Attr(L"style")) {
                for (auto& dcl : ParseDeclarations(*sa)) {
                    if (dcl.prop == L"stop-color") ParseColor(dcl.value, c);
                    else if (dcl.prop == L"stop-opacity") op = float(_wtof(dcl.value.c_str()));
                }
            }
            op = std::max(0.f, std::min(op, 1.f));
            c = (uint32_t(float(c >> 24) * op) << 24) | (c & 0xFFFFFF);
            cs.push_back(c);
        }
        if (cs.empty()) {
            // href ile baska degradeden miras
            const std::wstring* h = g->Attr(L"href");
            if (!h) h = g->Attr(L"xlink:href");
            if (h && !h->empty() && (*h)[0] == L'#') return GradientColor(h->substr(1), out, depth + 1);
            return false;
        }
        float a = 0, r = 0, gg = 0, b = 0;
        for (uint32_t c : cs) {
            float al = float(c >> 24) / 255.f;
            a += al; r += float((c >> 16) & 255) * al; gg += float((c >> 8) & 255) * al; b += float(c & 255) * al;
        }
        if (a <= 0) { out = 0; return true; }
        out = (uint32_t(a / float(cs.size()) * 255.f + 0.5f) << 24) | (uint32_t(r / a) << 16) | (uint32_t(gg / a) << 8) | uint32_t(b / a);
        return true;
    }

    void ParsePaint(const std::wstring& vIn, Paint& p, const SvgStyle& st) {
        std::wstring v = ToLower(Trim(vIn));
        if (v.empty() || v == L"inherit") return;
        if (v == L"none" || v == L"transparent") { p.kind = 0; return; }
        if (v == L"currentcolor") { p.kind = 2; return; }
        if (StartsWith(v, L"url(")) {
            size_t h = v.find(L'#'), e = v.find(L')');
            if (h != std::wstring::npos && e != std::wstring::npos && e > h) {
                std::wstring id = Trim(vIn.substr(h + 1, e - h - 1));
                while (!id.empty() && (id.back() == L'"' || id.back() == L'\'')) id.pop_back();
                uint32_t c;
                if (GradientColor(id, c)) { p.kind = 1; p.color = c; return; }
            }
            std::wstring rest = e != std::wstring::npos ? Trim(v.substr(e + 1)) : L"";
            if (!rest.empty()) { ParsePaint(rest, p, st); return; }
            p.kind = 0;
            return;
        }
        uint32_t c;
        if (ParseColor(v, c)) { p.kind = 1; p.color = c; }
    }

    void Apply(SvgStyle& st, const std::wstring& prop, const std::wstring& val) {
        std::wstring v = Trim(val);
        if (v.empty()) return;
        std::wstring lv = ToLower(v);
        if (lv == L"inherit") return;
        if (prop == L"fill") ParsePaint(v, st.fill, st);
        else if (prop == L"stroke") ParsePaint(v, st.stroke, st);
        else if (prop == L"stroke-width") st.strokeWidth = std::max(0.f, ParseLength(&v, st.strokeWidth));
        else if (prop == L"fill-opacity") st.fillOpacity = std::max(0.f, std::min(float(_wtof(v.c_str())) * (EndsWith(v, L"%") ? 0.01f : 1.f), 1.f));
        else if (prop == L"stroke-opacity") st.strokeOpacity = std::max(0.f, std::min(float(_wtof(v.c_str())) * (EndsWith(v, L"%") ? 0.01f : 1.f), 1.f));
        else if (prop == L"opacity") st.opacity = std::max(0.f, std::min(float(_wtof(v.c_str())) * (EndsWith(v, L"%") ? 0.01f : 1.f), 1.f));
        else if (prop == L"fill-rule") st.evenOdd = (lv == L"evenodd");
        else if (prop == L"stroke-linecap") st.cap = lv == L"round" ? 1 : lv == L"square" ? 2 : 0;
        else if (prop == L"stroke-linejoin") st.join = lv == L"round" ? 1 : lv == L"bevel" ? 2 : 0;
        else if (prop == L"color") { uint32_t c; if (ParseColor(v, c)) st.color = c; }
        else if (prop == L"display") st.display = lv != L"none";
        else if (prop == L"visibility") st.hidden = (lv == L"hidden" || lv == L"collapse");
    }

    SvgStyle Compute(const Node* n, const SvgStyle& parent) {
        SvgStyle st = parent;
        st.opacity = 1;
        st.display = true;
        static const wchar_t* props[] = {L"fill", L"stroke", L"stroke-width", L"fill-opacity", L"stroke-opacity", L"opacity",
                                         L"fill-rule", L"stroke-linecap", L"stroke-linejoin", L"color", L"display", L"visibility"};
        for (auto p : props) if (auto* v = n->Attr(p)) Apply(st, p, *v);
        if (d_.hasSheet) {
            struct M { int spec, order; const Decl* d; };
            std::vector<M> ms;
            for (auto& r : d_.sheet.rules) {
                if (r.sel.pseudo || !Matches(r.sel, n)) continue;
                for (auto& dc : *r.decls) ms.push_back({r.sel.spec + (dc.important ? 1000000 : 0), r.order, &dc});
            }
            std::stable_sort(ms.begin(), ms.end(), [](const M& a, const M& b) { return a.spec != b.spec ? a.spec < b.spec : a.order < b.order; });
            for (auto& m : ms) Apply(st, m.d->prop, m.d->value);
        }
        if (auto* sa = n->Attr(L"style")) for (auto& dc : ParseDeclarations(*sa)) Apply(st, dc.prop, dc.value);
        return st;
    }

    uint32_t Resolve(const Paint& p, const SvgStyle& st, float op) {
        uint32_t c = p.kind == 2 ? st.color : p.color;
        float a = float(c >> 24) / 255.f * op;
        return (uint32_t(std::max(0.f, std::min(a, 1.f)) * 255.f + 0.5f) << 24) | (c & 0xFFFFFF);
    }

    void Draw(PathBuilder& pb, const SvgStyle& st, float alpha, const Mat& m) {
        if (st.hidden || pb.segs.empty()) return;
        if (st.fill.kind) {
            uint32_t c = Resolve(st.fill, st, st.fillOpacity * alpha);
            if (c >> 24) p_.Fill(pb.segs, st.evenOdd, c);
        }
        if (st.stroke.kind && st.strokeWidth > 0) {
            uint32_t c = Resolve(st.stroke, st, st.strokeOpacity * alpha);
            if (c >> 24) p_.Stroke(pb.segs, st.strokeWidth * m.Scale(), c, st.cap, st.join);
        }
    }

    float Attr(const Node* n, const wchar_t* a, float def, float base) { return ParseLength(n->Attr(a), def, base); }

public:
    // viewBox -> viewport donusumu (preserveAspectRatio)
    static Mat ViewBoxMat(const Node* n, float vw, float vh) {
        Mat m;
        const std::wstring* vb = n->Attr(L"viewbox");
        if (!vb) return m;
        size_t i = 0;
        float v[4];
        for (int k = 0; k < 4; k++) if (!ReadNum(*vb, i, v[k])) return m;
        if (v[2] <= 0 || v[3] <= 0) return m;
        float sx = vw / v[2], sy = vh / v[3];
        std::wstring par = n->Attr(L"preserveaspectratio") ? ToLower(*n->Attr(L"preserveaspectratio")) : L"xmidymid meet";
        if (par.find(L"none") != std::wstring::npos) {
            m.a = sx; m.d = sy; m.e = -v[0] * sx; m.f = -v[1] * sy;
            return m;
        }
        bool slice = par.find(L"slice") != std::wstring::npos;
        float s = slice ? std::max(sx, sy) : std::min(sx, sy);
        float tx = -v[0] * s, ty = -v[1] * s;
        float ex = vw - v[2] * s, ey = vh - v[3] * s;
        if (par.find(L"xmid") != std::wstring::npos) tx += ex / 2;
        else if (par.find(L"xmax") != std::wstring::npos) tx += ex;
        if (par.find(L"ymid") != std::wstring::npos) ty += ey / 2;
        else if (par.find(L"ymax") != std::wstring::npos) ty += ey;
        m.a = m.d = s; m.e = tx; m.f = ty;
        return m;
    }

private:
    void Walk(const Node* n, const Mat& parentM, const SvgStyle& parentSt, float alpha, int depth, float vpW, float vpH, bool isRoot) {
        if (!n || depth > 40 || ++work_ > 200000) return;
        const std::wstring& t = n->name;
        if (t == L"defs" || t == L"clippath" || t == L"mask" || t == L"pattern" || t == L"marker" || t == L"lineargradient" ||
            t == L"radialgradient" || t == L"filter" || t == L"style" || t == L"script" || t == L"title" || t == L"desc" ||
            t == L"metadata" || t == L"text" || t == L"foreignobject" || t == L"symbol" || t == L"image" || t == L"view")
            return;
        SvgStyle st = Compute(n, parentSt);
        if (!st.display) return;
        float a = alpha * st.opacity;
        if (a <= 0.003f) return;
        Mat m = parentM;
        if (auto* tr = n->Attr(L"transform")) m = m * ParseTransform(*tr);
        float diag = std::sqrt((vpW * vpW + vpH * vpH) / 2.f);

        if (t == L"svg") {
            if (!isRoot) {
                float x = Attr(n, L"x", 0, vpW), y = Attr(n, L"y", 0, vpH);
                float w = Attr(n, L"width", vpW, vpW), h = Attr(n, L"height", vpH, vpH);
                Mat tr;
                tr.e = x; tr.f = y;
                m = m * tr * ViewBoxMat(n, w, h);
                vpW = w; vpH = h;
            }
            for (auto& k : n->kids) if (k->kind == Node::Elem) Walk(k.get(), m, st, a, depth + 1, vpW, vpH, false);
            return;
        }
        if (t == L"g" || t == L"a" || t == L"switch") {
            for (auto& k : n->kids) {
                if (k->kind != Node::Elem) continue;
                Walk(k.get(), m, st, a, depth + 1, vpW, vpH, false);
                if (t == L"switch") break;
            }
            return;
        }
        if (t == L"use") {
            const std::wstring* h = n->Attr(L"href");
            if (!h) h = n->Attr(L"xlink:href");
            if (!h || h->empty() || (*h)[0] != L'#') return;
            auto it = d_.ids.find(h->substr(1));
            if (it == d_.ids.end()) return;
            const Node* ref = it->second;
            for (const Node* p = n; p; p = p->parent) if (p == ref) return; // dongu
            Mat tr;
            tr.e = Attr(n, L"x", 0, vpW);
            tr.f = Attr(n, L"y", 0, vpH);
            m = m * tr;
            if (ref->name == L"symbol" || ref->name == L"svg") {
                float w = Attr(n, L"width", Attr(ref, L"width", vpW, vpW), vpW);
                float hh = Attr(n, L"height", Attr(ref, L"height", vpH, vpH), vpH);
                Mat vbm = m * ViewBoxMat(ref, w, hh);
                SvgStyle rs = Compute(ref, st);
                for (auto& k : ref->kids) if (k->kind == Node::Elem) Walk(k.get(), vbm, rs, a, depth + 1, w, hh, false);
                return;
            }
            Walk(ref, m, st, a, depth + 1, vpW, vpH, false);
            return;
        }
        PathBuilder pb(m);
        if (t == L"path") {
            if (auto* d = n->Attr(L"d")) ParsePathData(*d, pb);
        } else if (t == L"rect") {
            float x = Attr(n, L"x", 0, vpW), y = Attr(n, L"y", 0, vpH);
            float w = Attr(n, L"width", 0, vpW), h = Attr(n, L"height", 0, vpH);
            if (w <= 0 || h <= 0) return;
            float rx = Attr(n, L"rx", -1, vpW), ry = Attr(n, L"ry", -1, vpH);
            if (rx < 0 && ry >= 0) rx = ry;
            if (ry < 0 && rx >= 0) ry = rx;
            rx = std::min(std::max(rx, 0.f), w / 2);
            ry = std::min(std::max(ry, 0.f), h / 2);
            if (rx > 0 && ry > 0) {
                const float k = 0.5522848f;
                pb.MoveTo(x + rx, y);
                pb.LineTo(x + w - rx, y);
                pb.CubicTo(x + w - rx + rx * k, y, x + w, y + ry - ry * k, x + w, y + ry);
                pb.LineTo(x + w, y + h - ry);
                pb.CubicTo(x + w, y + h - ry + ry * k, x + w - rx + rx * k, y + h, x + w - rx, y + h);
                pb.LineTo(x + rx, y + h);
                pb.CubicTo(x + rx - rx * k, y + h, x, y + h - ry + ry * k, x, y + h - ry);
                pb.LineTo(x, y + ry);
                pb.CubicTo(x, y + ry - ry * k, x + rx - rx * k, y, x + rx, y);
            } else {
                pb.MoveTo(x, y); pb.LineTo(x + w, y); pb.LineTo(x + w, y + h); pb.LineTo(x, y + h);
            }
            pb.Close();
        } else if (t == L"circle" || t == L"ellipse") {
            float cx = Attr(n, L"cx", 0, vpW), cy = Attr(n, L"cy", 0, vpH);
            float rx, ry;
            if (t == L"circle") rx = ry = Attr(n, L"r", 0, diag);
            else { rx = Attr(n, L"rx", -1, vpW); ry = Attr(n, L"ry", -1, vpH); if (rx < 0) rx = ry; if (ry < 0) ry = rx; }
            if (rx <= 0 || ry <= 0) return;
            const float k = 0.5522848f;
            pb.MoveTo(cx + rx, cy);
            pb.CubicTo(cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
            pb.CubicTo(cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
            pb.CubicTo(cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
            pb.CubicTo(cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
            pb.Close();
        } else if (t == L"line") {
            pb.MoveTo(Attr(n, L"x1", 0, vpW), Attr(n, L"y1", 0, vpH));
            pb.LineTo(Attr(n, L"x2", 0, vpW), Attr(n, L"y2", 0, vpH));
            SvgStyle ls = st;
            ls.fill.kind = 0;
            Draw(pb, ls, a, m);
            return;
        } else if (t == L"polyline" || t == L"polygon") {
            const std::wstring* pts = n->Attr(L"points");
            if (!pts) return;
            size_t i = 0;
            float x, y;
            bool first = true;
            while (ReadNum(*pts, i, x) && ReadNum(*pts, i, y)) {
                if (first) { pb.MoveTo(x, y); first = false; }
                else pb.LineTo(x, y);
            }
            if (t == L"polygon") pb.Close();
        } else {
            return;
        }
        Draw(pb, st, a, m);
    }
};

bool RootSize(const SvgDoc& d, float& w, float& h, float vb[4], bool& hasVb) {
    const Node* s = d.svg;
    if (!s) return false;
    hasVb = false;
    if (auto* v = s->Attr(L"viewbox")) {
        size_t i = 0;
        hasVb = ReadNum(*v, i, vb[0]) && ReadNum(*v, i, vb[1]) && ReadNum(*v, i, vb[2]) && ReadNum(*v, i, vb[3]) && vb[2] > 0 && vb[3] > 0;
    }
    auto isPct = [](const std::wstring* a) { return a && Trim(*a).find(L'%') != std::wstring::npos; };
    const std::wstring* wa = s->Attr(L"width");
    const std::wstring* ha = s->Attr(L"height");
    float ww = isPct(wa) ? -1 : ParseLength(wa, -1);
    float hh = isPct(ha) ? -1 : ParseLength(ha, -1);
    if (ww > 0 && hh > 0) { w = ww; h = hh; }
    else if (ww > 0) { w = ww; h = hasVb ? ww * vb[3] / vb[2] : 150; }
    else if (hh > 0) { h = hh; w = hasVb ? hh * vb[2] / vb[3] : 300; }
    else if (hasVb) { w = vb[2]; h = vb[3]; }
    else { w = 300; h = 150; }
    w = std::min(std::max(w, 1.f), 8192.f);
    h = std::min(std::max(h, 1.f), 8192.f);
    return true;
}

} // namespace

bool LooksLikeSvg(const std::string& b) {
    std::string head = b.substr(0, std::min<size_t>(b.size(), 4096));
    size_t i = 0;
    if (head.size() >= 3 && (unsigned char)head[0] == 0xEF) i = 3;
    while (i < head.size() && (head[i] == ' ' || head[i] == '\n' || head[i] == '\r' || head[i] == '\t')) i++;
    if (i >= head.size() || head[i] != '<') return false;
    return head.find("<svg") != std::string::npos || head.find("<SVG") != std::string::npos || head.find(":svg") != std::string::npos;
}

bool SvgNaturalSize(const std::wstring& markup, float& w, float& h) {
    SvgDoc d;
    ParseXml(markup, d);
    float vb[4];
    bool hv;
    return RootSize(d, w, h, vb, hv);
}

bool RenderDoc(SvgDoc& d, float targetW, float targetH, uint32_t currentColor, ISvgPainter& painter) {
    if (!d.svg) return false;
    float w, h, vb[4];
    bool hv;
    if (!RootSize(d, w, h, vb, hv)) return false;
    Mat root;
    root.a = targetW / w;
    root.d = targetH / h;
    Mat m = root * Renderer::ViewBoxMat(d.svg, w, h);
    Renderer r(d, painter);
    r.Render(m, hv ? vb[2] : w, hv ? vb[3] : h, currentColor);
    return true;
}

bool RenderSvg(const std::wstring& markup, float targetW, float targetH, uint32_t currentColor, ISvgPainter& painter) {
    SvgDoc d;
    ParseXml(markup, d);
    return RenderDoc(d, targetW, targetH, currentColor, painter);
}

#ifdef _WIN32

namespace {

template <class T> void SafeRel(T*& p) { if (p) { p->Release(); p = nullptr; } }

class D2DPainter : public ISvgPainter {
public:
    D2DPainter(ID2D1Factory* f, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br) : f_(f), rt_(rt), br_(br) {}
    ~D2DPainter() { for (auto& s : styles_) SafeRel(s); }
    void Fill(const std::vector<SvgSeg>& path, bool evenOdd, uint32_t argb) override {
        ID2D1PathGeometry* g = Build(path, evenOdd, true);
        if (!g) return;
        br_->SetColor(Col(argb));
        rt_->FillGeometry(g, br_);
        g->Release();
    }
    void Stroke(const std::vector<SvgSeg>& path, float width, uint32_t argb, int cap, int join) override {
        ID2D1PathGeometry* g = Build(path, false, false);
        if (!g) return;
        br_->SetColor(Col(argb));
        rt_->DrawGeometry(g, br_, std::max(width, 0.1f), Style(cap, join));
        g->Release();
    }

private:
    ID2D1Factory* f_;
    ID2D1RenderTarget* rt_;
    ID2D1SolidColorBrush* br_;
    ID2D1StrokeStyle* styles_[9] = {};

    static D2D1_COLOR_F Col(uint32_t c) {
        return D2D1::ColorF(float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, float(c & 255) / 255.f, float(c >> 24) / 255.f);
    }
    ID2D1StrokeStyle* Style(int cap, int join) {
        int k = std::max(0, std::min(cap, 2)) * 3 + std::max(0, std::min(join, 2));
        if (!styles_[k]) {
            D2D1_CAP_STYLE cs = cap == 1 ? D2D1_CAP_STYLE_ROUND : cap == 2 ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_FLAT;
            D2D1_LINE_JOIN js = join == 1 ? D2D1_LINE_JOIN_ROUND : join == 2 ? D2D1_LINE_JOIN_BEVEL : D2D1_LINE_JOIN_MITER;
            f_->CreateStrokeStyle(D2D1::StrokeStyleProperties(cs, cs, cs, js, 4.f), nullptr, 0, &styles_[k]);
        }
        return styles_[k];
    }
    ID2D1PathGeometry* Build(const std::vector<SvgSeg>& segs, bool evenOdd, bool fill) {
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(f_->CreatePathGeometry(&g))) return nullptr;
        if (FAILED(g->Open(&sink))) { g->Release(); return nullptr; }
        sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
        bool in = false;
        for (auto& s : segs) {
            switch (s.kind) {
            case 0:
                if (in) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(D2D1::Point2F(s.x[0], s.y[0]), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                in = true;
                break;
            case 1: if (in) sink->AddLine(D2D1::Point2F(s.x[0], s.y[0])); break;
            case 2: if (in) sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(s.x[0], s.y[0]), D2D1::Point2F(s.x[1], s.y[1]), D2D1::Point2F(s.x[2], s.y[2]))); break;
            case 3: if (in) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); in = false; } break;
            }
        }
        if (in) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        HRESULT hr = sink->Close();
        sink->Release();
        if (FAILED(hr)) { g->Release(); return nullptr; }
        return g;
    }
};

} // namespace

bool RasterizeSvg(const std::wstring& markup, float scale, uint32_t currentColor,
                  uint32_t& outW, uint32_t& outH, float& natW, float& natH, std::vector<uint8_t>& px) {
    if (markup.size() > 8u * 1024 * 1024) return false;
    SvgDoc d;
    ParseXml(markup, d);
    float vb[4];
    bool hv;
    if (!d.svg || !RootSize(d, natW, natH, vb, hv)) return false;
    float sc = std::max(0.05f, scale);
    if (natW * sc > 2048) sc = 2048 / natW;
    if (natH * sc > 2048) sc = std::min(sc, 2048 / natH);
    if (natW * sc * natH * sc > 4.2e6f) sc = std::sqrt(4.2e6f / (natW * natH));
    UINT W = std::max(1u, UINT(std::ceil(natW * sc))), H = std::max(1u, UINT(std::ceil(natH * sc)));
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* bmp = nullptr;
    ID2D1Factory* f = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) break;
        if (FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) break;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f))) break;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(f->CreateWicBitmapRenderTarget(bmp, props, &rt))) break;
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br))) break;
        rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        {
            D2DPainter painter(f, rt, br);
            RenderDoc(d, float(W), float(H), currentColor, painter);
        }
        if (FAILED(rt->EndDraw())) break;
        px.resize(size_t(W) * H * 4);
        if (FAILED(bmp->CopyPixels(nullptr, W * 4, UINT(px.size()), px.data()))) { px.clear(); break; }
        outW = W;
        outH = H;
        ok = true;
    } while (false);
    SafeRel(br);
    SafeRel(rt);
    SafeRel(f);
    SafeRel(bmp);
    SafeRel(wic);
    return ok;
}

#else

bool RasterizeSvg(const std::wstring&, float, uint32_t, uint32_t&, uint32_t&, float&, float&, std::vector<uint8_t>&) { return false; }

#endif

} // namespace mer
