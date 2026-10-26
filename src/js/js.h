// merOS Shield - merJS : kendi JavaScript motorumuz
// Agac-gezen (tree-walking) yorumlayici, isaretle-supur (mark & sweep) cop toplayici,
// adim/bellek/ozyineleme sinirlari ile korumali calisma.
#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mer {
struct Node;
struct Document;
class StyleSheet;

namespace js {
struct FuncInfo;
struct MediaState;   // js_media.cpp

struct Obj;
struct Env;
struct Ast;
class Runtime;

using Str = std::shared_ptr<const std::wstring>;

// ---------------- Deger ----------------
struct Value {
    enum Type : uint8_t { Undef, Null, Bool, Num, String, Symbol, Object, BigInt };   // BigInt: s icinde paketli (js_bigint.h)
    Type t = Undef;
    bool b = false;
    double n = 0;
    Str s;          // String ve Symbol (sembol anahtari benzersiz bir dizedir)
    Obj* o = nullptr;

    Value() {}
    static Value Null_() { Value v; v.t = Null; return v; }
    static Value B(bool x) { Value v; v.t = Bool; v.b = x; return v; }
    static Value N(double x) { Value v; v.t = Num; v.n = x; return v; }
    // Not: nesne const olmayan wstring olarak olusturulur (s += x hizli yolu tek sahipli dizgeyi yerinde buyutur)
    static Value S(const std::wstring& x) { Value v; v.t = String; v.s = std::make_shared<std::wstring>(x); return v; }
    static Value S(std::wstring&& x) { Value v; v.t = String; v.s = std::make_shared<std::wstring>(std::move(x)); return v; }
    static Value S(Str x) { Value v; v.t = String; v.s = std::move(x); return v; }
    static Value O(Obj* x) { Value v; if (x) { v.t = Object; v.o = x; } return v; }
    // Dizi boslugu (hole): Array(3), [1,,2], delete a[i]. undefined gibi davranir; forEach/map/keys atlar.
    static Value Hole() { Value v; v.b = true; return v; }
    bool IsHole() const { return t == Undef && b; }
    bool IsUndef() const { return t == Undef; }
    bool IsNullish() const { return t == Undef || t == Null; }
    bool IsObj() const { return t == Object; }
    bool IsStr() const { return t == String; }
    bool IsNum() const { return t == Num; }
    bool IsBig() const { return t == BigInt; }
};

inline Value Unhole(const Value& v) { return v.IsHole() ? Value() : v; }

// ---------------- Cop toplanan hucreler ----------------
struct Cell {
    bool marked = false;
    virtual ~Cell() {}
    virtual void Trace(std::vector<Cell*>& work) = 0;
};

struct Prop {
    Value v;
    Obj* getter = nullptr;
    Obj* setter = nullptr;
    bool accessor = false;
    bool enumerable = true;
    bool writable = true;
};

// Ekleme sirasini koruyan ozellik tablosu (kucukken dogrusal, buyuyunce dizinli)
class PropMap {
public:
    Prop* Find(const std::wstring& k);
    const Prop* Find(const std::wstring& k) const;
    Prop& Put(const std::wstring& k);   // yoksa ekler
    bool Erase(const std::wstring& k);
    size_t Size() const { return live_; }
    template <class F> void ForEach(F f) const {
        for (auto& e : list_) if (!e.dead) f(e.key, e.p);
    }
    void TraceInto(std::vector<Cell*>& work) const;
private:
    struct E { std::wstring key; Prop p; bool dead = false; };
    std::vector<E> list_;
    std::unordered_map<std::wstring, uint32_t> idx_;
    size_t live_ = 0;
    void Reindex();
};

enum class TypedKind : uint8_t {
    None,
    Int8, Uint8, Uint8Clamped,
    Int16, Uint16,
    Int32, Uint32,
    Float32, Float64,
    BigInt64, BigUint64
};

// BigInt tipli diziler icin (tanimlari js_runtime.cpp'de; js_bigint.h'yi burada gerektirmemek icin)
Value BigFromI64(int64_t v);
Value BigFromU64(uint64_t v);
uint64_t BigToU64Wrap(Runtime& rt, const Value& v);   // BigInt'e cevir, mod 2^64 (BigInt degilse TypeError)

inline size_t ElementSize(TypedKind kind) {
    switch (kind) {
    case TypedKind::Int8:
    case TypedKind::Uint8:
    case TypedKind::Uint8Clamped:
        return 1;
    case TypedKind::Int16:
    case TypedKind::Uint16:
        return 2;
    case TypedKind::Int32:
    case TypedKind::Uint32:
    case TypedKind::Float32:
        return 4;
    case TypedKind::Float64:
    case TypedKind::BigInt64:
    case TypedKind::BigUint64:
        return 8;
    default:
        return 1;
    }
}

inline const wchar_t* TypedKindName(TypedKind kind) {
    switch (kind) {
    case TypedKind::Int8: return L"Int8Array";
    case TypedKind::Uint8: return L"Uint8Array";
    case TypedKind::Uint8Clamped: return L"Uint8ClampedArray";
    case TypedKind::Int16: return L"Int16Array";
    case TypedKind::Uint16: return L"Uint16Array";
    case TypedKind::Int32: return L"Int32Array";
    case TypedKind::Uint32: return L"Uint32Array";
    case TypedKind::Float32: return L"Float32Array";
    case TypedKind::Float64: return L"Float64Array";
    case TypedKind::BigInt64: return L"BigInt64Array";
    case TypedKind::BigUint64: return L"BigUint64Array";
    default: return L"TypedArray";
    }
}

enum class Cls : uint8_t {
    Object, Array, Function, Error, Date, RegExp, BoolBox, NumBox, StrBox, Arguments,
    Map, Set, Promise, Symbol_, Node, StyleDecl, ClassList, Dataset, Storage, Event,
    ArrayBuffer, TypedArray, DataView
};

using NativeFn = Value (*)(Runtime& rt, Obj* self, const Value& thisV, std::vector<Value>& args);

struct Listener { std::wstring type; Value fn; bool capture = false, once = false; };

struct Reaction { Value onFul, onRej; Obj* derived = nullptr; };

// Nadiren gereken alanlar ayri tutulur (nesne basina bellek az olsun diye)
struct ObjExtra {
    // Fonksiyon
    Ast* fn = nullptr;             // kullanici fonksiyonu
    Env* scope = nullptr;
    NativeFn native = nullptr;
    std::vector<Value> slots;      // yerel fonksiyonlarin yakaladigi degerler (GC izler)
    bool ctor = false;             // new ile cagrilabilir mi
    int classKind = 0;             // 0 yok, 1 temel sinif, 2 turetilmis sinif
    Obj* home = nullptr;           // super icin
    Ast* classAst = nullptr;       // sinif tanimi (alanlar icin)
    Env* fieldsEnv = nullptr;
    Obj* parentClass = nullptr;
    Obj* boundTarget = nullptr;
    Value boundThis;
    std::vector<Value> boundArgs;
    std::wstring name;
    // Veri
    double num = 0;                // Date / NumBox
    std::wstring str;              // StrBox / RegExp kaynagi
    std::wstring flags;            // RegExp bayraklari
    std::shared_ptr<void> re;      // derlenmis regex (std::wregex)
    Node* node = nullptr;          // DOM dugumu
    // Map / Set
    std::vector<std::pair<Value, Value>> entries;
    std::vector<bool> dead;
    std::unordered_map<std::wstring, size_t> index;
    // Promise
    int pstate = 0;                // 0 bekliyor, 1 cozuldu, 2 reddedildi
    Value pvalue;
    std::vector<Reaction> reactions;
    bool handled = false;
    // Olay dinleyicileri
    std::vector<Listener> listeners;
    // Olay nesnesi bayraklari
    bool stopProp = false, defaultPrevented = false;
    // ArrayBuffer / TypedArray / DataView
    std::shared_ptr<std::vector<uint8_t>> bufferData;
    Obj* arrayBufferObj = nullptr; // TypedArray veya DataView'in ait oldugu ArrayBuffer nesnesi (GC izler)
    size_t byteOffset = 0;
    size_t byteLength = 0;
    size_t typedLength = 0;
    TypedKind typedKind = TypedKind::None;
    // Uretec / async fonksiyon durumu (askiya alinabilir govde)
    struct Coro* coro = nullptr;
};

struct Obj : Cell {
    Cls cls = Cls::Object;
    bool extensible = true;
    bool frozen = false;
    Obj* proto = nullptr;
    PropMap props;
    std::vector<Value> elems;             // Array / Arguments yogun ogeleri
    std::unique_ptr<ObjExtra> x;

