// merOS Shield - merJS : sözcük çözümleyici (lexer) ve ayrıştırıcı (parser)
// ES2015+ alt kumesi: let/const, ok fonksiyonlari, siniflar, sablon dizeler,
// destructuring, spread/rest, varsayilan parametreler, ?. ve ??, async/await ve uretec sozdizimi.
#include "js_ast.h"
#include <cmath>
#include <cstdlib>
#include <cwctype>

namespace mer {
namespace js {

namespace {

constexpr int kMaxNest = 1500;   // JS is parcaciklari 64 MB yiginla calisir

enum TT : uint8_t { T_EOF, T_Ident, T_Num, T_Str, T_Tpl, T_Regex, T_Punct };

// Sablon dizelerin ve duzenli ifadelerin ek verisi (cogu belirtecte yok: Tok kucuk kalir)
struct TokExtra {
    std::wstring flags;  // regex bayraklari
    std::vector<std::wstring> cooked, raws, exprs;
    std::vector<int> exprLines;
};

struct Tok {
    TT t = T_EOF;
    bool nl = false;     // oncesinde satir sonu var mi (ASI)
    bool escaped = false;
    bool ctrl = false;   // ')' : if/for/while/with basligini kapatiyor (ardindan '/' duzenli ifadedir)
    bool big = false;    // BigInt sabiti (123n): metni v'de
    int line = 1;
    uint32_t pos = 0;    // kaynaktaki baslangic konumu (tembel govdeler icin)
    double num = 0;
    std::wstring v;      // ad, dize degeri, noktalama, regex kaynagi
    std::shared_ptr<TokExtra> ex;
    TokExtra& X() { if (!ex) ex = std::make_shared<TokExtra>(); return *ex; }
};

// Hizli karsilastirma (wcslen yok): ilk karakter cogu zaman farkli
inline bool StrEq(const std::wstring& a, const wchar_t* b) {
    size_t n = a.size();
    if (n == 0) return b[0] == 0;
    if (a[0] != b[0]) return false;
    for (size_t i = 1; i < n; i++) if (b[i] != a[i] || b[i] == 0) return false;
    return b[n] == 0;
}

// ASCII karakter siniflari tablosu (1: tanimlayici basi, 2: tanimlayici devami, 4: bosluk)
struct CharTable {
    uint8_t c[128];
    CharTable() {
        for (int i = 0; i < 128; i++) {
            uint8_t f = 0;
            if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '$' || i == '_') f |= 3;
            if (i >= '0' && i <= '9') f |= 2;
            if (i == ' ' || i == '\t' || i == 0x0B || i == 0x0C) f |= 4;
            c[i] = f;
        }
    }
};
static const CharTable kCT;
inline bool IsLineTerm(wchar_t c) { return c == L'\n' || c == L'\r' || c == 0x2028 || c == 0x2029; }
inline bool IsIdStart(wchar_t c) { return c < 128 ? (kCT.c[c] & 1) != 0 : true; }
inline bool IsIdPart(wchar_t c) { return c < 128 ? (kCT.c[c] & 2) != 0 : true; }   // 0x200C/0x200D de > 127
inline bool IsSpace(wchar_t c) { return c < 128 ? (kCT.c[c] & 4) != 0 : (c == 0xA0 || c == 0xFEFF || (iswspace(c) && !IsLineTerm(c))); }

int Hex(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

void AppendCp(std::wstring& o, uint32_t cp) {
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
        cp -= 0x10000;
        o += wchar_t(0xD800 + (cp >> 10));
        o += wchar_t(0xDC00 + (cp & 0x3FF));
    } else o += wchar_t(cp);
}

const wchar_t* kPuncts[] = {
    L">>>=", L"...", L"===", L"!==", L"**=", L"<<=", L">>=", L">>>", L"&&=", L"||=", L"?\?=",
    L"=>", L"==", L"!=", L"<=", L">=", L"&&", L"||", L"??", L"?.", L"++", L"--", L"+=", L"-=", L"*=",
    L"/=", L"%=", L"&=", L"|=", L"^=", L"**", L"<<", L">>",
    L"{", L"}", L"(", L")", L"[", L"]", L";", L",", L"<", L">", L"+", L"-", L"*", L"/", L"%", L"&",
    L"|", L"^", L"!", L"~", L"?", L":", L"=", L".", L"@", nullptr};

class Lexer {
public:
    Lexer(const std::wstring& s, int line0) : s_(s), line_(line0) {}

    std::vector<Tok> Run() {
        std::vector<Tok> out;
        out.reserve(s_.size() / 5 + 16);
        bool nl = false;
        const size_t n = s_.size();
        while (true) {
            // bosluk ve yorumlar
            while (i_ < n) {
                wchar_t c = s_[i_];
                if (IsLineTerm(c)) { if (c == L'\n' || (c == L'\r' && !(i_ + 1 < n && s_[i_ + 1] == L'\n'))) line_++; nl = true; i_++; atLineStart_ = true; continue; }
                if (IsSpace(c)) { i_++; continue; }
                if (c == L'/' && i_ + 1 < n && s_[i_ + 1] == L'/') { while (i_ < n && !IsLineTerm(s_[i_])) i_++; continue; }
                if (c == L'/' && i_ + 1 < n && s_[i_ + 1] == L'*') {
                    size_t e = s_.find(L"*/", i_ + 2);
                    if (e == std::wstring::npos) Err(L"kapanmamis yorum");
                    for (size_t k = i_; k < e; k++) if (s_[k] == L'\n') { line_++; nl = true; }
                    i_ = e + 2;
                    continue;
                }
                if (c == L'<' && s_.compare(i_, 4, L"<!--") == 0) { while (i_ < n && !IsLineTerm(s_[i_])) i_++; continue; }
                if (c == L'-' && atLineStart_ && s_.compare(i_, 3, L"-->") == 0) { while (i_ < n && !IsLineTerm(s_[i_])) i_++; continue; }
                break;
            }
            Tok t;
            t.nl = nl;
            t.line = line_;
            t.pos = uint32_t(i_);
            nl = false;
            atLineStart_ = false;
            if (i_ >= n) { t.t = T_EOF; out.push_back(t); break; }
            if (out.size() > 4000000) Err(L"betik cok buyuk");
            wchar_t c = s_[i_];
            if (IsIdStart(c) || c == L'\\' || (c == L'#' && i_ + 1 < n && IsIdStart(s_[i_ + 1]))) {
                std::wstring id;
                // hizli yol: kacissiz tanimlayici tek seferde kopyalanir
                {
                    size_t st = i_, k = i_ + 1;
                    while (k < n && IsIdPart(s_[k])) k++;
                    if (c != L'\\' && (k >= n || s_[k] != L'\\')) { id.assign(s_, st, k - st); i_ = k; }
                    else if (c == L'#') { id += L'#'; i_++; }
                }
                while (i_ < n) {
                    wchar_t d = s_[i_];
                    if (d == L'\\') {
                        if (i_ + 1 >= n || s_[i_ + 1] != L'u') Err(L"gecersiz kacis");
                        i_ += 2;
                        AppendCp(id, ReadUnicodeEscape());
                        t.escaped = true;
                        continue;
                    }
                    if (!IsIdPart(d)) break;
                    id += d;
                    i_++;
                }
                t.t = T_Ident;
                t.v = std::move(id);
            } else if ((c >= L'0' && c <= L'9') || (c == L'.' && i_ + 1 < n && s_[i_ + 1] >= L'0' && s_[i_ + 1] <= L'9')) {
                t.t = T_Num;
                size_t numStart = i_;
                t.num = ReadNumber();
                if (lastBig_) {
                    t.big = true;
                    for (size_t k = numStart; k + 1 < i_; k++) if (s_[k] != L'_') t.v += s_[k];   // sondaki 'n' haric
                    lastBig_ = false;
                }
            } else if (c == L'"' || c == L'\'') {
                t.t = T_Str;
                t.v = ReadString(c);
            } else if (c == L'`') {
                t.t = T_Tpl;
                ReadTemplate(t);
            } else if (c == L'/' && RegexAllowed(out)) {
                t.t = T_Regex;
                ReadRegex(t);
            } else {
                t.t = T_Punct;
                bool found = false;
                // ilk karaktere gore aday noktalama listesi (uzundan kisaya; tablo bir kez kurulur)
                struct PunctIndex {
                    std::vector<int> by[128];
                    size_t len[128] = {0};
                    PunctIndex() { for (int k = 0; kPuncts[k]; k++) { len[k] = wcslen(kPuncts[k]); by[kPuncts[k][0] & 127].push_back(k); } }
                };
                static const PunctIndex kPI;
                static const std::vector<int> kNone;
                const std::vector<int>& cand = c < 128 ? kPI.by[c] : kNone;
                for (int k : cand) {
                    size_t len = kPI.len[k];
                    if (i_ + len <= n && wmemcmp(s_.data() + i_, kPuncts[k], len) == 0) {
                        // "?." ardindan rakam geliyorsa kosul operatorudur: a?.5:b
                        if (len == 2 && kPuncts[k][0] == L'?' && kPuncts[k][1] == L'.' && i_ + 2 < n && iswdigit(s_[i_ + 2])) continue;
                        t.v.assign(kPuncts[k], len);
                        i_ += len;
                        found = true;
                        break;
                    }
                }
                if (!found) Err(std::wstring(L"beklenmeyen karakter '") + c + L"'");
                if (StrEq(t.v, L"(")) {
                    bool ctl = false;
                    if (!out.empty() && out.back().t == T_Ident && !out.back().escaped) {
                        const std::wstring& w = out.back().v;
                        ctl = StrEq(w, L"if") || StrEq(w, L"for") || StrEq(w, L"while") || StrEq(w, L"with");
                    }
                    parenCtl_.push_back(ctl);
                } else if (StrEq(t.v, L")")) {
                    if (!parenCtl_.empty()) { t.ctrl = parenCtl_.back(); parenCtl_.pop_back(); }
                }
            }
            out.push_back(std::move(t));
        }
        return out;
    }

private:
    const std::wstring& s_;
    size_t i_ = 0;
    int line_;
    bool atLineStart_ = true;
    std::vector<bool> parenCtl_;
    bool lastBig_ = false;

