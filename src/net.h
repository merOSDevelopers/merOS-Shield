// merOS Shield - ağ katmanı (WinHTTP + Windows SChannel TLS)
#pragma once
#include <windows.h>
#include <atomic>
#include <string>

namespace mer {

struct FetchResult {
    bool ok = false;
    DWORD status = 0;          // HTTP durum kodu
    DWORD error = 0;           // WinHTTP hata kodu
    std::string body;
    std::wstring contentType;
    std::wstring finalUrl;     // yonlendirmelerden sonraki adres
    bool truncated = false;
    std::wstring rawHeaders;   // CRLF ile ayrilmis yanit basliklari
    std::wstring statusText;   // HTTP durum metni (orn. OK)
};

struct FetchOptions {
    std::wstring method = L"GET";
    std::string body;                 // POST govdesi
    std::wstring contentType;         // POST icerik turu
    bool cookies = false;             // yalnizca birinci taraf belge istekleri (oturumluk, diske yazilmaz)
    std::wstring referer;             // bos = gonderme
    std::wstring extraHeaders;        // CRLF ile ayrilmis ek basliklar
};

bool NetInit();
void NetShutdown();

// Is parcaciginda cagrilir. cancel != expected olursa indirme yarida kesilir.
FetchResult Fetch(const std::wstring& url, size_t maxBytes, const std::atomic<int>* cancel = nullptr, int expected = 0,
                  const FetchOptions* opt = nullptr);

std::wstring DecodeBody(const std::string& body, const std::wstring& contentType);
std::wstring NetErrorText(DWORD err);

} // namespace mer
