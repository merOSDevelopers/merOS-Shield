// merOS YouTube gorunumu testi: kaydedilmis YouTube sayfalarindan uretim
#include <cstdio>
#include <fstream>
#include <sstream>
#include "../src/util.h"
#include "../src/youtube.h"

using namespace mer;

static std::wstring ReadUtf8(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    std::string b = ss.str();
    std::wstring o;
    for (size_t i = 0; i < b.size();) {
        unsigned char c = (unsigned char)b[i];
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else if ((c >> 3) == 30) { cp = c & 7; n = 4; } else { i++; continue; }
        if (i + n > b.size()) break;
        for (int k = 1; k < n; k++) cp = (cp << 6) | (b[i + k] & 63);
        AppendCodepoint(o, cp);
        i += n;
    }
    return o;
}

int main(int argc, char** argv) {
    // kullanim: youtube_test <html> <sayfa-adresi> <cikti.html>
    if (argc < 4) return 2;
    std::wstring html = ReadUtf8(argv[1]);
    std::wstring url;
    for (const char* p = argv[2]; *p; p++) url += wchar_t(*p);
    std::wstring out;
    bool ok = BuildYouTubePage(html, url, true, out);
    size_t cards = 0, pos = 0;
    while ((pos = out.find(L"class=kart", pos)) != std::wstring::npos) { cards++; pos++; }
    size_t small = 0; pos = 0;
    while ((pos = out.find(L"class=oneri", pos)) != std::wstring::npos) { small++; pos++; }
    std::printf("%s ok=%d kart=%zu oneri=%zu boyut=%zu\n", argv[1], ok, cards, small, out.size());
    std::string u8 = ToUtf8(out);
    std::ofstream(argv[3], std::ios::binary) << u8;
    return ok ? 0 : 1;
}
