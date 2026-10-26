// merJS async/await ve uretec testleri
// Gercek askiya alma: await zamanlayici/ag bekleyen Promise'lerde de calismali,
// ureteçler tembel (lazy) olmali, sira (mikro gorev) kurallari V8 ile ayni olmali.
var fails = [];
var count = 0;
function eq(a, b, msg) {
  count++;
  var sa = typeof a === "object" ? JSON.stringify(a) : String(a);
  var sb = typeof b === "object" ? JSON.stringify(b) : String(b);
  if (sa !== sb) fails.push((msg || "test " + count) + ": " + sa + " !== " + sb);
}
const sleep = (ms, v) => new Promise(r => setTimeout(() => r(v), ms));
const R = {};
const jobs = [];
function job(name, p, expected) {
  jobs.push(p.then(v => eq(v, expected, name), e => eq("HATA: " + (e && e.message || e), expected, name)));
}

// 1) Sira: await her zaman askiya alir
const L1 = [];
async function a1() { L1.push("a1-bas"); await null; L1.push("a1-son"); }
a1();
L1.push("senkron");
job("await sirasi", Promise.resolve().then(() => null).then(() => L1.join(",")), "a1-bas,senkron,a1-son");

// 2) Zamanlayici bekleyen await (eskiden hata veriyordu)
async function t2() { const a = await sleep(10, "x"); const b = await sleep(5, "y"); return a + b; }
job("zamanlayici await", t2(), "xy");

// 3) try/catch/finally ile reddedilen await
async function t3() {
  try { await Promise.reject(new Error("bum")); return "olmamali"; }
  catch (e) { return "yakalandi:" + e.message; }
  finally { R.t3f = true; }
}
job("await reddi", t3().then(v => v + ":" + R.t3f), "yakalandi:bum:true");

// 4) Donguler, etiketler, switch
async function t4() {
  let s = 0;
  for (let i = 0; i < 5; i++) s += await i;
  let j = 0;
  while (j < 3) j = await (j + 1);
  for (const x of [10, 20]) s += await x;
  outer: for (const a of [1, 2, 3]) {
    for (const b of [1, 2]) {
      if (b === 2) continue outer;
      if (a === 3) break outer;
      s += await (a * b);
    }
  }
  switch (await "k") { case "z": s = -1; break; case "k": s += 100; break; default: s = -2; }
  let d = 0;
  do { d = await (d + 2); } while (d < 6);
  const keys = [];
  for (const k in { p: 1, q: 2 }) { await null; keys.push(k); }
  const fns = [];
  for (let i = 0; i < 3; i++) { await null; fns.push(() => i); }
  return s + ":" + j + ":" + d + ":" + keys.join("") + ":" + fns.map(f => f()).join("");
}
job("donguler", t4(), "143:3:6:pq:012");

// 5) Iki async fonksiyonun ic ice ilerlemesi
const L5 = [];
async function p5(n) { for (let i = 0; i < 3; i++) { L5.push(n + i); await null; } }
job("ic ice", Promise.all([p5("a"), p5("b")]).then(() => L5.join(",")), "a0,b0,a1,b1,a2,b2");

// 6) Ifadelerin icinde await
async function t6() {
  const o = { v: 2, f(x) { return this.v * x; } };
  const arr = [await 1, ...(await [2, 3])];
  const obj = { a: await "A", ["k" + 1]: await "K", ...(await { z: 1 }) };
  const tpl = `${await "x"}-${await "y"}`;
  const call = o.f(await 3);
  const mem = (await o).v;
  let c = 1; c += await 4;
  const opt = (await null)?.foo.bar;
  const lg = (await 0) || (await "or");
  const cond = (await true) ? await "evet" : "hayir";
  const nw = new Array(await 3).length;
  const un = typeof (await 5) + !(await 0);
  let q = null; q ??= await "q";
  const seq = (await 1, await 2);
  return [arr.join(""), obj.a + obj.k1 + obj.z, tpl, call, mem, c, opt, lg, cond, nw, un, q, seq].join("|");
}
job("ifadeler", t6(), "123|AK1|x-y|6|2|5||or|evet|3|numbertrue|q|2");

// 7) async ok fonksiyonu ve this
const objA = { n: 7, m() { return (async () => { await null; return this.n; })(); } };
job("async ok this", objA.m(), 7);

// 8) Thenable nesne
job("thenable", (async () => await { then(r) { r(42); } })(), 42);

// 9) await oncesi hata reddedilmis Promise doner (senkron firlatmaz)
let syncThrow = false;
async function bad() { throw new Error("e1"); }
let badP;
try { badP = bad(); } catch (e) { syncThrow = true; }
job("ilk hata", badP.then(() => "olmamali", e => e.message + ":" + syncThrow), "e1:false");

// 10) Ozyineleme
async function fib(n) { return n < 2 ? n : await fib(n - 1) + await fib(n - 2); }
job("async ozyineleme", fib(15), 610);

// 11) Uzun dongu (performans / yigin buyumesi yok)
async function uzun() { let s = 0; for (let i = 0; i < 20000; i++) s += await i; return s; }
job("20000 await", uzun(), 199990000);