    ObjExtra& X() { if (!x) x.reset(new ObjExtra()); return *x; }
    bool IsCallable() const { return cls == Cls::Function; }
    void Trace(std::vector<Cell*>& work) override;
};

struct Binding { Value v; uint8_t kind = 0; /* 0 var, 1 let, 2 const */ };

// Atom: tanimlayici adinin tekil kopyasi (ayni metin -> ayni isaretci). Degisken aramada dize
// karsilastirmasi yerine isaretci karsilastirmasi yapilir. Tablo surec boyunca yasar (yalnizca adlar).
using Atom = const std::wstring*;
Atom AtomOf(const std::wstring& s);

struct Env : Cell {
    Env* parent = nullptr;
    Obj* withObj = nullptr;   // with (obj) kapsami
    bool isFunc = false, hasThis = false, isGlobal = false, thisUninit = false;
    Value thisVal;
    Obj* func = nullptr;
    Obj* newTarget = nullptr;
    std::vector<std::pair<Atom, Binding>> vars;
    std::unordered_map<Atom, uint32_t> idx;

    Binding* Find(Atom a) {
        if (!idx.empty()) {
            auto it = idx.find(a);
            return it == idx.end() ? nullptr : &vars[it->second].second;
        }
        for (auto& v : vars) if (v.first == a) return &v.second;
        return nullptr;
    }
    Binding* Find(const std::wstring& k) { return Find(AtomOf(k)); }
    Binding& Define(Atom a, uint8_t kind);
    Binding& Define(const std::wstring& k, uint8_t kind) { return Define(AtomOf(k), kind); }
    void Trace(std::vector<Cell*>& work) override;
};

// ---------------- Askiya alinabilir yurutme (async/await, ureteçler) ----------------
// async ve uretec fonksiyonlarinin govdesi, C++ yigini yerine bu acik cerceve yiginiyla
// yurutulur. Boylece 'await' / 'yield' noktasinda tum durum (ara degerler dahil) GC'nin
// izledigi bir hucrede saklanir ve fonksiyon daha sonra kaldigi yerden surdurulebilir.
struct CoFrame {
    Ast* node = nullptr;
    Env* env = nullptr;
    Env* e1 = nullptr;             // dongu / catch / switch kapsami
    Env* e2 = nullptr;             // yineleme kapsami
    int pc = 0;                    // dugum icindeki adim
    bool stmt = false;             // deyim cercevesi mi (ifade degil)
    size_t idx = 0;                // liste konumu
    std::vector<Value> vals;       // ara degerler (GC izler)
    Value cv;                      // saklanan tamamlanma degeri (try/finally)
    uint8_t ct = 0;                // saklanan tamamlanma turu
    const std::wstring* clabel = nullptr;
    std::vector<const std::wstring*> labels;
    std::wstring key;              // ozellik anahtari
    int rkind = 0;                 // bilesik atama referansi turu
    const std::wstring* rname = nullptr;
};

struct CoReq { int mode = 0; Value v; Obj* promise = nullptr; };   // async uretec istek kuyrugu

struct Coro : Cell {
    enum Kind : uint8_t { Gen = 1, Async = 2, AsyncGen = 3 };
    enum State : uint8_t { Start, SuspYield, Running, SuspAwait, Done };
    enum Outcome : uint8_t { OutNone, OutYield, OutReturn, OutThrow };
    uint8_t kind = 0;
    uint8_t state = Start;
    Env* fenv = nullptr;
    Obj* fn = nullptr;
    Obj* holder = nullptr;         // uretec nesnesi ya da async tutucu
    Obj* promise = nullptr;        // async fonksiyonun dondurdugu Promise
    std::vector<CoFrame> frames;
    std::vector<CoReq> queue;
    // Son biten alt cercevenin sonucu
    Value res;
    uint8_t rct = 0;
    const std::wstring* rlabel = nullptr;
    // Calismanin sonucu
    uint8_t outcome = OutNone;
    Value outVal;
    void Trace(std::vector<Cell*>& work) override;
};

// ---------------- Hatalar ----------------
struct JSThrow { Value v; };                  // JS'in yakalayabilecegi istisna
struct JSAbort { std::wstring reason; };      // sinir asimi: yakalanamaz, betik durdurulur

// ---------------- Ana bilgisayar (tarayici) arayuzu ----------------
class Host {
public:
    virtual ~Host() {}
    virtual void Log(int level, const std::wstring& msg) = 0;     // 0 log, 1 uyari, 2 hata
    virtual void Navigate(const std::wstring& url) = 0;
    virtual std::wstring PageUrl() = 0;
    virtual std::wstring Resolve(const std::wstring& rel) = 0;
    virtual void ViewportSize(double& w, double& h) = 0;
    virtual bool NodeRect(const Node* n, double& x, double& y, double& w, double& h) = 0;
    virtual void SetUrlNoNav(const std::wstring& url) = 0;         // history.pushState
    virtual void HistoryGo(int delta) = 0;
    virtual void RequestScript(Node* scriptNode, const std::wstring& url) = 0; // dinamik <script src>
    virtual void SubmitForm(Node* form, Node* submitter) = 0;
    virtual void UpdateVideoFrame(const std::wstring& key, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgra) { (void)key; (void)w; (void)h; (void)bgra; }
    // <video> karelerinin gidecegi yer (oynatici is parcacigindan cagrilir; is parcacigi guvenli olmali)
    virtual std::function<void(int, int, std::shared_ptr<std::vector<uint8_t>>)> VideoSink(const Node* videoNode) { (void)videoNode; return {}; }
    // Ag istegi guvenlik politikasi (fetch, XHR, ES modulleri): izleyici engeli, karisik icerik, sema.
    virtual bool CheckRequest(const std::wstring& url) { (void)url; return true; }
    // Uzun betiklerde ara ara sorulur: surec bellegi tehlikeli duzeye ciktiysa betik durdurulur
    virtual bool OverMemory() { return false; }
    // meros-yt: kaynakli <video> (YouTube yerel oynaticisi) yalnizca merOS YouTube sayfalarinda
    virtual bool YouTubeStreamAllowed() { return false; }
};

struct Timer { int id; double due; double interval; Value fn; std::vector<Value> args; };

struct Cookie {
    std::wstring value;
    double expires = -1; // Unix epoch ms (-1: oturum cerezi)
    std::wstring path = L"/";
    std::wstring domain;
    bool secure = false;
    bool httpOnly = false;
};

struct ModuleRecord {
    std::wstring url;
    Obj* namespaceObj = nullptr;
    Env* env = nullptr;
    bool evaluated = false;
    bool evaluating = false;
    std::unordered_map<std::wstring, Value> exports;
    std::vector<std::pair<std::wstring, std::wstring>> localExports; // (disa aktarilan ad, yerel ad)
    void RefreshExports();   // govde bittikten sonra yerel baglarin son degerlerini al
};

// ---------------- Custom Elements ----------------
struct CEDef {
    std::wstring name;
    Obj* ctor = nullptr;
    Obj* proto = nullptr;
    Value connected, disconnected, adopted, attrChanged;   // tanimlama aninda okunan geri cagirmalar
    std::vector<std::wstring> observed;                    // static observedAttributes
    bool extends = false;                                  // ozellestirilmis yerlesik oge (desteklenmez, yalnizca kayit)
};
// MutationObserver kaydi. flags: 1 childList, 2 attributes, 4 characterData, 8 subtree, 16/32 oldValue
struct MORegistration { Obj* observer = nullptr; Node* target = nullptr; uint8_t flags = 0; std::vector<std::wstring> filter; };
// kind: 0 yukseltme, 1 connected, 2 disconnected, 3 attributeChanged
struct CEReaction { uint8_t kind = 0; Node* node = nullptr; std::wstring name; Value oldV, newV; };

// ---------------- Calisma ortami ----------------
class Runtime {
public:
    Runtime();
    ~Runtime();

