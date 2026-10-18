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
    enum Unit : uint8_t { Px, Pct, Auto, None };
    float v = 0;
    Unit u = Px;
};

using VarMap = std::unordered_map<std::wstring, std::wstring>;

struct Style {
    enum Display : uint8_t { Inline, Block, ListItem, Table, Row, Cell, None };
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
    std::shared_ptr<const VarMap> vars;

    // Kalitimla gecmeyenler
    uint32_t bg = 0;
    Len m[4], p[4];               // ust, sag, alt, sol
    float bw[4] = {0, 0, 0, 0};
    uint32_t bc[4] = {0xFF888888, 0xFF888888, 0xFF888888, 0xFF888888};
    Len width{0, Len::Auto}, maxWidth{0, Len::None}, height{0, Len::Auto};
    bool borderBox = false;
    uint8_t valign = 1;           // 0 ust, 1 orta, 2 alt (tablo hucreleri)
    uint8_t position = 0;         // 0 static, 1 relative, 2 absolute, 3 fixed
    bool overflowHidden = false, hiddenClip = false, offscreen = false, invisible = false;
    float cellSpacing = 2;
    int colspan = 1;

    // Flexbox / Grid
    uint8_t flex = 0;             // 0 yok, 1 satir (row), 2 sutun (column)
    bool flexWrap = false;
    uint8_t justify = 0;          // 0 bas, 1 orta, 2 son, 3 space-between, 4 space-around/evenly
    uint8_t alignItems = 0;       // 0 stretch/bas, 1 orta, 2 son
    float gapRow = 0, gapCol = 0;
    float flexGrow = 0;
    Len flexBasis{0, Len::Auto};
    int gridCols = 0;             // >0 ise grid: sutun sayisi
    uint8_t floatSide = 0;        // 1 sol, 2 sag
    int order = 0;
    bool flexDirCol = false;
    int gridTemplate = 0;
};

// Tum dugumlerin stilini hesaplar (indeks = Node::id). Is parcaciginda calisabilir.
std::vector<Style> ComputeStyles(const Document& doc, const StyleSheet& sheet, bool scripting = false);
bool ParseColor(const std::wstring& v, uint32_t& out);

} // namespace mer
