// Preloaded into compiled Bun programs, Claude Code among them, which execve
// runs on the app's Bun with this in BUN_OPTIONS (tvm/tvm.c, "Bun
// programs"): on iOS their JavaScriptCore runs without its JIT. With the
// JIT off, JavaScriptCore on Apple systems also turns off SharedArrayBuffer
// (Options::notifyOptionsChanged: the JIT being off stands in for "the
// sandbox blocks mach exception handlers"), and Claude Code dies at start
// on `new SharedArrayBuffer(4)`.
//
// This gives it a SharedArrayBuffer that is an ArrayBuffer: memory of the
// thread that made it, not shared with workers (postMessage copies it).
// Atomics.wait on it, the synchronous sleep programs use it for, sleeps; a
// wait with no timeout could only be ended by another thread, so it throws.
// Bun runs the preload in the main thread only: workers get it as theirs.
if (typeof SharedArrayBuffer === "undefined") {
  class SharedArrayBuffer extends ArrayBuffer {
    get [Symbol.toStringTag]() { return "SharedArrayBuffer"; }
  }
  Object.defineProperty(globalThis, "SharedArrayBuffer", {
    value: SharedArrayBuffer, writable: true, configurable: true, enumerable: false,
  });

  const unshared = (ta) => ArrayBuffer.isView(ta) && ta.buffer instanceof SharedArrayBuffer;
  const wait = Atomics.wait, waitAsync = Atomics.waitAsync, notify = Atomics.notify;
  const result = (ta, index, value, timeout) => {
    if (Atomics.load(ta, index) !== value) return "not-equal";
    timeout = Number(timeout);
    if (!(timeout < Infinity))
      throw new TypeError("Atomics.wait with no timeout on an unshared SharedArrayBuffer (tvm-sab.js) would never end");
    return timeout;
  };
  Atomics.wait = function (ta, index, value, timeout) {
    if (!unshared(ta)) return wait.apply(Atomics, arguments);
    const ms = result(ta, index, value, timeout);
    if (typeof ms === "string") return ms;
    if (ms > 0) Bun.sleepSync(ms);
    return "timed-out";
  };
  if (waitAsync)
    Atomics.waitAsync = function (ta, index, value, timeout) {
      if (!unshared(ta)) return waitAsync.apply(Atomics, arguments);
      const ms = result(ta, index, value, timeout);
      if (typeof ms === "string") return { async: false, value: ms };
      return { async: true, value: Bun.sleep(Math.max(ms, 0)).then(() => "timed-out") };
    };
  Atomics.notify = function (ta) {
    return unshared(ta) ? 0 : notify.apply(Atomics, arguments);
  };

  const self = import.meta.path;
  const BunWorker = globalThis.Worker;
  if (BunWorker)
    globalThis.Worker = class Worker extends BunWorker {
      constructor(url, options) {
        const preload = [].concat(options?.preload ?? []);
        super(url, { ...options, preload: [self, ...preload] });
      }
    };
}
