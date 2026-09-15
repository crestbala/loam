import { defineConfig } from "vite";
import { createReadStream, existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const app = process.env.ZEUS_APP || "examples/counter";
const appDir = resolve(here, app);
const buildDir = resolve(appDir, "build");
const loaderPath = resolve(here, "hosts/web/zeus-wasm/loader.js");

function sendFile(res, file, type) {
  if (!existsSync(file)) {
    res.statusCode = 404;
    res.end("missing " + file);
    return;
  }
  res.setHeader("Content-Type", type);
  createReadStream(file).pipe(res);
}

export default defineConfig({
  root: appDir,
  publicDir: buildDir,
  server: {
    host: "127.0.0.1",
    port: 5173,
    proxy: {
      "/api": {
        target: "http://127.0.0.1:8080",
        changeOrigin: true,
      },
    },
  },
  plugins: [
    {
      name: "zeus-loader",
      configureServer(server) {
        server.middlewares.use((req, res, next) => {
          const url = (req.url || "").split("?")[0];
          if (url !== "/loader.js") return next();
          sendFile(res, loaderPath, "text/javascript; charset=utf-8");
        });
      },
    },
  ],
});