    [[noreturn]] void Err(const std::wstring& m) { throw SyntaxErr{m, line_}; }

    uint32_t ReadUnicodeEscape() {
        // i_ "u" sonrasinda
        const size_t n = s_.size();
        if (i_ < n && s_[i_] == L'{') {
            i_++;
            uint32_t v = 0;
            while (i_ < n && s_[i_] != L'}') { int h = Hex(s_[i_]); if (h < 0) Err(L"gecersiz \\u{}"); v = v * 16 + uint32_t(h); if (v > 0x10FFFF) Err(L"gecersiz kod noktasi"); i_++; }
            i_++;
            return v;
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            if (i_ >= n) Err(L"gecersiz \\u");
            int h = Hex(s_[i_++]);
            if (h < 0) Err(L"gecersiz \\u");
            v = v * 16 + uint32_t(h);
        }
        return v;
    }

    double ReadNumber() {
        const size_t n = s_.size();
        size_t st = i_;
        if (s_[i_] == L'0' && i_ + 1 < n && (s_[i_ + 1] == L'x' || s_[i_ + 1] == L'X' || s_[i_ + 1] == L'o' || s_[i_ + 1] == L'O' || s_[i_ + 1] == L'b' || s_[i_ + 1] == L'B')) {
            wchar_t k = wchar_t(towlower(s_[i_ + 1]));
            int base = k == L'x' ? 16 : k == L'o' ? 8 : 2;
            i_ += 2;
            double v = 0;
            bool any = false;
            while (i_ < n) {
                if (s_[i_] == L'_') { i_++; continue; }
                int h = Hex(s_[i_]);
                if (h < 0 || h >= base) break;
                v = v * base + h;
                any = true;
                i_++;
            }
            if (!any) Err(L"gecersiz sayi");
            if (i_ < n && s_[i_] == L'n') { i_++; lastBig_ = true; }
            return v;
        }
        std::string buf;
        // eski sekizlik (0755) - sadece rakamsa
        bool legacyOctal = s_[i_] == L'0' && i_ + 1 < n && s_[i_ + 1] >= L'0' && s_[i_ + 1] <= L'7';
        while (i_ < n && ((s_[i_] >= L'0' && s_[i_] <= L'9') || s_[i_] == L'_')) { if (s_[i_] != L'_') buf += char(s_[i_]); i_++; }
        if (legacyOctal && (i_ >= n || (s_[i_] != L'.' && s_[i_] != L'e' && s_[i_] != L'E'))) {
            bool ok = true;
            for (char ch : buf) if (ch > '7') ok = false;
            if (ok) return double(strtoll(buf.c_str(), nullptr, 8));
        }
        if (i_ < n && s_[i_] == L'.') {
            buf += '.';
            i_++;
            while (i_ < n && ((s_[i_] >= L'0' && s_[i_] <= L'9') || s_[i_] == L'_')) { if (s_[i_] != L'_') buf += char(s_[i_]); i_++; }
        }
        if (i_ < n && (s_[i_] == L'e' || s_[i_] == L'E')) {
            size_t save = i_;
            std::string e = "e";
            i_++;
            if (i_ < n && (s_[i_] == L'+' || s_[i_] == L'-')) e += char(s_[i_++]);
            if (i_ < n && s_[i_] >= L'0' && s_[i_] <= L'9') {
                while (i_ < n && s_[i_] >= L'0' && s_[i_] <= L'9') e += char(s_[i_++]);
                buf += e;
            } else i_ = save;
        }
        if (i_ < n && s_[i_] == L'n') { i_++; lastBig_ = true; }   // BigInt sabiti
        if (i_ < n && IsIdStart(s_[i_])) Err(L"sayidan hemen sonra tanimlayici");
        (void)st;
        return strtod(buf.c_str(), nullptr);
    }

    bool ReadEscape(std::wstring& o) {
        // i_ ters egik cizgiden sonra; satir devami ise false (hicbir sey eklenmez)
        const size_t n = s_.size();
        if (i_ >= n) Err(L"kapanmamis dize");
        wchar_t c = s_[i_++];
        switch (c) {
        case L'n': o += L'\n'; break;
        case L't': o += L'\t'; break;
        case L'r': o += L'\r'; break;
        case L'b': o += L'\b'; break;
        case L'f': o += L'\f'; break;
        case L'v': o += L'\v'; break;
        case L'0':
            if (i_ < n && s_[i_] >= L'0' && s_[i_] <= L'7') {
                int v = 0, k = 0;
                i_--;
                while (k < 3 && i_ < n && s_[i_] >= L'0' && s_[i_] <= L'7') { v = v * 8 + (s_[i_] - L'0'); i_++; k++; }
                o += wchar_t(v);
            } else o += L'\0';
            break;
        case L'x': {
            if (i_ + 2 > n) Err(L"gecersiz \\x");
            int a = Hex(s_[i_]), b = Hex(s_[i_ + 1]);
            if (a < 0 || b < 0) Err(L"gecersiz \\x");
            o += wchar_t(a * 16 + b);
            i_ += 2;
            break;
        }
        case L'u': AppendCp(o, ReadUnicodeEscape()); break;
        case L'\r': if (i_ < n && s_[i_] == L'\n') i_++; line_++; return false;
        case L'\n': line_++; return false;
        case 0x2028: case 0x2029: return false;
        default:
            if (c >= L'1' && c <= L'7') {
                int v = c - L'0', k = 1;
                while (k < 3 && i_ < n && s_[i_] >= L'0' && s_[i_] <= L'7') { v = v * 8 + (s_[i_] - L'0'); i_++; k++; }
                o += wchar_t(v);
            } else o += c;
        }
        return true;
    }

    std::wstring ReadString(wchar_t q) {
        const size_t n = s_.size();
        std::wstring o;
        i_++;
        // hizli yol: kacis/satir sonu yoksa alt dizi
        {
            size_t k = i_;
            while (k < n) { wchar_t c = s_[k]; if (c == q || c == L'\\' || c == L'\n' || c == L'\r') break; k++; }
            if (k < n && s_[k] == q) { o.assign(s_, i_, k - i_); i_ = k + 1; return o; }
            o.assign(s_, i_, k - i_);
            i_ = k;
        }
        while (true) {
            if (i_ >= n) Err(L"kapanmamis dize");
            wchar_t c = s_[i_];
            if (c == q) { i_++; break; }
            if (c == L'\\') { i_++; ReadEscape(o); continue; }
            if (c == L'\n' || c == L'\r') Err(L"dize icinde satir sonu");
            o += c;
            i_++;
        }
        return o;
    }

    void ReadTemplate(Tok& t) {
        const size_t n = s_.size();
        i_++;
        std::wstring cooked, raw;
        while (true) {
            if (i_ >= n) Err(L"kapanmamis sablon dize");
            wchar_t c = s_[i_];
            if (c == L'`') { i_++; t.X().cooked.push_back(cooked); t.X().raws.push_back(raw); break; }
            if (c == L'\\') {
                size_t st = i_;
                i_++;
                ReadEscape(cooked);
                raw.append(s_, st, i_ - st);
                continue;
            }
            if (c == L'$' && i_ + 1 < n && s_[i_ + 1] == L'{') {
                t.X().cooked.push_back(cooked);
                t.X().raws.push_back(raw);
                cooked.clear();
                raw.clear();
                i_ += 2;
                size_t st = i_;
                int startLine = line_;
                SkipBalanced();
                t.X().exprs.push_back(s_.substr(st, i_ - st));
                t.X().exprLines.push_back(startLine);
                i_++; // '}'
                continue;
            }
            if (c == L'\n') line_++;
            if (c == L'\r') { i_++; if (i_ < n && s_[i_] == L'\n') i_++; cooked += L'\n'; raw += L'\n'; line_++; continue; }
            cooked += c;
            raw += c;
            i_++;
        }
    }

    // ${ ... } icinde dengeli kapanisa kadar ilerle (ic ice dizeler/sablonlar dahil)
    // Duzenli ifade mi bolme mi: ana lekserle ayni kural (onceki anlamli belirtece gore)
    static bool RegexAfter(wchar_t last, const std::wstring& lastWord) {
        if (last == 0) return true;
        if (last == L'a') {
            static const wchar_t* kw[] = {L"return", L"typeof", L"instanceof", L"in", L"of", L"new", L"delete", L"void",
                                          L"throw", L"case", L"do", L"else", L"yield", L"await", nullptr};
            for (int k = 0; kw[k]; k++) if (lastWord == kw[k]) return true;
            return false;
        }
        if (last == L'"' || last == L')' || last == L']') return false;
        return true;
    }

