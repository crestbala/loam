// Headless smoke test for the web wasm build: instantiate it with stubbed host
// imports (no browser, no network) and exercise the host entry points the
// loader uses — accessibility dump, open_url, and the hot-reload state
// snapshot/restore. This runs the real wasm, so it catches link/trap/buffer
// regressions a JS-only check cannot.

let mem: WebAssembly.Memory | null = null;

const stub = (): number => 0;

const dec = new TextDecoder();
const enc = new TextEncoder();

const special: Record<string, (...a: number[]) => unknown> = {
  now_ms: () => 0n,
  view_w: () => 400,
  view_h: () => 300,
  fetch_rpc_async: () => -1,
  fetch_rpc: () => 0,
  __multi3: () => 0n,
  write: (p: number, n: number) => {
    if (mem && p > 0) {
      Deno.stderr.writeSync(enc.encode("[wasm] " + dec.decode(new Uint8Array(mem.buffer, p, n)) + "\n"));
    }
  },
  measure: (s: number, px: number, wp: number, hp: number) => {
    if (mem && wp) new DataView(mem.buffer).setInt32(wp, 10, true);
    if (mem && hp) new DataView(mem.buffer).setInt32(hp, 10, true);
    void s;
    void px;
  },
  pick_image: () => 0,
};

function importsFor(): Record<string, unknown> {
  return new Proxy({}, {
    get: (_t, name: string) => (name in special ? special[name] : stub),
  });
}

function cstr(ptr: number): string {
  if (!mem || !ptr) return "";
  const u8 = new Uint8Array(mem.buffer);
  let end = ptr;
  while (end < u8.length && u8[end] !== 0) end++;
  return dec.decode(u8.subarray(ptr, end));
}

let failures = 0;
function must(ok: boolean, what: string): void {
  if (!ok) {
    console.error("FAIL", what);
    failures++;
  }
}

const wasmPath = Deno.args[0] ?? "packages/loam/tests/routes_app/build/web/app.wasm";
const bytes = Deno.readFileSync(wasmPath);
const { instance } = await WebAssembly.instantiate(bytes, {
  env: importsFor(),
  zeus: importsFor(),
});
const exp = instance.exports as Record<string, CallableFunction>;
must(typeof exp.zeus_start === "function", "module exports zeus_start");
mem = (exp.memory as WebAssembly.Memory) ?? null;
must(mem !== null, "module exports memory");

exp.zeus_start();

// Accessibility dump: the fixture builds a tree, so the dump is non-empty.
const a11yPtr = exp.zeus_a11y_sync() as number;
const a11y = cstr(a11yPtr);
must(a11y.length > 0, "a11y dump is non-empty");

// Deep link: routes the app; must not trap.
exp.zeus_open_url(0);
exp.zeus_open_url(a11yPtr); // any valid C string stands in for a path

// Hot reload: snapshot, mutate, restore.
const snapPtr = exp.zeus_state_snapshot() as number;
const snap = cstr(snapPtr);
must(snap.length > 0, "state snapshot is non-empty");
const applied = exp.zeus_state_restore(snapPtr) as number;
must(applied > 0, "state restore applies slots");

// Steady state must not leak: the loader paints and syncs the a11y mirror
// every frame, and runtime strings are never freed, so any per-frame string
// shows up here as monotonic heap growth (the dump used to cost ~16 KB a
// frame). Warm up, then hold the live heap flat over 600 frames with the
// clock advancing so timers fire.
let clock = 0;
special.now_ms = () => BigInt(clock);
const heapKb = () => (exp.zeus_heap_kb ? (exp.zeus_heap_kb() as number) : 0);
for (let f = 0; f < 120; f++) {
  clock += 16;
  exp.zeus_paint();
  exp.zeus_a11y_sync();
}
const warm = heapKb();
for (let f = 0; f < 600; f++) {
  clock += 16;
  exp.zeus_paint();
  exp.zeus_a11y_sync();
}
const grown = heapKb() - warm;
must(grown <= 8, "live heap flat over 600 frames (grew " + grown + " KB)");

if (failures) Deno.exit(1);
console.log("wasm smoke ok");
