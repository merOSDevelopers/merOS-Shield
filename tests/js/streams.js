// Streams testi (beklenen: Node/V8 yerel akislarinin ciktisi). Calistirma: js_runner --dom tests/js/streams.js
var out = [];
function log(x) { out.push(String(x)); }
var rs = new ReadableStream({
  start: function (c) { c.enqueue('a'); c.enqueue('b'); },
  pull: function (c) { if (!this.n) this.n = 0; this.n++; if (this.n > 2) c.close(); else c.enqueue('p' + this.n); }
});
log(rs.locked);
var rd = rs.getReader();
log(rs.locked);
var chain = rd.read().then(function (r) { log(r.value + r.done); return rd.read(); })
  .then(function (r) { log(r.value + r.done); return rd.read(); })
  .then(function (r) { log(r.value + r.done); return rd.read(); })
  .then(function (r) { log(r.value + r.done); return rd.read(); })
  .then(function (r) { log(r.value + '|' + r.done); return rd.closed; })
  .then(function () { log('closed'); rd.releaseLock(); log(rs.locked); });
// tee + pipeThrough + TextDecoderStream
chain = chain.then(function () {
  var src = new ReadableStream({ start: function (c) { c.enqueue(new Uint8Array([72, 105, 0xC3])); c.enqueue(new Uint8Array([0xA7, 33])); c.close(); } });
  var t = src.tee();
  var r1 = t[0].pipeThrough(new TextDecoderStream()).getReader();
  var acc = '';
  function pump() { return r1.read().then(function (r) { if (r.done) return; acc += r.value; return pump(); }); }
  return pump().then(function () { log('dec=' + acc); var r2 = t[1].getReader(); return r2.read(); }).then(function (r) { log('tee2=' + r.value.length); });
});
// TransformStream
chain = chain.then(function () {
  var ts = new TransformStream({ transform: function (ch, c) { c.enqueue(ch.toUpperCase()); c.enqueue(ch + '!'); }, flush: function (c) { c.enqueue('END'); } });
  var w = ts.writable.getWriter();
  w.write('x'); w.write('y'); w.close();
  var r = ts.readable.getReader(), got = [];
  function pump() { return r.read().then(function (x) { if (x.done) return; got.push(x.value); return pump(); }); }
  return pump().then(function () { log('ts=' + got.join(',')); });
});
// error
chain = chain.then(function () {
  var es = new ReadableStream({ start: function (c) { c.error(new TypeError('boom')); } });
  return es.getReader().read().then(function () { log('no'); }, function (e) { log('err=' + e.message); });
});
// WritableStream sink
chain = chain.then(function () {
  var got = [];
  var ws = new WritableStream({ write: function (c) { got.push(c); }, close: function () { got.push('closed'); } });
  var rs2 = new ReadableStream({ start: function (c) { c.enqueue(1); c.enqueue(2); c.close(); } });
  return rs2.pipeTo(ws).then(function () { log('pipe=' + got.join(',')); });
});
// cancel
chain = chain.then(function () {
  var why;
  var cs = new ReadableStream({ cancel: function (r) { why = r; } });
  return cs.cancel('neden').then(function () { log('cancel=' + why); });
});
chain = chain.then(function () { var got = out.join(" "), want = "false true afalse bfalse p1false p2false undefined|true closed false dec=Hiç! tee2=3 ts=X,x!,Y,y!,END err=boom pipe=1,2,closed cancel=neden"; result = got === want ? "TUM 1 TEST GECTI" : "HATALAR:\n" + got; });
var result = 'bekleniyor';
