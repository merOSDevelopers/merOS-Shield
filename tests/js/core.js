// merJS cekirdek testleri
var fails = [];
var count = 0;
function eq(a, b, msg) {
  count++;
  var sa = typeof a === "object" ? JSON.stringify(a) : String(a);
  var sb = typeof b === "object" ? JSON.stringify(b) : String(b);
  if (sa !== sb) fails.push((msg || "test " + count) + ": " + sa + " !== " + sb);
}

// Temel aritmetik ve donusumler
eq(1 + 2 * 3, 7);
eq("5" * "2", 10);
eq("5" + 2, "52");
eq(0.1 + 0.2, 0.30000000000000004);
eq(1 / 3, 0.3333333333333333);
eq(1e21, "1e+21");
eq(123e-20, "1.23e-18");
eq(-0 === 0, true);
eq(NaN === NaN, false);
eq(typeof null, "object");
eq(typeof undefined, "undefined");
eq(typeof function () {}, "function");
eq(typeof notDefinedAnywhere, "undefined");
eq(null == undefined, true);
eq("1" == 1, true);
eq([] + [], "");
eq([1, 2] + "", "1,2");
eq({} + "", "[object Object]");
eq(7 % -3, 1);
eq(2 ** 10, 1024);
eq(-7 >> 1, -4);
eq(-7 >>> 28, 15);
eq(5 & 3 | 8 ^ 1, 9);
eq(~5, -6);
eq("b" > "a", true);
eq(parseInt("08"), 8);
eq(parseInt("0x1F"), 31);
eq(parseFloat("3.14abc"), 3.14);
eq(Number("  42  "), 42);
eq(Number(""), 0);
eq(isNaN("abc"), true);
eq((255).toString(16), "ff");
eq((3.14159).toFixed(2), "3.14");
eq((1234.5).toLocaleString(), "1,234.5");

// Degiskenler, kapsam, kapanislar
let x = 1;
{ let x = 2; eq(x, 2); }
eq(x, 1);
const fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
eq(fns.map(f => f()), [0, 1, 2]);
var vfns = [];
for (var j = 0; j < 3; j++) vfns.push(function () { return j; });
eq(vfns[0](), 3);
function counter() { let c = 0; return { inc: () => ++c, get: () => c }; }
const ctr = counter(); ctr.inc(); ctr.inc();
eq(ctr.get(), 2);
eq(hoisted(), "ok");
function hoisted() { return "ok"; }
try { constV = 1; } catch (e) { }
const cc = 5;
let threw = false;
try { cc = 6; } catch (e) { threw = e instanceof TypeError; }
eq(threw, true, "const atama");

// Fonksiyonlar
function sum(...nums) { return nums.reduce((a, b) => a + b, 0); }
eq(sum(1, 2, 3, 4), 10);
function def(a, b = a * 2) { return a + b; }
eq(def(3), 9);
function args() { return arguments.length; }
eq(args(1, 2, 3), 3);
eq(Math.max(...[1, 5, 3]), 5);
const bound = function () { return this.v; }.bind({ v: 42 });
eq(bound(), 42);
eq((function () { return this === globalThis; })(), true);
const objThis = { v: 1, f() { return this.v; }, a: () => typeof this };
eq(objThis.f(), 1);
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }
eq(fact(10), 3628800);
const named = function inner(n) { return n ? inner(n - 1) + 1 : 0; };
eq(named(5), 5);
eq(named.name, "inner");
const arrowName = () => 1;
eq(arrowName.name, "arrowName");