// 12) finally icinde await, return degeri korunur
async function tf() { try { return await 1; } finally { await null; R.tf = "f"; } }
job("finally await", tf().then(v => v + R.tf), "1f");

// 13) Ic ice try, catch icinde await
async function t13() {
  let s = "";
  try {
    try { await Promise.reject("ic"); }
    catch (e) { s += e; await sleep(1); throw "dis"; }
  } catch (e2) { s += "+" + e2; }
  return s;
}
job("ic ice try", t13(), "ic+dis");

// 14) Sinif metodu ve super
class Taban { async ad() { await null; return "taban"; } }
class Tureyen extends Taban { async ad() { const t = await super.ad(); return t + ">tureyen"; } }
job("sinif async", new Tureyen().ad(), "taban>tureyen");

// ---------------- Ureteçler ----------------
function* nat() { let i = 0; while (true) yield i++; }
const g = nat(); g.next(); g.next();
eq(g.next().value, 2, "sonsuz uretec");
function* take(n, it) { for (const x of it) { if (n-- <= 0) return; yield x; } }
eq([...take(4, nat())].join(), "0,1,2,3", "tembel take");
const [d1, d2] = nat();
eq(d1 + ":" + d2, "0:1", "uretecten destructuring");
eq(Object.prototype.toString.call(nat()), "[object Generator]", "toStringTag");

function* echo() { const a = yield 1; const b = yield a * 2; return a + b; }
const e = echo();
eq(e.next().value, 1);
eq(e.next(5).value, 10, "next(v) degeri");
eq(e.next(7), { value: 12, done: true }, "return degeri");
eq(e.next(), { done: true }, "bitmis uretec");

const L10 = [];
function* gf() { try { yield 1; yield 2; } finally { L10.push("fin"); } }
const gi = gf(); gi.next();
eq(gi.return(9), { value: 9, done: true }, "return()");
eq(gi.next().done, true);
function* gc() { try { yield 1; yield 2; } finally { L10.push("kapandi"); } }
for (const x of gc()) break;
eq(L10.join(), "fin,kapandi", "finally / for-of break kapatma");

function* gt() { while (true) { try { yield "w"; } catch (err) { yield "aldi " + err; } } }
const tg = gt(); tg.next();
eq(tg.throw("E").value, "aldi E", "throw() yakalandi");
function* gnt() { yield 1; }
const gn = gnt(); gn.next();
let thrownOut = "";
try { gn.throw(new Error("disari")); } catch (err) { thrownOut = err.message; }
eq(thrownOut, "disari", "throw() disari");

function* inner() { const x = yield "i1"; yield "i2:" + x; return "ret"; }
function* outer() { const r = yield* inner(); yield r; yield* [7, 8]; }
const og = outer();
eq([og.next().value, og.next("X").value, og.next().value, og.next().value, og.next().value, og.next().done].join(), "i1,i2:X,ret,7,8,true", "yield*");

class Torba { constructor() { this.l = [3, 4]; } *[Symbol.iterator]() { for (const x of this.l) yield x * 10; } }
eq(Array.from(new Torba()).join(), "30,40", "sinif uretec metodu");
function* argsGen() { yield arguments.length; yield this === undefined || this === globalThis; }
eq([...argsGen(1, 2, 3)].join(), "3,true", "arguments");
const yRun = nat();
let yRe = "";
function* reent() { try { yRe2.next(); } catch (err) { yRe = err.message; } yield 1; }
const yRe2 = reent();
yRe2.next();
eq(yRe, "Generator is already running", "yeniden giris");

// ---------------- Async ureteçler ----------------
async function* ag() { for (let i = 0; i < 3; i++) { await sleep(1); yield i; } }
async function t20() {
  const s = [];
  for await (const v of ag()) s.push(v);
  for await (const v of [Promise.resolve("p"), "q"]) s.push(v);
  return s.join("");
}
job("for await", t20(), "012pq");
async function t21() {
  const it = ag();
  const a = it.next(), b = it.next();   // kuyruk: ayni anda iki istek
  const ra = await a, rb = await b;
  const rr = await it.return("son");
  const rn = await it.next();
  return [ra.value, rb.value, rr.value, rr.done, rn.done].join();
}
job("async uretec kuyrugu", t21(), "0,1,son,true,true");
async function* agd() { yield* ag(); yield "x"; }
async function t22() { const s = []; for await (const v of agd()) s.push(v); return s.join(""); }
job("async yield*", t22(), "012x");

// ---------------- GC: askidaki fonksiyonun yerel degerleri korunur ----------------
async function gcTest() {
  const local = { etiket: "tut" };
  const r = [{ k: "ucus" }, await sleep(30, "x")];   // ara deger cercevede bekliyor
  return local.etiket + ":" + r[0].k + ":" + r[1];
}
job("GC sonrasi", gcTest(), "tut:ucus:x");
for (const ms of [5, 10, 15, 20]) setTimeout(() => { let junk = []; for (let i = 0; i < 120000; i++) junk.push({ i }); }, ms);

var result = "bekleniyor";
sleep(60).then(() => Promise.all(jobs)).then(() => {
  result = fails.length ? "HATALAR (" + fails.length + "/" + count + "):\n" + fails.join("\n") : "TUM " + count + " TEST GECTI";
});
