// merOS Shield - YouTube görünümü
// YouTube'un masaustu uygulamasi on megabaytlarca JavaScript (Polymer, Custom Elements,
// Shadow DOM, MSE) gerektirir. merOS Shield bunun yerine sayfanin sunucudan gelen verisini
// (ytInitialData / ytInitialPlayerResponse) okur ve kendi motoruyla hizli, hafif, izleyicisiz
// bir YouTube sayfasi cizer: ana sayfa, arama, kanal, video sayfasi ve onerilenler.
#pragma once
#include <string>

namespace mer {

// youtube.com / www / m. / youtu.be adresi mi?
bool IsYouTubeUrl(const std::wstring& url);
// Masaustu www.youtube.com adresine cevirir (youtu.be/ID -> /watch?v=ID, m. -> www.)
std::wstring NormalizeYouTubeUrl(const std::wstring& url);
// Indirilen YouTube HTML'inden merOS YouTube sayfasi uretir. Veri bulunamazsa false.
bool BuildYouTubePage(const std::wstring& html, const std::wstring& pageUrl, bool turkish, std::wstring& out);
// meros-yt-play: baglantisinin hedefi (izin verilen tek dis adres: youtube.com/watch)
bool YouTubeExternalTarget(const std::wstring& href, std::wstring& target);

} // namespace mer
