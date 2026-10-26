// merOS Shield - kucuk, sinirli JSON ayristirici (platformdan bagimsiz)
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace mer {

struct JsonValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type t = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<JsonValue> a;
    std::vector<std::pair<std::wstring, JsonValue>> o;

    const JsonValue* Get(const wchar_t* key) const;      // nesne alani (yoksa nullptr)
    const JsonValue* At(size_t i) const { return t == Arr && i < a.size() ? &a[i] : nullptr; }
    const std::wstring& AsStr() const;
    // "a.b.0.c" yolu
    const JsonValue* Path(const wchar_t* path) const;
};

// text[pos] '{' ya da '[' ile baslayan JSON degerini ayristirir; basarida pos degerin sonuna gelir.
// Sinirlar: derinlik 400, 4 milyon dugum.
bool ParseJson(const std::wstring& text, size_t& pos, JsonValue& out);

} // namespace mer