    // DOM baglama (opsiyonel). doc ve sheet sayfa omru boyunca yasar.
    void AttachDocument(Document* doc, Host* host);

    // Betik calistir. Hata olursa false doner ve Host::Log ile bildirilir.
    bool RunScript(const std::wstring& src, const std::wstring& name, Node* scriptNode = nullptr);
    bool RunModule(const std::wstring& src, const std::wstring& url, Node* scriptNode = nullptr);
    std::shared_ptr<ModuleRecord> LoadModule(const std::wstring& url, Node* scriptNode = nullptr);
    std::shared_ptr<ModuleRecord> LoadModuleInternal(const std::wstring& url, const std::wstring& referrer = L"");
    bool EvalForTest(const std::wstring& src, std::wstring& result);

    // Olay dongusu
    void RunMicrotasks();
    double NextTimerDue() const;                 // ms, yoksa -1
    void RunDueTimers(double nowMs);
    void FireLoadEvents();                       // DOMContentLoaded + load
    bool DispatchClick(Node* target);            // false: varsayilan eylem (link) engellendi
    // Fare konumlu tiklama: client (gorunur alan) ve page (belge) koordinatlari, CSS px
    bool DispatchClickAt(Node* target, double clientX, double clientY, double pageX, double pageY);
    double evClientX = -1, evClientY = -1, evPageX = -1, evPageY = -1;   // DispatchClickAt icin gecici
    bool DispatchSimple(Node* target, const std::wstring& type, bool bubbles); // input/change/submit
    bool DispatchKey(Node* target, const std::wstring& type, const std::wstring& key);
    bool DispatchEvent(Node* target, Obj* event);   // ic kullanim (gorev icinde)
    void ScriptLoaded(Node* scriptNode, bool ok);   // dinamik betik sonrasi load/error olayi
    void MaybeGC();
    double Now() const;

