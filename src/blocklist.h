// merOS Shield - izleyici ve reklam engelleme (alt kaynak istekleri için)
#pragma once
#include <string>

namespace mer {

// Sunucu adi bilinen bir izleyici/reklam alan adina (ya da alt alan adina) aitse true.
bool IsBlockedHost(const std::wstring& host);

} // namespace mer