    void SkipBalanced() {
        const size_t n = s_.size();
        int depth = 0;
        wchar_t last = 0;            // onceki anlamli belirtec: 'a' ad/sayi, '"' dize/sablon/regex, yoksa noktalama
        std::wstring lastWord;
        while (i_ < n) {
            wchar_t c = s_[i_];
            if (c == L'{') { depth++; last = c; }
            else if (c == L'}') { if (depth == 0) return; depth--; last = c; }
            else if (c == L'"' || c == L'\'') { ReadString(c); last = L'"'; continue; }
            else if (c == L'`') { Tok tmp; ReadTemplate(tmp); last = L'"'; continue; }
            else if (c == L'/' && i_ + 1 < n && s_[i_ + 1] == L'/') { while (i_ < n && !IsLineTerm(s_[i_])) i_++; continue; }
            else if (c == L'/' && i_ + 1 < n && s_[i_ + 1] == L'*') { size_t e = s_.find(L"*/", i_ + 2); if (e == std::wstring::npos) Err(L"kapanmamis yorum"); for (size_t k = i_; k < e; k++) if (s_[k] == L'\n') line_++; i_ = e + 2; continue; }
            else if (c == L'/' && RegexAfter(last, lastWord)) { Tok tmp; ReadRegex(tmp); last = L'"'; continue; }
            else if (IsIdPart(c) || c == L'$' || c == L'\\') {
                size_t st = i_;
                while (i_ < n && (IsIdPart(s_[i_]) || s_[i_] == L'$' || s_[i_] == L'\\')) i_++;
                lastWord.assign(s_, st, i_ - st);
                last = L'a';
                continue;
            }
            else if (c == L'\n') line_++;
            else if (c != L' ' && c != L'\t' && c != L'\f' && c != L'\v' && c != 0xA0 && c != 0xFEFF && !IsLineTerm(c)) last = c;
            i_++;
        }
        Err(L"kapanmamis ${");
    }

    bool RegexAllowed(const std::vector<Tok>& out) {
        if (out.empty()) return true;
        const Tok& p = out.back();
        if (p.t == T_Num || p.t == T_Str || p.t == T_Tpl || p.t == T_Regex) return false;
        if (p.t == T_Ident) {
            static const wchar_t* kw[] = {L"return", L"typeof", L"instanceof", L"in", L"of", L"new", L"delete", L"void",
                                          L"throw", L"case", L"do", L"else", L"yield", L"await", nullptr};
            for (int k = 0; kw[k]; k++) if (p.v == kw[k]) return true;
            return false;
        }
        if (StrEq(p.v, L")")) return p.ctrl;   // if (x) /re/.test(y)
        if (StrEq(p.v, L"]")) return false;
        if (StrEq(p.v, L"}")) return true;
        if (StrEq(p.v, L"++") || StrEq(p.v, L"--")) return false;
        return true;
    }

    void ReadRegex(Tok& t) {
        const size_t n = s_.size();
        i_++;
        bool cls = false;
        std::wstring body;
        while (true) {
            if (i_ >= n || IsLineTerm(s_[i_])) Err(L"kapanmamis duzenli ifade");
            wchar_t c = s_[i_];
            if (c == L'\\') { body += c; i_++; if (i_ < n) body += s_[i_++]; continue; }
            if (c == L'[') cls = true;
            else if (c == L']') cls = false;
            else if (c == L'/' && !cls) { i_++; break; }
            body += c;
            i_++;
        }
        while (i_ < n && IsIdPart(s_[i_])) t.X().flags += s_[i_++];
        t.v = body;
    }
};

bool IsReserved(const std::wstring& s) {
    // hizli eleme: ayrilmis sozcukler 2..10 harf ve su harflerle baslar: b c d e f i n r s t v w
    if (s.size() < 2 || s.size() > 10) return false;
    wchar_t c0 = s[0];
    if (c0 < L'b' || c0 > L'w') return false;
    static const uint32_t kFirst = (1u << ('b' - 'a')) | (1u << ('c' - 'a')) | (1u << ('d' - 'a')) | (1u << ('e' - 'a')) | (1u << ('f' - 'a')) |
                                   (1u << ('i' - 'a')) | (1u << ('n' - 'a')) | (1u << ('r' - 'a')) | (1u << ('s' - 'a')) | (1u << ('t' - 'a')) |
                                   (1u << ('v' - 'a')) | (1u << ('w' - 'a'));
    if (!(kFirst & (1u << (c0 - L'a')))) return false;
    static const wchar_t* kw[] = {L"break", L"case", L"catch", L"class", L"const", L"continue", L"debugger", L"default",
                                  L"delete", L"do", L"else", L"export", L"extends", L"finally", L"for", L"function",
                                  L"if", L"import", L"in", L"instanceof", L"new", L"return", L"super", L"switch",
                                  L"this", L"throw", L"try", L"typeof", L"var", L"void", L"while", L"with",
                                  L"null", L"true", L"false", L"enum", nullptr};
    for (int k = 0; kw[k]; k++) if (StrEq(s, kw[k])) return true;
    return false;
}

Str MkStr(const std::wstring& s) { return std::make_shared<const std::wstring>(s); }

bool g_lazyParsing = true;

class Parser {
public:
    Parser(std::vector<Tok> toks, Script& sc, std::vector<FuncInfo*>& fstack, std::shared_ptr<const std::wstring> src = nullptr)
        : t_(std::move(toks)), sc_(sc), fs_(fstack), src_(std::move(src)) {}
    friend void ::mer::js::EnsureParsed(FuncInfo* fi);

    Ast* ParseProgramBody() {
        Ast* p = sc_.New(K::Program, 1);
        while (!AtEOF()) p->list.push_back(Statement());
        p->flag2 = HasLexical(p->list);
        return p;
    }

    Ast* ParseSingleExpression() {
        Ast* e = Expression(false);
        if (!AtEOF()) Err(L"sablon ifadesinde fazla belirtec");
        return e;
    }

private:
    std::vector<Tok> t_;
    size_t i_ = 0;
    Script& sc_;
    std::vector<FuncInfo*>& fs_;
    std::shared_ptr<const std::wstring> src_;   // bos: tembel ayristirma yok
    bool eagerNext_ = false;                    // siradaki govde hemen ayristirilsin (IIFE)
    int nest_ = 0;
    std::vector<bool> asyncCtx_{false};
    std::vector<bool> genCtx_{false};

    struct NestGuard {
        int& n;
        explicit NestGuard(int& x, Parser* p) : n(x) { if (++n > kMaxNest) p->Err(L"ic ice gecme cok derin"); }
        ~NestGuard() { n--; }
    };

    Str MkStr(const std::wstring& s) { return sc_.Intern(s); }   // ortak (interned) dize
    [[noreturn]] void Err(const std::wstring& m) { throw SyntaxErr{m, Cur().line}; }
    const Tok& Cur() const { return t_[std::min(i_, t_.size() - 1)]; }
    const Tok& PeekT(size_t k) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
    bool AtEOF() const { return Cur().t == T_EOF; }
    bool IsP(const wchar_t* p, size_t k = 0) const { const Tok& t = PeekT(k); return t.t == T_Punct && StrEq(t.v, p); }
    bool IsId(const wchar_t* w, size_t k = 0) const { const Tok& t = PeekT(k); return t.t == T_Ident && !t.escaped && StrEq(t.v, w); }
    bool Eat(const wchar_t* p) { if (IsP(p)) { i_++; return true; } return false; }
    void Expect(const wchar_t* p) { if (!Eat(p)) Err(std::wstring(L"'") + p + L"' bekleniyordu, '" + Cur().v + L"' bulundu"); }
    bool EatId(const wchar_t* w) { if (IsId(w)) { i_++; return true; } return false; }
    void ExpectId(const wchar_t* w) { if (!EatId(w)) Err(std::wstring(L"'") + w + L"' bekleniyordu, '" + Cur().v + L"' bulundu"); }
    int Line() const { return Cur().line; }

    void Semi() {
        if (Eat(L";")) return;
        if (IsP(L"}") || AtEOF() || Cur().nl) return;
        Err(L"';' bekleniyordu, '" + Cur().v + L"' bulundu");
    }

    std::wstring IdentName() {
        // ozellik adi: anahtar sozcukler dahil her tanimlayici
        if (Cur().t != T_Ident) Err(L"ad bekleniyordu");
        return t_[i_++].v;
    }

    std::wstring BindingIdent() {
        const Tok& t = Cur();
        if (t.t != T_Ident || (IsReserved(t.v) && !t.escaped)) Err(L"tanimlayici bekleniyordu: '" + t.v + L"'");
        i_++;
        return t.v;
    }

    FuncInfo* NewFunc() {
        sc_.funcs.emplace_back(new FuncInfo());
        return sc_.funcs.back().get();
    }

    void AddVarNames(Ast* pat) {
        if (!pat) return;
        FuncInfo* f = fs_.back();
        switch (pat->k) {
        case K::Ident: f->varNames.push_back(*pat->str); break;
        case K::ArrPat: for (auto* e : pat->list) AddVarNames(e); break;
        case K::ObjPat: for (auto* p : pat->list) AddVarNames(p->k == K::Rest ? p->a : p->b); break;
        case K::AssignPat: AddVarNames(pat->a); break;
        case K::Rest: AddVarNames(pat->a); break;
        default: break;
        }
    }

    static bool HasLexical(const std::vector<Ast*>& l) {
        for (auto* s : l) {
            if (s->k == K::VarDecl && s->kind != 0) return true;
            if (s->k == K::Func && s->flag2) return true;
            if (s->k == K::Class && s->flag2) return true;
        }
        return false;
    }

    // ---------- Deyimler ----------

