'use strict';
// fetch() for node run jitless (NODE_OPTIONS, init.flutter.rc), preloaded with
// --require.
//
// Node's fetch is undici's, which parses HTTP/1.1 with llhttp compiled to
// WebAssembly. V8 has no WebAssembly without its JIT: the official builds
// leave out DrumBrake, its interpreter, so --jitless removes the WebAssembly
// global, and every fetch() failed once its TLS handshake was done
// ("fetch failed", cause "WebAssembly is not defined"). Claude Code calls
// its API through fetch(); its requests made with node's http module
// (axios) went through.
//
// This one runs requests on node's http and https modules, whose parser is
// node's own (C, llhttp), and answers with the WHATWG Request, Response and
// Headers classes node already has. Installed only where WebAssembly is
// missing. Undici's options that pick a connection (dispatcher: a proxy, a
// client certificate) are not undici's here and are ignored; the global
// agents' settings (NODE_EXTRA_CA_CERTS, ...) apply.
if (typeof WebAssembly === 'undefined' && typeof globalThis.fetch === 'function') {
  const NativeRequest = globalThis.Request;
  const NativeResponse = globalThis.Response;
  const NativeHeaders = globalThis.Headers;
  let mods = null;
  const load = () => mods || (mods = {
    http: require('http'),
    https: require('https'),
    zlib: require('zlib'),
    Readable: require('stream').Readable,
  });
  let agents = null;
  const agentFor = (protocol) => {
    const { http, https } = load();
    if (!agents)
      agents = {
        'http:': new http.Agent({ keepAlive: true }),
        'https:': new https.Agent({ keepAlive: true }),
      };
    return agents[protocol];
  };

  const fetchFailed = (cause) => {
    const err = new TypeError('fetch failed');
    err.cause = cause;
    return err;
  };
  const abortReason = (signal) =>
    signal.reason !== undefined ? signal.reason : new DOMException('This operation was aborted', 'AbortError');
  const nullBodyStatus = (status) => status === 101 || status === 204 || status === 205 || status === 304;

  // the response's content-coding undone, as fetch does (last applied first)
  function decode(res, method, status) {
    const { zlib } = load();
    let stream = res;
    const enc = res.headers['content-encoding'];
    if (!enc || method === 'HEAD' || nullBodyStatus(status))
      return stream;
    const codings = enc.toLowerCase().split(',').map((s) => s.trim()).filter(Boolean).reverse();
    for (const c of codings) {
      let d;
      if (c === 'gzip' || c === 'x-gzip')
        d = zlib.createGunzip({ flush: zlib.constants.Z_SYNC_FLUSH, finishFlush: zlib.constants.Z_SYNC_FLUSH });
      else if (c === 'deflate')
        d = zlib.createInflate({ flush: zlib.constants.Z_SYNC_FLUSH, finishFlush: zlib.constants.Z_SYNC_FLUSH });
      else if (c === 'br')
        d = zlib.createBrotliDecompress({ flush: zlib.constants.BROTLI_OPERATION_FLUSH, finishFlush: zlib.constants.BROTLI_OPERATION_FLUSH });
      else if (c === 'zstd' && zlib.createZstdDecompress)
        d = zlib.createZstdDecompress();
      else
        return stream;  // unknown: handed over as it came
      const src = stream;
      src.on('error', (e) => d.destroy(e));
      stream = src.pipe(d);
    }
    return stream;
  }

  function send(url, method, headers, body, signal) {
    const { http, https } = load();
    return new Promise((resolve, reject) => {
      const mod = url.protocol === 'https:' ? https : http;
      let req;
      const onAbort = () => {
        const reason = abortReason(signal);
        if (req)
          req.destroy(reason);
        reject(reason);
      };
      req = mod.request(url, { method, headers, agent: agentFor(url.protocol) }, (res) => {
        signal.removeEventListener('abort', onAbort);
        resolve(res);
      });
      req.on('error', (e) => {
        signal.removeEventListener('abort', onAbort);
        reject(signal.aborted ? abortReason(signal) : fetchFailed(e));
      });
      signal.addEventListener('abort', onAbort, { once: true });
      if (body && body.length)
        req.end(body);
      else
        req.end();
    });
  }

  async function tvmFetch(input, init) {
    const request = new NativeRequest(input, init);
    const signal = request.signal;
    if (signal.aborted)
      throw abortReason(signal);

    let method = request.method;
    let body = request.body ? Buffer.from(await request.arrayBuffer()) : null;
    const headers = {};
    for (const [k, v] of request.headers)
      headers[k] = v;
    if (!('accept' in headers)) headers['accept'] = '*/*';
    if (!('accept-language' in headers)) headers['accept-language'] = '*';
    if (!('sec-fetch-mode' in headers)) headers['sec-fetch-mode'] = 'cors';
    if (!('user-agent' in headers)) headers['user-agent'] = 'node';
    if (!('accept-encoding' in headers)) headers['accept-encoding'] = 'gzip, deflate, br';
    if (body)
      headers['content-length'] = String(body.length);

    let url = new URL(request.url);
    let redirected = false;
    for (let hops = 0; ; hops++) {
      if (url.protocol !== 'http:' && url.protocol !== 'https:')
        throw fetchFailed(new Error(`unsupported protocol ${url.protocol}`));
      const res = await send(url, method, headers, body, signal);
      const status = res.statusCode;

      if ([301, 302, 303, 307, 308].includes(status) && res.headers.location && request.redirect !== 'manual') {
        res.resume();
        if (request.redirect === 'error')
          throw fetchFailed(new Error('unexpected redirect'));
        if (hops >= 20)
          throw fetchFailed(new Error('redirect count exceeded'));
        const next = new URL(res.headers.location, url);
        if (status === 303 ? method !== 'HEAD' : (status === 301 || status === 302) && method === 'POST') {
          method = 'GET';
          body = null;
          for (const h of ['content-length', 'content-type', 'content-encoding', 'content-language', 'content-location'])
            delete headers[h];
        }
        if (next.origin !== url.origin) {
          delete headers['authorization'];
          delete headers['cookie'];
          delete headers['proxy-authorization'];
        }
        delete headers['host'];
        url = next;
        redirected = true;
        continue;
      }

      const outHeaders = new NativeHeaders();
      for (let i = 0; i + 1 < res.rawHeaders.length; i += 2)
        outHeaders.append(res.rawHeaders[i], res.rawHeaders[i + 1]);

      let outBody = null;
      if (method === 'HEAD' || nullBodyStatus(status)) {
        res.resume();
      } else {
        const stream = decode(res, method, status);
        const onAbort = () => res.destroy(abortReason(signal));
        signal.addEventListener('abort', onAbort, { once: true });
        stream.on('close', () => signal.removeEventListener('abort', onAbort));
        outBody = load().Readable.toWeb(stream);
      }

      const response = new NativeResponse(outBody, {
        status: status < 200 || status > 599 ? 599 : status,
        statusText: res.statusMessage || '',
        headers: outHeaders,
      });
      Object.defineProperty(response, 'url', { value: url.href });
      Object.defineProperty(response, 'redirected', { value: redirected });
      return response;
    }
  }

  Object.defineProperty(globalThis, 'fetch', {
    value: tvmFetch, writable: true, enumerable: true, configurable: true,
  });
}