    bool dirty = false;          // DOM degisti: yeniden stil + yerlesim gerekli
    std::wstring pendingNav;     // location.href = ...
    std::vector<std::wstring> consoleLines;

    // Sinirlar
    uint64_t stepBudget = 60000000;     // gorev basina adim
    size_t maxCells = 6000000;
    int maxDepth = 10000;   // JS cagri derinligi (V8 ~11k); asil koruma bayt tabanli CheckStack (stackLimit)

    // --------- Motor ici API (yerlesik fonksiyonlar kullanir) ---------
    template <class T> T* Alloc() {
        if (cells_.size() > maxCells) throw JSAbort{L"bellek s\u0131n\u0131r\u0131 a\u015F\u0131ld\u0131"};
        T* c = new T();
        cells_.push_back(c);
        return c;
    }
    Value EvalGlobal(const std::wstring& src, const std::wstring& name);   // eval / new Function
    size_t CellCount() const { return cells_.size(); }
    Obj* NewObject(Obj* proto = nullptr);
    Obj* NewArray(size_t n = 0);
    Obj* NewArrayFrom(const std::vector<Value>& v);
    Obj* NewFunction(NativeFn fn, const std::wstring& name, int length, bool ctor = false);
    Obj* NewError(const wchar_t* ctorName, const std::wstring& msg);
    [[noreturn]] void Throw(const wchar_t* ctorName, const std::wstring& msg);
    Obj* NewPromise();
    void ResolvePromise(Obj* p, const Value& v);
    void RejectPromise(Obj* p, const Value& v);
    void PromiseThen(Obj* p, const Value& onFul, const Value& onRej, Obj* derived);
    void Enqueue(const Value& fn, std::vector<Value> args);
    Obj* NewArrayBuffer(size_t byteLength);
    Obj* NewArrayBufferFrom(const void* data, size_t byteLength);
    Obj* NewTypedArray(TypedKind kind, size_t length);
    Obj* NewTypedArrayFromBuffer(TypedKind kind, Obj* bufferObj, size_t byteOffset, size_t length);