// Nesneler ve diziler
const o = { a: 1, b: { c: 2 }, ["d" + 1]: 3, get e() { return this.a + 10; } };
eq(o.d1, 3);
eq(o.e, 11);
eq(Object.keys(o), ["a", "b", "d1", "e"]);
const { a, b: { c }, z = 9, ...rest } = { a: 1, b: { c: 2 }, q: 7, r: 8 };
eq([a, c, z], [1, 2, 9]);
eq(rest, { q: 7, r: 8 });
const [p, , q2 = 5, ...more] = [1, 2, undefined, 4, 5];
eq([p, q2, more], [1, 5, [4, 5]]);
let s1 = 1, s2 = 2;
[s1, s2] = [s2, s1];
eq([s1, s2], [2, 1]);
eq({ ...o, a: 5 }.a, 5);
eq([1, 2, 3].map(n => n * 2).filter(n => n > 2), [4, 6]);
eq([3, 1, 2].sort(), [1, 2, 3]);
eq([10, 9, 1].sort((x, y) => x - y), [1, 9, 10]);
eq([1, [2, [3, [4]]]].flat(2), [1, 2, 3, [4]]);
eq(Array.from({ length: 3 }, (_, i) => i * i), [0, 1, 4]);
eq(Array.from("abc"), ["a", "b", "c"]);
eq([1, 2, 3].includes(2), true);
eq([1, 2, 3].indexOf(4), -1);
const arr = [1, 2, 3, 4, 5];
eq(arr.splice(1, 2, "x"), [2, 3]);
eq(arr, [1, "x", 4, 5]);
eq(arr.length, 4);
arr.length = 2;
eq(arr, [1, "x"]);
const sparse = [];
sparse[3] = 1;
eq(sparse.length, 4);
eq([1, 2, 3].reduce((s, v) => s + v), 6);
eq(Object.entries({ x: 1, y: 2 }), [["x", 1], ["y", 2]]);
eq(Object.assign({}, { a: 1 }, { b: 2 }), { a: 1, b: 2 });
eq("a" in { a: undefined }, true);
eq(delete o.a, true);
eq(o.a, undefined);
eq([..."héllo"].length, 5);

// Dizeler
eq("Merhaba Dünya".toUpperCase(), "MERHABA DÜNYA");
eq("  boşluk  ".trim(), "boşluk");
eq("a,b,,c".split(","), ["a", "b", "", "c"]);
eq("abc".split(""), ["a", "b", "c"]);
eq("abcdef".slice(-3), "def");
eq("abcdef".substring(4, 1), "bcd");
eq("x".padStart(3, "0"), "00x");
eq("ab".repeat(3), "ababab");
eq(`toplam: ${1 + 2} adet`, "toplam: 3 adet");
eq("İstanbul".startsWith("İst"), true);
function tag(strs, ...vals) { return strs.raw.join("|") + vals.join(","); }
eq(tag`a${1}b${2}c`, "a|b|c1,2");

// Regex
eq("2026-10-15".replace(/(\d+)-(\d+)-(\d+)/, "$3.$2.$1"), "15.10.2026");
eq("aaa".replace(/a/g, "b"), "bbb");
eq("a1b2c3".match(/\d/g), ["1", "2", "3"]);
eq(/^[\w.]+@\w+\.\w+$/.test("mert@meros.dev"), true);
eq("Ali Veli".replace(/(\w+) (\w+)/, (m, x1, x2) => x2 + " " + x1), "Veli Ali");
const re = /(?<yil>\d{4})-(?<ay>\d{2})/;
eq("2026-10".match(re).groups.ay, "10");
eq("a, b ,c".split(/\s*,\s*/), ["a", "b", "c"]);
eq("x".replace(/y/, "z"), "x");
eq([..."a1b22".matchAll(/\d+/g)].map(m => m[0]), ["1", "22"]);
eq("AbC".replace(/b/i, "x"), "AxC");
eq(/a.c/s.test("a\nc"), true);

