/** Shared Vite config for Zeus Canvas2D wasm examples.
 *
 *   ZEUS_APP=gallery ZEUS_WEB_PORT=5174 npx vite --config packages/zeus/hosts/web/vite.config.js
 *
 * Compiles `examples/zeus/<app>/<app>.yuga` to `build/<app>.wasm`, serves the
 * live `web/loader.js`, and rebuilds when Yuga / runtime / loader sources change.
 */
import { spawnSync } from "node:child_process";
import { createReadStream, existsSync, mkdirSync, renameSync, rmSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, "../../../..");
const name = process.env.ZEUS_APP || "gallery";
const port = Number(process.env.ZEUS_WEB_PORT || 5174);
const appDir = resolve(repo, "examples/zeus", name);
const src = resolve(appDir, name + ".yuga");
const buildDir = resolve(appDir, "build");
const wasmFile = resolve(buildDir, name + ".wasm");
const yugac = resolve(repo, "bin/yugac");
const loader = resolve(here, "loader.js");

function compileWasm() {
  if (!existsSync(yugac)) {
    throw new Error("missing " + yugac + " — run `make` in the yuga repo first");
  }
  if (!existsSync(src)) {
    throw new Error("no Zeus app at " + src);
  }
  mkdirSync(buildDir, { recursive: true });
  const tmp = resolve(buildDir, name + ".wasm.tmp");
  const r = spawnSync(yugac, ["build", "--target=wasm32", "-o", tmp, src], {
    cwd: repo,
    encoding: "utf8",
  });
  if (r.stdout) process.stdout.write(r.stdout);
  if (r.stderr) process.stderr.write(r.stderr);
  if (r.status !== 0) {
    rmSync(tmp, { force: true });
    throw new Error(r.stderr?.trim() || r.stdout?.trim() || "yugac failed");
  }
  if (!existsSync(tmp)) {
    throw new Error("yugac did not produce " + tmp);
  }
  renameSync(tmp, wasmFile);
}

function rebuild(reason) {
  console.log("[yugac] " + reason + ": compiling " + name + " wasm");
  compileWasm();
}

function shouldRebuild(file) {
  const n = file.replace(/\\/g, "/");
  if (n.includes("/node_modules/") || n.includes("/build/")) return false;
  return /\.(yuga|c|h)$/.test(n) || n.endsWith("/web/loader.js");
}

export default defineConfig({
  root: appDir,
  publicDir: buildDir,
  server: {
    host: "127.0.0.1",
    port: port,
    strictPort: true,
    open: true,
  },
  plugins: [
    {
      name: "yugac-wasm",
      buildStart() {
        rebuild("start");
      },
      configureServer(server) {
        const watch = [
          appDir,
          resolve(repo, "packages/yuga/std"),
          resolve(repo, "packages/yuga/runtime"),
          resolve(repo, "packages/zeus/std"),
          resolve(repo, "packages/http/std"),
          resolve(repo, "packages/maya/std"),
          here,
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
              console.error("[yugac]", e.message || e);
            }
          }, 80);
        });
        server.middlewares.use((req, res, next) => {
          const url = (req.url || "").split("?")[0];
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