    Value Get(const Value& o, const std::wstring& key);
    Value GetV(const Value& o, const Value& key);
    void Put(const Value& o, const std::wstring& key, const Value& v);
    void PutV(const Value& o, const Value& key, const Value& v);
    void DefineData(Obj* o, const std::wstring& key, const Value& v, bool enumerable = true);
    void DefineAccessor(Obj* o, const std::wstring& key, NativeFn getter, NativeFn setter);
    void DefineMethod(Obj* o, const std::wstring& key, NativeFn fn, int length);
    bool HasProperty(Obj* o, const std::wstring& key);
    bool HasOwn(Obj* o, const std::wstring& key);
    bool Delete(Obj* o, const std::wstring& key);
    std::vector<std::wstring> OwnKeys(Obj* o, bool onlyEnumerable);

    Value Call(const Value& f, const Value& thisV, std::vector<Value>& args);
    Value Construct(const Value& f, std::vector<Value>& args, Obj* newTarget = nullptr);

    std::wstring ToStr(const Value& v);
    Str ToStrS(const Value& v);
    double ToNum(const Value& v);
    bool Truthy(const Value& v) const;
    Value ToPrimitive(const Value& v, int hint);       // 0 varsayilan, 1 sayi, 2 dize
    Obj* ToObject(const Value& v);
    std::wstring KeyOf(const Value& v);                  // ozellik anahtari
    bool StrictEq(const Value& a, const Value& b) const;
    bool LooseEq(const Value& a, const Value& b);
    bool SameValueZero(const Value& a, const Value& b) const;
    std::wstring TypeOf(const Value& v) const;
    bool InstanceOf(const Value& v, const Value& ctor);
    static std::wstring NumToStr(double d);
    std::vector<Value> IterateToList(const Value& iterable, size_t limit = 10000000);
    std::wstring Inspect(const Value& v, int depth = 0);
    std::wstring MapKey(const Value& v);

