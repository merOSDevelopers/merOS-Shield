// merOS Shield - MerEngine : DOM ağacı ve HTML ayrıştırıcı arayüzü
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mer {

struct Node {
    enum Kind : unsigned char { Doc, Elem, Text };
    Kind kind = Elem;
    std::wstring name;   // etiket adi (kucuk harf)
    std::wstring text;   // metin dugumu icerigi
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
std::wstring DecodeEntities(const std::wstring& s);

// --- DOM degistirme yardimcilari (JS motoru kullanir) ---
// innerHTML icin: parcayi ayristirir, dugumlere doc'tan yeni id verir.
std::vector<std::unique_ptr<Node>> ParseFragment(const std::wstring& html, Document& doc);
Node* NewNode(Document& doc, Node::Kind kind, const std::wstring& nameOrText); // 'detached' havuzunda olusur
std::unique_ptr<Node> Detach(Document& doc, Node* n);       // agactan ya da havuzdan sahipligi al
bool AppendChild(Document& doc, Node* parent, Node* child);  // dongu kontrolu yapar
bool InsertBefore(Document& doc, Node* parent, Node* child, Node* ref);
void RemoveNode(Document& doc, Node* n);                     // havuza tasir
bool Contains(const Node* ancestor, const Node* n);
void SyncAttrCache(Node* n);                                 // class/id onbellegini yenile
std::wstring TextContent(const Node* n);
std::wstring SerializeHTML(const Node* n, bool outer);

} // namespace mer
