// merOS Shield - kucuk JSON ayristirici
#include "json.h"
#include <cstdlib>
#include <cwchar>

namespace mer {

namespace {

struct P {
    const std::wstring& s;
    size_t i;
    size_t nodes = 0;
    void Ws() { while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n' || s[i] == L'\r')) i++; }
    int Hex(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }
    bool String(std::wstring& out) {
        if (i >= s.size() || s[i] != L'"') return false;
        i++;
        while (i < s.size()) {
            wchar_t c = s[i++];
            if (c == L'"') return true;
            if (c != L'\\') { out += c; continue; }
            if (i >= s.size()) return false;
            wchar_t e = s[i++];
            switch (e) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'u': {
                if (i + 4 > s.size()) return false;
                int v = 0;
                for (int k = 0; k < 4; k++) { int h = Hex(s[i + size_t(k)]); if (h < 0) return false; v = v * 16 + h; }
                i += 4;
                out += wchar_t(v);
                break;
            }
            default: out += e;
            }
        }
        return false;
    }
    bool Value(JsonValue& v, int depth) {
        if (depth > 400 || ++nodes > 4000000) return false;
        Ws();
        if (i >= s.size()) return false;
        wchar_t c = s[i];
        if (c == L'{') {
            v.t = JsonValue::Obj;
            i++;
            Ws();
            if (i < s.size() && s[i] == L'}') { i++; return true; }
            for (;;) {
                Ws();
                std::wstring key;
                if (!String(key)) return false;
                Ws();
                if (i >= s.size() || s[i] != L':') return false;
                i++;
                v.o.emplace_back(std::move(key), JsonValue());
                if (!Value(v.o.back().second, depth + 1)) return false;
                Ws();
                if (i < s.size() && s[i] == L',') { i++; continue; }
                if (i < s.size() && s[i] == L'}') { i++; return true; }
                return false;
            }
        }
        if (c == L'[') {
            v.t = JsonValue::Arr;
            i++;
            Ws();
            if (i < s.size() && s[i] == L']') { i++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!Value(v.a.back(), depth + 1)) return false;
                Ws();
                if (i < s.size() && s[i] == L',') { i++; continue; }
                if (i < s.size() && s[i] == L']') { i++; return true; }
                return false;
            }
        }
        if (c == L'"') { v.t = JsonValue::Str; return String(v.s); }
        if (s.compare(i, 4, L"true") == 0) { v.t = JsonValue::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, L"false") == 0) { v.t = JsonValue::Bool; i += 5; return true; }
        if (s.compare(i, 4, L"null") == 0) { v.t = JsonValue::Null; i += 4; return true; }
        const wchar_t* b = s.c_str() + i;
        wchar_t* e = nullptr;
        double d = wcstod(b, &e);
        if (e == b) return false;
        i += size_t(e - b);
        v.t = JsonValue::Num;
        v.n = d;
        return true;
    }
};

} // namespace

const JsonValue* JsonValue::Get(const wchar_t* key) const {
    if (t != Obj) return nullptr;
    for (auto& kv : o) if (kv.first == key) return &kv.second;
    return nullptr;
}

const std::wstring& JsonValue::AsStr() const {
    static const std::wstring kEmpty;
    return t == Str ? s : kEmpty;
}

const JsonValue* JsonValue::Path(const wchar_t* path) const {
    const JsonValue* cur = this;
    std::wstring part;
    for (const wchar_t* p = path;; p++) {
        if (*p == L'.' || *p == 0) {
            if (!part.empty()) {
                if (!cur) return nullptr;
                if (cur->t == Arr) {
                    wchar_t* e = nullptr;
                    unsigned long idx = wcstoul(part.c_str(), &e, 10);
                    cur = (e && !*e) ? cur->At(idx) : nullptr;
                } else cur = cur->Get(part.c_str());
                part.clear();
            }
            if (*p == 0) break;
        } else part += *p;
    }
    return cur;
}

bool ParseJson(const std::wstring& text, size_t& pos, JsonValue& out) {
    P p{text, pos};
    if (!p.Value(out, 0)) return false;
    pos = p.i;
    return true;
}

} // namespace mer