    // Dahili prototipler
    Obj *objectProto = nullptr, *functionProto = nullptr, *arrayProto = nullptr, *stringProto = nullptr,
        *numberProto = nullptr, *booleanProto = nullptr, *errorProto = nullptr, *regexpProto = nullptr,
        *dateProto = nullptr, *promiseProto = nullptr, *mapProto = nullptr, *setProto = nullptr,
        *symbolProto = nullptr, *iteratorProto = nullptr, *bigintProto = nullptr;
    Obj *arrayBufferProto = nullptr, *typedArrayProto = nullptr, *dataViewProto = nullptr;
    Obj *eventTargetProto = nullptr, *nodeProto = nullptr, *elementProto = nullptr, *textProto = nullptr, *documentProto = nullptr,
        *styleProto = nullptr, *classListProto = nullptr, *eventProto = nullptr, *storageProto = nullptr,
        *mediaSourceProto = nullptr, *sourceBufferProto = nullptr, *blobProto = nullptr;
    Obj* global = nullptr;
    Env* globalEnv = nullptr;
    std::unordered_map<std::wstring, Obj*> ctors;   // "Error", "TypeError", ...

    // DOM
    Document* doc = nullptr;
    Host* host = nullptr;
    Node* currentScript = nullptr;
    Obj* WrapNode(Node* n);
    std::unordered_map<Node*, Obj*> wrappers;
    std::unordered_map<std::wstring, Obj*> objectUrls;
    int nextBlobId = 1;
    std::unordered_map<std::wstring, std::shared_ptr<ModuleRecord>> modules;
    std::shared_ptr<ModuleRecord> currentModule;
    std::vector<Timer> timers;
    int nextTimerId = 1;
    std::map<std::wstring, std::wstring> localStore, sessionStore;
    std::map<std::wstring, Cookie> cookies;
    std::vector<std::pair<Value, std::vector<Value>>> microtasks;
    uint64_t steps = 0;
    uint64_t stepCheck = 4096;               // steps bunu gecince StepSlow (butce + kesme denetimi)
    std::atomic<bool> interrupt{false};      // baska is parcacigindan: gecerli betigi durdur
    const std::atomic<bool>* extInterrupt = nullptr;   // sekmenin kesme bayragi (arayuz yazar)
    uint32_t slowCount = 0;
    void StepSlow();
    void CountStep() { if (++steps > stepCheck) StepSlow(); }
    bool Interrupted() const { return interrupt.load(std::memory_order_relaxed) || (extInterrupt && extInterrupt->load(std::memory_order_relaxed)); }
    int depth = 0;
    int curLine = 0;                 // yorumlanan son dugumun satiri (hata mesajlari icin)
    const std::wstring* curFile = nullptr;   // yorumlanan betigin adi/URL'si
    std::shared_ptr<MediaState> media;       // <video>/<audio>, MediaSource (js_media.cpp)
    struct CallRec { const FuncInfo* fi; int callLine; const std::wstring* callerFile; };
    std::vector<CallRec> callStack;  // JS cagri yigini (error.stack ve konsol icin)
    std::wstring StackTrace(size_t maxFrames = 12) const;
    void FillErrorStack(Obj* e, const std::wstring& head);
    int symbolCounter = 0;
    bool inTask = false;

