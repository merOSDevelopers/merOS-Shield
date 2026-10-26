// merOS Shield - MerEngine : hesaplanmış stil (cascade + kalıtım)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "css.h"
#include "dom.h"

namespace mer {

struct Len {
    // Calc: v + p% (yuzde kismi kutu genisligine gore); capKind 1 ise min(.., cap), 2 ise max(.., cap)
    // Fit: fit-content / max-content / min-content (icerige gore daralan genislik)
    enum Unit : uint8_t { Px, Pct, Auto, None, Calc, Fit };
    float v = 0;
    Unit u = Px;
    float p = 0;
    float cap = 0;
    uint8_t capKind = 0;
    bool Def() const { return u == Px || u == Pct || u == Calc; }
};

using VarMap = std::unordered_map<std::wstring, std::wstring>;

// Grid izi boyutu: k = 0 px, 1 yuzde, 2 fr, 3 auto, 4 min-content, 5 max-content
struct TrackSize { uint8_t k = 3; float v = 0; };
struct Track { TrackSize mn, mx; };
struct GridSpec {
    std::vector<Track> cols, rows;
    std::vector<Track> autoRepeat;        // repeat(auto-fill|auto-fit, ...)
    int autoRepeatAt = -1;                // cols icinde eklenecegi yer
    bool autoFit = false;
    std::vector<std::vector<std::wstring>> areas;
    Track autoRow;                        // grid-auto-rows
    bool hasAutoRow = false;
};

// Arka plan / maske resmi
struct BgImage {
    std::wstring url;                     // mutlak adres (ya da data:)
    uint8_t size = 0;                     // 0 auto, 1 cover, 2 contain, 3 uzunluk
    Len sw{0, Len::Auto}, sh{0, Len::Auto};
    Len px{0, Len::Pct}, py{0, Len::Pct}; // konum (varsayilan 0% 0%)
    uint8_t repeat = 0;                   // 0 repeat, 1 no-repeat, 2 repeat-x, 3 repeat-y
};

// Degrade (linear-gradient / radial-gradient)
struct GradStop { uint32_t color = 0; float pos = 0; uint8_t unit = 0; };   // unit: 0 otomatik, 1 yuzde, 2 px
struct Gradient {
    bool radial = false;
    bool repeating = false;
    bool circle = false;            // radial: circle (yoksa elips)
    float angle = 180;              // derece (CSS: 0 = yukari, 90 = saga); 'to right' -> 90
    bool corner = false;            // 'to top right' gibi: aci kutu oranina gore hesaplanir
    int cornerX = 0, cornerY = 0;   // -1/0/1
    std::vector<GradStop> stops;
};

// box-shadow
struct BoxShadow { float x = 0, y = 0, blur = 0, spread = 0; uint32_t color = 0x40000000; bool inset = false; };

struct Style;
// ::before / ::after sozde ogeleri
struct PseudoPair {
    std::shared_ptr<Style> before, after;
};

struct Style {
    enum Display : uint8_t { Inline, Block, ListItem, Table, Row, Cell, None, Contents };
    Display display = Inline;

    // Kalitimla gecenler
    uint32_t color = 0xFF1B1F27;  // ARGB
    float fontSize = 16;
    int weight = 400;
    bool italic = false, underline = false, strike = false;
    std::wstring family = L"segoe ui";
    uint8_t align = 0;            // 0 sol, 1 orta, 2 sag
    bool pre = false, nowrap = false;
    float lineHeight = 1.3f;      // carpan (lineHeightPx > 0 ise o kullanilir)
    float lineHeightPx = 0;
    uint8_t listType = 0;         // 0 disk, 1 sayi, 2 yok, 3 daire, 4 kare
    uint8_t textTransform = 0;    // 0 yok, 1 BUYUK, 2 kucuk, 3 Bas Harf
    bool collapse = false;
    bool hidden = false;          // visibility:hidden (kalitimla gecer, alt oge visible ile acabilir)
    bool textHidden = false;      // text-indent:-9999px (metin yerine resim teknigi)
    uint8_t cursor = 0;           // 0 varsayilan, 1 pointer, 2 text, 3 not-allowed (kalitimla gecer)
    std::shared_ptr<const VarMap> vars;

