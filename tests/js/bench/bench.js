// merJS mikro karsilastirma testleri: her biri ms cinsinden sure yazar (Node ile karsilastirmak icin)
var results = [];
function bench(name, fn) {
  var t0 = Date.now();
  var r = fn();
  results.push(name + '=' + (Date.now() - t0) + 'ms(' + r + ')');
}
bench('loop', function () { var s = 0; for (var i = 0; i < 1000000; i++) s += i & 7; return s; });
bench('letloop', function () { let s = 0; for (let i = 0; i < 300000; i++) s += i % 7; return s; });
bench('fib', function () { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(22); });
bench('prop', function () { var o = { a: 1, b: 2, c: 3 }; var s = 0; for (var i = 0; i < 300000; i++) { s += o.a + o.b + o.c; o.a = i & 3; } return s; });
bench('method', function () {
  function P(x) { this.x = x; } P.prototype.get = function () { return this.x; };
  var p = new P(3), s = 0; for (var i = 0; i < 200000; i++) s += p.get(); return s;
});
bench('closure', function () { function mk(k) { return function (x) { return x + k; }; } var f = mk(2), s = 0; for (var i = 0; i < 200000; i++) s = f(s) & 1023; return s; });
bench('array', function () { var a = []; for (var i = 0; i < 200000; i++) a.push(i); var s = 0; for (var j = 0; j < a.length; j++) s += a[j]; return s; });
bench('arrfn', function () { var a = []; for (var i = 0; i < 50000; i++) a.push(i); return a.map(function (x) { return x * 2; }).filter(function (x) { return x % 3; }).reduce(function (p, c) { return p + c; }, 0); });
bench('string', function () { var s = ''; for (var i = 0; i < 50000; i++) s += String.fromCharCode(65 + i % 26); var c = 0; for (var j = 0; j < s.length; j++) if (s.charCodeAt(j) === 65) c++; return c + s.indexOf('XYZ'); });
bench('objalloc', function () { var last; for (var i = 0; i < 100000; i++) last = { i: i, n: { v: i } }; return last.n.v; });
bench('json', function () { var o = []; for (var i = 0; i < 2000; i++) o.push({ id: i, name: 'n' + i, tags: ['a', 'b'] }); var s = JSON.stringify(o); return JSON.parse(s).length + s.length; });
bench('regex', function () { var re = /(\d+)-(\w+)/g, s = ''; for (var i = 0; i < 2000; i++) s += i + '-ab '; var c = 0, m; while ((m = re.exec(s))) c++; return c; });
bench('switch', function () { var s = 0; for (var i = 0; i < 200000; i++) { switch (i & 3) { case 0: s += 1; break; case 1: s += 2; break; default: s += 3; } } return s; });
bench('class', function () {
  class A { constructor(v) { this.v = v; } inc() { this.v++; return this; } get val() { return this.v; } }
  var a = new A(0); for (var i = 0; i < 100000; i++) a.inc(); return a.val;
});
bench('spread', function () { var s = 0; function f(a, b, c) { return a + b + c; } var arr = [1, 2, 3]; for (var i = 0; i < 50000; i++) s += f(...arr); return s; });
bench('map', function () { var m = new Map(); for (var i = 0; i < 50000; i++) m.set('k' + i, i); var s = 0; for (var i2 = 0; i2 < 50000; i2++) s += m.get('k' + i2); return s; });
var result = 'GECTI ' + results.join(' ');
if (typeof console !== 'undefined' && typeof window === 'undefined' && typeof process !== 'undefined') console.log(result);