    Ast* Statement() {
        NestGuard g(nest_, this);
        int ln = Line();
        if (IsP(L"{")) return Block();
        if (IsP(L";")) { i_++; return sc_.New(K::Empty, ln); }
        if (Cur().t == T_Ident && !Cur().escaped) {
            const std::wstring& w = Cur().v;
            if (StrEq(w, L"var") || StrEq(w, L"const") || (StrEq(w, L"let") && (PeekT(1).t == T_Ident || IsP(L"[", 1) || IsP(L"{", 1)))) {
                Ast* d = VarDecl(false);
                Semi();
                return d;
            }
            if (StrEq(w, L"function")) return FunctionNode(true, false);
            if (StrEq(w, L"async") && IsId(L"function", 1) && !PeekT(1).nl) { i_++; return FunctionNode(true, true); }
            if (StrEq(w, L"class")) return ClassNode(true);
            if (StrEq(w, L"if")) {
                i_++;
                Expect(L"(");
                Ast* s = sc_.New(K::If, ln);
                s->a = Expression(false);
                Expect(L")");
                s->b = Statement();
                if (IsId(L"else")) { i_++; s->c = Statement(); }
                return s;
            }
            if (StrEq(w, L"for")) return ForStatement();
            if (StrEq(w, L"while")) {
                i_++;
                Expect(L"(");
                Ast* s = sc_.New(K::While, ln);
                s->a = Expression(false);
                Expect(L")");
                s->b = Statement();
                return s;
            }
            if (StrEq(w, L"do")) {
                i_++;
                Ast* s = sc_.New(K::DoWhile, ln);
                s->b = Statement();
                if (!IsId(L"while")) Err(L"'while' bekleniyordu");
                i_++;
                Expect(L"(");
                s->a = Expression(false);
                Expect(L")");
                Eat(L";");
                return s;
            }
            if (StrEq(w, L"return")) {
                i_++;
                Ast* s = sc_.New(K::Return, ln);
                if (!IsP(L";") && !IsP(L"}") && !AtEOF() && !Cur().nl) s->a = Expression(false);
                Semi();
                return s;
            }
            if (StrEq(w, L"break") || StrEq(w, L"continue")) {
                i_++;
                Ast* s = sc_.New(StrEq(w, L"break") ? K::Break : K::Continue, ln);
                if (Cur().t == T_Ident && !Cur().nl && !IsReserved(Cur().v)) s->str2 = t_[i_++].v;
                Semi();
                return s;
            }
            if (StrEq(w, L"throw")) {
                i_++;
                if (Cur().nl) Err(L"throw sonrasi satir sonu");
                Ast* s = sc_.New(K::Throw, ln);
                s->a = Expression(false);
                Semi();
                return s;
            }
            if (StrEq(w, L"try")) return TryStatement();
            if (StrEq(w, L"switch")) return SwitchStatement();
            if (StrEq(w, L"debugger")) { i_++; Semi(); return sc_.New(K::Empty, ln); }
            if (StrEq(w, L"import") && !IsP(L"(", 1) && !IsP(L".", 1)) return ImportDeclaration();
            if (StrEq(w, L"export")) return ExportDeclaration();
            if (StrEq(w, L"with")) {
                i_++;
                Expect(L"(");
                Ast* s = sc_.New(K::With, ln);
                s->a = Expression(false);
                Expect(L")");
                s->b = Statement();
                return s;
            }
            if (PeekT(1).t == T_Punct && StrEq(PeekT(1).v, L":") && !IsReserved(w)) {
                i_ += 2;
                Ast* s = sc_.New(K::Labeled, ln);
                s->str2 = w;
                s->a = Statement();
                return s;
            }
        }
        Ast* s = sc_.New(K::ExprStmt, ln);
        s->a = Expression(false);
        Semi();
        return s;
    }

    Ast* Block() {
        Ast* b = sc_.New(K::Block, Line());
        Expect(L"{");
        while (!IsP(L"}")) {
            if (AtEOF()) Err(L"'}' bekleniyordu");
            b->list.push_back(Statement());
        }
        i_++;
        b->flag2 = HasLexical(b->list);
        return b;
    }

    Ast* VarDecl(bool noIn) {
        Ast* d = sc_.New(K::VarDecl, Line());
        const std::wstring& w = Cur().v;
        d->kind = StrEq(w, L"var") ? 0 : StrEq(w, L"let") ? 1 : 2;
        i_++;
        do {
            Ast* dc = sc_.New(K::Declarator, Line());
            dc->a = BindingTarget();
            if (Eat(L"=")) dc->b = Assignment(noIn);
            if (d->kind == 0) AddVarNames(dc->a);
            d->list.push_back(dc);
        } while (Eat(L","));
        return d;
    }

    Ast* BindingTarget() {
        if (IsP(L"[") || IsP(L"{")) {
            Ast* e = Primary();
            return ToPattern(e);
        }
        Ast* id = sc_.New(K::Ident, Line());
        id->str = MkStr(BindingIdent());
        return id;
    }

    Ast* ForStatement() {
        int ln = Line();
        i_++;
        bool isAwait = false;
        if (IsId(L"await")) {
            if (!asyncCtx_.back()) Err(L"'for await' yalnizca async fonksiyonlarda kullanilabilir");
            i_++;
            isAwait = true;
        }
        Expect(L"(");
        Ast* init = nullptr;
        if (Cur().t == T_Ident && (StrEq(Cur().v, L"var") || StrEq(Cur().v, L"const") || (StrEq(Cur().v, L"let") && (PeekT(1).t == T_Ident || IsP(L"[", 1) || IsP(L"{", 1))))) {
            init = VarDecl(true);
            if (init->list.size() == 1 && !init->list[0]->b && (IsId(L"of") || IsId(L"in"))) {
                bool of = IsId(L"of");
                i_++;
                if (isAwait && !of) Err(L"'for await' yalnizca 'of' ile kullanilabilir");
                Ast* s = sc_.New(of ? K::ForOf : K::ForIn, ln);
                s->flag = isAwait;
                s->a = init;
                s->b = of ? Assignment(false) : Expression(false);
                Expect(L")");
                s->c = Statement();
                return s;
            }
        } else if (!IsP(L";")) {
            Ast* e = Expression(true);
            if (IsId(L"of") || IsId(L"in")) {
                bool of = IsId(L"of");
                i_++;
                if (isAwait && !of) Err(L"'for await' yalnizca 'of' ile kullanilabilir");
                Ast* s = sc_.New(of ? K::ForOf : K::ForIn, ln);
                s->flag = isAwait;
                s->a = ToPattern(e);
                s->b = of ? Assignment(false) : Expression(false);
                Expect(L")");
                s->c = Statement();
                return s;
            }
            init = sc_.New(K::ExprStmt, ln);
            init->a = e;
        }
        if (isAwait) Err(L"'for await' yalnizca 'of' ile kullanilabilir");
        Expect(L";");
        Ast* s = sc_.New(K::For, ln);
        s->a = init;
        if (!IsP(L";")) s->b = Expression(false);
        Expect(L";");
        if (!IsP(L")")) s->c = Expression(false);
        Expect(L")");
        s->d = Statement();
        return s;
    }

    Ast* TryStatement() {
        Ast* s = sc_.New(K::Try, Line());
        i_++;
        s->a = Block();
        if (IsId(L"catch")) {
            i_++;
            if (Eat(L"(")) {
                s->b = BindingTarget();
                Expect(L")");
            }
            s->c = Block();
        }
        if (IsId(L"finally")) { i_++; s->d = Block(); }
        if (!s->c && !s->d) Err(L"catch ya da finally bekleniyordu");
        return s;
    }

    Ast* SwitchStatement() {
        Ast* s = sc_.New(K::Switch, Line());
        i_++;
        Expect(L"(");
        s->a = Expression(false);
        Expect(L")");
        Expect(L"{");
        while (!Eat(L"}")) {
            if (AtEOF()) Err(L"'}' bekleniyordu");
            Ast* c = sc_.New(K::Case, Line());
            if (IsId(L"case")) { i_++; c->a = Expression(false); }
            else if (IsId(L"default")) { i_++; }
            else Err(L"case bekleniyordu");
            Expect(L":");
            while (!IsId(L"case") && !IsId(L"default") && !IsP(L"}")) {
                if (AtEOF()) Err(L"'}' bekleniyordu");
                c->list.push_back(Statement());
            }
            s->list.push_back(c);
        }
        return s;
    }

    // ---------- ES Modülleri (import / export) ----------

    Ast* ImportDeclaration() {
        int ln = Line();
        i_++; // 'import'
        Ast* node = sc_.New(K::ImportDecl, ln);

        // 1) import "module-name";
        if (Cur().t == T_Str) {
            node->str = MkStr(Cur().v);
            i_++;
            Semi();
            return node;
        }

        // 2) import defaultExport ...
        if (Cur().t == T_Ident && !StrEq(Cur().v, L"*") && !StrEq(Cur().v, L"{")) {
            Ast* defSpec = sc_.New(K::ImportSpec, ln);
            defSpec->str = MkStr(L"default");
            defSpec->str2 = BindingIdent();
            node->list.push_back(defSpec);
            if (Eat(L",")) {
                if (Eat(L"*")) {
                    ExpectId(L"as");
                    Ast* nsSpec = sc_.New(K::ImportSpec, ln);
                    nsSpec->str = MkStr(L"*");
                    nsSpec->str2 = BindingIdent();
                    node->list.push_back(nsSpec);
                } else if (Eat(L"{")) {
                    ParseNamedImports(node);
                } else {
                    Err(L"beklenmeyen belirtec");
                }
            }
        } else if (Eat(L"*")) {
            // 3) import * as name from "module-name";
            ExpectId(L"as");
            Ast* nsSpec = sc_.New(K::ImportSpec, ln);
            nsSpec->str = MkStr(L"*");
            nsSpec->str2 = BindingIdent();
            node->list.push_back(nsSpec);
        } else if (Eat(L"{")) {
            // 4) import { a, b as c } from "module-name";
            ParseNamedImports(node);
        }

        ExpectId(L"from");
        if (Cur().t != T_Str) Err(L"modül yolu dizesi bekleniyordu");
        node->str = MkStr(Cur().v);
        i_++;
        Semi();
        return node;
    }

