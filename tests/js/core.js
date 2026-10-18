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