// Siniflar
class Animal {
  static count = 0;
  #secret = 1;
  name;
  constructor(name) { this.name = name; Animal.count++; }
  speak() { return this.name + " ses çıkarır"; }
  get upper() { return this.name.toUpperCase(); }
  static create(n) { return new this(n); }
}
class Dog extends Animal {
  tricks = [];
  constructor(name) { super(name); this.kind = "köpek"; }
  speak() { return super.speak() + " (hav)"; }
}
const d = new Dog("Karabaş");
eq(d.speak(), "Karabaş ses çıkarır (hav)");
eq(d.upper, "KARABAŞ");
eq(d instanceof Animal, true);
eq(Animal.count, 1);
eq(Dog.create("Tarçın").name, "Tarçın");
eq(Animal.count, 2);
eq(Array.isArray(d.tricks), true);
let classThrew = false;
try { Animal("x"); } catch (e) { classThrew = e instanceof TypeError; }
eq(classThrew, true, "sinif new olmadan");
class MyErr extends Error { constructor(m) { super(m); this.name = "MyErr"; } }
try { throw new MyErr("özel"); } catch (e) { eq(e.message + "/" + e.name + "/" + (e instanceof Error), "özel/MyErr/true"); }

// Prototipler ve eski usul
function Person(n) { this.n = n; }
Person.prototype.hi = function () { return "selam " + this.n; };
eq(new Person("Mert").hi(), "selam Mert");
eq(Object.getPrototypeOf(new Person("x")) === Person.prototype, true);
const proto = { greet() { return "hey " + this.who; } };
const child = Object.create(proto);
child.who = "sen";
eq(child.greet(), "hey sen");
Object.defineProperty(child, "ro", { value: 5, writable: false, enumerable: false });
child.ro = 10;
eq(child.ro, 5);
eq(Object.keys(child), ["who"]);

// Hatalar ve kontrol akisi
function tf() { try { return "try"; } finally { log.push("finally"); } }
const log = [];
eq(tf(), "try");
eq(log, ["finally"]);
try { null.x; } catch (e) { eq(e instanceof TypeError, true); }
try { undefinedFn(); } catch (e) { eq(e.name, "ReferenceError"); }
outer: for (let i = 0; i < 3; i++) { for (let k = 0; k < 3; k++) { if (k == 1) continue outer; if (i == 2) break outer; log.push(i + "" + k); } }
eq(log, ["finally", "00", "10"]);
function sw(v) { switch (v) { case 1: return "bir"; case 2: case 3: return "iki-üç"; default: return "diğer"; } }
eq([sw(1), sw(3), sw(9)], ["bir", "iki-üç", "diğer"]);
let w = 0; do { w++; } while (w < 5);
eq(w, 5);
const forIn = [];
for (const k in { a: 1, b: 2 }) forIn.push(k);
eq(forIn, ["a", "b"]);
eq(o?.zz?.yy, undefined);
eq(o.zz?.(), undefined);
eq(null ?? "varsayılan", "varsayılan");
eq(0 ?? 5, 0);
let la = null; la ??= 3; eq(la, 3);
let lo = 0; lo ||= 7; eq(lo, 7);

// Map / Set / Symbol / yineleyiciler / uretecler
const m = new Map([["a", 1]]);
m.set("b", 2).set(NaN, "nan");
eq(m.get("b"), 2);
eq(m.get(NaN), "nan");
eq(m.size, 3);
eq([...m.keys()].length, 3);
const st = new Set([1, 2, 2, 3]);
eq(st.size, 3);
eq([...st], [1, 2, 3]);
const sym = Symbol("etiket");
const so = { [sym]: 1, n: 2 };
eq(so[sym], 1);
eq(Object.keys(so), ["n"]);
eq(sym.toString(), "Symbol(etiket)");
const iterable = { *[Symbol.iterator]() { yield 1; yield 2; yield 3; } };
eq([...iterable], [1, 2, 3]);
function* gen() { let i = 0; while (i < 3) yield i++; }
eq(Array.from(gen()), [0, 1, 2]);
const g = gen();
eq(g.next().value, 0);
const custom = { from: 1, to: 3, [Symbol.iterator]() { let c = this.from, t = this.to; return { next: () => c <= t ? { value: c++, done: false } : { value: undefined, done: true } }; } };
const got = [];
for (const v of custom) got.push(v);
eq(got, [1, 2, 3]);
for (const [k, v] of new Map([["x", 1]])) eq(k + v, "x1");

