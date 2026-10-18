// merOS Shield - Uluslararasılaştırma ve Yerelleştirme (i18n) Katmanı
// İngilizce varsayılan dil, Türkçe alternatif olarak tam desteklenir.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace mer {
namespace i18n {

enum class Language {
    English = 0,
    Turkish = 1
};

enum class StringId {
    // UI & Durum çubuğu
    AppName,
    SearchOrAddress,
    NewTab,
    Loading,
    LoadingImages,
    BlockedTrackers,
    JsErrors,
    JsOk,
    JsDisabled,
    SnapshotSaved,
    SnapshotFailed,
    ScriptLoadFailed,
    NetInitFailed,
    PageTruncated,
    InlineScript,

    // Dahili Sayfalar - Giriş (Home)
    HomeTitle,
    HomeTagline,
    HomeSearchPlaceholder,
    HomeSearchButton,
    HomeSecurityTitle,
    HomeSecItem1,
    HomeSecItem2,
    HomeSecItem3,
    HomeSecItem4,
    HomeShortcuts,

    // Dahili Sayfalar - Motor (Engine)
    EngineTitle,
    EngineSubtitle,
    EnginePipelineTitle,
    EnginePipeline1,
    EnginePipeline2,
    EnginePipeline3,
    EnginePipeline4,
    EnginePipeline5,
    EnginePipeline6,
    EngineTestTitle,
    EngineTestFail,
    EngineTestOk,

    // Uyarı & Hata Sayfaları
    InsecureWarningTitle,
    InsecureWarningBody,
    InsecureWarningAction,
    InvalidAddressTitle,
    InvalidAddressBody,
    BlockedSchemeTitle,
    BlockedSchemeBody,
    SuspiciousAddressTitle,
    SuspiciousAddressBody,
    SuspiciousAddressAction,
    PageNotFoundTitle,
    PageNotFoundBody,
    UnsupportedFileTitle,
    UnsupportedFileBody,
    PageLoadFailedTitle,

    // Ağ Hataları
    NetErrTlsFailure,
    NetErrNameNotResolved,
    NetErrCannotConnect,
    NetErrTimeout,
    NetErrRedirectFailed,
    NetErrUnrecognizedScheme,
    NetErrOperationCancelled,
    NetErrGeneric,

    Count_
};

// Dil yönetimi
Language GetCurrentLanguage();
void SetCurrentLanguage(Language lang);

// Dil kodları (Navigator & Ağ için)
const wchar_t* GetLanguageCode();       // örn: L"en-US" (varsayılan) veya L"tr-TR"
const wchar_t* GetLanguageCodeShort();  // örn: L"en" veya L"tr"
std::vector<std::wstring> GetLanguageList();
std::wstring GetAcceptLanguageHeader();

// Metin getirme ve biçimlendirme
const wchar_t* Get(StringId id);
const wchar_t* Get(StringId id, Language lang);
std::wstring NetErrorText(DWORD err);

// Dahili sayfa oluşturucuları
std::wstring GetHomePageHtml();
std::wstring GetEnginePageHtml();

} // namespace i18n
} // namespace mer
