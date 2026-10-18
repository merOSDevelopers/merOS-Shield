// Basit rastgele girdi testi: motor bozuk HTML/CSS ile cokmemeli.
#include <cstdio>
#include <cstdlib>
#include <random>
#include "../src/css.h"
#include "../src/dom.h"
#include "../src/layout.h"
#include "../src/style.h"
#include "../src/url.h"
using namespace mer;
class FM : public ITextMeasurer { public: std::vector<float> s; int Font(const Style& x) override { s.push_back(x.fontSize); return int(s.size())-1;} float Width(int f,const std::wstring&t) override {return float(t.size())*s[size_t(f)]*0.5f;} FontMetrics Metrics(int f) override {float z=s[size_t(f)];return {z*0.9f,z*1.2f,z*0.3f};}};
int main(int argc,char**argv){
  int N = argc>1?atoi(argv[1]):3000;
  const wchar_t* toks[]={L"<",L">",L"</",L"/>",L"div",L"p",L"li",L"ul",L"table",L"tr",L"td",L"a href='",L"'",L"\"",L"=",L"&",L"&#x",L";",L"amp",L"<style>",L"</style>",L"{",L"}",L":",L"color",L"var(--a",L")",L"--a:",L"@media",L"(",L"display:table-cell",L"margin:-99999px",L"width:1e30px",L"font-size:1e9px",L"<br>",L"<pre>",L"\n",L" ",L"x",L"ç",L"\xD800",L"<script>",L"</",L"colspan=999",L"<!--",L"-->",L"calc(",L"%",L"#",L".",L"*",L">",L"\x0",L"<img alt=",L"<input "};
  std::mt19937 rng(42);
  for(int it=0;it<N;it++){
    std::wstring h; int n=rng()%400;
    for(int i=0;i<n;i++) h+=toks[rng()%(sizeof(toks)/sizeof(*toks))];
    auto d=ParseHTML(h); StyleSheet sh; for(auto&x:d->inlineStyles) sh.Add(x); sh.Add(h); sh.BuildIndex();
    auto st=ComputeStyles(*d,sh); FM fm; PageLayout pl; LayoutPage(*d,st,float(rng()%1500),fm,pl);
    ResolveUrl(L"https://a.com/b/c", h.substr(0,60)); ParseUrl(h.substr(0,80));
  }
  printf("fuzz ok %d\n",N);
}
