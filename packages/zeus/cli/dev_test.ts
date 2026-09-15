// Unit test for the `zeus dev` request handler. No port is bound — this
// exercises the routing, content types, HTML injection, and SPA fallback that
// the live server delegates to `devReply`.

import { devReply } from "./zeus.ts";

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

let r = devReply(root, 3, "/.zeus-rev", SNIP);
must(r.status === 200 && dec.decode(r.body) === "3", "revision endpoint");

r = devReply(root, 3, "/", SNIP);
must(r.status === 200 && r.contentType.startsWith("text/html"), "root is html");
must(dec.decode(r.body).includes("<!--LIVE-->"), "root gets the reload snippet");

r = devReply(root, 3, "/blog", SNIP);
must(dec.decode(r.body).includes("<h1>blog</h1>"), "nested dir serves its index");
r = devReply(root, 3, "/blog/", SNIP);
must(dec.decode(r.body).includes("<h1>blog</h1>"), "trailing slash serves its index");

r = devReply(root, 3, "/app.wasm", SNIP);
must(r.contentType === "application/wasm", "wasm content type");
must(r.body[0] === 0x00 && r.body[1] === 0x61, "wasm bytes");

r = devReply(root, 3, "/loader.js", SNIP);
must(r.contentType === "text/javascript", "js content type");

r = devReply(root, 3, "/does-not-exist", SNIP);
must(r.status === 200 && dec.decode(r.body).includes("<!--LIVE-->"), "unknown falls back to the shell");

if (failures) Deno.exit(1);
console.log("dev handler ok");