    // Kalitimla gecmeyenler
    uint32_t bg = 0;
    Len m[4], p[4];               // ust, sag, alt, sol
    float bw[4] = {0, 0, 0, 0};
    uint32_t bc[4] = {0xFF888888, 0xFF888888, 0xFF888888, 0xFF888888};
    Len width{0, Len::Auto}, maxWidth{0, Len::None}, height{0, Len::Auto};
    Len minWidth{0, Len::Auto}, minHeight{0, Len::Auto}, maxHeight{0, Len::None};
    bool borderBox = false;
    uint8_t valign = 1;           // tablo hucresi: 0 ust, 1 orta, 2 alt
    uint8_t vAlign = 0;           // satir ici: 0 taban, 1 orta, 2 ust, 3 alt, 4 text-top, 5 text-bottom, 6 sub, 7 super
    uint8_t position = 0;         // 0 static, 1 relative, 2 absolute, 3 fixed, 4 sticky
    Len inset[4] = {{0, Len::Auto}, {0, Len::Auto}, {0, Len::Auto}, {0, Len::Auto}}; // top right bottom left
    Len tx{0, Len::Px}, ty{0, Len::Px};   // transform: translate (yuzde = kendi boyutu)
    bool overflowHidden = false, hiddenClip = false, offscreen = false, invisible = false;
    bool transformHidden = false;         // scale(0) / translate(-100%) gibi gizleme
    bool bfc = false;                     // yeni blok bicimlendirme baglami (overflow, flow-root)
    bool inlineBlock = false;             // atomik satir ici kutu (inline-block/flex/grid/table)
    float opacity = 1;
    uint8_t clear = 0;                    // 1 sol, 2 sag, 3 ikisi
    float radius = 0;                     // border-radius (px)
    bool radiusPct = false;               // 50% gibi: daire
    float aspect = 0;                     // aspect-ratio (genislik / yukseklik)
    float cellSpacing = 2;
    int colspan = 1;
    std::shared_ptr<const BgImage> bgImage, mask;
    std::shared_ptr<const Gradient> grad;               // arka plan degradesi (renk uzerine, resim altina)
    std::shared_ptr<const std::vector<BoxShadow>> shadows;
    bool ellipsis = false;                              // text-overflow: ellipsis
    uint8_t lineClamp = 0;                              // -webkit-line-clamp
    bool webkitBox = false;                             // display: -webkit-box (line-clamp icin blok gibi)
    std::shared_ptr<PseudoPair> pseudo;
    std::shared_ptr<std::wstring> content;  // yalnizca sozde ogelerde: uretilen metin

    // Flexbox / Grid
    uint8_t flex = 0;             // 0 yok, 1 satir (row), 2 sutun (column)
    bool flexWrap = false, flexReverse = false;
    uint8_t justify = 0;          // 0 bas, 1 orta, 2 son, 3 space-between, 4 space-around, 5 space-evenly
    uint8_t alignItems = 0;       // 0 stretch, 1 orta, 2 son, 3 bas, 4 taban
    uint8_t alignSelf = 0;        // 0 auto, digerleri alignItems+1
    uint8_t justifyItems = 0;     // grid: 0 stretch, 1 orta, 2 son, 3 bas
    float gapRow = 0, gapCol = 0;
    float flexGrow = 0, flexShrink = 1;
    Len flexBasis{0, Len::Auto};
    int gridCols = 0;             // >0 ise grid kapsayicisi
    std::shared_ptr<const GridSpec> grid;
    std::wstring gridArea;        // grid-area adi
    int16_t gcs = 0, gce = 0, grs = 0, gre = 0;  // grid-column/row cizgileri (0 = otomatik, <0 = sondan)
    int16_t gspanC = 1, gspanR = 1;               // span N
    uint8_t floatSide = 0;        // 1 sol, 2 sag
    int order = 0;
    bool flexDirCol = false;
    int32_t zIndex = 0;
    bool hasZIndex = false;
};

struct StyleContext {
    float vw = 1280, vh = 800;    // gorunur alan (vw/vh birimleri icin)
    bool scripting = false;
};

// Tum dugumlerin stilini hesaplar (indeks = Node::id). Is parcaciginda calisabilir.
std::vector<Style> ComputeStyles(const Document& doc, const StyleSheet& sheet, bool scripting = false);
std::vector<Style> ComputeStyles(const Document& doc, const StyleSheet& sheet, const StyleContext& ctx);
bool ParseColor(const std::wstring& v, uint32_t& out);
// Uzunluk cozumleme (yerlesim): yuzdeler 'base'e gore
float ResolveLen(const Len& l, float base);

} // namespace mer
