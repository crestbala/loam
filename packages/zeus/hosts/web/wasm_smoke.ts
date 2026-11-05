// Headless smoke test for the web wasm build: instantiate it with stubbed host
// imports (no browser, no network) and exercise the host entry points the
// loader uses — accessibility dump, open_url, and the hot-reload state
// snapshot/restore. This runs the real wasm, so it catches link/trap/buffer
// regressions a JS-only check cannot.

let mem: WebAssembly.Memory | null = null;

// The real frame decoder from loader.js (a plain script: evaluated globally,
// it defines `zeusDecodeFrame` and skips its DOM auto-boot). Every frame the
// wasm paints is decoded into a recorder, so a format drift between the
// engine and the web host fails here.
(0, eval)(Deno.readTextFileSync(new URL("./loader.js", import.meta.url)));
{
  type Sample = { t: number; x: number; y: number };
  const fling = (globalThis as Record<string, unknown>).zeusFling as {
    velocity(samples: Sample[], now: number): { x: number; y: number };
    step(v: number, dt: number): { d: number; v: number };
  };
  const swipe = [
    { t: 0, x: 0, y: 500 },
    { t: 40, x: 0, y: 460 },
    { t: 80, x: 0, y: 420 },
  ];
  // The fingertip moves up the glass, so the content scrolls down: positive.
  // 1 px/ms measured, released at the fling's 1.35 gain.
  if (fling.velocity(swipe, 90).y !== 1.35) throw new Error("fling: velocity of a swipe");
  if (fling.velocity(swipe, 400).y !== 0) throw new Error("fling: a rested finger has no velocity");
  if (fling.velocity(swipe.slice(0, 1), 5).y !== 0) throw new Error("fling: one sample has no velocity");
  const whole = fling.step(1, 32);
  const half = fling.step(1, 16);
  const rest = fling.step(half.v, 16);
  if (Math.abs(whole.d - (half.d + rest.d)) > 1e-9) throw new Error("fling: distance depends on frame rate");
}

{
  // The analytics host, on a fake browser: a clock to move and a tab to hide.
  const store = new Map<string, string>();
  const posts: { vid: string; events: Record<string, unknown>[] }[] = [];
  let clock = 1_000_000;
  let next = 0;
  let visible = true;
  let blocked = false;
  let show = () => {};
  let hide = () => {};
  const env = {
    now: () => clock,
    id: () => `id${++next}`,
    get: (k: string) => store.get(k) ?? null,
    set: (k: string, v: string) => void store.set(k, v),
    remove: (k: string) => void store.delete(k),
    after: () => 1,
    cancel: () => {},
    post: (_url: string, body: string) => void posts.push(JSON.parse(body)),
    visible: () => visible,
    width: () => 1200,
    referrer: () => "news.example",
    blocked: () => blocked,
    watch: (s: () => void, h: () => void) => {
      show = s;
      hide = h;
    },
  };
  type Tracker = {
    start(url: string, local: boolean): void;
    send(kind: number, name: string, data: string): void;
    optOut(off: boolean): void;
    optedOut(): boolean;
  };
  const make = (globalThis as Record<string, unknown>).zeusTracker as (e: typeof env) => Tracker;
  const check = (ok: boolean, what: string) => {
    if (!ok) throw new Error(`tracker: ${what}`);
  };
  const events = () => posts.flatMap((p) => p.events);
  const hideTab = () => {
    visible = false;
    hide();
  };
  const showTab = () => {
    visible = true;
    show();
  };

  const t = make(env);
  t.start("/api/track", false);
  t.send(0, "/", "");
  clock += 3000;
  t.send(1, "Run", "");
  t.send(0, "/language", "");
  clock += 2000;
  hideTab();
  clock += 60_000;
  showTab();
  clock += 500;
  hideTab();
  const e = events();
  check(e.map((x) => x.k).join() === "page,click,leave,page,leave,leave", `order ${e.map((x) => x.k)}`);
  check(e[0].ref === "news.example", "first page keeps the referrer");
  check(!("ref" in e[3]), "a later page has no referrer");
  check(e[1].path === "/" && e[1].name === "Run", "a click names its control and page");
  check(e[2].ms === 3000 && e[2].path === "/", "time on a page");
  check(e[3].pv !== e[0].pv, "each view has its own id");
  check(e[4].ms === 2000 && e[5].ms === 500, "hidden time does not count");
  check(new Set(e.map((x) => x.sid)).size === 1, "one session");

  // Thirty idle minutes: the same visitor, a new visit, counted as a page.
  const vid = posts[0].vid;
  clock += 31 * 60 * 1000;
  showTab();
  hideTab();
  const later = events().filter((x) => x.sid !== e[0].sid);
  check(later[0].k === "page" && later[0].path === "/language", "a return opens a new session");
  check(later[0].ref === "news.example", "a new session keeps its referrer");
  check(posts.at(-1)!.vid === vid, "the visitor survives the session");

  // Twenty events go out without waiting for the timer.
  posts.length = 0;
  showTab();
  for (let i = 0; i < 20; i++) t.send(2, "e", "");
  check(posts.length === 1 && posts[0].events.length === 20, "a full batch flushes");

  // Opted out: nothing is sent and the ids are gone.
  t.optOut(true);
  check(t.optedOut() && !store.has("zeus.track.id"), "opting out forgets the visitor");
  posts.length = 0;
  t.send(0, "/x", "");
  t.send(1, "y", "");
  hideTab();
  check(posts.length === 0, "an opted out browser sends nothing");
  t.optOut(false);
  check(!t.optedOut(), "opting back in");

  // A blocked browser (Global Privacy Control, a bot) never starts.
  blocked = true;
  const b = make(env);
  b.start("/api/track", false);
  b.send(0, "/", "");
  check(!store.has("zeus.track.id") && posts.length === 0, "a blocked browser sends nothing");
}

