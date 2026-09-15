/** WebSocket wasm demo dev server: compiles `app.loam`, serves
 *  `build/app.wasm`, and proxies `ws://127.0.0.1:5173/ws` to the native
 *  backend on :8080 (`examples/zeus/ws/backend`). Start the backend first. */
import { spawnSync } from "node:child_process";
import { createReadStream, existsSync, mkdirSync, renameSync, rmSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, "../../../..");
const loam = resolve(repo, "bin/loam");
const buildDir = resolve(here, "build");
const wasmFile = resolve(buildDir, "app.wasm");
const loader = resolve(repo, "packages/zeus/hosts/web/loader.js");
const app = resolve(here, "app.loam");

function wipeBuild() {
  rmSync(buildDir, { recursive: true, force: true });
  mkdirSync(buildDir, { recursive: true });
}

function compileWasm() {
  if (!existsSync(loam)) {
    throw new Error("missing " + loam + " — run `make` in the yuga repo first");
  }
  /* Compile to a temp name and rename into place: the dev server must never
     serve a half-written or missing /app.wasm (vite's SPA fallback would
     answer with index.html and the loader dies on the magic word). */
  const tmp = resolve(buildDir, "app.wasm.tmp");
  const r = spawnSync(loam, ["build", "--target=wasm32", "-o", tmp, app], {
    cwd: repo,
    encoding: "utf8",
  });
  if (r.stdout) process.stdout.write(r.stdout);
  if (r.stderr) process.stderr.write(r.stderr);
  if (r.status !== 0) {
    rmSync(tmp, { force: true });
    throw new Error(r.stderr?.trim() || r.stdout?.trim() || "loam failed");
  }
  if (!existsSync(tmp)) {
    throw new Error("loam did not produce " + tmp);
  }
  renameSync(tmp, wasmFile);
}

function rebuild(reason) {
  console.log("[loam] " + reason + ": removing build/, compiling wasm");
  wipeBuild();
  compileWasm();
}

function shouldRebuild(file) {
  const n = file.replace(/\\/g, "/");
  if (n.includes("/node_modules/") || n.includes("/build/")) return false;
  return /\.(yuga|c|h)$/.test(n) || n.endsWith("/web/loader.js");
}

export default defineConfig({
  publicDir: "build",
  server: {
    host: "127.0.0.1",
    port: 5173,
    proxy: {
      "/ws": {
        target: "ws://127.0.0.1:8080",
        ws: true,
      },
    },
  },
  plugins: [
    {
      name: "loam-wasm",
      buildStart() {
        rebuild("start");
      },
      configureServer(server) {
        const watch = [
          here,
          resolve(here, ".."),
          resolve(here, "../backend"),
          resolve(repo, "packages/loam/std"),
          resolve(repo, "packages/loam/runtime"),
          resolve(repo, "packages/zeus/std"),
          resolve(repo, "packages/http/std"),
          resolve(repo, "packages/maya/std"),
          resolve(repo, "packages/zeus/hosts/web"),
        ];
        for (const p of watch) {
          if (existsSync(p)) server.watcher.add(p);
        }
        let armed = false;
        let timer = null;
        setTimeout(() => {
          armed = true;
        }, 400);
        server.watcher.on("all", (_ev, file) => {
          if (!armed) return;
          if (!shouldRebuild(file)) return;
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
          if (url === "/app.wasm") {
            // Serve the binary ourselves so vite's SPA fallback can never
            // answer /app.wasm with index.html. Missing = still compiling:
            // tell the loader to retry instead of dying on HTML bytes.
            if (!existsSync(wasmFile)) {
              res.statusCode = 503;
              res.setHeader("Content-Type", "text/plain; charset=utf-8");
              res.end("compiling app.wasm, reload in a moment");
              return;
            }
            res.setHeader("Content-Type", "application/wasm");
            createReadStream(wasmFile).pipe(res);
            return;
          }
          if (url === "/loader.js") {
            if (!existsSync(loader)) {
              res.statusCode = 404;
              res.end("missing loader.js");
              return;
            }
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