    void ParseNamedImports(Ast* node) {
        int ln = Line();
        while (!IsP(L"}")) {
            if (AtEOF()) Err(L"'}' bekleniyordu");
            std::wstring importedName = IdentName();
            std::wstring localName = importedName;
            if (EatId(L"as")) {
                localName = BindingIdent();
            }
            Ast* sp = sc_.New(K::ImportSpec, ln);
            sp->str = MkStr(importedName);
            sp->str2 = localName;
            node->list.push_back(sp);
            if (!Eat(L",")) break;
        }
        Expect(L"}");
    }

    Ast* ExportDeclaration() {
        int ln = Line();
        i_++; // 'export'

        // 1) export default ...
        if (EatId(L"default")) {
            Ast* node = sc_.New(K::ExportDefault, ln);
            if (IsId(L"function")) {
                node->a = FunctionNode(false, false);
            } else if (IsId(L"async") && IsId(L"function", 1)) {
                i_++;
                node->a = FunctionNode(false, true);
            } else if (IsId(L"class")) {
                node->a = ClassNode(false);
            } else {
                node->a = Assignment(false);
                Semi();
            }
            return node;
        }

        // 2) export * from '...' OR export * as ns from '...'
        if (Eat(L"*")) {
            Ast* node = sc_.New(K::ExportAll, ln);
            if (EatId(L"as")) {
                node->str2 = IdentName();
            }
            ExpectId(L"from");
            if (Cur().t != T_Str) Err(L"modül yolu dizesi bekleniyordu");
            node->str = MkStr(Cur().v);
            i_++;
            Semi();
            return node;
        }

        // 3) export var / let / const ...
        if (IsId(L"var") || IsId(L"let") || IsId(L"const")) {
            Ast* node = sc_.New(K::ExportDecl, ln);
            node->a = VarDecl(false);
            return node;
        }

        // 4) export function / class
        if (IsId(L"function")) {
            Ast* node = sc_.New(K::ExportDecl, ln);
            node->a = FunctionNode(true, false);
            return node;
        }
        if (IsId(L"async") && IsId(L"function", 1)) {
            i_++;
            Ast* node = sc_.New(K::ExportDecl, ln);
            node->a = FunctionNode(true, true);
            return node;
        }
        if (IsId(L"class")) {
            Ast* node = sc_.New(K::ExportDecl, ln);
            node->a = ClassNode(true);
            return node;
        }

        // 5) export { a, b as c } [from '...']
        if (Eat(L"{")) {
            Ast* node = sc_.New(K::ExportDecl, ln);
            while (!IsP(L"}")) {
                if (AtEOF()) Err(L"'}' bekleniyordu");
                std::wstring localName = IdentName();
                std::wstring exportedName = localName;
                if (EatId(L"as")) {
                    exportedName = IdentName();
                }
                Ast* sp = sc_.New(K::ImportSpec, ln);
                sp->str = MkStr(localName);
                sp->str2 = exportedName;
                node->list.push_back(sp);
                if (!Eat(L",")) break;
            }
            Expect(L"}");
            if (EatId(L"from")) {
                if (Cur().t != T_Str) Err(L"modül yolu dizesi bekleniyordu");
                node->str = MkStr(Cur().v);
                i_++;
            }
            Semi();
            return node;
        }

        Err(L"geçersiz export bildirimi");
    }

    // ---------- Fonksiyonlar ve siniflar ----------

    void Params(FuncInfo* f) {
        Expect(L"(");
        while (!IsP(L")")) {
            if (Eat(L"...")) {
                Ast* r = sc_.New(K::Rest, Line());
                r->a = BindingTarget();
                f->params.push_back(r);
                f->simpleParams = false;
                break;
            }
            Ast* p = BindingTarget();
            if (Eat(L"=")) {
                Ast* ap = sc_.New(K::AssignPat, Line());
                ap->a = p;
                ap->b = Assignment(false);
                p = ap;
            }
            if (p->k != K::Ident) f->simpleParams = false;
            f->params.push_back(p);
            if (!Eat(L",")) break;
        }
        Expect(L")");
    }

    // '{' konumundan eslesen '}' belirtecine (sablon/regex belirtecleri tek parca oldugundan basit sayim)
    size_t MatchBrace(size_t i) const {
        int depth = 0;
        for (size_t k = i; k < t_.size(); k++) {
            const Tok& t = t_[k];
            if (t.t == T_EOF) return size_t(-1);
            if (t.t != T_Punct || t.v.size() != 1) continue;
            if (t.v[0] == L'{') depth++;
            else if (t.v[0] == L'}') { if (--depth == 0) return k; }
        }
        return size_t(-1);
    }

    void FunctionBody(FuncInfo* f) {
        bool eager = eagerNext_;
        eagerNext_ = false;
        if (g_lazyParsing && src_ && !eager && IsP(L"{")) {
            size_t j = MatchBrace(i_);
            if (j != size_t(-1)) {
                std::unique_ptr<LazyBody> lz(new LazyBody());
                lz->src = src_;
                lz->begin = t_[i_].pos;
                lz->end = t_[j].pos + 1;
                lz->line = t_[i_].line;
                f->lazy = std::move(lz);
                f->script = &sc_;
                i_ = j + 1;
                return;
            }
        }
        fs_.push_back(f);
        asyncCtx_.push_back(f->isAsync);
        genCtx_.push_back(f->isGen);
        f->body = Block();
        genCtx_.pop_back();
        asyncCtx_.pop_back();
        fs_.pop_back();
    }

    // 'function' anahtar sozcugunde
    Ast* FunctionNode(bool decl, bool isAsync) {
        int ln = Line();
        // "(function(){...})()" / "!function(){...}()": hemen cagrilacak - tembel ayristirma faydasiz
        bool iife = i_ > 0 && t_[i_ - 1].t == T_Punct && (StrEq(t_[i_ - 1].v, L"(") || StrEq(t_[i_ - 1].v, L"!"));
        i_++; // function
        FuncInfo* f = NewFunc();
        f->isAsync = isAsync;
        if (Eat(L"*")) f->isGen = true;
        Ast* fn = sc_.New(K::Func, ln);
        fn->fi = f;
        fn->flag2 = decl;
        if (Cur().t == T_Ident && !IsP(L"(")) {
            f->name = BindingIdent();
            fn->str = MkStr(f->name);
        } else if (decl) Err(L"fonksiyon adi bekleniyordu");
        fs_.push_back(f); // parametrelerdeki 'arguments'/varsayilanlar icin
        Params(f);
        fs_.pop_back();
        eagerNext_ = iife;
        FunctionBody(f);
        if (decl && !fs_.empty()) fs_.back()->varNames.push_back(f->name);
        return fn;
    }

    Ast* MethodFunction(int kind, bool isAsync, bool isGen, const std::wstring& name) {
        FuncInfo* f = NewFunc();
        f->kind = kind;
        f->isAsync = isAsync;
        f->isGen = isGen;
        f->name = name;
        Ast* fn = sc_.New(K::Func, Line());
        fn->fi = f;
        fs_.push_back(f);
        Params(f);
        fs_.pop_back();
        FunctionBody(f);
        return fn;
    }

    Ast* ArrowFrom(size_t paramStart, bool isAsync) {
        // i_ parametrelerin basinda: ya tek tanimlayici ya da '('
        (void)paramStart;
        int ln = Line();
        FuncInfo* f = NewFunc();
        f->arrow = true;
        f->isAsync = isAsync;
        if (Cur().t == T_Ident) {
            Ast* id = sc_.New(K::Ident, ln);
            id->str = MkStr(BindingIdent());
            f->params.push_back(id);
        } else {
            fs_.push_back(f);
            Params(f);
            fs_.pop_back();
        }
        if (Cur().nl) Err(L"'=>' oncesinde satir sonu");
        Expect(L"=>");
        Ast* fn = sc_.New(K::Func, ln);
        fn->fi = f;
        // ok fonksiyonlari 'arguments' ve var adlarini cevreleyen fonksiyondan kullanir
        if (IsP(L"{")) {
            fs_.push_back(f);
            asyncCtx_.push_back(isAsync);
            genCtx_.push_back(false);
            f->body = Block();
            genCtx_.pop_back();
            asyncCtx_.pop_back();
            fs_.pop_back();
            if (f->usesArgs && fs_.size() > 0) fs_.back()->usesArgs = true;
        } else {
            f->exprBody = true;
            fs_.push_back(f);
            asyncCtx_.push_back(isAsync);
            genCtx_.push_back(false);
            f->body = Assignment(false);
            genCtx_.pop_back();
            asyncCtx_.pop_back();
            fs_.pop_back();
            if (f->usesArgs && fs_.size() > 0) fs_.back()->usesArgs = true;
        }
        return fn;
    }

