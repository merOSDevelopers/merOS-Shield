// core.js / async.js gibi JS test dosyalarini calistirir: js_runner tests/js/async.js
// Betikten sonra zamanlayicilar (sanal zamanla) bitene kadar calistirilir, sonra 'result' okunur.
// --dom: bos bir belge baglanir (DOM/medya testleri icin: js_runner --dom tests/js/media.js)
#include "js.h"
#include "../dom.h"
#include "../url.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <locale>
#include <algorithm>
#include <windows.h>
using namespace mer::js;
using mer::Node;
static std::atomic<int> g_frames{0};
struct TestHost : Host {
  void Log(int, const std::wstring&) override {}
  void Navigate(const std::wstring&) override {}
  std::wstring PageUrl() override { return L"https://ornek.com/"; }
  std::wstring Resolve(const std::wstring& r) override { return mer::ResolveUrl(L"https://ornek.com/", r); }
  void ViewportSize(double& w, double& h) override { w = 1200; h = 800; }
  bool NodeRect(const Node*, double& x, double& y, double& w, double& h) override { x = y = 0; w = 100; h = 20; return true; }
  void SetUrlNoNav(const std::wstring&) override {}
  void HistoryGo(int) override {}
  void RequestScript(Node*, const std::wstring&) override {}
  void SubmitForm(Node*, Node*) override {}
  std::function<void(int, int, std::shared_ptr<std::vector<uint8_t>>)> VideoSink(const Node*) override {
    return [](int, int, std::shared_ptr<std::vector<uint8_t>>) { g_frames++; };
  }
};
static std::string U8(const std::wstring& w) {
  std::string o; for (wchar_t c : w) { uint32_t cp=c; if(cp<0x80)o+=char(cp); else if(cp<0x800){o+=char(0xC0|(cp>>6));o+=char(0x80|(cp&63));} else if(cp<0x10000){o+=char(0xE0|(cp>>12));o+=char(0x80|((cp>>6)&63));o+=char(0x80|(cp&63));} else {o+=char(0xF0|(cp>>18));o+=char(0x80|((cp>>12)&63));o+=char(0x80|((cp>>6)&63));o+=char(0x80|(cp&63));} }
  return o;
}
int main(int argc, char** argv) {
  if (argc < 2) { printf("kullanim: js_runner [--dom] dosya.js\n"); return 2; }
  bool dom = std::string(argv[1]) == "--dom";
  if (dom && argc < 3) return 2;
  std::ifstream f(argv[dom ? 2 : 1]); std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
  std::wstring w; for (size_t i=0;i<s.size();) { unsigned char c=s[i]; uint32_t cp; int n;
    if(c<0x80){cp=c;n=1;} else if((c>>5)==6){cp=c&31;n=2;} else if((c>>4)==14){cp=c&15;n=3;} else {cp=c&7;n=4;}
    for(int k=1;k<n && i+k<s.size();k++) cp=(cp<<6)|(s[i+k]&63);
    if (sizeof(wchar_t)==2 && cp>=0x10000) { cp-=0x10000; w+=wchar_t(0xD800+(cp>>10)); w+=wchar_t(0xDC00+(cp&0x3FF)); } else w+=wchar_t(cp); i+=n; }
  std::unique_ptr<mer::Document> doc;
  TestHost host;
  Runtime rt;
  if (dom) { doc = mer::ParseHTML(L"<html><head><title>t</title></head><body></body></html>"); rt.AttachDocument(doc.get(), &host); }
  std::wstring out; bool ok = rt.EvalForTest(w, out);
  if (!ok) printf("ilk calistirma hatasi: %s\n", U8(out).c_str());
  size_t cells0 = rt.CellCount(), cellsMax = cells0;
  for (int i = 0; i < 100000 && rt.NextTimerDue() >= 0; i++) {
    double due = rt.NextTimerDue();
    // medya etkinse gercek zaman: oynatici kendi is parcaciginda gercek saatle ilerler
    if (rt.media && MediaNextDue(rt) >= 0) {
      double now = rt.Now();
      if (due > now) Sleep(DWORD(std::min(50.0, due - now)));
      due = std::max(due, rt.Now());
    }
    if (getenv("MER_RUNNER_DEBUG") && i % 25 == 0) { std::wstring o; rt.EvalForTest(L"typeof ev==='undefined'?'':ev.join(',')+' t='+(typeof v!=='undefined'?v.currentTime:'')", o); printf("[%d] due=%.0f %s\n", i, due - rt.Now(), U8(o).c_str()); fflush(stdout); }
    rt.RunDueTimers(due);
    // medya testleri: sonuc hazirsa dur (oynatici etkin kalsa bile)
    if (rt.media && i % 10 == 0) { std::wstring o; rt.EvalForTest(L"0", o); if (o.find(L"GECTI") != std::wstring::npos || o.find(L"HATALAR") != std::wstring::npos) break; }
    if (rt.CellCount() > cellsMax) cellsMax = rt.CellCount();
  }
  rt.EvalForTest(L"result", out);
  for (auto& l : rt.consoleLines) printf("%s\n", U8(l).c_str());
  if (dom) printf("[gosterilen video karesi: %d]\n", g_frames.load());
  printf("[hucre: baslangic %zu, en fazla %zu, son %zu]\n", cells0, cellsMax, rt.CellCount());
  printf("%s => %s\n", ok ? "OK" : "FAIL", U8(out).c_str());
  return ok && out.find(L"GECTI") != std::wstring::npos ? 0 : 1;
}
