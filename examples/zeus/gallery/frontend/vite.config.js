/** Gallery wasm dev server: compiles `app.loam`, serves `build/app.wasm`. */
import { spawn } from "node:child_process";
import {
  createReadStream,
  existsSync,
  mkdirSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
} from "node:fs";
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

function newestMtime(dir, acc) {
  let ents;
  try {
    ents = readdirSync(dir, { withFileTypes: true });
  } catch {
    return acc;
  }
  for (const e of ents) {
    if (e.name === "node_modules" || e.name === "build" || e.name === ".git") continue;
    const p = resolve(dir, e.name);
    if (e.isDirectory()) newestMtime(p, acc);
    else if (/\.(yuga|c|h)$/.test(e.name)) {
      try {
        const t = statSync(p).mtimeMs;
        if (t > acc.t) acc.t = t;
      } catch {}
    }
  }
  return acc;
}

function wasmIsCurrent() {
  if (!existsSync(wasmFile)) return false;
  const wasmT = statSync(wasmFile).mtimeMs;
  const acc = { t: 0 };
  newestMtime(here, acc);
  newestMtime(resolve(here, ".."), acc);
  newestMtime(resolve(repo, "packages/loam/std"), acc);
  newestMtime(resolve(repo, "packages/loam/runtime"), acc);
  newestMtime(resolve(repo, "packages/zeus/std"), acc);
  newestMtime(resolve(repo, "packages/http/std"), acc);
  newestMtime(resolve(repo, "packages/maya/std"), acc);
  newestMtime(resolve(repo, "packages/zeus/hosts/web"), acc);
  return acc.t <= wasmT;
}

let compileProc = null;
let compileQueued = false;

function runLoamc() {
  if (!existsSync(loam)) {
    return Promise.reject(
      new Error("missing " + loam + " — run `make` in the yuga repo first"),
    );
  }
  mkdirSync(buildDir, { recursive: true });
  const tmp = resolve(buildDir, "app.wasm.tmp");
  return new Promise((resolveP, reject) => {
    const child = spawn(loam, ["build", "--target=wasm32", "-o", tmp, app], {
      cwd: repo,
    });
    let stderr = "";
    let stdout = "";
    child.stdout.on("data", (d) => {
      process.stdout.write(d);
      stdout += d;
    });
    child.stderr.on("data", (d) => {
      process.stderr.write(d);
      stderr += d;
    });
    child.on("error", reject);
    child.on("close", (status) => {
      if (status !== 0) {
        rmSync(tmp, { force: true });
        reject(new Error(stderr.trim() || stdout.trim() || "loam failed"));
        return;
      }
      if (!existsSync(tmp)) {
        reject(new Error("loam did not produce " + tmp));
        return;
      }
      renameSync(tmp, wasmFile);
      resolveP();
    });
  });
}

function compileWasm() {
  if (compileProc) {
    compileQueued = true;
    return compileProc;
  }
  compileProc = runLoamc().finally(() => {
    compileProc = null;
    if (compileQueued) {
      compileQueued = false;
      return compileWasm();
    }
  });
  return compileProc;
}

function rebuild(reason) {
  if (reason === "start" && wasmIsCurrent()) {
    console.log("[loam] start: wasm is current");
    return Promise.resolve();
  }
  console.log("[loam] " + reason + ": compiling wasm");
  return compileWasm();
}

function shouldRebuild(file) {
  const n = file.replace(/\\/g, "/");
  if (n.includes("/node_modules/") || n.includes("/build/")) return false;
  return /\.(yuga|c|h)$/.test(n) || n.endsWith("/web/loader.js");
}

export default defineConfig({
  publicDir: false,
  server: {
    host: "127.0.0.1",
    port: 5174,
    strictPort: true,
    open: true,
  },
  plugins: [
    {
      name: "loam-wasm",
      async buildStart() {
        await rebuild("start");
      },
      configureServer(server) {
        const watch = [
          here,
          resolve(here, ".."),
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
            rebuild("change " + file)
              .then(() => server.ws.send({ type: "full-reload" }))
              .catch((e) => console.error("[loam]", e.message || e));
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
          if (url === "/app.wasm") {
            if (!existsSync(wasmFile)) {
              res.statusCode = 404;
              res.end("missing app.wasm");
              return;
            }
            res.setHeader("Content-Type", "application/wasm");
            res.setHeader("Cache-Control", "no-cache");
            createReadStream(wasmFile).pipe(res);
            return;
          }
          next();
        });
      },
    },
  ],
});