// JSON
eq(JSON.stringify({ a: [1, "x", null, true], b: { c: undefined } }), '{"a":[1,"x",null,true],"b":{}}');
eq(JSON.parse('{"a":[1,2,{"b":"ç\\u00e7"}]}').a[2].b, "çç");
eq(JSON.stringify({ a: 1 }, null, 2), '{\n  "a": 1\n}');
eq(JSON.stringify(new Date(Date.UTC(2026, 9, 15))), '"2026-10-15T00:00:00.000Z"');
let circ = {}; circ.self = circ;
try { JSON.stringify(circ); } catch (e) { eq(e instanceof TypeError, true); }

// Date
const dt = new Date(Date.UTC(2026, 0, 31, 12, 30));
eq(dt.toISOString(), "2026-01-31T12:30:00.000Z");
eq(new Date("2026-10-15T10:00:00Z").getTime(), 1792058400000);
eq(new Date(2026, 1, 1).getMonth(), 1);
eq(typeof Date.now(), "number");

// URI / base64
eq(encodeURIComponent("çay & simit"), "%C3%A7ay%20%26%20simit");
eq(decodeURIComponent("%C3%A7ay"), "çay");
eq(btoa("merOS"), "bWVyT1M=");
eq(atob("bWVyT1M="), "merOS");

// Promise ve async
const order = [];
Promise.resolve(1).then(v => order.push("then" + v));
order.push("sync");
queueMicrotask(() => order.push("micro"));
async function af() { const v = await Promise.resolve(5); return v * 2; }
let asyncResult = 0;
af().then(v => { asyncResult = v; });
const allP = Promise.all([1, Promise.resolve(2), new Promise(r => r(3))]).then(v => order.push("all:" + v.join("")));
new Promise((_, rej) => rej(new Error("x"))).catch(e => order.push("caught:" + e.message));

// Etiketli sablon: ayni cagri noktasi ayni dondurulmus dizgi dizisi (lit-html onbellegi buna dayanir)
function etiket(s) { return s; }
const sablonAl = (x) => etiket`a${x}b`;
eq(sablonAl(1) === sablonAl(2), true, "sablon kimligi");
eq(Object.isFrozen(sablonAl(1)) && Object.isFrozen(sablonAl(1).raw), true, "sablon donmus");
eq(etiket`a${1}b` === etiket`a${1}b`, false, "farkli cagri noktasi");