    Ast* PropertyKey(bool& computed) {
        computed = false;
        int ln = Line();
        if (Eat(L"[")) {
            computed = true;
            Ast* e = Assignment(false);
            Expect(L"]");
            return e;
        }
        Ast* k = sc_.New(K::Str, ln);
        const Tok& t = Cur();
        if (t.t == T_Ident || t.t == T_Str) k->str = MkStr(t.v);
        else if (t.t == T_Num) {
            double d = t.num;
            wchar_t buf[64];
            if (d == std::floor(d) && std::fabs(d) < 1e15) swprintf(buf, 64, L"%.0f", d);
            else swprintf(buf, 64, L"%.17g", d);
            k->str = MkStr(buf);
        } else Err(L"ozellik adi bekleniyordu");
        i_++;
        return k;
    }

    Ast* ClassNode(bool decl) {
        int ln = Line();
        i_++; // class
        Ast* c = sc_.New(K::Class, ln);
        c->flag2 = decl;
        if (Cur().t == T_Ident && !IsId(L"extends") && !IsP(L"{")) {
            std::wstring nm = BindingIdent();
            c->str = MkStr(nm);
        } else if (decl) Err(L"sinif adi bekleniyordu");
        if (IsId(L"extends")) { i_++; c->a = LeftHandSide(); }
        Expect(L"{");
        while (!Eat(L"}")) {
            if (AtEOF()) Err(L"'}' bekleniyordu");
            if (Eat(L";")) continue;
            Ast* m = sc_.New(K::ClassMember, Line());
            if (IsId(L"static") && !IsP(L"(", 1) && !IsP(L"=", 1)) {
                i_++;
                m->isStatic = true;
                if (IsP(L"{")) {
                    // static { } blogu: hemen calisan statik metot gibi
                    FuncInfo* f = NewFunc();
                    f->kind = 1;
                    FunctionBody(f);
                    Ast* fn = sc_.New(K::Func, Line());
                    fn->fi = f;
                    m->kind = 4;
                    m->b = fn;
                    c->list.push_back(m);
                    continue;
                }
            }
            bool isAsync = false, isGen = false;
            int kind = 0;
            if (IsId(L"async") && !IsP(L"(", 1) && !IsP(L"=", 1) && !PeekT(1).nl) { i_++; isAsync = true; }
            if (Eat(L"*")) isGen = true;
            if ((IsId(L"get") || IsId(L"set")) && !IsP(L"(", 1) && !IsP(L"=", 1) && !IsP(L";", 1) && !IsP(L"}", 1)) {
                kind = StrEq(Cur().v, L"get") ? 1 : 2;
                i_++;
            }
            bool computed;
            m->a = PropertyKey(computed);
            m->flag = computed;
            std::wstring nm = (!computed && m->a->str) ? *m->a->str : L"";
            if (IsP(L"(")) {
                bool isCtor = !m->isStatic && !computed && StrEq(nm, L"constructor") && kind == 0;
                m->kind = uint8_t(kind);
                m->b = MethodFunction(isCtor ? 4 : (kind == 1 ? 2 : kind == 2 ? 3 : 1), isAsync, isGen, nm);
                if (isCtor) { c->b = m->b; continue; }
            } else {
                m->kind = 3; // alan
                if (Eat(L"=")) {
                    // alan baslaticisi: this'e erisen metot gibi derlenir
                    FuncInfo* f = NewFunc();
                    f->kind = 1;
                    f->exprBody = true;
                    fs_.push_back(f);
                    f->body = Assignment(false);
                    fs_.pop_back();
                    Ast* fn = sc_.New(K::Func, Line());
                    fn->fi = f;
                    m->b = fn;
                }
                Semi();
            }
            c->list.push_back(m);
        }
        if (decl && !c->str) Err(L"sinif adi bekleniyordu");
        return c;
    }

    // ---------- Ifadeler ----------

    Ast* Expression(bool noIn) {
        Ast* e = Assignment(noIn);
        if (IsP(L",")) {
            Ast* s = sc_.New(K::Seq, e->line);
            s->list.push_back(e);
            while (Eat(L",")) s->list.push_back(Assignment(noIn));
            return s;
        }
        return e;
    }

    size_t MatchParen(size_t open) const {
        int depth = 0;
        for (size_t k = open; k < t_.size(); k++) {
            const Tok& t = t_[k];
            if (t.t != T_Punct) continue;
            if (StrEq(t.v, L"(") || StrEq(t.v, L"[") || StrEq(t.v, L"{")) depth++;
            else if (StrEq(t.v, L")") || StrEq(t.v, L"]") || StrEq(t.v, L"}")) { if (--depth == 0) return k; }
            if (t.t == T_EOF) break;
        }
        return std::string::npos;
    }

    bool ArrowAhead() const {
        // i_ bir '(' uzerinde
        size_t m = MatchParen(i_);
        if (m == std::string::npos || m + 1 >= t_.size()) return false;
        const Tok& t = t_[m + 1];
        return t.t == T_Punct && StrEq(t.v, L"=>") && !t.nl;
    }

    static uint8_t AssignOp(const std::wstring& v) {
        if (StrEq(v, L"=")) return O_None;
        if (StrEq(v, L"+=")) return O_Add; if (StrEq(v, L"-=")) return O_Sub; if (StrEq(v, L"*=")) return O_Mul;
        if (StrEq(v, L"/=")) return O_Div; if (StrEq(v, L"%=")) return O_Mod; if (StrEq(v, L"**=")) return O_Exp;
        if (StrEq(v, L"<<=")) return O_Shl; if (StrEq(v, L">>=")) return O_Shr; if (StrEq(v, L">>>=")) return O_UShr;
        if (StrEq(v, L"&=")) return O_BitAnd; if (StrEq(v, L"|=")) return O_BitOr; if (StrEq(v, L"^=")) return O_BitXor;
        if (StrEq(v, L"&&=")) return O_And; if (StrEq(v, L"||=")) return O_Or; if (StrEq(v, L"?\?=")) return O_Nullish;
        return 255;
    }

    Ast* Assignment(bool noIn) {
        NestGuard g(nest_, this);
        int ln = Line();
        // ok fonksiyonlari
        if (Cur().t == T_Ident && !Cur().escaped) {
            if (StrEq(Cur().v, L"async") && !PeekT(1).nl) {
                if (PeekT(1).t == T_Ident && IsP(L"=>", 2)) { i_++; return ArrowFrom(i_, true); }
                if (IsP(L"(", 1)) {
                    size_t save = i_;
                    i_++;
                    if (ArrowAhead()) return ArrowFrom(i_, true);
                    i_ = save;
                }
            }
            if (IsP(L"=>", 1) && !IsReserved(Cur().v)) return ArrowFrom(i_, false);
            if (StrEq(Cur().v, L"yield") && genCtx_.back()) {
                i_++;
                Ast* y = sc_.New(K::Yield, ln);
                if (Eat(L"*")) y->flag = true;
                if (!Cur().nl && !IsP(L")") && !IsP(L"]") && !IsP(L"}") && !IsP(L",") && !IsP(L";") && !IsP(L":") && !AtEOF())
                    y->a = Assignment(noIn);
                return y;
            }
        }
        if (IsP(L"(") && ArrowAhead()) return ArrowFrom(i_, false);

        Ast* left = Conditional(noIn);
        if (Cur().t == T_Punct) {
            uint8_t op = AssignOp(Cur().v);
            if (op != 255) {
                i_++;
                Ast* a = sc_.New(K::Assign, ln);
                a->op = op;
                if (op == O_None && (left->k == K::Arr || left->k == K::ObjLit)) a->a = ToPattern(left);
                else {
                    if (left->k != K::Ident && left->k != K::Member && left->k != K::Index) Err(L"gecersiz atama hedefi");
                    a->a = left;
                }
                a->b = Assignment(noIn);
                return a;
            }
        }
        return left;
    }

    Ast* Conditional(bool noIn) {
        Ast* c = Binary(0, noIn);
        if (IsP(L"?")) {
            i_++;
            Ast* e = sc_.New(K::Cond, c->line);
            e->a = c;
            e->b = Assignment(false);
            Expect(L":");
            e->c = Assignment(noIn);
            return e;
        }
        return c;
    }

    int BinPrec(const Tok& t, bool noIn, uint8_t& op, bool& logic) const {
        logic = false;
        if (t.t == T_Ident && !t.escaped) {
            if (StrEq(t.v, L"instanceof")) { op = O_InstanceOf; return 8; }
            if (StrEq(t.v, L"in") && !noIn) { op = O_In; return 8; }
            return -1;
        }
        if (t.t != T_Punct) return -1;
        const std::wstring& v = t.v;
        if (StrEq(v, L"??")) { op = O_Nullish; logic = true; return 1; }
        if (StrEq(v, L"||")) { op = O_Or; logic = true; return 2; }
        if (StrEq(v, L"&&")) { op = O_And; logic = true; return 3; }
        if (StrEq(v, L"|")) { op = O_BitOr; return 4; }
        if (StrEq(v, L"^")) { op = O_BitXor; return 5; }
        if (StrEq(v, L"&")) { op = O_BitAnd; return 6; }
        if (StrEq(v, L"==")) { op = O_Eq; return 7; }
        if (StrEq(v, L"!=")) { op = O_Ne; return 7; }
        if (StrEq(v, L"===")) { op = O_SEq; return 7; }
        if (StrEq(v, L"!==")) { op = O_SNe; return 7; }
        if (StrEq(v, L"<")) { op = O_Lt; return 8; }
        if (StrEq(v, L">")) { op = O_Gt; return 8; }
        if (StrEq(v, L"<=")) { op = O_Le; return 8; }
        if (StrEq(v, L">=")) { op = O_Ge; return 8; }
        if (StrEq(v, L"<<")) { op = O_Shl; return 9; }
        if (StrEq(v, L">>")) { op = O_Shr; return 9; }
        if (StrEq(v, L">>>")) { op = O_UShr; return 9; }
        if (StrEq(v, L"+")) { op = O_Add; return 10; }
        if (StrEq(v, L"-")) { op = O_Sub; return 10; }
        if (StrEq(v, L"*")) { op = O_Mul; return 11; }
        if (StrEq(v, L"/")) { op = O_Div; return 11; }
        if (StrEq(v, L"%")) { op = O_Mod; return 11; }
        if (StrEq(v, L"**")) { op = O_Exp; return 12; }
        return -1;
    }

