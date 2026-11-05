/** Minify a browser script in place, with the esbuild that Vite ships.
 *
 *   node packages/zeus/hosts/web/minify.mjs build/web/loader.js
 *
 * Local names are shortened and property names are not, so the exports and
 * imports a wasm module shares with the loader keep theirs. `zeli` runs this on
 * release builds. On a failure the file is left as it was.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { transformWithEsbuild } from "vite";

const file = process.argv[2];
const out = await transformWithEsbuild(readFileSync(file, "utf8"), file, {
  loader: "js",
  minify: true,
  target: "esnext",
  sourcemap: false,
  legalComments: "none",
});
writeFileSync(file, out.code);
