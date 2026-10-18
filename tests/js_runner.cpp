// core.js gibi JS test dosyalarini calistirir: js_runner tests/js/core.js
#include "js.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <locale>
using namespace mer::js;
int main(int argc, char** argv) {
  std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
  std::wstring w; for (size_t i=0;i<s.size();) { unsigned char c=s[i]; uint32_t cp; int n;
    if(c<0x80){cp=c;n=1;} else if((c>>5)==6){cp=c&31;n=2;} else if((c>>4)==14){cp=c&15;n=3;} else {cp=c&7;n=4;}
    for(int k=1;k<n;k++) cp=(cp<<6)|(s[i+k]&63); w+=wchar_t(cp); i+=n; }
  Runtime rt; std::wstring out; bool ok = rt.EvalForTest(w, out);
  for (auto& l : rt.consoleLines) { std::string o; for (wchar_t c : l) { uint32_t cp=c; if(cp<0x80)o+=char(cp); else if(cp<0x800){o+=char(0xC0|(cp>>6));o+=char(0x80|(cp&63));} else {o+=char(0xE0|(cp>>12));o+=char(0x80|((cp>>6)&63));o+=char(0x80|(cp&63));} } printf("%s\n", o.c_str()); }
  std::string o; for (wchar_t c : out) { uint32_t cp=c; if(cp<0x80)o+=char(cp); else if(cp<0x800){o+=char(0xC0|(cp>>6));o+=char(0x80|(cp&63));} else {o+=char(0xE0|(cp>>12));o+=char(0x80|((cp>>6)&63));o+=char(0x80|(cp&63));} }
  printf("%s => %s\n", ok ? "OK" : "FAIL", o.c_str());
}