    Ast* Binary(int minPrec, bool noIn) {
        NestGuard g(nest_, this);
        Ast* left = Unary();
        while (true) {
            uint8_t op;
            bool logic;
            int p = BinPrec(Cur(), noIn, op, logic);
            if (p < 0 || p < minPrec || (p == minPrec && op != O_Exp && minPrec > 0 && false)) break;
            if (p <= minPrec - 1) break;
            int ln = Line();
            i_++;
            Ast* right = (op == O_Exp) ? Binary(p, noIn) : Binary(p + 1, noIn);
            Ast* b = sc_.New(logic ? K::Logic : K::Bin, ln);
            b->op = op;
            b->a = left;
            b->b = right;
            left = b;
        }
        return left;
    }

    Ast* Unary() {
        NestGuard g(nest_, this);
        int ln = Line();
        const Tok& t = Cur();
        if (t.t == T_Punct) {
            uint8_t op = 0;
            if (StrEq(t.v, L"!")) op = O_Not;
            else if (StrEq(t.v, L"-")) op = O_Neg;
            else if (StrEq(t.v, L"+")) op = O_Pos;
            else if (StrEq(t.v, L"~")) op = O_BitNot;
            if (op) {
                i_++;
                Ast* u = sc_.New(K::Unary, ln);
                u->op = op;
                u->a = Unary();
                return u;
            }
            if (StrEq(t.v, L"++") || StrEq(t.v, L"--")) {
                i_++;
                Ast* u = sc_.New(K::Update, ln);
                u->op = StrEq(t_[i_ - 1].v, L"++") ? O_Inc : O_Dec;
                u->flag = true;
                u->a = Unary();
                if (u->a->k != K::Ident && u->a->k != K::Member && u->a->k != K::Index) Err(L"gecersiz ++/-- hedefi");
                return u;
            }
        }
        if (t.t == T_Ident && !t.escaped) {
            uint8_t op = 0;
            if (StrEq(t.v, L"typeof")) op = O_TypeOf;
            else if (StrEq(t.v, L"void")) op = O_Void;
            else if (StrEq(t.v, L"delete")) op = O_Delete;
            if (op) {
                i_++;
                Ast* u = sc_.New(K::Unary, ln);
                u->op = op;
                u->a = Unary();
                return u;
            }
            if (StrEq(t.v, L"await") && asyncCtx_.back()) {
                i_++;
                Ast* u = sc_.New(K::Await, ln);
                u->a = Unary();
                return u;
            }
        }
        Ast* e = LeftHandSide();
        if (Cur().t == T_Punct && !Cur().nl && (StrEq(Cur().v, L"++") || StrEq(Cur().v, L"--"))) {
            if (e->k != K::Ident && e->k != K::Member && e->k != K::Index) Err(L"gecersiz ++/-- hedefi");
            Ast* u = sc_.New(K::Update, ln);
            u->op = StrEq(Cur().v, L"++") ? O_Inc : O_Dec;
            u->a = e;
            i_++;
            return u;
        }
        return e;
    }

    void Arguments(std::vector<Ast*>& out) {
        Expect(L"(");
        while (!IsP(L")")) {
            if (Eat(L"...")) {
                Ast* s = sc_.New(K::Spread, Line());
                s->a = Assignment(false);
                out.push_back(s);
            } else out.push_back(Assignment(false));
            if (!Eat(L",")) break;
        }
        Expect(L")");
    }

    Ast* TemplateNode(const Tok& t, int ln) {
        Ast* n = sc_.New(K::Tpl, ln);
        static const TokExtra kNoExtra;
        const TokExtra& tx = t.ex ? *t.ex : kNoExtra;
        for (auto& s : tx.cooked) n->cooked.push_back(MkStr(s));
        for (auto& s : tx.raws) n->raw.push_back(MkStr(s));
        for (size_t k = 0; k < tx.exprs.size(); k++) {
            Lexer lx(tx.exprs[k], tx.exprLines[k]);
            Parser sub(lx.Run(), sc_, fs_, src_ ? std::make_shared<const std::wstring>(tx.exprs[k]) : nullptr);
            sub.asyncCtx_ = asyncCtx_;
            sub.genCtx_ = genCtx_;
            sub.nest_ = nest_;
            n->list.push_back(sub.ParseSingleExpression());
        }
        return n;
    }

    Ast* LeftHandSide() {
        NestGuard g(nest_, this);
        int ln = Line();
        Ast* e = nullptr;
        if (IsId(L"new")) {
            i_++;
            if (Eat(L".")) {
                if (!IsId(L"target")) Err(L"new.target bekleniyordu");
                i_++;
                e = sc_.New(K::NewTarget, ln);
            } else {
                Ast* callee = nullptr;
                if (IsId(L"new")) callee = LeftHandSideNoCall();
                else callee = MemberOnly(Primary());
                Ast* n = sc_.New(K::New, ln);
                n->a = callee;
                if (IsP(L"(")) Arguments(n->list);
                e = n;
            }
        } else {
            e = Primary();
        }
        bool optional = false;
        while (true) {
            int l2 = Line();
            if (Eat(L".")) {
                Ast* m = sc_.New(K::Member, l2);
                m->a = e;
                m->str = MkStr(IdentName());
                e = m;
            } else if (IsP(L"?.")) {
                i_++;
                optional = true;
                if (IsP(L"(")) {
                    Ast* c = sc_.New(K::Call, l2);
                    c->a = e;
                    c->flag = true;
                    Arguments(c->list);
                    e = c;
                } else if (Eat(L"[")) {
                    Ast* m = sc_.New(K::Index, l2);
                    m->a = e;
                    m->b = Expression(false);
                    m->flag = true;
                    Expect(L"]");
                    e = m;
                } else {
                    Ast* m = sc_.New(K::Member, l2);
                    m->a = e;
                    m->str = MkStr(IdentName());
                    m->flag = true;
                    e = m;
                }
            } else if (Eat(L"[")) {
                Ast* m = sc_.New(K::Index, l2);
                m->a = e;
                m->b = Expression(false);
                Expect(L"]");
                e = m;
            } else if (IsP(L"(")) {
                Ast* c = sc_.New(K::Call, l2);
                c->a = e;
                Arguments(c->list);
                e = c;
            } else if (Cur().t == T_Tpl) {
                Ast* tt = sc_.New(K::TaggedTpl, l2);
                tt->a = e;
                tt->b = TemplateNode(Cur(), l2);
                i_++;
                e = tt;
            } else break;
        }
        if (optional) e->flag2 = true; // zincir koku: kisa devre burada yakalanir
        return e;
    }

    Ast* MemberOnly(Ast* e) {
        while (true) {
            int l2 = Line();
            if (Eat(L".")) {
                Ast* m = sc_.New(K::Member, l2);
                m->a = e;
                m->str = MkStr(IdentName());
                e = m;
            } else if (Eat(L"[")) {
                Ast* m = sc_.New(K::Index, l2);
                m->a = e;
                m->b = Expression(false);
                Expect(L"]");
                e = m;
            } else break;
        }
        return e;
    }

    Ast* LeftHandSideNoCall() {
        int ln = Line();
        i_++; // new
        Ast* callee = IsId(L"new") ? LeftHandSideNoCall() : MemberOnly(Primary());
        Ast* n = sc_.New(K::New, ln);
        n->a = callee;
        if (IsP(L"(")) Arguments(n->list);
        return MemberOnly(n);
    }

