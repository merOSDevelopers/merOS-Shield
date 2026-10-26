# Lit 3 uyumluluk testleri

Gerçek [Lit](https://lit.dev) 3 kütüphanesi (BSD-3-Clause, lisans metni paketlerin içinde) merJS'in
Custom Elements + Shadow DOM desteğiyle çalıştırılır.

- `*.src.js` — bileşen kaynağı
- `*.bundle.js` — `esbuild --bundle --format=iife --minify --target=es2020` çıktısı (Lit dahil)
- `*.check.js` — sonucu `result` değişkenine yazan kontrol betiği

Çalıştırma (`tests/lib_harness.cpp` derlendikten sonra):

```
lib_harness tests/js/lit/sayac.bundle.js tests/js/lit/sayac.check.js
lib_harness tests/js/lit/todo.bundle.js  tests/js/lit/todo.check.js
```

Beklenen (Chrome ile aynı):

```
sayac: Merhaba Mert|3|Tik: 0 || Tik: 1|Mert|1style
todo:  Liste (2) | 2 | true | true | color:red; | cls:   | Ekmek | Liste (2) | Ekmek+Peynir | ayni-dugum | 1 | isler,filtre | Bos
```

Yeniden paketlemek için: `npm i lit@3 esbuild` ve yukarıdaki esbuild komutu.
