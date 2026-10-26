// merOS Shield - MerEngine : DOM ağacı ve HTML ayrıştırıcı arayüzü
#pragma once
#include <cwchar>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mer {

struct Node {
    enum Kind : unsigned char { Doc, Elem, Text, Shadow, Comment };
    Kind kind = Elem;
    std::wstring name;   // etiket adi (kucuk harf)
    std::wstring text;   // metin / yorum dugumu icerigi
    std::vector<std::pair<std::wstring, std::wstring>> attrs;
    std::vector<std::wstring> classes;
    std::wstring idAttr;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> kids;
    int id = 0; // stil dizisindeki indeks

    // Form durumu (kullanicinin yazdigi deger, isaretli kutu)
    std::wstring value;
    bool dirtyValue = false;
    bool checked = false, dirtyChecked = false;

    // Shadow DOM: bu elemana bagli shadow root (kind == Shadow). Shadow root'un cocuklarinin
    // parent'i shadow root'tur; shadow root'un parent'i yoktur, sahibi 'host'tur.
    std::unique_ptr<Node> shadow;
    Node* host = nullptr;          // yalnizca Shadow dugumunde
    uint8_t shadowMode = 0;        // 1 open, 2 closed
    // Custom Elements
    uint8_t ceState = 0;           // 0 normal/tanimsiz, 1 ozel oge (yukseltildi), 2 yukseltme basarisiz
    bool ceConnected = false;      // connectedCallback cagrildi (disconnected beklenir)
    int16_t ceDef = -1;            // Runtime::ceDefs indeksi

    const std::wstring* Attr(const wchar_t* n) const {
        for (auto& a : attrs) if (a.first == n) return &a.second;
        return nullptr;
    }
};

struct Document {
    std::unique_ptr<Node> root;
    int count = 0;
    std::wstring title;
    std::wstring baseHref;
    std::vector<std::wstring> inlineStyles; // <style> icerikleri (belge sirasiyla)
    std::vector<std::wstring> styleLinks;   // <link rel=stylesheet href>
    // Stil kaynaklarinin belge sirasi: inline ise >=0 (inlineStyles indeksi), link ise -(i+1)
    std::vector<int> styleOrder;
    // Agactan cikarilmis ya da henuz eklenmemis dugumler (JS referanslari gecerli kalsin diye silinmez)
    std::vector<std::unique_ptr<Node>> detached;
};

// Guvenlik sinirlari: kotu niyetli/bozuk sayfalar motoru cokertemez.
constexpr int kMaxNodes = 250000;
constexpr size_t kMaxDepth = 400;

std::unique_ptr<Document> ParseHTML(const std::wstring& src);

// <video> karelerinin resim deposundaki anahtari (dugum omru boyunca sabit)
inline std::wstring VideoKey(const Node* n) {
    wchar_t buf[40];
    swprintf(buf, 40, L"\x01v%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(n)));
    return buf;
}
inline bool IsVideoKey(const std::wstring& k) { return k.size() > 2 && k[0] == L'\x01' && k[1] == L'v'; }
std::wstring DecodeEntities(const std::wstring& s);

// --- DOM degistirme yardimcilari (JS motoru kullanir) ---
// innerHTML icin: parcayi ayristirir, dugumlere doc'tan yeni id verir.
std::vector<std::unique_ptr<Node>> ParseFragment(const std::wstring& html, Document& doc);
Node* NewNode(Document& doc, Node::Kind kind, const std::wstring& nameOrText); // 'detached' havuzunda olusur
std::unique_ptr<Node> Detach(Document& doc, Node* n);       // agactan ya da havuzdan sahipligi al
bool AppendChild(Document& doc, Node* parent, Node* child);  // dongu kontrolu yapar
bool InsertBefore(Document& doc, Node* parent, Node* child, Node* ref);
void RemoveNode(Document& doc, Node* n);                     // havuza tasir
bool Contains(const Node* ancestor, const Node* n);         // shadow sinirini (root -> host) gecer
bool ContainsLight(const Node* ancestor, const Node* n);    // yalnizca ayni agac (Node.contains)
void SyncAttrCache(Node* n);                                 // class/id onbellegini yenile
std::wstring TextContent(const Node* n);
std::wstring SerializeHTML(const Node* n, bool outer);

// --- Shadow DOM ---
Node* AttachShadowRoot(Document& doc, Node* host, uint8_t mode);   // zaten varsa nullptr
const Node* ComposedParent(const Node* n);                          // shadow root -> host
const Node* ShadowRootOf(const Node* n);                            // dugumu iceren shadow root (yoksa nullptr)
std::wstring SlotNameOf(const Node* n);                             // light dugumun slot adi ("" varsayilan)
const Node* AssignedSlot(const Node* n);                            // light dugumun atandigi <slot>
void AssignedNodes(const Node* slot, std::vector<const Node*>& out);
// Cizim (flat) agaci cocuklari: host icin shadow root'un cocuklari, <slot> icin atanmis
// dugumler (yoksa yedek icerik), digerleri icin kendi cocuklari.
void FlatKids(const Node* n, std::vector<const Node*>& out);
inline bool HasFlatOverride(const Node* n) { return n->shadow || (n->kind == Node::Elem && n->name == L"slot"); }
// <template shadowrootmode="open"> (bildirimsel shadow DOM) -> gercek shadow root
void ApplyDeclarativeShadow(Document& doc, Node* root);
// Ayristirici <template> icerigini ham metin olarak saklar; bunu gercek dugumlere cevirir
std::vector<std::unique_ptr<Node>> TemplateNodes(Node* tmpl, Document& doc);

} // namespace mer