    // Yorumlayici
    Obj* nativeNewTarget = nullptr;              // yerel kurucu 'new' ile mi cagrildi
    bool (*exoticGet)(Runtime&, Obj*, const std::wstring&, Value&) = nullptr;
    bool (*exoticSet)(Runtime&, Obj*, const std::wstring&, const Value&) = nullptr;
    size_t stackLimit = 6 * 1024 * 1024;         // bayt (ana bilgisayar buyuk yigin verirse artirir)
    uintptr_t stackBase = 0;
    std::vector<Obj*> unhandled;
    Obj *generatorProto = nullptr, *asyncGeneratorProto = nullptr;
    // Custom Elements + Shadow DOM
    Obj* shadowRootProto = nullptr;
    Obj* commentProto = nullptr;
    std::vector<CEDef> ceDefs;
    std::unordered_map<std::wstring, int> ceByName;
    std::unordered_map<std::wstring, Obj*> ceWhenDefined;   // bekleyen whenDefined() Promise'leri
    std::vector<CEReaction> ceQueue;                        // ozel oge tepki kuyrugu (DOM cagrisi sonunda bosaltilir)
    size_t ceHead = 0;
    bool ceFlushing = false;
    std::vector<std::pair<int, Node*>> ceConstruct;         // yukseltme yigini (HTMLElement kurucusu icin)
    std::unordered_map<Node*, Node*> templateContent;       // <template>.content parcasi
    std::unordered_map<Node*, bool> templateFragments;      // icerik parcalari (etkisiz sablon belgesine ait)
    Node* templateDoc = nullptr;                            // etkisiz sablon belgesi (ozel ogeler yukseltilmez)
    std::vector<Node*> slotChangePending;
    std::vector<MORegistration> moRegs;
    std::vector<Obj*> moPending;
    bool moScheduled = false;
    std::unordered_map<const Ast*, Obj*> templateObjects;   // etiketli sablon dizgi dizileri (cagri noktasi basina)
    void FlushCEReactions();
    int coroDepth = 0;                           // calisan coroutine sayisi (ic ice)
    // Uretec / async uretec istekleri: mode 0 next, 1 throw, 2 return
    Value GeneratorResume(Obj* gen, int mode, const Value& v);
    Value AsyncGeneratorEnqueue(Obj* gen, int mode, const Value& v);
    void CheckStack();
    Obj* MakeClosure(Ast* fnNode, Env* env);
    Obj* MakeIteratorFromList(std::vector<Value> list);
    Obj* NewRegExp(const std::wstring& src, const std::wstring& flags);
    Value GetFrom(Obj* start, const std::wstring& key, const Value& recv);
    Value EvalAst(Ast* e, Env* env);
    void BeginTask();
    void EndTask();
    void Report(const std::wstring& where, const JSThrow& t);
    Value CallUser(Obj* f, const Value& thisV, std::vector<Value>& args, Obj* newTarget);

private:
    std::vector<Cell*> cells_;
    std::vector<std::unique_ptr<struct Script>> scripts_;
    size_t lastGcCells_ = 0;
    void Collect();
    friend struct Interp;
};

// Yerlesik fonksiyon kurulumlari (js_builtins.cpp, js_dom.cpp)
void InstallBuiltins(Runtime& rt);
void InstallDom(Runtime& rt);
void InstallIntl(Runtime& rt);
void InstallCoroutines(Runtime& rt);
// Medya (js_media.cpp)
void InstallMedia(Runtime& rt, Obj* elementProto);
void MediaSrcChanged(Runtime& rt, Node* n);
void MediaScan(Runtime& rt, Node* root);
void MediaTogglePlay(Runtime& rt, Node* n);       // <video controls> tiklamasi          // ayristirilmis/eklenmis <video>/<audio> ogelerini yukle
void MediaTick(Runtime& rt);
double MediaNextDue(Runtime& rt);                 // -1: medya etkin degil
void MediaGcRoots(Runtime& rt, std::vector<Cell*>& work);
// js_dom.cpp yardimcilari
void JsDispatchObj(Runtime& rt, Obj* target, const std::wstring& type);
Obj* JsNativeCtor(Runtime& rt, const wchar_t* name, NativeFn fn, Obj* proto);
void InstallBigInt(Runtime& rt);
double ParseDate(const std::wstring& in);

inline double GetTypedElementNum(const ObjExtra& x, size_t index) {
    if (!x.bufferData || index >= x.typedLength) return 0;
    size_t elSize = ElementSize(x.typedKind);
    size_t bytePos = x.byteOffset + index * elSize;
    if (bytePos + elSize > x.bufferData->size()) return 0;
    const uint8_t* ptr = x.bufferData->data() + bytePos;
    switch (x.typedKind) {
    case TypedKind::Int8: return double(*(const int8_t*)ptr);
    case TypedKind::Uint8:
    case TypedKind::Uint8Clamped: return double(*ptr);
    case TypedKind::Int16: {
        int16_t v; memcpy(&v, ptr, 2); return double(v);
    }
    case TypedKind::Uint16: {
        uint16_t v; memcpy(&v, ptr, 2); return double(v);
    }
    case TypedKind::Int32: {
        int32_t v; memcpy(&v, ptr, 4); return double(v);
    }
    case TypedKind::Uint32: {
        uint32_t v; memcpy(&v, ptr, 4); return double(v);
    }
    case TypedKind::Float32: {
        float v; memcpy(&v, ptr, 4); return double(v);
    }
    case TypedKind::Float64: {
        double v; memcpy(&v, ptr, 8); return v;
    }
    case TypedKind::BigInt64: {
        int64_t v; memcpy(&v, ptr, 8); return double(v);
    }
    case TypedKind::BigUint64: {
        uint64_t v; memcpy(&v, ptr, 8); return double(v);
    }
    default:
        return 0;
    }
}

inline Value GetTypedElement(const ObjExtra& x, size_t index) {
    if (index >= x.typedLength) return Value();
    if (x.typedKind == TypedKind::BigInt64 || x.typedKind == TypedKind::BigUint64) {
        size_t bytePos = x.byteOffset + index * 8;
        if (!x.bufferData || bytePos + 8 > x.bufferData->size()) return Value();
        uint64_t u;
        memcpy(&u, x.bufferData->data() + bytePos, 8);
        return x.typedKind == TypedKind::BigInt64 ? BigFromI64(int64_t(u)) : BigFromU64(u);
    }
    return Value::N(GetTypedElementNum(x, index));
}

inline void SetTypedElement(Runtime& rt, ObjExtra& x, size_t index, const Value& v) {
    if (!x.bufferData || index >= x.typedLength) return;
    size_t elSize = ElementSize(x.typedKind);
    size_t bytePos = x.byteOffset + index * elSize;
    if (bytePos + elSize > x.bufferData->size()) return;
    uint8_t* ptr = x.bufferData->data() + bytePos;
    if (x.typedKind == TypedKind::BigInt64 || x.typedKind == TypedKind::BigUint64) {
        uint64_t u = BigToU64Wrap(rt, v);
        memcpy(ptr, &u, 8);
        return;
    }
    double n = rt.ToNum(v);
    switch (x.typedKind) {
    case TypedKind::Int8: {
        int8_t val = int8_t(int32_t(n));
        *ptr = uint8_t(val);
        break;
    }
    case TypedKind::Uint8: {
        *ptr = uint8_t(uint32_t(n));
        break;
    }
    case TypedKind::Uint8Clamped: {
        if (std::isnan(n) || n <= 0.0) *ptr = 0;
        else if (n >= 255.0) *ptr = 255;
        else {
            double f = std::floor(n);
            double diff = n - f;
            if (diff > 0.5) f += 1.0;
            else if (diff == 0.5) {
                if (fmod(f, 2.0) != 0.0) f += 1.0;
            }
            *ptr = uint8_t(f);
        }
        break;
    }
    case TypedKind::Int16: {
        int16_t val = int16_t(int32_t(n));
        memcpy(ptr, &val, 2);
        break;
    }
    case TypedKind::Uint16: {
        uint16_t val = uint16_t(uint32_t(n));
        memcpy(ptr, &val, 2);
        break;
    }
    case TypedKind::Int32: {
        int32_t val = int32_t(uint32_t(n));
        memcpy(ptr, &val, 4);
        break;
    }
    case TypedKind::Uint32: {
        uint32_t val = uint32_t(n);
        memcpy(ptr, &val, 4);
        break;
    }
    case TypedKind::Float32: {
        float val = float(n);
        memcpy(ptr, &val, 4);
        break;
    }
    case TypedKind::Float64: {
        memcpy(ptr, &n, 8);
        break;
    }
    default:
        break;
    }
}

} // namespace js
} // namespace mer
