// Unit test for the `zeli dev` request handler and the two pure pieces the
// change-driven reload is built from: the source filter the watcher applies, and
// the SSE frame the server pushes. No port is bound — the routing, content
// types, HTML injection, and SPA fallback are exercised through `devReply`,
// which is where the live server delegates them.

import { devReply, isWatchedSource, LIVE_PATH, liveSnippet, sseFrame, watchSources } from "./zeli.ts";

const dec = new TextDecoder();
let failures = 0;
function must(ok: boolean, what: string): void {
  if (!ok) {
    console.error("FAIL", what);
    failures++;
  }
}

const root = "packages/loam/tests/tmp/devweb";
try {
  Deno.removeSync(root, { recursive: true });
} catch { /* absent */ }
Deno.mkdirSync(root + "/blog", { recursive: true });
Deno.writeTextFileSync(root + "/index.html", "<html><body><canvas id=\"zeus\"></canvas></body></html>");
Deno.writeTextFileSync(root + "/blog/index.html", "<html><body><h1>blog</h1></body></html>");
Deno.writeFileSync(root + "/app.wasm", new Uint8Array([0x00, 0x61, 0x73, 0x6d]));
Deno.writeTextFileSync(root + "/loader.js", "// loader");

const SNIP = "<!--LIVE-->";

let r = devReply(root, "/", SNIP);
must(r.status === 200 && r.contentType.startsWith("text/html"), "root is html");
must(dec.decode(r.body).includes("<!--LIVE-->"), "root gets the reload snippet");

r = devReply(root, "/blog", SNIP);
must(dec.decode(r.body).includes("<h1>blog</h1>"), "nested dir serves its index");
r = devReply(root, "/blog/", SNIP);
must(dec.decode(r.body).includes("<h1>blog</h1>"), "trailing slash serves its index");

r = devReply(root, "/app.wasm", SNIP);
must(r.contentType === "application/wasm", "wasm content type");
must(r.body[0] === 0x00 && r.body[1] === 0x61, "wasm bytes");

r = devReply(root, "/loader.js", SNIP);
must(r.contentType === "text/javascript", "js content type");

r = devReply(root, "/does-not-exist", SNIP);
must(r.status === 200 && dec.decode(r.body).includes("<!--LIVE-->"), "unknown falls back to the shell");

// What a rebuild reacts to. The build output lives inside a watched root, so the
// filter is the only thing standing between an edit and a rebuild loop.
must(isWatchedSource("examples/zeus/myapp/app.loam"), "app source is watched");
must(isWatchedSource("examples/zeus/myapp/routes/blog/page.loam"), "a route is watched");
must(isWatchedSource("examples/zeus/myapp/routes/pricing/page.loam"), "a new route is watched");
must(isWatchedSource("packages/zeus/std/zeus.loam"), "framework source is watched");
must(!isWatchedSource("examples/zeus/myapp/build/web/app.wasm"), "wasm output is not");
must(!isWatchedSource("examples/zeus/myapp/build/web/index.html"), "shell output is not");
must(!isWatchedSource("examples/zeus/myapp/build/web/x.loam"), "nothing under build/ is");
must(!isWatchedSource("examples/zeus/myapp/.zeus/meta_dump.loam"), "the cache dir is not");
must(!isWatchedSource("examples/zeus/myapp/.git/x.loam"), "the git dir is not");
must(!isWatchedSource("packages/zeus/node_modules/p/x.loam"), "node_modules is not");
must(!isWatchedSource("examples/zeus/myapp/app_routes.loam.bak"), "a non-.loam file is not");
// Relative to the root, so a checkout under a dotted directory still watches.
must(
  isWatchedSource("/home/u/.src/loam/app/routes/page.loam", ["/home/u/.src/loam/app"]),
  "a dotted checkout path is still watched",
);
must(
  !isWatchedSource("/home/u/.src/loam/app/build/web/x.loam", ["/home/u/.src/loam/app"]),
  "...but its build output is not",
);

// The push channel: a frame per rebuild, and a snippet that waits for one
// instead of asking for it.
must(sseFrame(7) === "data: 7\n\n", "sse frame carries the revision");
const snip = liveSnippet();
must(snip.includes(LIVE_PATH), "snippet subscribes to the live path");
must(snip.includes("EventSource"), "snippet uses SSE");
must(!snip.includes("setInterval"), "snippet polls nothing");
must(!snip.includes("fetch("), "snippet makes no request of its own");

// The watcher itself, on a real directory: an edit wakes it, and the build
// output it writes into does not — the loop that would otherwise never settle.
const wroot = "packages/loam/tests/tmp/devwatch";
try {
  Deno.removeSync(wroot, { recursive: true });
} catch { /* absent */ }
Deno.mkdirSync(wroot + "/build", { recursive: true });
Deno.mkdirSync(wroot + "/.zeus", { recursive: true });
const settle = () => new Promise((r) => setTimeout(r, 500));

const seen: string[][] = [];
const watcher = watchSources([wroot], 30, (paths) => seen.push(paths));
await new Promise((r) => setTimeout(r, 200)); // let it arm before the edit

Deno.writeTextFileSync(wroot + "/app.loam", "// an edit\n");
await settle();
must(seen.length >= 1, "an edit wakes the watcher (no events at all? a sandbox can block fs events)");
must(seen.flat().some((p) => p.endsWith("app.loam")), "the batch names the edited file");

const afterEdit = seen.length;
Deno.writeTextFileSync(wroot + "/build/app.wasm", "not a source");
Deno.writeTextFileSync(wroot + "/.zeus/meta_dump.loam", "cache");
await settle();
must(seen.length === afterEdit, "build output and the cache dir do not wake it");

watcher.close();

if (failures) Deno.exit(1);
console.log("dev handler ok");
