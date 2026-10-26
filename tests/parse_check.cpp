// Ayristirici dogrulama: dosya listesindeki her betigi merJS ile ayristirir.
#include "js_ast.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
using namespace mer::js;
static std::wstring U(const std::string& s) {
  std::wstring w; for (size_t i = 0; i < s.size();) { unsigned char c = s[i]; uint32_t cp; int n;
    if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else { cp = c & 7; n = 4; }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 63);
    w += wchar_t(cp); i += n; }
  return w;
}
int main(int argc, char** argv) {
  mer::js::SetLazyParsing(false);   // tum fonksiyon govdeleri denetlensin
  std::ifstream list(argv[1]); std::string path; int fails = 0, n = 0;
  while (std::getline(list, path)) {
    std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    Script sc; n++;
    try { ParseProgram(U(ss.str()), sc); }
    catch (SyntaxErr& e) { fails++; printf("FAIL\t%s\t%d\t%ls\n", path.c_str(), e.line, e.msg.c_str()); fflush(stdout); }
    catch (...) { fails++; printf("FAIL\t%s\t0\t(istisna)\n", path.c_str()); }
  }
  fprintf(stderr, "%d/%d basarisiz\n", fails, n);
}
