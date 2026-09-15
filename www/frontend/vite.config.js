/** Docs wasm dev server: compiles `app.loam`, serves `build/app.wasm`,
 *  and proxies gRPC-Web `POST /Docs/*` to the native backend on :8082. */
import { spawnSync } from "node:child_process";
import { createReadStream, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, "../..");
const yugac = resolve(repo, "bin/yugac");
const buildDir = resolve(here, "build");
const loader = resolve(here, "../../packages/zeus/hosts/web/loader.js");
const app = resolve(here, "app.loam");

function wipeBuild() {
  rmSync(buildDir, { recursive: true, force: true });
  mkdirSync(buildDir, { recursive: true });
}

function compileWasm() {
  if (!existsSync(yugac)) {
    throw new Error("missing " + yugac + " — run `make` in the yuga repo first");
  }
  const r = spawnSync(yugac, ["build", "--target=wasm32", app], {
    cwd: repo,
    encoding: "utf8",
  });
  if (r.stdout) process.stdout.write(r.stdout);
  if (r.stderr) process.stderr.write(r.stderr);
  if (r.status !== 0) {
    throw new Error(r.stderr?.trim() || r.stdout?.trim() || "yugac failed");
  }
}

function rebuild(reason) {
  console.log("[yugac] " + reason + ": removing build/, compiling wasm");
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
    port: 5175,
    proxy: {
      "/Docs": "http://127.0.0.1:8082",
    },
  },
  plugins: [
    {
      name: "yugac-wasm",
      buildStart() {
        rebuild("start");
      },
      configureServer(server) {
        const watch = [
          here,
          resolve(here, ".."),
          resolve(here, "../backend"),
          resolve(repo, "zeus"),
          resolve(repo, "packages/yuga/std"),
          resolve(repo, "packages/yuga/runtime"),
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
