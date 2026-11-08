// Run a plain (non-Zeus) Loam wasm module's `main` and print what it wrote.
// Every import other than `zeus.write` is a no-op. Used by `nob`'s wasm_float.
const mod = await WebAssembly.compile(await Deno.readFile(Deno.args[0]));
let mem: WebAssembly.Memory;
let out = "";
const imports: Record<string, Record<string, unknown>> = {};
for (const imp of WebAssembly.Module.imports(mod)) {
  if (imp.kind !== "function") continue;
  const ns = (imports[imp.module] ??= {});
  ns[imp.name] = imp.module === "zeus" && imp.name === "write"
    ? (p: number, n: number) => {
      out += new TextDecoder().decode(new Uint8Array(mem.buffer, p, n));
    }
    : () => 0;
}
const inst = await WebAssembly.instantiate(mod, imports);
mem = inst.exports.memory as WebAssembly.Memory;
(inst.exports.main as CallableFunction)(0, 0);
await Deno.stdout.write(new TextEncoder().encode(out));
