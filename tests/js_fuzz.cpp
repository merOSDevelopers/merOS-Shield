// merJS rastgele girdi testi: bozuk/kotu niyetli betikler motoru cokertmemeli
#include <cstdio>
#include <random>
#include "../src/dom.h"
#include "../src/js/js.h"
using namespace mer;
int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 2000;
    const wchar_t* toks[] = {L"var ", L"let ", L"x", L"y", L"=", L"(", L")", L"{", L"}", L"[", L"]", L";", L",", L"+", L"-", L"*", L"/", L".", L"?.", L"??",
        L"=>", L"function", L"class", L"extends", L"super", L"this", L"new ", L"return ", L"if", L"else", L"for", L"of ", L"in ", L"while(0)", L"1", L"0", L"'s'",
        L"`a${", L"`", L"...", L"async ", L"await ", L"yield ", L"*", L"try{", L"}catch(e){", L"}finally{", L"throw ", L"typeof ", L"delete ", L"/re/g", L"document",
        L".body", L".appendChild", L"Array", L"Object", L"Promise", L"JSON.parse", L"Symbol()", L"null", L"undefined", L"\n", L"\\u0041", L"#", L":", L"?", L"get ", L"set ",
        L"switch(x){case 1:", L"break", L"continue", L"label:", L"with(x)", L"eval", L"arguments", L"=>{", L"0x", L"1e", L"<!--", L"/*", L"*/", L"//"};
    std::mt19937 rng(7);
    auto doc = ParseHTML(L"<body><div id=a></div></body>");
    for (int it = 0; it < N; it++) {
        js::Runtime rt;
        rt.stepBudget = 200000;
        rt.AttachDocument(doc.get(), nullptr);
        std::wstring src;
        int n = rng() % 60;
        for (int i = 0; i < n; i++) src += toks[rng() % (sizeof(toks) / sizeof(*toks))];
        rt.RunScript(src, L"fuzz");
        for (int k = 0; k < 3 && rt.NextTimerDue() >= 0; k++) rt.RunDueTimers(rt.Now() + 1e6);
    }
    printf("js fuzz ok %d\n", N);
}
