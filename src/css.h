// merOS Shield - MerEngine : CSS ayrıştırıcı ve seçici eşleştirme
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "dom.h"

namespace mer {

struct AttrSel {
    std::wstring name, value;
    wchar_t op = 0;                   // 0 var mi, '=' esit, '^' basla, '$' bit, '*' icer, '~' kelime, '|' tire
    bool icase = false;
};

// :nth-child(an+b) ailesi. kind: 0 child, 1 last-child, 2 of-type, 3 last-of-type
struct NthSel { int a = 0, b = 1; uint8_t kind = 0; };

struct Selector;

struct Compound {
    std::wstring tag;                 // bos = herhangi
    std::wstring id;
    std::vector<std::wstring> classes;
    std::vector<AttrSel> attrs;
    bool root = false;                // :root
    bool firstChild = false, lastChild = false, onlyChild = false, empty = false;
    bool firstType = false, lastType = false, onlyType = false;
    bool checked = false, disabled = false, enabled = false, placeholderShown = false;
    bool never = false;               // asla eslesmez (:hover gibi durumlar :is() icinde)
    std::vector<NthSel> nths;
    std::vector<Selector> nots;       // :not(liste)
    std::vector<std::vector<Selector>> iss;  // :is()/:where()/:matches() listeleri
    std::vector<std::vector<Selector>> has;  // :has() (torun ya da '>' cocuk)
    // Shadow DOM
    bool host = false;                // :host, :host(x), :host-context(x)
    std::vector<Selector> hostArgs;   // :host(x)
    std::vector<Selector> hostCtx;    // :host-context(x)
    bool slotted = false;             // ::slotted(x) (yalnizca son parcada)
    std::vector<Selector> slottedArgs;
    bool defined = false;             // :defined
    wchar_t comb = L' ';              // soldaki parcayla iliski: ' ' torun, '>' cocuk, '+' hemen sonraki, '~' sonraki kardes
};

struct Selector {
    std::vector<Compound> parts;      // soldan saga
    int spec = 0;                     // ozgulluk
    uint8_t pseudo = 0;               // 0 yok, 1 ::before, 2 ::after
};

struct Decl {
    std::wstring prop, value;
    bool important = false;
};

struct Rule {
    Selector sel;
    const std::vector<Decl>* decls = nullptr;
    int order = 0;
};

class StyleSheet {
public:
    // base: CSS dosyasinin adresi (url() goreli yollari buna gore cozulur)
    void Add(const std::wstring& css, const std::wstring& base = L"");
    void BuildIndex();

    std::vector<Rule> rules;
    // Hiz: her kural en sagdaki secici parcasina gore kovalanir, eleman basina
    // yalnizca ilgili kurallar denenir.
    std::unordered_map<std::wstring, std::vector<int>> byId, byClass, byTag;
    std::vector<int> universal;
    std::vector<int> hostRules;        // konu parcasi :host olan kurallar (shadow host'a uygulanir)
    std::vector<int> slottedRules;     // ::slotted() kurallari (slota atanmis light dugumlere)
    bool hasPseudo = false;            // ::before/::after kurali var mi

    // Ortam: @media degerlendirmesi icin gorunur alan
    float vw = 1280, vh = 800;
    // @import: adres -> CSS metni (yoksa nullptr; cagiran indirme listesine ekler)
    std::function<const std::wstring*(const std::wstring& url)> importer;
    std::wstring pageBase;             // style="" ozniteliklerindeki url() icin

    void AddRule(const std::wstring& selectorText, std::vector<Decl> decls);
private:
    void ParseList(const std::wstring& s, int depth, const std::wstring& base);
    std::vector<std::unique_ptr<std::vector<Decl>>> store_;
    int order_ = 0;
};

std::vector<Decl> ParseDeclarations(const std::wstring& s);
bool Matches(const Selector& sel, const Node* n);
// querySelector icin: virgulle ayrilmis listeyi ayristirir; desteklenmeyen secici varsa false.
bool ParseSelectorList(const std::wstring& text, std::vector<Selector>& out);
// Medya sorgusu ("screen and (min-width: 600px)") gorunur alana gore
bool MediaMatches(const std::wstring& query, float vw, float vh);
// url(...) icindeki goreli adresleri 'base'e gore mutlak yapar
std::wstring ResolveCssUrls(const std::wstring& value, const std::wstring& base);

} // namespace mer