// Ayristirici: sablon ifadesinde regex, if/for basligindan sonra regex (YouTube kevlar hatasi)
const kacis = (v) => `"${v.replace(/\\/g, `\\\\`).replace(/"/g, `\\"`)}"`;
eq(kacis('a"b'), '"a\\"b"', "sablonda regex");
var sayim = 0; for (var ii = 0; ii < 3; ii++)/x/.test('x') && sayim++;
eq(sayim, 3, "for basligindan sonra regex");
if (true) /^a/.test('ab') ? sayim++ : 0;
eq(sayim, 4, "if basligindan sonra regex");
eq((4)/2/1, 2, "parantezden sonra bolme");
// Regex Annex B: duz { }, sinif icinde [ ve \\w-
eq(/a{b}/.test('a{b}'), true, "duz suslu");
eq(/x{2}/.test('xx'), true, "niceleyici");
eq(/[[.:#*]/.test(':'), true, "sinifta [");
eq(/^[\w-\d]+$/.test('a-1'), true, "sinif kacisi yaninda -");
eq(/(?:^[^;\-\s}]+)?--[^;{}]*?:[^{};]*?{[^}]*?}(?:[;\n]|$)?/.test('--x:{a}'), true, "ShadyCSS regex");

// Dizi bosluklari (holes): YouTube kevlar d3 kodu Array(3).concat(...).map(f) kullanir
(function () {
var r = [];
var a = Array(3).concat("x", "y");
r.push(a.map(function (s) { return s.length; }).join("|"));
r.push(a.length, 0 in a, 3 in a, Object.keys(a).join());
var b = [1, , 3]; r.push(b.indexOf(undefined), b.includes(undefined), b.filter(function(){return true;}).length);
var c = [1, 2, 3]; delete c[1]; r.push(c.length, 1 in c, c.reduce(function (s, x) { return s + x; }, 0));
var cnt = 0; [, , 5].forEach(function () { cnt++; }); r.push(cnt);
var d = []; d[3] = 1; r.push(d.length, 0 in d, [...d].join("-"), Array.from(d).length);
var e = [3, , 1, undefined]; e.sort(); r.push(e.length, 3 in e, e[2] === undefined, 2 in e);
r.push([1, [2, , 3]].flat().length, Array(2).join("-"), JSON.stringify([, 1]));
var f = []; f.length = 2; r.push(0 in f, f.some(function(){return true;}));
for (var k in [7, , 9]) r.push("k" + k);
r.push(new Array(5).fill(0).join(""), [, 'a'].findIndex(function (x) { return x === undefined; }));
eq(r.join(","), "|||1|1,5,false,true,3,4,-1,true,2,3,false,4,1,4,false,---1,4,4,false,true,true,3,-,[null,1],false,false,k0,k2,00000,0", "dizi bosluklari (V8 ile ayni)");
})();

// 'u' kipinde astral karakter siniflari (YouTube kevlar: /[\u{1D400}-\u{1D7FF}\u2100-\u214F]/u)
(function () {
var r = [];
var re = /[\u{1D400}-\u{1D7FF}℀-⅏]/u;
r.push(re.test('\u{1D400}'), re.test('\u{1D7FF}'), re.test('™'), re.test('a'), re.test('\u{1F600}'));
var e = /[\u{1F600}-\u{1F64F}]/u; r.push(e.test('😀'), e.test('x'));
var n = /^[^\u{1F600}-\u{1F64F}]$/u; r.push(n.test('😀'), n.test('a'), n.test('\u{10000}'));
var w = /[\u{10000}-\u{10FFFF}a-c]/u; r.push(w.test('b'), w.test('\u{10FFFF}'), w.test('\u{20000}'), w.test('d'));
r.push('a😀b😃'.replace(/[\u{1F600}-\u{1F64F}]/gu, '*'));
eq(r.join(','), 'true,true,true,false,false,true,false,false,true,true,true,true,true,false,a*b*', 'u kipi astral sinif (V8 ile ayni)');
})();

// BigInt (YouTube oynatici base.js kullaniyor) - beklenen degerler V8'den
(function () {
var o = [];
var a = new BigInt64Array(3); a[0] = -5n; a[1] = 2n ** 63n; a[2] = 123456789012345678901n;
o.push(String(a[0]), String(a[1]), String(a[2]), a.BYTES_PER_ELEMENT);
var b = new BigUint64Array(a.buffer); o.push(String(b[0]), String(b[1]));
var dv = new DataView(new ArrayBuffer(16)); dv.setBigInt64(0, -2n); dv.setBigUint64(8, 0x0102030405060708n, true);
o.push(String(dv.getBigInt64(0)), String(dv.getBigUint64(0)), String(dv.getBigUint64(8, true)), dv.getUint8(8));
try { a[0] = 1; } catch (e) { o.push(e.name); }
try { 1n + 1; } catch (e) { o.push(e.name); }
o.push(typeof 10n, 10n > 9, 2n == 2, 2n === 2, 0x1fn + 1n + "", BigInt("0x10") + "", BigInt.asIntN(8, 255n) + "", BigInt.asUintN(64, -1n) + "",
  (-7n / 2n) + "", (-7n % 2n) + "", (5n << 70n) + "", (-9n >> 1n) + "", (6n & -3n) + "", (255n).toString(16), 1n ? "t" : "f", 0n ? "t" : "f");
eq(o.join("|"), "-5|-9223372036854775808|-5670419503621182411|8|18446744073709551611|9223372036854775808|-2|18446744073709551614|72623859790382856|8|TypeError|TypeError|bigint|true|true|false|32|16|-1|18446744073709551615|-3|-1|5902958103587056517120|-5|4|ff|t|f", "BigInt (V8 ile ayni)");
})();

// Polymer baglama regex'i (MSVC std::regex kacis/ayrac katiligi)
(function () {
var re = /(\[\[|{{)\s*(?:(!)\s*)?((?:[a-zA-Z_$][\w.:$\-*]*)\s*(?:\(\s*(?:(?:(?:((?:[a-zA-Z_$][\w.:$\-*]*)|(?:[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)|(?:(?:'(?:[^'\\]|\\.)*')|(?:"(?:[^"\\]|\\.)*")))\s*)(?:,\s*(?:((?:[a-zA-Z_$][\w.:$\-*]*)|(?:[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)|(?:(?:'(?:[^'\\]|\\.)*')|(?:"(?:[^"\\]|\\.)*")))\s*))*)?)\)\s*)?)(?:]]|}})/g;
var m = re.exec("x [[foo.bar]] y {{!baz(a, 'q')}}"); var m2 = re.exec("x [[foo.bar]] y {{!baz(a, 'q')}}");
eq(m[3] + "|" + m2[2] + "|" + m2[3], "foo.bar|!|baz(a, 'q')", "Polymer baglama regex'i");
})();

// s += x hizli yolu (yerinde buyutme) anlamsal olarak ayni kalmali - beklenenler Node/V8'den
(function () {
function f1() { var s = 'a'; var t = s; s += 'b'; return t + '|' + s; }
function f2() { var s = 'x'; var arr = [s]; s += 'y'; return arr[0] + '|' + s; }
function f3() { var s = 'q'; s += (s = 'Z', '!'); return s; }
function f4() { let s = ''; for (let i = 0; i < 5; i++) s += i; return s; }
function f5() { const c = 'k'; try { c += 'x'; } catch (e) { return e.name; } }
function f6() { var s = 'n'; s += { toString: function () { return 'T'; } }; s += 1; s += null; return s; }
function f7() { var o = { s: 'm' }; with (o) { s += 'w'; } return o.s; }
function f8() { var s = 'e'; var g = function () { return s; }; s += 'v'; return g() + s; }
function f9() { var s = 'r'; var o = { v: s }; s += 'z'; return o.v + s + eval('var w = "a"; w += "b"; w'); }
eq([f1(), f2(), f3(), f4(), f5(), f6(), f7(), f8(), f9()].join(','), 'a|ab,x|xy,q!,01234,TypeError,nT1null,mw,evev,rrzab', 's += x anlamsal');
var big = ''; for (var i = 0; i < 20000; i++) big += 'ab'; eq(big.length + ':' + big.charCodeAt(39999), '40000:98', 'buyuk dize kurma');
var re = /(\d+)-(\w+)/g, src = ''; for (var k = 0; k < 500; k++) src += k + '-ab '; var cnt = 0, mm; while ((mm = re.exec(src))) cnt++;
eq(cnt + ':' + (re.exec('1-a') && 0), '500:0', 'regex exec dongusu');
})();

// eval ve Function
eq(eval("1 + 2"), 3);
eq(new Function("a", "b", "return a * b")(6, 7), 42);

// Getter/setter, defineProperty ile erisimci
let backing = 0;
const acc = {};
Object.defineProperty(acc, "val", { get() { return backing * 2; }, set(v) { backing = v; } });
acc.val = 21;
eq(acc.val, 42);

// Sonsuz dongu korumasi baska testte; burada sonuc
var result;
setTimeout(() => {}, 0);
allP.then(() => 0).then(() => {
  eq(order.join(","), "sync,then1,micro,caught:x,all:123", "mikro gorev sirasi");
  eq(asyncResult, 10, "async");
  result = fails.length ? "HATALAR (" + fails.length + "/" + count + "):\n" + fails.join("\n") : "TUM " + count + " TEST GECTI";
});
result = "bekleniyor";
