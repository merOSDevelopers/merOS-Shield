# YouTube yığını: Polymer 3 + ShadyDOM

YouTube'un masaüstü sitesi `ytd-*` bileşenlerini Polymer ile, **ShadyDOM** polyfill'i zorunlu kipte
(`ShadyDOM.force`) ve web-animations polyfill'iyle çalıştırır. Bu test aynı yığını kurar:

- `web-animations-next-lite.min.js` — web-animations-js (Apache-2.0), YouTube'un yüklediği dosyayla aynı ad
- `webcomponents-sd.js` — @webcomponents/webcomponentsjs (BSD-3-Clause), ShadyDOM + ShadyCSS
- `ytd-kart.*` — Polymer 3 bileşeni: `dom-repeat`, `dom-if`, veri bağlama, `on-click`, `notify` / `reflectToAttribute`

Çalıştırma:

```
lib_harness tests/js/polymer/shadydom-force.js tests/js/polymer/web-animations-next-lite.min.js ^
            tests/js/polymer/webcomponents-sd.js tests/js/polymer/ytd-kart.bundle.js tests/js/polymer/ytd-kart.check.js
```

Beklenen (Chrome ile aynı): `true | true | Ana Sayfa | 2 | 0:Video A/1:Video B | 1 | 1 | Video B | 3 | true | Kesfet`
