// WHATWG Streams (sade): ReadableStream, WritableStream, TransformStream, TextDecoderStream, TextEncoderStream
// ve Response.body. merJS prelude'una gomulur (js_dom.cpp kDomPreludeParts); bu dosya kaynak/test kopyasidir.
(function () {
  var g = typeof globalThis !== 'undefined' ? globalThis : this;
  if (typeof g.ReadableStream !== 'undefined') return;
  function resolved(v) { return Promise.resolve(v); }
  function Deferred() { var d = this; this.promise = new Promise(function (r, j) { d.resolve = r; d.reject = j; }); }

  function ReadableStreamDefaultController(stream) { this._s = stream; }
  Object.defineProperty(ReadableStreamDefaultController.prototype, 'desiredSize', { configurable: true, get: function () {
    var s = this._s; if (s._state === 'errored') return null; if (s._state === 'closed') return 0;
    return s._hwm - s._queue.length; } });
  ReadableStreamDefaultController.prototype.enqueue = function (chunk) {
    var s = this._s;
    if (s._closeRequested || s._state !== 'readable') throw new TypeError('Cannot enqueue a chunk into a closed ReadableStream');
    if (s._reads.length) s._reads.shift().resolve({ value: chunk, done: false });
    else s._queue.push(chunk);
    s._pullIfNeeded();
  };
  ReadableStreamDefaultController.prototype.close = function () {
    var s = this._s;
    if (s._closeRequested || s._state !== 'readable') throw new TypeError('Cannot close a closed ReadableStream');
    s._closeRequested = true;
    if (!s._queue.length) s._finishClose();
  };
  ReadableStreamDefaultController.prototype.error = function (e) { this._s._error(e); };

  function ReadableStream(source, strategy) {
    if (!(this instanceof ReadableStream)) throw new TypeError("Failed to construct 'ReadableStream': Please use the 'new' operator");
    source = source || {};
    this._src = source; this._state = 'readable'; this._queue = []; this._reads = []; this._reader = null;
    this._closeRequested = false; this._pulling = false; this._pullAgain = false; this._started = false; this._err = undefined;
    this._hwm = strategy && typeof strategy.highWaterMark === 'number' ? strategy.highWaterMark : 1;
    this._ctl = new ReadableStreamDefaultController(this);
    var self = this;
    var r;
    try { r = typeof source.start === 'function' ? source.start.call(source, this._ctl) : undefined; } catch (e) { this._error(e); return; }
    resolved(r).then(function () { self._started = true; self._pullIfNeeded(); }, function (e) { self._error(e); });
  }
  ReadableStream.prototype._pullIfNeeded = function () {
    var self = this;
    if (!this._started || this._state !== 'readable' || this._closeRequested || typeof this._src.pull !== 'function') return;
    if (this._queue.length >= this._hwm && !this._reads.length) return;
    if (this._pulling) { this._pullAgain = true; return; }
    this._pulling = true;
    var r;
    try { r = this._src.pull.call(this._src, this._ctl); } catch (e) { this._error(e); return; }
    resolved(r).then(function () {
      self._pulling = false;
      if (self._pullAgain) { self._pullAgain = false; self._pullIfNeeded(); }
    }, function (e) { self._error(e); });
  };
  ReadableStream.prototype._finishClose = function () {
    if (this._state !== 'readable') return;
    this._state = 'closed';
    while (this._reads.length) this._reads.shift().resolve({ value: undefined, done: true });
    if (this._reader) this._reader._closedD.resolve();
  };
  ReadableStream.prototype._error = function (e) {
    if (this._state !== 'readable') return;
    this._state = 'errored'; this._err = e; this._queue = [];
    while (this._reads.length) this._reads.shift().reject(e);
    if (this._reader) { this._reader._closedD.reject(e); this._reader._closedD.promise.catch(function () {}); }
  };
  ReadableStream.prototype._read = function () {
    if (this._state === 'errored') return Promise.reject(this._err);
    if (this._queue.length) {
      var v = this._queue.shift();
      if (this._closeRequested && !this._queue.length) this._finishClose();
      else this._pullIfNeeded();
      return resolved({ value: v, done: false });
    }
    if (this._state === 'closed') return resolved({ value: undefined, done: true });
    var d = new Deferred(); this._reads.push(d); this._pullIfNeeded();
    return d.promise;
  };
  ReadableStream.prototype._cancel = function (reason) {
    if (this._state === 'closed') return resolved();
    if (this._state === 'errored') return Promise.reject(this._err);
    this._queue = []; this._closeRequested = true; this._finishClose();
    var c = this._src.cancel;
    try { return resolved(typeof c === 'function' ? c.call(this._src, reason) : undefined).then(function () {}); } catch (e) { return Promise.reject(e); }
  };
  Object.defineProperty(ReadableStream.prototype, 'locked', { configurable: true, get: function () { return !!this._reader; } });
  ReadableStream.prototype.getReader = function (opts) {
    if (opts && opts.mode === 'byob') throw new TypeError("BYOB okuyucu desteklenmiyor");
    return new ReadableStreamDefaultReader(this);
  };
  ReadableStream.prototype.cancel = function (reason) {
    if (this._reader) return Promise.reject(new TypeError('Cannot cancel a locked stream'));
    return this._cancel(reason);
  };
  ReadableStream.prototype.tee = function () {
    var reader = this.getReader(), c1, c2, canceled = 0;
    function pull() {
      return reader.read().then(function (r) {
        if (r.done) { try { c1.close(); } catch (e) {} try { c2.close(); } catch (e) {} return; }
        try { c1.enqueue(r.value); } catch (e) {} try { c2.enqueue(r.value); } catch (e) {}
      });
    }
    var a = new ReadableStream({ start: function (c) { c1 = c; }, pull: pull, cancel: function (r) { if (++canceled === 2) return reader.cancel(r); } });
    var b = new ReadableStream({ start: function (c) { c2 = c; }, pull: pull, cancel: function (r) { if (++canceled === 2) return reader.cancel(r); } });
    return [a, b];
  };
  ReadableStream.prototype.pipeTo = function (dest, opts) {
    var reader = this.getReader(), writer = dest.getWriter();
    opts = opts || {};
    return new Promise(function (resolve, reject) {
      function step() {
        reader.read().then(function (r) {
          if (r.done) {
            reader.releaseLock();
            if (opts.preventClose) { writer.releaseLock(); resolve(); } else writer.close().then(resolve, reject);
            return;
          }
          writer.write(r.value).then(step, function (e) { reader.cancel(e); reject(e); });
        }, function (e) { if (!opts.preventAbort) writer.abort(e); reject(e); });
      }
      step();
    });
  };
  ReadableStream.prototype.pipeThrough = function (pair, opts) {
    this.pipeTo(pair.writable, opts).catch(function () {});
    return pair.readable;
  };
  ReadableStream.prototype.values = function (opts) {
    var reader = this.getReader(), keep = opts && opts.preventCancel;
    var it = {
      next: function () { return reader.read().then(function (r) { if (r.done) reader.releaseLock(); return r; }); },
      'return': function (v) { if (!keep) reader.cancel(v); reader.releaseLock(); return resolved({ value: v, done: true }); }
    };
    it[Symbol.asyncIterator] = function () { return it; };
    return it;
  };
  if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;
  ReadableStream.from = function (iterable) {
    var it = iterable[Symbol.asyncIterator] ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
    return new ReadableStream({ pull: function (c) {
      return resolved(it.next()).then(function (r) { if (r.done) c.close(); else return resolved(r.value).then(function (v) { c.enqueue(v); }); });
    } });
  };

  function ReadableStreamDefaultReader(stream) {
    if (stream._reader) throw new TypeError('ReadableStreamDefaultReader constructor can only accept readable streams that are not yet locked to a reader');
    this._s = stream; stream._reader = this; this._closedD = new Deferred();
    if (stream._state === 'closed') this._closedD.resolve();
    else if (stream._state === 'errored') { this._closedD.reject(stream._err); this._closedD.promise.catch(function () {}); }
  }
  Object.defineProperty(ReadableStreamDefaultReader.prototype, 'closed', { configurable: true, get: function () { return this._closedD.promise; } });
  ReadableStreamDefaultReader.prototype.read = function () {
    if (!this._s) return Promise.reject(new TypeError('This readable stream reader has been released and cannot be used to read from its previous owner stream'));
    return this._s._read();
  };
  ReadableStreamDefaultReader.prototype.releaseLock = function () {
    if (!this._s) return;
    var s = this._s;
    while (s._reads.length) s._reads.shift().reject(new TypeError('Releasing reader'));
    s._reader = null; this._s = null;
  };
  ReadableStreamDefaultReader.prototype.cancel = function (r) { return this._s ? this._s._cancel(r) : Promise.reject(new TypeError('released')); };

  function WritableStreamDefaultController(s) { this._s = s; }
  WritableStreamDefaultController.prototype.error = function (e) { this._s._fail(e); };
  function WritableStream(sink) {
    sink = sink || {};
    this._sink = sink; this._state = 'writable'; this._writer = null; this._chain = resolved(); this._err = undefined;
    this._ctl = new WritableStreamDefaultController(this);
    var self = this;
    try { this._chain = resolved(typeof sink.start === 'function' ? sink.start.call(sink, this._ctl) : undefined); }
    catch (e) { this._state = 'errored'; this._err = e; }
    this._chain.catch(function (e) { self._fail(e); });
  }
  WritableStream.prototype._fail = function (e) { if (this._state === 'errored') return; this._state = 'errored'; this._err = e; };
  WritableStream.prototype._enqueue = function (fn) {
    var self = this;
    var p = this._chain.then(function () { if (self._state === 'errored') throw self._err; return fn(); });
    this._chain = p.catch(function (e) { self._fail(e); });
    return p;
  };
  Object.defineProperty(WritableStream.prototype, 'locked', { configurable: true, get: function () { return !!this._writer; } });
  WritableStream.prototype.getWriter = function () { return new WritableStreamDefaultWriter(this); };
  WritableStream.prototype.close = function () { var s = this, k = s._sink; s._state = 'closing'; return s._enqueue(function () { return typeof k.close === 'function' ? k.close.call(k) : undefined; }).then(function () { s._state = 'closed'; }); };
  WritableStream.prototype.abort = function (r) { var k = this._sink; this._fail(r); return resolved(typeof k.abort === 'function' ? k.abort.call(k, r) : undefined).then(function () {}); };
  function WritableStreamDefaultWriter(s) {
    if (s._writer) throw new TypeError('WritableStream is locked');
    this._s = s; s._writer = this;
  }
  WritableStreamDefaultWriter.prototype.write = function (chunk) {
    var s = this._s, k = s._sink;
    if (s._state !== 'writable') return Promise.reject(s._err || new TypeError('stream is not writable'));
    return s._enqueue(function () { return typeof k.write === 'function' ? k.write.call(k, chunk, s._ctl) : undefined; });
  };
  WritableStreamDefaultWriter.prototype.close = function () { return this._s.close(); };
  WritableStreamDefaultWriter.prototype.abort = function (r) { return this._s.abort(r); };
  WritableStreamDefaultWriter.prototype.releaseLock = function () { if (this._s) { this._s._writer = null; this._s = null; } };
  Object.defineProperty(WritableStreamDefaultWriter.prototype, 'ready', { configurable: true, get: function () { return this._s ? this._s._chain : resolved(); } });
  Object.defineProperty(WritableStreamDefaultWriter.prototype, 'closed', { configurable: true, get: function () { var s = this._s; return s ? s._chain.then(function () { if (s._state === 'errored') throw s._err; }) : resolved(); } });
  Object.defineProperty(WritableStreamDefaultWriter.prototype, 'desiredSize', { configurable: true, get: function () { return 1; } });

  function TransformStream(tr) {
    tr = tr || {};
    var rc, self = this;
    this.readable = new ReadableStream({ start: function (c) { rc = c; } });
    var ctl = {
      enqueue: function (c) { rc.enqueue(c); },
      error: function (e) { rc.error(e); },
      terminate: function () { try { rc.close(); } catch (e) {} },
      get desiredSize() { return rc.desiredSize; }
    };
    if (typeof tr.start === 'function') tr.start.call(tr, ctl);
    this.writable = new WritableStream({
      write: function (chunk) { return typeof tr.transform === 'function' ? tr.transform.call(tr, chunk, ctl) : ctl.enqueue(chunk); },
      close: function () { return resolved(typeof tr.flush === 'function' ? tr.flush.call(tr, ctl) : undefined).then(function () { try { rc.close(); } catch (e) {} }); },
      abort: function (r) { rc.error(r); }
    });
    void self;
  }

  function TextDecoderStream(label, opts) {
    var dec = new TextDecoder(label, opts);
    var ts = new TransformStream({
      transform: function (chunk, c) { var s = dec.decode(chunk, { stream: true }); if (s) c.enqueue(s); },
      flush: function (c) { var s = dec.decode(); if (s) c.enqueue(s); }
    });
    this.readable = ts.readable; this.writable = ts.writable; this.encoding = dec.encoding;
  }
  function TextEncoderStream() {
    var enc = new TextEncoder();
    var ts = new TransformStream({ transform: function (chunk, c) { c.enqueue(enc.encode(String(chunk))); } });
    this.readable = ts.readable; this.writable = ts.writable; this.encoding = 'utf-8';
  }

  g.ReadableStream = ReadableStream;
  g.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
  g.ReadableStreamDefaultController = ReadableStreamDefaultController;
  g.WritableStream = WritableStream;
  g.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
  g.TransformStream = TransformStream;
  g.TextDecoderStream = TextDecoderStream;
  g.TextEncoderStream = TextEncoderStream;
  g.ByteLengthQueuingStrategy = function (o) { this.highWaterMark = o && o.highWaterMark; this.size = function (c) { return c.byteLength; }; };
  g.CountQueuingStrategy = function (o) { this.highWaterMark = o && o.highWaterMark; this.size = function () { return 1; }; };

  // Response.body: govde tek parca halinde akistan okunur
  if (typeof Response !== 'undefined' && Response.prototype && !Object.getOwnPropertyDescriptor(Response.prototype, 'body')) {
    Object.defineProperty(Response.prototype, 'body', { configurable: true, get: function () {
      if (this._merBody) return this._merBody;
      var resp = this;
      this._merBody = new ReadableStream({
        start: function (c) {
          return resp.arrayBuffer().then(function (b) { if (b.byteLength) c.enqueue(new Uint8Array(b)); c.close(); }, function (e) { c.error(e); });
        }
      });
      return this._merBody;
    } });
  }
})();
