// merOS Shield - merJS : sözdizimi ağacı, sözcük çözümleyici ve ayrıştırıcı
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "js.h"

namespace mer {
namespace js {

enum class K : uint8_t {
    // Ifadeler
    Num, Str, Tpl, TaggedTpl, Ident, This, Super, Null, True, False, Regex, Arr, ObjLit, Property,
    Func, Class, ClassMember, Unary, Update, Bin, Logic, Assign, Cond, Call, New, Member, Index, Seq,
    Spread, Hole, NewTarget, Await, Yield, ImportCall, ImportMeta,
    // Kaliplar (destructuring)
    ObjPat, ArrPat, AssignPat, Rest,
    // Deyimler
    VarDecl, Declarator, ExprStmt, Block, If, For, ForIn, ForOf, While, DoWhile, Return, Break,
    Continue, Throw, Try, Switch, Case, Empty, Labeled, Program, With,
    // ES Modulleri
    ImportDecl, ImportSpec, ExportDecl, ExportDefault, ExportAll
};

// Operatorler (Bin/Logic/Assign/Unary/Update op alani)
enum Op : uint8_t {
    O_None, O_Add, O_Sub, O_Mul, O_Div, O_Mod, O_Exp, O_Shl, O_Shr, O_UShr, O_BitAnd, O_BitOr, O_BitXor,
    O_Eq, O_Ne, O_SEq, O_SNe, O_Lt, O_Gt, O_Le, O_Ge, O_In, O_InstanceOf,
    O_And, O_Or, O_Nullish,
    O_Not, O_Neg, O_Pos, O_BitNot, O_TypeOf, O_Void, O_Delete,
    O_Inc, O_Dec
};

struct Script;
// Tembel ayristirma: govde ilk cagrida ayristirilir (cogu fonksiyon hic cagrilmaz)
struct LazyBody {
    std::shared_ptr<const std::wstring> src;   // belirteclerin geldigi kaynak
    uint32_t begin = 0, end = 0;               // '{' ... '}' (dahil) konumlari
    int line = 1;
};

struct FuncInfo {
    std::vector<Ast*> params;
    Ast* body = nullptr;           // Block ya da (exprBody ise) ifade
    bool arrow = false, exprBody = false, usesArgs = false, isAsync = false, isGen = false;
    bool simpleParams = true;
    std::wstring name;
    std::vector<std::wstring> varNames;   // yukari tasinan var adlari
    std::vector<Atom> varAtoms;           // varNames'in atomlari (ilk cagrida)
    int kind = 0;                  // 0 normal, 1 metot, 2 getter, 3 setter, 4 sinif kurucusu
    const std::wstring* file = nullptr;   // tanimlandigi betik (Script::name; hata yigini icin)
    Script* script = nullptr;             // tembel govde icin
    std::unique_ptr<LazyBody> lazy;       // dolu ise govde henuz ayristirilmadi
};

struct Ast {
    K k;
    uint8_t op = 0;
    bool flag = false;     // Member/Call: optional (?.), Update: prefix, Property: computed, VarDecl: -
    bool flag2 = false;    // Property: shorthand/method, Block: sozcuksel bildirim icerir, Member: optional zincir koku
    uint8_t kind = 0;      // VarDecl: 0 var 1 let 2 const; Property: 0 deger 1 get 2 set; ClassMember: 0 metot 1 get 2 set 3 alan
    bool isStatic = false;
    bool sus = false;      // alt agacta (ic fonksiyonlar haric) await/yield var: askiya alinabilir
    int line = 0;
    double num = 0;
    Str str;               // ad / dize / regex kaynagi
    std::wstring str2;     // regex bayraklari / etiket
    Ast *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
    std::vector<Ast*> list;
    std::vector<Str> cooked; // sablon dize parcalari
    std::vector<Str> raw;
    FuncInfo* fi = nullptr;
    Atom atom = nullptr;   // str'nin atomu (ilk kullanimda doldurulur)
};

inline Atom NodeAtom(Ast* n) { if (!n->atom) n->atom = AtomOf(*n->str); return n->atom; }

struct Script {
    // Dugumler bloklar halinde ayrilir (tek tek new yerine): hizli ve az parcali bellek
    std::vector<std::unique_ptr<Ast[]>> blocks;
    size_t used = 0, cap = 0;
    std::vector<std::unique_ptr<FuncInfo>> funcs;
    Ast* root = nullptr;
    std::wstring name;
    std::shared_ptr<const std::wstring> source;   // tembel fonksiyon govdeleri icin kaynak
    // Ayni tanimlayici/dize metni tek kopya (kucultulmus kodda adlar cok tekrarlanir)
    std::unordered_map<std::wstring, Str> strings;
    Ast* New(K k, int line) {
        if (used == cap) {
            size_t n = cap == 0 ? 256 : std::min<size_t>(cap * 2, 65536);
            blocks.emplace_back(new Ast[n]);
            used = 0;
            cap = n;
        }
        Ast* a = &blocks.back()[used++];
        a->k = k;
        a->line = line;
        return a;
    }
    const Str& Intern(const std::wstring& s) {
        auto it = strings.find(s);
        if (it != strings.end()) return it->second;
        return strings.emplace(s, std::make_shared<const std::wstring>(s)).first->second;
    }
};

struct SyntaxErr { std::wstring msg; int line; };

// Kaynagi ayristirir; hata durumunda SyntaxErr firlatir.
void ParseProgram(const std::wstring& src, Script& out);
// Tembel fonksiyon govdesini ayristirir (gerekirse); hata: SyntaxErr
void EnsureParsed(FuncInfo* fi);
// false: tum govdeler hemen ayristirilir (ayristirici testleri / sozdizimi denetimi icin)
void SetLazyParsing(bool on);

} // namespace js
} // namespace mer
