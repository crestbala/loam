/** Vidget wasm dev server.
 *
 *  - compiles `app.loam` to `build/app.wasm` on start and on any `.loam`
 *    change (the shared UI, the store, the API contract, or the std libs),
 *    then reloads the page;
 *  - serves the Zeus Canvas2D `loader.js`;
 *  - proxies gRPC-Web `POST /Vidget/*` to the native engine
 *    (`../server/main.loam`, :8080 or `LOAM_RPC_ADDR`). The engine must run
 *    natively: a browser cannot spawn aria2c or ffmpeg, and CORS would stop
 *    it fetching most video hosts anyway.
 *
 *    zeli serve examples/zeus/vidget --target web --port 5176
 *    (zeli runs this config when web/vite.config.js exists, after starting the engine)
 */
import { spawnSync } from "node:child_process";
import { createReadStream, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const here = dirname(fileURLToPath(import.meta.url));
const appDir = resolve(here, "..");
const repo = resolve(here, "../../../..");
const loam = resolve(repo, "bin/loamc");
const buildDir = resolve(here, "build");
const loader = resolve(repo, "packages/zeus/hosts/web/loader.js");
const app = resolve(here, "app.loam");
const rpc = process.env.LOAM_RPC_ADDR || "http://127.0.0.1:8080";
const port = Number(process.env.VIDGET_WEB_PORT || 5176);

function compileWasm() {
  if (!existsSync(loam)) {
    throw new Error("missing " + loam + " — run `make` in the loam repo first");
  }
  const r = spawnSync(loam, ["build", "--target=wasm32", app], { cwd: repo, encoding: "utf8" });
  if (r.stdout) process.stdout.write(r.stdout);
  if (r.stderr) process.stderr.write(r.stderr);
  if (r.status !== 0) {
    throw new Error(r.stderr?.trim() || r.stdout?.trim() || "loam failed");
  }
}

function rebuild(reason) {
  console.log("[loam] " + reason + ": compiling wasm");
  rmSync(buildDir, { recursive: true, force: true });
  mkdirSync(buildDir, { recursive: true });
  compileWasm();
}

function shouldRebuild(file) {
  const n = file.replace(/\\/g, "/");
  if (n.includes("/node_modules/") || n.includes("/build/") || n.includes("/.zeus/")) return false;
  return /\.(loam|c|h)$/.test(n) || n.endsWith("/web/loader.js");
}

export default defineConfig({
  root: here,
  publicDir: "build",
  server: {
    host: "127.0.0.1",
    port,
    strictPort: true,
    proxy: { "/Vidget": rpc.startsWith("http") ? rpc : "http://" + rpc },
  },
  plugins: [
    {
      name: "loam-wasm",
      buildStart() {
        rebuild("start");
      },
      configureServer(server) {
        const watch = [
          appDir,
          resolve(repo, "packages/loam/std"),
          resolve(repo, "packages/loam/runtime"),
          resolve(repo, "packages/zeus/std"),
          resolve(repo, "packages/zeus-components/std"),
          resolve(repo, "packages/http/std"),
          resolve(repo, "packages/zeus/hosts/web"),
        ];
        for (const p of watch) if (existsSync(p)) server.watcher.add(p);
        let armed = false;
        let timer = null;
        setTimeout(() => { armed = true; }, 400);
        server.watcher.on("all", (_ev, file) => {
          if (!armed || !shouldRebuild(file)) return;
          clearTimeout(timer);
          timer = setTimeout(() => {
            try {
              rebuild("change " + file);
              server.ws.send({ type: "full-reload" });
            } catch (e) {
              console.error("[loam]", e.message || e);
            }
          }, 80);
        });
        server.middlewares.use((req, res, next) => {
          const url = (req.url || "").split("?")[0];
          /* The page's log lines (`std:log`, `fmt.println`): printed here, in
             the terminal `zeli serve` echoes, instead of the browser console. */
          if (url === "/__zeus/log" && req.method === "POST") {
            let body = "";
            req.setEncoding("utf8");
            req.on("data", (c) => { body += c; });
            req.on("end", () => {
              for (const line of body.split("\n")) if (line.trim()) process.stdout.write("web │ " + line + "\n");
              res.statusCode = 204;
              res.end();
            });
            return;
          }
          if (url === "/loader.js") {
            res.setHeader("Content-Type", "text/javascript; charset=utf-8");
            createReadStream(loader).pipe(res);
            return;
          }
          next();
        });
      },
    },
  ],
});