const decodeFrame = (globalThis as Record<string, unknown>).zeusDecodeFrame as (
  buf: ArrayBuffer,
  head: number,
  paint: Record<string, (...a: unknown[]) => void>,
) => number;
const painted: Record<string, number> = {};
let framesSeen = 0;
let frameErrors = 0;
const recorder = new Proxy({}, {
  get: (_t, name: string) => (...a: unknown[]) => {
    painted[name] = (painted[name] ?? 0) + 1;
    for (const v of a) {
      if (typeof v === "number" && !Number.isFinite(v)) frameErrors++;
    }
    if ((name === "text" || name === "text_rot") && typeof a[2] !== "string") frameErrors++;
  },
}) as Record<string, (...a: unknown[]) => void>;

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
  frame: (head: number) => {
    if (!mem) return;
    framesSeen++;
    if (decodeFrame(mem.buffer, head, recorder) < 0) frameErrors++;
  },
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
must(a11y.split("\n")[0].split("\t").length === 9, "a11y lines carry id and state columns");
must(typeof exp.zeus_a11y_activate === "function", "module exports zeus_a11y_activate");
must(typeof exp.zeus_a11y_focus === "function", "module exports zeus_a11y_focus");
must(typeof exp.zeus_focus_id === "function", "module exports zeus_focus_id");

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
  exp.zeus_a11y_sync(1);
}
const warm = heapKb();
for (let f = 0; f < 600; f++) {
  clock += 16;
  exp.zeus_paint();
  exp.zeus_a11y_sync(1); // forced: the loader's periodic re-dump, every frame here
}
const grown = heapKb() - warm;
must(grown <= 8, "live heap flat over 600 frames (grew " + grown + " KB)");

must(framesSeen > 0, "the wasm handed frames to the JS decoder");
must(frameErrors === 0, "every frame decoded (" + frameErrors + " bad ops)");
must((painted.text ?? 0) > 0, "decoded frames draw text");

if (failures) Deno.exit(1);
console.log("wasm smoke ok");
