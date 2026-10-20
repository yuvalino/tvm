// Preloaded into compiled Bun programs, which execve runs on the app's Bun
// with this in BUN_OPTIONS (tvm/tvm.c, "Bun programs"): Bun.ant.CellSegmenter
// for the official Bun runtime.
//
// Claude Code (2.1.271 and later) paints its terminal UI through a native
// class of Anthropic's own Bun build (@anthropic-ai/bun-internal), which the
// official Bun does not have:
//
//     new Bun.ant.CellSegmenter({ ambiguousIsNarrow, substitute, screen })
//       .graphemes / .sgrKeys / .sgrCloseKeys / .uris        (live arrays)
//       .segment(line, cells:Int32Array, runs:Int32Array, reordered) -> count
//       .paint(screen, width, x, y, cells, count, order, charMap, words) -> packed
//       .setCell(screen, width, x, y, charIndex, word) -> packed
//
// It splits a line into terminal cells (graphemes, their widths, SGR and OSC 8
// runs) and paints them into the renderer's screen buffer. Without it the
// first render throws ("This runtime has no Bun.ant.CellSegmenter") and the
// TUI never draws: Claude Code has no fallback. This is the same contract in
// JavaScript, its behaviour matched to the native class by differential
// testing (about 4500 recorded cases of segment, paint and setCell). Where
// Bun.ant.CellSegmenter exists (Anthropic's runtime), it does nothing.
//
// Not covered as the native class does: bidi reordering (only when the
// terminal needs it: Windows Terminal, VS Code) is a UAX #9 subset without
// explicit embeddings; widths are this Bun's Bun.stringWidth. Workers do not
// get it (Claude Code's do not paint).
(() => {
  if (typeof Bun === "undefined") return;
  if (typeof Bun.ant?.CellSegmenter === "function") return;

  const TAG = "CellSegmenter";
  const i32err = (n) => new TypeError(`Expected ${n} to be a non-shared, fixed-length, 4-byte-aligned Int32Array for '${TAG}'.`);
  const isI32 = (a) => a instanceof Int32Array;
  const toInt = (v) => { v = +v; return v !== v ? 0 : v | 0; };
  const checkInt = (v, name) => {
    if (typeof v !== "number" || !Number.isInteger(v)) throw new TypeError(`The "${name}" property must be of type integer. Received ${v === null ? "null" : typeof v}`);
    if (v < 0 || v > 9007199254740991) throw new RangeError(`The value of "${name}" is out of range. It must be >= 0 and <= 9007199254740991. Received ${v}`);
    return v;
  };

  // ---- cell packing (shared with the JS side) ----
  const W_MASK = 255, TAB = 256, SUBST = 512, RUN_SHIFT = 10;
  const INITIAL = [" ", ""];
  for (let c = 0x21; c <= 0x7e; c++) INITIAL.push(String.fromCharCode(c));
  INITIAL.push("\t", "�");
  const TAB_INDEX = 96, FFFD_INDEX = 97;
  const DEFAULT_SCREEN = { widthMask: 3, narrow: 0, wide: 1, spacerTail: 2, spacerHead: 3, emptyCharIndex: 0, spacerCharIndex: 1, emptyWord: 0, tabWidth: 8 };

  // ---- SGR attribute slots and close codes (as the native implementation) ----
  const closeCode = (n) =>
    n === 1 || n === 2 ? 22 : n === 3 || n === 20 ? 23 : n === 4 || n === 21 ? 24 : n === 5 || n === 6 ? 25 :
    n === 7 ? 27 : n === 8 ? 28 : n === 9 ? 29 :
    (n >= 30 && n <= 38) || (n >= 90 && n <= 97) ? 39 : (n >= 40 && n <= 48) || (n >= 100 && n <= 107) ? 49 :
    n === 51 || n === 52 ? 54 : n === 53 ? 55 : n === 58 ? 59 : n === 73 || n === 74 ? 75 : 0;
  const slotOf = (n) =>
    n === 1 ? 1 : n === 2 ? 2 : n === 3 ? 3 : n === 20 ? 4 : n === 4 ? 5 : n === 21 ? 6 : n === 5 || n === 6 ? 7 :
    n === 7 ? 8 : n === 8 ? 9 : n === 9 ? 10 : (n >= 30 && n <= 38) || (n >= 90 && n <= 97) ? 11 :
    (n >= 40 && n <= 48) || (n >= 100 && n <= 107) ? 12 : n === 51 ? 13 : n === 52 ? 14 : n === 53 ? 15 : n === 58 ? 16 :
    n === 73 || n === 74 ? 17 : 0;
  const CLOSERS = new Set([22, 23, 24, 25, 27, 28, 29, 39, 49, 54, 55, 59, 75]);
  const MAX_SGR_TOKENS = 32;
  const leadingInt = (s) => {
    let i = 0; while (i < s.length) { const c = s.charCodeAt(i); if (c < 0x30 || c > 0x39) break; i++; }
    if (i === 0) return -1;
    if (i > 15) return -1;
    return parseInt(s.slice(0, i), 10);
  };
  const normNum = (s) => { // pure digit token -> canonical decimal, empty -> "0", colon token -> verbatim
    if (s.indexOf(":") >= 0) return s;
    if (s === "") return "0";
    if (s.length > 15) return s;
    return String(parseInt(s, 10));
  };
  const isDigits = (s) => { for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c < 0x30 || c > 0x39) return false; } return true; };

  // ---- bidi (UAX#9 subset: no explicit embeddings; those are substituted or dropped) ----
  const L = 0, R = 1, AL = 2, EN = 3, ES = 4, ET = 5, AN = 6, CS = 7, NSM = 8, BN = 9, B = 10, S = 11, WS = 12, ON = 13;
  const RANGES = [
    // [lo, hi, class], sorted by lo. Approximation of DerivedBidiClass for the classes that matter.
    [0x00A0, 0x00A0, CS], [0x00A2, 0x00A5, ET], [0x00B0, 0x00B1, ET], [0x00B2, 0x00B3, EN], [0x00B9, 0x00B9, EN],
    [0x0590, 0x05FF, R], [0x0600, 0x0605, AN], [0x0606, 0x0608, AL], [0x0609, 0x060A, ET], [0x060B, 0x060B, AL], [0x060C, 0x060C, CS],
    [0x060D, 0x065F, AL], [0x0660, 0x0669, AN], [0x066A, 0x066A, ET], [0x066B, 0x066C, AN], [0x066D, 0x06DC, AL], [0x06DD, 0x06DD, AN],
    [0x06DE, 0x06EF, AL], [0x06F0, 0x06F9, EN], [0x06FA, 0x07BF, AL], [0x07C0, 0x085F, R], [0x0860, 0x088F, AL], [0x0890, 0x0891, AN],
    [0x0892, 0x08E1, AL], [0x08E2, 0x08E2, AN], [0x08E3, 0x08FF, AL], [0x09F2, 0x09F3, ET], [0x09FB, 0x09FB, ET], [0x0AF1, 0x0AF1, ET],
    [0x0BF9, 0x0BF9, ET], [0x0E3F, 0x0E3F, ET], [0x1680, 0x1680, WS], [0x17DB, 0x17DB, ET], [0x2000, 0x200A, WS], [0x200E, 0x200F, L],
    [0x2028, 0x2028, WS], [0x2029, 0x2029, B], [0x202F, 0x202F, CS], [0x2044, 0x2044, CS], [0x205F, 0x205F, WS], [0x2070, 0x2070, EN],
    [0x2074, 0x2079, EN], [0x207A, 0x207B, ES], [0x2080, 0x2089, EN], [0x208A, 0x208B, ES], [0x20A0, 0x20C0, ET], [0x212E, 0x212E, ET],
    [0x2212, 0x2212, ES], [0x2213, 0x2213, ET], [0x2488, 0x249B, EN], [0x3000, 0x3000, WS], [0xA838, 0xA839, ET], [0xFB1D, 0xFB4F, R],
    [0xFB29, 0xFB29, ES], [0xFB50, 0xFDFF, AL], [0xFE50, 0xFE50, CS], [0xFE52, 0xFE52, CS], [0xFE55, 0xFE55, CS], [0xFE5F, 0xFE5F, ET],
    [0xFE62, 0xFE63, ES], [0xFE69, 0xFE6A, ET], [0xFE70, 0xFEFE, AL], [0xFF03, 0xFF05, ET], [0xFF0B, 0xFF0B, ES], [0xFF0C, 0xFF0C, CS],
    [0xFF0D, 0xFF0D, ES], [0xFF0E, 0xFF0F, CS], [0xFF10, 0xFF19, EN], [0xFF1A, 0xFF1A, CS], [0xFFE0, 0xFFE1, ET], [0xFFE5, 0xFFE6, ET],
    [0x10800, 0x10CFF, R], [0x10D00, 0x10D3F, AL], [0x10D40, 0x10E5F, R], [0x10E60, 0x10E7E, AN], [0x10E7F, 0x10EBF, R],
    [0x10EC0, 0x10EFF, AL], [0x10F00, 0x10F2F, R], [0x10F30, 0x10F6F, AL], [0x10F70, 0x10FFF, R], [0x1D7CE, 0x1D7FF, EN],
    [0x1E2FF, 0x1E2FF, ET], [0x1E800, 0x1EC6F, R], [0x1EC70, 0x1ECBF, AL], [0x1ECC0, 0x1ECFF, R], [0x1ED00, 0x1ED4F, AL],
    [0x1ED50, 0x1EDFF, R], [0x1EE00, 0x1EEFF, AL], [0x1EF00, 0x1EFFF, R], [0x1F100, 0x1F10A, EN], [0x1FBF0, 0x1FBF9, EN],
  ];
  const reMark = /^[\p{Mn}\p{Me}]/u, reFormat = /^\p{Cf}/u, reNeutral = /^[\p{P}\p{S}]/u;
  const reModifier = /[\u{1F3FB}-\u{1F3FF}]/u, reExtPict = /^\p{Extended_Pictographic}/u;
  const bidiClass = (cp) => {
    if (cp < 0x80) {
      if (cp >= 0x30 && cp <= 0x39) return EN;
      if ((cp >= 0x41 && cp <= 0x5A) || (cp >= 0x61 && cp <= 0x7A)) return L;
      if (cp === 0x20 || cp === 0x0C) return WS;
      if (cp === 0x2B || cp === 0x2D) return ES;
      if (cp >= 0x23 && cp <= 0x25) return ET;
      if (cp === 0x2C || cp === 0x2E || cp === 0x2F || cp === 0x3A) return CS;
      if (cp === 0x09 || cp === 0x0B || cp === 0x1F) return S;
      if (cp === 0x0A || cp === 0x0D || (cp >= 0x1C && cp <= 0x1E)) return B;
      if (cp < 0x20 || cp === 0x7F) return BN;
      return ON;
    }
    if (cp === 0x85) return B;
    if (cp < 0xA0) return BN;
    if (cp >= 0x300 && reMark.test(String.fromCodePoint(cp))) return NSM;
    let lo = 0, hi = RANGES.length - 1;
    while (lo <= hi) { const m = (lo + hi) >> 1, r = RANGES[m]; if (cp < r[0]) hi = m - 1; else if (cp > r[1]) lo = m + 1; else return r[2]; }
    const s = String.fromCodePoint(cp);
    if (reMark.test(s)) return NSM;
    if (reFormat.test(s)) return BN;
    if (reNeutral.test(s)) return ON;
    return L;
  };
  // Resolves embedding levels for one line of code points. Returns Uint8Array of levels.
  const bidiLevels = (cps, n) => {
    const t = new Uint8Array(n), orig = new Uint8Array(n);
    let para = 0;
    for (let i = 0; i < n; i++) { const c = bidiClass(cps[i]); orig[i] = t[i] = c; }
    for (let i = 0; i < n; i++) { const c = t[i]; if (c === L) { para = 0; break; } if (c === R || c === AL) { para = 1; break; } }
    const sos = para ? R : L, eos = sos;
    let prev = sos;
    for (let i = 0; i < n; i++) { if (t[i] === NSM || t[i] === BN) t[i] = prev; else prev = t[i]; }        // W1 (+BN folded into neighbour)
    let last = sos;
    for (let i = 0; i < n; i++) { const c = t[i]; if (c === L || c === R || c === AL) last = c; else if (c === EN && last === AL) t[i] = AN; } // W2
    for (let i = 0; i < n; i++) if (t[i] === AL) t[i] = R;                                              // W3
    for (let i = 1; i + 1 < n; i++) {                                                                  // W4
      if (t[i] === ES && t[i - 1] === EN && t[i + 1] === EN) t[i] = EN;
      else if (t[i] === CS) { if (t[i - 1] === EN && t[i + 1] === EN) t[i] = EN; else if (t[i - 1] === AN && t[i + 1] === AN) t[i] = AN; }
    }
    for (let i = 0; i < n; i++) {                                                                      // W5
      if (t[i] !== ET) continue;
      let j = i; while (j < n && t[j] === ET) j++;
      if ((i > 0 && t[i - 1] === EN) || (j < n && t[j] === EN)) for (let k = i; k < j; k++) t[k] = EN;
      i = j - 1;
    }
    for (let i = 0; i < n; i++) if (t[i] === ES || t[i] === ET || t[i] === CS) t[i] = ON;                 // W6
    last = sos;
    for (let i = 0; i < n; i++) { const c = t[i]; if (c === L || c === R) last = c; else if (c === EN && last === L) t[i] = L; } // W7
    const strongDir = (c) => (c === L ? L : c === R || c === EN || c === AN ? R : -1);
    for (let i = 0; i < n; i++) {                                                                      // N1, N2
      const c = t[i];
      if (!(c === B || c === S || c === WS || c === ON)) continue;
      let j = i; while (j < n && (t[j] === B || t[j] === S || t[j] === WS || t[j] === ON)) j++;
      const before = i > 0 ? strongDir(t[i - 1]) : sos, after = j < n ? strongDir(t[j]) : eos;
      const dir = before === after && before !== -1 ? before : (para ? R : L);
      for (let k = i; k < j; k++) t[k] = dir;
      i = j - 1;
    }
    const lv = new Uint8Array(n);
    for (let i = 0; i < n; i++) {                                                                      // I1, I2
      const c = t[i]; let l = para;
      if (para === 0) { if (c === R) l = 1; else if (c === AN || c === EN) l = 2; }
      else { if (c === L || c === EN || c === AN) l = 2; }
      lv[i] = l;
    }
    for (let i = 0; i < n; i++) {                                                                      // L1
      const c = orig[i];
      if (c === S || c === B) { lv[i] = para; let k = i - 1; while (k >= 0 && (orig[k] === WS || orig[k] === BN)) lv[k--] = para; }
    }
    { let k = n - 1; while (k >= 0 && (orig[k] === WS || orig[k] === BN)) lv[k--] = para; }
    return lv;
  };

  class CellSegmenter {
    constructor(options) {
      const o = options === undefined || options === null ? {} : options;
      this._ambig = o.ambiguousIsNarrow === undefined ? true : !!o.ambiguousIsNarrow;
      this._widthOpts = { ambiguousIsNarrow: this._ambig };
      const sub = [];
      if (o.substitute !== undefined && o.substitute !== null) {
        if (!Array.isArray(o.substitute)) throw new TypeError(`Expected options.substitute to be a list of [first, last] pairs for '${TAG}'.`);
        for (const p of o.substitute) {
          if (!Array.isArray(p) || p.length !== 2) throw new TypeError(`Expected options.substitute[] to be a [first, last] pair for '${TAG}'.`);
          for (let k = 0; k < 2; k++) {
            if (typeof p[k] !== "number") throw new TypeError(`The "substitute[][${k}]" property must be of type number. Received ${typeof p[k]}`);
            if (p[k] < 128 || p[k] > 0x10ffff) throw new RangeError(`The value of "substitute[][${k}]" is out of range. It must be >= 128 and <= 1114111. Received ${p[k]}`);
          }
          sub.push([p[0], p[1]]);
        }
      }
      this._sub = sub;
      const cfg = { ...DEFAULT_SCREEN };
      if (o.screen !== undefined && o.screen !== null && typeof o.screen === "object") for (const k in DEFAULT_SCREEN) if (o.screen[k] !== undefined) cfg[k] = o.screen[k];
      if (typeof cfg.tabWidth !== "number" || !Number.isInteger(cfg.tabWidth)) throw new TypeError(`The "tabWidth" property must be of type integer. Received ${typeof cfg.tabWidth}`);
      for (const k in cfg) if (k !== "tabWidth") cfg[k] = toInt(cfg[k]);
      this._cfg = cfg;
      this._graphemes = INITIAL.slice();
      this._gmap = new Map(INITIAL.map((g, i) => [g, i]));
      this._sgrKeys = [""]; this._sgrClose = [""]; this._sgrMap = new Map([["", 0]]);
      this._uris = [""]; this._uriMap = new Map([["", 0]]);
      this._seg = null;
      for (const k of Object.keys(this)) Object.defineProperty(this, k, { enumerable: false, writable: true });
    }
    get graphemes() { return this._graphemes; }
    get sgrKeys() { return this._sgrKeys; }
    get sgrCloseKeys() { return this._sgrClose; }
    get uris() { return this._uris; }
    get [Symbol.toStringTag]() { return TAG; }

    _internGrapheme(g) {
      let i = this._gmap.get(g);
      if (i === undefined) { i = this._graphemes.length; this._graphemes.push(g); this._gmap.set(g, i); }
      return i;
    }
    _internSgr(key, close) {
      let idx = this._sgrMap.get(key);
      if (idx === undefined) { idx = this._sgrKeys.length; this._sgrKeys.push(key); this._sgrClose.push(close); this._sgrMap.set(key, idx); }
      return idx;
    }
    _internUri(u) {
      if (u === "") return 0;
      let idx = this._uriMap.get(u);
      if (idx === undefined) { idx = this._uris.length; this._uris.push(u); this._uriMap.set(u, idx); }
      return idx;
    }
    _isSub(cp) { const s = this._sub; for (let i = 0; i < s.length; i++) if (cp >= s[i][0] && cp <= s[i][1]) return true; return false; }

    // Applies one SGR parameter string to the attribute stack `st` (array of {slot, key, close}).
    _applySgr(st, params) {
      const push = (n, key) => {
        for (let i = 0; i < st.length; i++) if (st[i].key === key) return;
        const slot = slotOf(n), c = closeCode(n);
        for (let i = st.length - 1; i >= 0; i--) if (st[i].slot === slot) st.splice(i, 1);
        st.push({ slot, key, close: "\x1b[" + c + "m" });
      };
      if (params === "") { st.length = 0; return; }
      const toks = params.split(";");
      let verbatim = toks.length > MAX_SGR_TOKENS;
      if (!verbatim) for (let i = 0; i < toks.length; i++) { const t = toks[i]; if (t.length !== 0) { const c = t.charCodeAt(0); if (c < 0x30 || c > 0x39) { verbatim = true; break; } } }
      if (verbatim) { push(leadingInt(params), "\x1b[" + params + "m"); return; }
      for (let i = 0; i < toks.length; i++) {
        const t = toks[i];
        if (t === "") { st.length = 0; continue; }
        const colon = t.indexOf(":");
        const n = leadingInt(colon < 0 ? t : t.slice(0, colon));
        if (colon >= 0) { push(n, "\x1b[" + t + "m"); continue; }
        if (n === 0) { st.length = 0; continue; }
        if (n === 38 || n === 48 || n === 58) {
          const m = i + 1 < toks.length && isDigits(toks[i + 1]) ? leadingInt(toks[i + 1]) : -1;
          if (m === 5 && i + 2 < toks.length) { push(n, `\x1b[${n};5;${normNum(toks[i + 2])}m`); i += 2; continue; }
          if (m === 2 && i + 4 < toks.length) { push(n, `\x1b[${n};2;${normNum(toks[i + 2])};${normNum(toks[i + 3])};${normNum(toks[i + 4])}m`); i += 4; continue; }
        }
        if (CLOSERS.has(n)) { const c = "\x1b[" + n + "m"; for (let k = st.length - 1; k >= 0; k--) if (st[k].close === c) st.splice(k, 1); continue; }
        push(n, "\x1b[" + (t.length > 15 ? t : String(n)) + "m");
      }
    }

    segment(line, cells, runs, reordered) {
      if (typeof line !== "string") throw new TypeError("Expected line to be a string for 'segment'.");
      if (!isI32(cells)) throw i32err("cells");
      if (!isI32(runs)) throw i32err("runs");
      const cellCap = cells.length >> 1, runCap = runs.length >> 1;
      const n = line.length;
      // ---- pass 1: strip escapes into text chunks carrying their (sgr, uri) state ----
      const chunks = [], chunkSgr = [], chunkUri = [], chunkKey = [], chunkClose = [];
      const st = []; let curSgr = 0, sgrDirty = false, curKey = "", curClose = "", curUri = 0, i = 0, textStart = 0, ascii = true;
      const flush = (end) => {
        if (end <= textStart) return;
        let s = line.slice(textStart, end);
        if (ascii) for (let k = 0; k < s.length; k++) if (s.charCodeAt(k) >= 0x80) { ascii = false; break; }
        if (sgrDirty) { // keys are interned only when a cell starts under them (see emit)
          sgrDirty = false;
          if (st.length === 0) { curSgr = 0; curKey = ""; }
          else { curSgr = -1; curKey = st[0].key; curClose = st[0].close; for (let k = 1; k < st.length; k++) { curKey += "\0" + st[k].key; curClose += "\0" + st[k].close; } }
        }
        chunks.push(s); chunkSgr.push(curSgr); chunkUri.push(curUri); chunkKey.push(curKey); chunkClose.push(curClose);
      };
      const parseCSI = (j) => { // returns index after the sequence
        let p = j, valid = true;
        while (p < n) {
          const c = line.charCodeAt(p);
          if (c === 0x1b) return p;                       // a new escape restarts parsing
          if (c >= 0x40 && c <= 0x7e) break;              // final byte
          if (!((c >= 0x30 && c <= 0x39) || c === 0x3b || c === 0x3a)) valid = false;
          p++;
        }
        if (p >= n) return n;
        if (valid && line.charCodeAt(p) === 0x6d) { this._applySgr(st, line.slice(j, p)); sgrDirty = true; }
        return p + 1;
      };
      const parseOSC = (j) => {
        let p = j;
        while (p < n) {
          const c = line.charCodeAt(p);
          if (c === 0x07 || c === 0x9c) { oscBody(line.slice(j, p)); return p + 1; }
          if (c === 0x1b) { if (p + 1 < n && line.charCodeAt(p + 1) === 0x5c) { oscBody(line.slice(j, p)); return p + 2; } return p; }
          p++;
        }
        return n;
      };
      const oscBody = (body) => {
        if (body.length < 2 || body.charCodeAt(0) !== 0x38 || body.charCodeAt(1) !== 0x3b) return;
        const k = body.indexOf(";", 2);
        if (k < 0) return;
        curUri = this._internUri(body.slice(k + 1));
      };
      const skipString = (j) => { // DCS/SOS/PM/APC: until ST
        let p = j;
        while (p < n) {
          const c = line.charCodeAt(p);
          if (c === 0x9c) return p + 1;
          if (c === 0x1b) { if (p + 1 < n && line.charCodeAt(p + 1) === 0x5c) return p + 2; return p; }
          p++;
        }
        return n;
      };
      while (i < n) {
        const c = line.charCodeAt(i);
        if (c === 0x1b) {
          flush(i);
          if (i + 1 >= n) { i = n; textStart = n; break; }
          const c1 = line.charCodeAt(i + 1);
          if (c1 === 0x5b) i = parseCSI(i + 2);
          else if (c1 === 0x5d) i = parseOSC(i + 2);
          else if (c1 === 0x50 || c1 === 0x58 || c1 === 0x5e || c1 === 0x5f) i = skipString(i + 2);
          else if (c1 >= 0x20 && c1 <= 0x2f) { let j = i + 2; while (j < n && line.charCodeAt(j) >= 0x20 && line.charCodeAt(j) <= 0x2f) j++; i = Math.min(n, j + 1); }
          else i += 2;
          textStart = i;
        } else if (c === 0x9b) { flush(i); i = parseCSI(i + 1); textStart = i; }
        else if (c === 0x9d) { flush(i); i = parseOSC(i + 1); textStart = i; }
        else if (c === 0x90 || c === 0x98 || c === 0x9e || c === 0x9f) { flush(i); i = skipString(i + 1); textStart = i; }
        else i++;
      }
      flush(n);

      // ---- pass 2: graphemes -> cells ----
      let count = 0, nruns = 0, runSgr = -1, runUri = -1, overflow = false;
      let offsets = null; // clean-text offset of each emitted cell (only for bidi)
      const sgrOf = (k) => { let v = chunkSgr[k]; if (v < 0) { v = this._internSgr(chunkKey[k], chunkClose[k]); for (let j = k; j < chunks.length && chunkKey[j] === chunkKey[k]; j++) chunkSgr[j] = v; } return v; };
      const emit = (gi, low, sgr, uri, off) => {
        if (nruns === 0 || runSgr !== sgr || runUri !== uri) {
          if (!overflow) { if (nruns < runCap) { runs[2 * nruns] = sgr; runs[2 * nruns + 1] = uri; } else overflow = true; }
          runSgr = sgr; runUri = uri; nruns++;
        }
        if (!overflow) {
          if (count < cellCap) { cells[2 * count] = gi; cells[2 * count + 1] = low | ((nruns - 1) << RUN_SHIFT); if (offsets !== null) offsets[count] = off; }
          else overflow = true;
        }
        count++;
      };
      let clean = null;
      if (ascii) {
        let off = 0;
        if (reordered) offsets = new Int32Array(cellCap);
        for (let k = 0; k < chunks.length; k++) {
          const s = chunks[k], uri = chunkUri[k];
          for (let j = 0; j < s.length; j++, off++) {
            const c = s.charCodeAt(j);
            if (c === 0x09) emit(TAB_INDEX, TAB, sgrOf(k), uri, off);
            else if (c < 0x20 || c === 0x7f) continue;
            else emit(c === 0x20 ? 0 : c - 0x21 + 2, 1, sgrOf(k), uri, off);
          }
        }
        if (reordered && !overflow && count > 1) clean = chunks.join("");
      } else {
        // Build the escape-free text; drop unpaired surrogates; apply substitution.
        const bounds = new Int32Array(chunks.length + 1);
        let text = "", subPos = null;
        const hasSub = this._sub.length !== 0;
        for (let k = 0; k < chunks.length; k++) {
          bounds[k] = text.length;
          let s = chunks[k];
          let needScan = hasSub;
          if (!needScan) for (let j = 0; j < s.length; j++) { const c = s.charCodeAt(j); if (c >= 0xd800 && c <= 0xdfff) { needScan = true; break; } }
          if (!needScan) { text += s; continue; }
          for (let j = 0; j < s.length; j++) {
            const c = s.charCodeAt(j);
            if (c >= 0xd800 && c <= 0xdbff) {
              if (j + 1 < s.length) { const d = s.charCodeAt(j + 1); if (d >= 0xdc00 && d <= 0xdfff) { const cp = ((c - 0xd800) << 10) + (d - 0xdc00) + 0x10000; j++; if (hasSub && this._isSub(cp)) { (subPos ??= new Set()).add(text.length); text += "�"; } else text += s[j - 1] + s[j]; } }
              continue; // unpaired high surrogate dropped
            }
            if (c >= 0xdc00 && c <= 0xdfff) continue; // unpaired low surrogate dropped
            if (hasSub && c >= 128 && this._isSub(c)) { (subPos ??= new Set()).add(text.length); text += "�"; } else text += s[j];
          }
        }
        bounds[chunks.length] = text.length;
        if (reordered) offsets = new Int32Array(cellCap);
        const seg = (this._seg ??= new Intl.Segmenter(undefined, { granularity: "grapheme" }));
        const widthOpts = this._widthOpts;
        let k = 0;
        const cluster = (g, index) => {
          while (k + 1 < chunks.length && bounds[k + 1] <= index) k++;
          const c0 = g.charCodeAt(0);
          if (c0 === 0x09) { emit(TAB_INDEX, TAB, sgrOf(k), chunkUri[k], index); return; }
          if (c0 < 0x20 || (c0 >= 0x7f && c0 <= 0x9f)) return;
          let w = c0 < 0x80 && g.length === 1 ? 1 : Bun.stringWidth(g, widthOpts);
          if (w <= 0) return;
          if (w > W_MASK) w = W_MASK;
          const subst = subPos !== null && subPos.has(index);
          const gi = subst ? FFFD_INDEX : c0 < 0x80 && g.length === 1 ? (c0 === 0x20 ? 0 : c0 - 0x21 + 2) : this._internGrapheme(g);
          emit(gi, w | (subst ? SUBST : 0), sgrOf(k), chunkUri[k], index);
        };
        for (const { segment: g, index } of seg.segment(text)) {
          // An emoji modifier (U+1F3FB..U+1F3FF) only joins an Extended_Pictographic base; ICU joins it to anything.
          if (g.length > 2 && g.indexOf("\uD83C") >= 0 && reModifier.test(g)) {
            let start = 0, prev = -1;
            for (let j = 0; j < g.length; ) {
              const c = g.codePointAt(j), len = c > 0xffff ? 2 : 1;
              if (c >= 0x1f3fb && c <= 0x1f3ff && j > start && !(prev >= 0 && reExtPict.test(String.fromCodePoint(prev)))) { cluster(g.slice(start, j), index + start); start = j; }
              prev = c; j += len;
            }
            cluster(g.slice(start), index + start);
            continue;
          }
          cluster(g, index);
        }
        if (reordered && !overflow && count > 1) clean = text;
      }
      if (overflow) return -count;
      if (clean !== null) this._reorder(clean, cells, offsets, count);
      return count;
    }

    // Visual reordering (UAX#9 L2) of the emitted cells, by the level of each cell's first code point.
    _reorder(text, cells, offsets, count) {
      const cps = [], cpOff = [];
      for (let i = 0; i < text.length; i++) {
        const c = text.charCodeAt(i); let cp = c;
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.length) { cp = ((c - 0xd800) << 10) + (text.charCodeAt(i + 1) - 0xdc00) + 0x10000; cpOff.push(i); cps.push(cp); i++; continue; }
        cpOff.push(i); cps.push(cp);
      }
      const lv = bidiLevels(cps, cps.length);
      const levels = new Uint8Array(count);
      let j = 0, maxL = 0, minOdd = 255;
      for (let i = 0; i < count; i++) {
        while (j + 1 < cpOff.length && cpOff[j + 1] <= offsets[i]) j++;
        const l = lv[j] ?? 0; levels[i] = l;
        if (l > maxL) maxL = l; if ((l & 1) && l < minOdd) minOdd = l;
      }
      if (maxL === 0 || minOdd === 255) return;
      const idx = new Int32Array(count); for (let i = 0; i < count; i++) idx[i] = i;
      for (let l = maxL; l >= minOdd; l--) {
        for (let i = 0; i < count; i++) {
          if (levels[idx[i]] < l) continue;
          let e = i; while (e < count && levels[idx[e]] >= l) e++;
          for (let a = i, b = e - 1; a < b; a++, b--) { const t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
          i = e;
        }
      }
      const copy = cells.slice(0, 2 * count);
      for (let i = 0; i < count; i++) { cells[2 * i] = copy[2 * idx[i]]; cells[2 * i + 1] = copy[2 * idx[i] + 1]; }
    }

    _packed(x, dmin, dmax) {
      const lo = x < 0 ? 0 : x > 1048575 ? 1048575 : x;
      if (dmax < 0) return lo + 65535 * 1048576;
      return lo + dmin * 1048576 + dmax * 68719476736;
    }

    paint(screen, width, x, y, cells, count, order, charMap, words) {
      if (!isI32(screen)) throw i32err("screen");
      width = checkInt(width, "screenWidth");
      if (!isI32(cells)) throw i32err("cells");
      count = checkInt(count, "count");
      if (count > (cells.length >> 1)) throw new Error("paint: count exceeds cells");
      if (order !== undefined && order !== null) { if (!isI32(order)) throw i32err("order"); if (order.length < count) throw new Error("paint: count exceeds order"); } else order = null;
      if (!isI32(charMap)) throw i32err("charMap");
      if (!isI32(words)) throw i32err("runWords");
      x = toInt(x); y = toInt(y);
      const cfg = this._cfg, mask = cfg.widthMask, narrow = cfg.narrow, wide = cfg.wide, tail = cfg.spacerTail, head = cfg.spacerHead;
      const emptyChar = cfg.emptyCharIndex, spacerChar = cfg.spacerCharIndex, emptyWord = cfg.emptyWord, tw = cfg.tabWidth;
      const height = width > 0 ? Math.floor(screen.length / (2 * width)) : 0;
      const rowOk = y >= 0 && y < height;
      const base = y * width * 2;
      let dmin = 0x7fffffff, dmax = -1;
      const clear = (cx) => { screen[base + 2 * cx] = emptyChar; screen[base + 2 * cx + 1] = emptyWord; if (cx < dmin) dmin = cx; if (cx + 1 > dmax) dmax = cx + 1; };
      const put = (cx, ch, word) => {
        const o = base + 2 * cx, old = screen[o + 1] & mask;
        if (old === wide) { if (cx + 1 < width && (screen[o + 3] & mask) === tail) clear(cx + 1); }
        else if (old === tail) { if (cx > 0 && (screen[o - 1] & mask) === wide) clear(cx - 1); }
        screen[o] = ch; screen[o + 1] = word;
        if (cx < dmin) dmin = cx; if (cx + 1 > dmax) dmax = cx + 1;
      };
      for (let i = 0; i < count; i++) {
        const ci = order === null ? i : order[i];
        const gi = cells[2 * ci], p = cells[2 * ci + 1];
        if (p & TAB) {
          if (x < width) {
            let nx = tw > 1 ? x + (tw - (x % tw)) : x + 1;
            if (nx > width) nx = width;
            if (rowOk) for (let cx = x < 0 ? 0 : x; cx < nx; cx++) put(cx, emptyChar, emptyWord | narrow);
            x = nx;
          }
          continue;
        }
        const w = p & W_MASK;
        const word = (words[p >>> RUN_SHIFT] | 0) & ~mask;
        const ch = charMap[gi] | 0;
        if (w >= 2) {
          if (x < 0) { x += w; continue; }
          if (x + w <= width) {
            if (rowOk) { put(x, ch, word | wide); put(x + 1, spacerChar, emptyWord | tail); for (let k = 2; k < w; k++) put(x + k, spacerChar, word | tail); }
            x += w;
          } else if (x < width) { if (rowOk) put(x, emptyChar, emptyWord | head); x += 1; }
          else x += 1;
        } else {
          if (rowOk && x >= 0 && x < width) put(x, ch, word | narrow);
          x += w;
        }
      }
      return this._packed(x, dmin, dmax);
    }

    setCell(screen, width, x, y, charIndex, word) {
      if (!isI32(screen)) throw i32err("screen");
      width = width === undefined ? 0 : checkInt(width, "screenWidth");
      x = toInt(x); y = toInt(y); charIndex = toInt(charIndex); word = toInt(word);
      const cfg = this._cfg, mask = cfg.widthMask, wide = cfg.wide, tail = cfg.spacerTail;
      const height = width > 0 ? Math.floor(screen.length / (2 * width)) : 0;
      if (x < 0 || x >= width || y < 0 || y >= height) return this._packed(x + 1, 0, -1);
      const base = y * width * 2;
      let dmin = 0x7fffffff, dmax = -1;
      const clear = (cx) => { screen[base + 2 * cx] = cfg.emptyCharIndex; screen[base + 2 * cx + 1] = cfg.emptyWord; if (cx < dmin) dmin = cx; if (cx + 1 > dmax) dmax = cx + 1; };
      const put = (cx, ch, w) => {
        const o = base + 2 * cx, old = screen[o + 1] & mask;
        if (old === wide) { if (cx + 1 < width && (screen[o + 3] & mask) === tail) clear(cx + 1); }
        else if (old === tail) { if (cx > 0 && (screen[o - 1] & mask) === wide) clear(cx - 1); }
        screen[o] = ch; screen[o + 1] = w;
        if (cx < dmin) dmin = cx; if (cx + 1 > dmax) dmax = cx + 1;
      };
      put(x, charIndex, word);
      if ((word & mask) === wide && x + 1 < width) put(x + 1, cfg.spacerCharIndex, cfg.emptyWord | tail);
      return this._packed(x + 1, dmin, dmax);
    }
  }

  Object.defineProperty(CellSegmenter, "length", { value: 0, configurable: true });
  const ant = Bun.ant && typeof Bun.ant === "object" ? Bun.ant : {};
  Object.defineProperty(ant, "CellSegmenter", { value: CellSegmenter, writable: true, enumerable: true, configurable: true });
  if (Bun.ant !== ant) Object.defineProperty(Bun, "ant", { value: ant, writable: true, enumerable: true, configurable: true });
})();
