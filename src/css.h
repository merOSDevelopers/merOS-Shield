// merOS Shield - MerEngine : CSS ayrıştırıcı ve seçici eşleştirme
#pragma once
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

struct Compound {
    std::wstring tag;                 // bos = herhangi
    std::wstring id;
    std::vector<std::wstring> classes;
    std::vector<AttrSel> attrs;
    bool root = false;                // :root
    bool firstChild = false, lastChild = false, onlyChild = false, empty = false;
    std::vector<Compound> nots;       // :not(basit secici)
    wchar_t comb = L' ';              // soldaki parcayla iliski: ' ' torun, '>' cocuk, '+' hemen sonraki, '~' sonraki kardes
};

struct Selector {
    std::vector<Compound> parts;      // soldan saga
    int spec = 0;                     // ozgulluk
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
    void Add(const std::wstring& css);
    void BuildIndex();

    std::vector<Rule> rules;
    // Hiz: her kural en sagdaki secici parcasina gore kovalanir, eleman basina
    // yalnizca ilgili kurallar denenir.
    std::unordered_map<std::wstring, std::vector<int>> byId, byClass, byTag;
    std::vector<int> universal;

    void AddRule(const std::wstring& selectorText, std::vector<Decl> decls);
private:
    void ParseList(const std::wstring& s, int depth);
    std::vector<std::unique_ptr<std::vector<Decl>>> store_;
    int order_ = 0;
};

std::vector<Decl> ParseDeclarations(const std::wstring& s);
bool Matches(const Selector& sel, const Node* n);
// querySelector icin: virgulle ayrilmis listeyi ayristirir; desteklenmeyen secici varsa false.
bool ParseSelectorList(const std::wstring& text, std::vector<Selector>& out);

} // namespace mer