    Ast* Primary() {
        NestGuard g(nest_, this);
        int ln = Line();
        const Tok& t = Cur();
        switch (t.t) {
        case T_Num: {
            Ast* n = sc_.New(K::Num, ln);
            n->num = t.num;
            if (t.big) { n->flag2 = true; n->str = MkStr(t.v); }   // 123n
            i_++;
            return n;
        }
        case T_Str: { Ast* n = sc_.New(K::Str, ln); n->str = MkStr(t.v); i_++; return n; }
        case T_Tpl: { Ast* n = TemplateNode(t, ln); i_++; return n; }
        case T_Regex: { Ast* n = sc_.New(K::Regex, ln); n->str = MkStr(t.v); n->str2 = t.ex ? t.ex->flags : std::wstring(); i_++; return n; }
        case T_EOF: Err(L"beklenmeyen dosya sonu");
        case T_Punct: {
            if (StrEq(t.v, L"(")) {
                i_++;
                Ast* e = Expression(false);
                Expect(L")");
                return e;
            }
            if (StrEq(t.v, L"[")) return ArrayLiteral();
            if (StrEq(t.v, L"{")) return ObjectLiteral();
            Err(L"beklenmeyen '" + t.v + L"'");
        }
        case T_Ident: {
            if (!t.escaped) {
                const std::wstring& w = t.v;
                if (StrEq(w, L"function")) return FunctionNode(false, false);
                if (StrEq(w, L"async") && IsId(L"function", 1) && !PeekT(1).nl) { i_++; return FunctionNode(false, true); }
                if (StrEq(w, L"class")) return ClassNode(false);
                if (StrEq(w, L"this")) { i_++; return sc_.New(K::This, ln); }
                if (StrEq(w, L"null")) { i_++; return sc_.New(K::Null, ln); }
                if (StrEq(w, L"true")) { i_++; return sc_.New(K::True, ln); }
                if (StrEq(w, L"false")) { i_++; return sc_.New(K::False, ln); }
                if (StrEq(w, L"super")) { i_++; return sc_.New(K::Super, ln); }
                if (StrEq(w, L"import")) {
                    if (IsP(L"(", 1)) {
                        i_++;
                        Expect(L"(");
                        Ast* imp = sc_.New(K::ImportCall, ln);
                        imp->a = Assignment(false);
                        Expect(L")");
                        return imp;
                    }
                    if (IsP(L".", 1) && IsId(L"meta", 2)) {
                        i_ += 3;
                        return sc_.New(K::ImportMeta, ln);
                    }
                    Err(L"beklenmeyen 'import'");
                }
                if (IsReserved(w)) Err(L"beklenmeyen anahtar sozcuk '" + w + L"'");
            }
            Ast* id = sc_.New(K::Ident, ln);
            id->str = MkStr(t.v);
            if (StrEq(t.v, L"arguments")) for (auto* f : fs_) if (!f->arrow) { (void)f; }
            if (StrEq(t.v, L"arguments") && !fs_.empty()) {
                // en yakin ok-olmayan fonksiyon 'arguments' kullanir
                for (size_t k = fs_.size(); k-- > 0;) { fs_[k]->usesArgs = true; if (!fs_[k]->arrow) break; }
            }
            i_++;
            return id;
        }
        }
        Err(L"ifade bekleniyordu");
    }

    Ast* ArrayLiteral() {
        Ast* a = sc_.New(K::Arr, Line());
        Expect(L"[");
        while (!IsP(L"]")) {
            if (IsP(L",")) { i_++; a->list.push_back(sc_.New(K::Hole, Line())); continue; }
            if (Eat(L"...")) {
                Ast* s = sc_.New(K::Spread, Line());
                s->a = Assignment(false);
                a->list.push_back(s);
            } else a->list.push_back(Assignment(false));
            if (!IsP(L"]")) Expect(L",");
        }
        i_++;
        return a;
    }

    Ast* ObjectLiteral() {
        Ast* o = sc_.New(K::ObjLit, Line());
        Expect(L"{");
        while (!IsP(L"}")) {
            int ln = Line();
            if (Eat(L"...")) {
                Ast* s = sc_.New(K::Spread, ln);
                s->a = Assignment(false);
                o->list.push_back(s);
                if (!IsP(L"}")) Expect(L",");
                continue;
            }
            Ast* p = sc_.New(K::Property, ln);
            bool isAsync = false, isGen = false;
            int kind = 0;
            if (IsId(L"async") && !IsP(L",", 1) && !IsP(L":", 1) && !IsP(L"(", 1) && !IsP(L"}", 1) && !IsP(L"=", 1) && !PeekT(1).nl) { i_++; isAsync = true; }
            if (Eat(L"*")) isGen = true;
            if ((IsId(L"get") || IsId(L"set")) && !IsP(L",", 1) && !IsP(L":", 1) && !IsP(L"(", 1) && !IsP(L"}", 1) && !IsP(L"=", 1)) {
                kind = StrEq(Cur().v, L"get") ? 1 : 2;
                i_++;
            }
            bool shorthandCandidate = Cur().t == T_Ident;
            std::wstring identName = Cur().v;
            bool computed;
            p->a = PropertyKey(computed);
            p->flag = computed;
            p->kind = uint8_t(kind);
            std::wstring nm = (!computed && p->a->str) ? *p->a->str : L"";
            if (IsP(L"(")) {
                p->flag2 = true;
                p->b = MethodFunction(kind == 1 ? 2 : kind == 2 ? 3 : 1, isAsync, isGen, nm);
            } else if (kind == 0 && Eat(L":")) {
                p->b = Assignment(false);
            } else if (kind == 0 && shorthandCandidate && !computed) {
                if (IsReserved(identName)) Err(L"gecersiz kisa ozellik");
                Ast* id = sc_.New(K::Ident, ln);
                id->str = MkStr(identName);
                if (identName == L"arguments") for (size_t k = fs_.size(); k-- > 0;) { fs_[k]->usesArgs = true; if (!fs_[k]->arrow) break; }
                if (IsP(L"=")) { // { a = 1 } yalnizca kaliplarda gecerli
                    i_++;
                    Ast* as = sc_.New(K::Assign, ln);
                    as->op = O_None;
                    as->a = id;
                    as->b = Assignment(false);
                    p->b = as;
                } else p->b = id;
                p->flag2 = false;
                p->isStatic = true; // kisa yazim isareti
            } else Err(L"':' bekleniyordu");
            o->list.push_back(p);
            if (!IsP(L"}")) Expect(L",");
        }
        i_++;
        return o;
    }

    Ast* ToPattern(Ast* e) {
        switch (e->k) {
        case K::Ident:
        case K::Member:
        case K::Index:
        case K::ObjPat:
        case K::ArrPat:
            return e;
        case K::Arr: {
            Ast* p = sc_.New(K::ArrPat, e->line);
            for (auto* el : e->list) {
                if (el->k == K::Hole) p->list.push_back(el);
                else if (el->k == K::Spread) {
                    Ast* r = sc_.New(K::Rest, el->line);
                    r->a = ToPattern(el->a);
                    p->list.push_back(r);
                } else p->list.push_back(ToPattern(el));
            }
            return p;
        }
        case K::ObjLit: {
            Ast* p = sc_.New(K::ObjPat, e->line);
            for (auto* pr : e->list) {
                if (pr->k == K::Spread) {
                    Ast* r = sc_.New(K::Rest, pr->line);
                    r->a = ToPattern(pr->a);
                    p->list.push_back(r);
                    continue;
                }
                if (pr->kind != 0 || pr->flag2) Err(L"gecersiz kalip");
                Ast* q = sc_.New(K::Property, pr->line);
                q->a = pr->a;
                q->flag = pr->flag;
                q->b = ToPattern(pr->b);
                p->list.push_back(q);
            }
            return p;
        }
        case K::Assign: {
            if (e->op != O_None) Err(L"gecersiz kalip");
            Ast* p = sc_.New(K::AssignPat, e->line);
            p->a = ToPattern(e->a);
            p->b = e->b;
            return p;
        }
        default:
            Err(L"gecersiz atama kalibi");
        }
    }
};

// Askiya alma noktalarini isaretle: dugumun alt agacinda (ic fonksiyonlar haric)
// await, yield ya da 'for await' varsa sus = true. Yorumlayici bu dugumleri
// coroutine makinesinde, digerlerini hizli ozyinelemeli yolda calistirir.
static bool MarkSuspend(Ast* n, int depth) {
    if (!n) return false;
    if (depth > 20000) return true;   // asiri derin agac: guvenli tarafta kal
    bool s = false;
    s |= MarkSuspend(n->a, depth + 1);
    s |= MarkSuspend(n->b, depth + 1);
    s |= MarkSuspend(n->c, depth + 1);
    s |= MarkSuspend(n->d, depth + 1);
    for (Ast* x : n->list) s |= MarkSuspend(x, depth + 1);
    if (n->k == K::Func && n->fi) {
        for (Ast* p : n->fi->params) MarkSuspend(p, depth + 1);
        MarkSuspend(n->fi->body, depth + 1);
        s = false;   // ic fonksiyonun await/yield'i dis fonksiyonu askiya almaz
    }
    if (n->k == K::Await || n->k == K::Yield || (n->k == K::ForOf && n->flag)) s = true;
    n->sus = s;
    return s;
}

} // namespace

void ParseProgram(const std::wstring& src, Script& out) {
    Lexer lx(src, 1);
    std::vector<Tok> toks = lx.Run();
    out.funcs.emplace_back(new FuncInfo());
    FuncInfo* top = out.funcs.back().get();
    std::vector<FuncInfo*> fs{top};
    if (g_lazyParsing) out.source = std::make_shared<const std::wstring>(src);
    Parser p(std::move(toks), out, fs, out.source);
    out.root = p.ParseProgramBody();
    out.root->fi = top;
    for (auto& f : out.funcs) f->file = &out.name;
    MarkSuspend(out.root, 0);
}

void SetLazyParsing(bool on) { g_lazyParsing = on; }

void EnsureParsed(FuncInfo* fi) {
    if (!fi || !fi->lazy) return;
    LazyBody& lz = *fi->lazy;
    Script& sc = *fi->script;
    auto sub = std::make_shared<const std::wstring>(lz.src->substr(lz.begin, lz.end - lz.begin));
    Lexer lx(*sub, lz.line);
    std::vector<Tok> toks = lx.Run();
    size_t firstNew = sc.funcs.size();
    std::vector<FuncInfo*> fs{fi};
    Parser p(std::move(toks), sc, fs, sub);
    p.asyncCtx_ = {fi->isAsync};
    p.genCtx_ = {fi->isGen};
    Ast* body = p.Block();
    if (!p.AtEOF()) p.Err(L"fonksiyon govdesinden sonra fazla belirtec");
    fi->body = body;
    fi->lazy.reset();   // basarili: artik tembel degil (hata olursa tembel kalir, tekrar denenir)
    for (size_t i = firstNew; i < sc.funcs.size(); i++) sc.funcs[i]->file = &sc.name;
    MarkSuspend(fi->body, 1);
}

} // namespace js
} // namespace mer
