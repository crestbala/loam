// Headless test of trap recovery on wasm, where `Boundary` has no setjmp. It
// mirrors the loader's `guardExports`: a RuntimeError out of an export resets
// the shadow stack and calls `zeus_recover`. A trap during startup (2) restarts
// from a fresh instance with the failed boundary pre-seeded; a trap in a click
// handler (1) shows the boundary's fallback on the next frame.

const dec = new TextDecoder();
const enc = new TextEncoder();
let mem: WebAssembly.Memory | null = null;
const stub = (): number => 0;
const special: Record<string, (...a: number[]) => unknown> = {
  now_ms: () => 0n,
  view_w: () => 320,
  view_h: () => 240,
  fetch_rpc_async: () => -1,
  fetch_rpc: () => 0,
  __multi3: () => 0n,
  write: () => {},
  measure: (_s: number, _px: number, wp: number, hp: number) => {
    if (mem && wp) new DataView(mem.buffer).setInt32(wp, 10, true);
    if (mem && hp) new DataView(mem.buffer).setInt32(hp, 10, true);
  },
  pick_image: () => 0,
  // The loader's `panic` import throws (`abort` calls it); mirror that.
  panic: () => {
    throw new WebAssembly.RuntimeError("zeus panic");
  },
};
const imports = () => new Proxy({}, { get: (_t, n: string) => (n in special ? special[n] : stub) });

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

type Exp = Record<string, CallableFunction> & { __stack_pointer?: WebAssembly.Global };

const presets: { seq: number; msg: string }[] = [];

/** Call `f` the way the loader does: recover on a trap. Returns the recovery code (-1 = no trap). */
function guarded(exp: Exp, spBase: number, f: () => unknown): number {
  try {
    f();
    return -1;
  } catch (e) {
    if (!(e instanceof WebAssembly.RuntimeError)) throw e;
    if (exp.__stack_pointer) exp.__stack_pointer.value = spBase;
    const r = exp.zeus_recover() as number;
    if (r === 2) presets.push({ seq: exp.zeus_trap_seq() as number, msg: cstr(exp.zeus_trap_msg() as number) });
    return r;
  }
}

const bytes = Deno.readFileSync(Deno.args[0]);
const module = await WebAssembly.compile(bytes);

async function boot(): Promise<{ exp: Exp; sp: number; code: number }> {
  const inst = await WebAssembly.instantiate(module, { env: imports(), zeus: imports() });
  const exp = inst.exports as unknown as Exp;
  mem = exp.memory as unknown as WebAssembly.Memory;
  must(exp.__stack_pointer !== undefined, "module exports __stack_pointer");
  const sp = exp.__stack_pointer ? (exp.__stack_pointer.value as number) : 0;
  for (const p of presets) {
    const ptr = exp.zeus_text_buf() as number;
    const b = enc.encode(p.msg);
    new Uint8Array(mem.buffer, ptr, b.length).set(b);
    new Uint8Array(mem.buffer, ptr + b.length, 1)[0] = 0;
    exp.zeus_boundary_preset(p.seq, ptr);
  }
  const code = guarded(exp, sp, () => exp.zeus_start());
  return { exp, sp, code };
}

const a11y = (exp: Exp) => cstr(exp.zeus_a11y_sync(1) as number);

// 1. Startup trap: the first boot reports a restart; the second builds the
//    fallback in place of the failed boundary and the rest of the app.
let b = await boot();
must(b.code === 2, "startup trap asks for a restart (got " + b.code + ")");
must(presets.length === 1 && presets[0].seq === 1, "the first boundary is the one that failed");
must(presets.length === 1 && presets[0].msg.indexOf("index out of bounds") >= 0, "the panic message is carried over");
b = await boot();
must(b.code === -1, "restarted instance boots without a trap");
guarded(b.exp, b.sp, () => b.exp.zeus_paint());
let tree = a11y(b.exp);
must(tree.indexOf("fallback startup") >= 0, "startup fallback shown");
must(tree.indexOf("live handler") >= 0, "second boundary built");
must(tree.indexOf("sibling") >= 0, "sibling built");

// 2. Handler trap: activate the button; the trap is recovered in place.
const line = tree.split("\n").find((l) => l.indexOf("explode") >= 0) ?? "";
// depth, role, label, x, y, w, h, id, state
const id = Number(line.split("\t")[7]);
must(id > 0, "found the button in the a11y tree (" + line + ")");
const code = guarded(b.exp, b.sp, () => b.exp.zeus_a11y_activate(id));
must(code === 1, "handler trap recovered in place (got " + code + ")");
for (let f = 0; f < 3; f++) guarded(b.exp, b.sp, () => b.exp.zeus_paint());
tree = a11y(b.exp);
must(tree.indexOf("fallback handler") >= 0, "handler fallback shown");
must(tree.indexOf("live handler") < 0, "trapped subtree removed");
must(tree.indexOf("sibling") >= 0, "sibling survives the handler trap");
must(b.exp.__stack_pointer === undefined || b.exp.__stack_pointer.value === b.sp, "shadow stack restored");

if (failures) Deno.exit(1);
console.log("wasm trap ok");
