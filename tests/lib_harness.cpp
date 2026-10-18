// Gercek kutuphaneleri (jQuery, lodash, preact...) merJS ile calistirma deney duzenegi
#include <cstdio>
#include <fstream>
#include <sstream>
#include "../src/dom.h"
#include "../src/js/js.h"
#include "../src/url.h"
#include "../src/util.h"
using namespace mer;
struct H : js::Host {
    void Log(int l, const std::wstring& m) override { std::printf("[log%d] %s\n", l, ToUtf8(m).c_str()); }
    void Navigate(const std::wstring&) override {}
    std::wstring PageUrl() override { return L"https://ornek.com/"; }
    std::wstring Resolve(const std::wstring& r) override { return ResolveUrl(L"https://ornek.com/", r); }
    void ViewportSize(double& w, double& h) override { w = 1200; h = 800; }
    bool NodeRect(const Node*, double& x, double& y, double& w, double& h) override { x = y = 0; w = 100; h = 20; return true; }
    void SetUrlNoNav(const std::wstring&) override {}
    void HistoryGo(int) override {}
    void RequestScript(Node*, const std::wstring&) override {}
    void SubmitForm(Node*, Node*) override {}
};
static std::wstring Read(const char* p) {
    std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
    std::wstring w; for (size_t i = 0; i < s.size();) { unsigned char c = s[i]; uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else { cp = c & 7; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 63);
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) { cp -= 0x10000; w += wchar_t(0xD800 + (cp >> 10)); w += wchar_t(0xDC00 + (cp & 0x3FF)); } else w += wchar_t(cp);
        i += n; }
    return w;
}
int main(int argc, char** argv) {
    auto doc = ParseHTML(L"<html><head><title>t</title></head><body><div id=app><ul class=liste><li>a</li><li class=s>b</li></ul><input id=q value=x></div></body></html>");
    H host; js::Runtime rt; rt.AttachDocument(doc.get(), &host);
    for (int i = 1; i < argc - 1; i++) {
        double t0 = rt.Now();
        bool ok = rt.RunScript(Read(argv[i]), L"lib");
        std::printf("%s yuklendi: %s (%.0f ms)\n", argv[i], ok ? "OK" : "HATA", rt.Now() - t0);
    }
    rt.FireLoadEvents();
    std::wstring out;
    bool ok = rt.EvalForTest(Read(argv[argc - 1]), out);
    for (int i = 0; i < 50 && rt.NextTimerDue() >= 0; i++) rt.RunDueTimers(rt.Now() + 100000.0 * (i + 1));
    std::wstring out2; rt.EvalForTest(L"typeof result2 === 'undefined' ? '' : result2", out2);
    std::printf("%s => %s %s\n", ok ? "OK" : "FAIL", ToUtf8(out).c_str(), ToUtf8(out2).c_str());
}
