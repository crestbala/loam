#!/usr/bin/env -S deno run --allow-read --allow-write --allow-run --allow-env
// zeli — the Loam/Zeus CLI: create an app, serve it, build it for every host.
//
//     deno run --allow-read --allow-write packages/zeus/cli/zeli.ts new <name> [dir]
//     deno run --allow-read --allow-write packages/zeus/cli/zeli.ts routes <appdir>
//     deno run --allow-read --allow-write packages/zeus/cli/zeli.ts fmt [dir]
//     deno run --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeli.ts build <appdir> [--targets a,b] [--base https://host]
//     deno run --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeli.ts pkg sync <appdir>
//     deno run --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeli.ts dev <appdir> [--port N] [--build-only]
//
// `bin/zeli` is a shim for this file (see the Makefile); an app is a directory
// with a `zeus.toml`. Generates <appdir>/app_routes.loam from the app's routes/
// tree; formats a tree in place; builds the five targets; emits the per-route
// web shell; vendors packages + lockfile; serves a dev build that rebuilds on a
// source change and pushes the reload to connected pages over SSE — no polling
// on either side.
//
// Every command that runs or emits an app gates on `loam check` first.
//
// File conventions (Next.js-shaped, no JS and no DOM):
//
//     routes/page.loam          page for the directory's URL   fn page(p: Params)
//     routes/layout.loam        chrome, then content           fn layout(build: fn())
//     routes/not-found.loam     404 page                        fn not_found()
//     routes/error.loam          route error page               fn error(msg: string)
//     routes/loading.loam        pending page                   fn loading()
//     routes/[slug]/page.loam   /:slug param
//     routes/(group)/...        URL-invisible group
//
// Layouts compose outermost-first down the directory chain. The root layout
// (`routes/layout.loam`) is applied once around the router, so its state
// survives navigation; segment layouts wrap the pages beneath them.

import * as path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..", "..", "..");
const LOAM = path.join(REPO, "bin", "loam");
const FMT = path.join(REPO, "bin", "loam-fmt");

type Entry = { parts: string[]; dir: string; files: Set<string> };
type Builder = { parts: string[]; alias: string; meta: boolean; paths: boolean };

function urlFor(parts: string[]): string {
  const out: string[] = [];
  for (const p of parts) {
    if (p.startsWith("(") && p.endsWith(")")) continue;
    if (p.startsWith("[") && p.endsWith("]")) out.push(":" + p.slice(1, -1));
    else out.push(p);
  }
  return out.length ? "/" + out.join("/") : "/";
}

function aliasFor(parts: string[], kind: string): string {
  const bits = parts.map((p) => {
    if (p.startsWith("(") && p.endsWith(")")) p = p.slice(1, -1);
    else if (p.startsWith("[") && p.endsWith("]")) p = p.slice(1, -1);
    return p.replace(/-/g, "_");
  });
  const name = bits.length ? bits.join("_") : "root";
  return `r_${name}_${kind}`;
}

function scan(routesDir: string): Entry[] {
  const out: Entry[] = [];
  const walk = (dir: string) => {
    const files = new Set<string>();
    const subdirs: string[] = [];
    for (const e of Deno.readDirSync(dir)) {
      if (e.name.startsWith(".")) continue;
      if (e.isDirectory) subdirs.push(e.name);
      else if (e.isFile) files.add(e.name);
    }
    const rel = path.relative(routesDir, dir);
    const parts = rel === "" ? [] : rel.split(path.sep);
    out.push({ parts, dir, files });
    for (const sub of subdirs.sort()) walk(path.join(dir, sub));
  };
  walk(routesDir);
  out.sort((a, b) => a.parts.join("/").localeCompare(b.parts.join("/")));
  return out;
}

function generate(appdir: string): string {
  const routesDir = path.join(appdir, "routes");
  let st: Deno.FileInfo | undefined;
  try {
    st = Deno.statSync(routesDir);
  } catch {
    st = undefined;
  }
  if (!st?.isDirectory) die(`zeli: no routes/ directory in ${appdir}`);

  const out = path.join(appdir, "app_routes.loam");
  const entries = scan(routesDir);

  const layouts = new Map<string, string>();
  const imports = new Map<string, string>();
  const builders: Builder[] = [];
  let rootNf: string | null = null;

  const key = (parts: string[]) => parts.join("/");
  const note = (alias: string, p: string) =>
    imports.set(alias, path.relative(path.dirname(out), p).split(path.sep).join("/"));

  for (const { parts, dir, files } of entries) {
    if (files.has("layout.loam")) {
      const al = aliasFor(parts, "layout");
      layouts.set(key(parts), al);
      note(al, path.join(dir, "layout.loam"));
    }
    if (files.has("page.loam")) {
      const pal = aliasFor(parts, "page");
      note(pal, path.join(dir, "page.loam"));
      const src = Deno.readTextFileSync(path.join(dir, "page.loam"));
      builders.push({
        parts,
        alias: pal,
        meta: /\bfn\s+meta\s*\(/.test(src),
        paths: /\bfn\s+paths\s*\(/.test(src),
      });
    }
    if (files.has("not-found.loam")) {
      const al = aliasFor(parts, "not_found");
      note(al, path.join(dir, "not-found.loam"));
      if (parts.length === 0) rootNf = al;
    }
  }

  if (builders.length === 0) die(`zeli: no page.loam under ${routesDir}`);

  const L: string[] = [];
  L.push("// Generated by `zeli routes`. Do not edit.");
  L.push('import "std:zeus"');
  L.push('import "std:router"');
  for (const al of [...imports.keys()].sort()) {
    L.push(`import "${imports.get(al)}" as ${al}`);
  }
  L.push("");

  for (const { parts, alias, meta, paths } of builders) {
    const bname = "b_" + alias.slice(2); // r_<x>_page -> b_<x>_page
    let body = `${alias}.page(p)`;
    for (let i = parts.length; i >= 1; i--) {
      const lay = layouts.get(key(parts.slice(0, i)));
      if (lay) body = `${lay}.layout(fn() { ${body} })`;
    }
    L.push(`fn ${bname}(p: Params) {`);
    L.push(`    ${body}`);
    L.push("}");
    L.push("");
    if (meta) {
      L.push(`fn m_${alias.slice(2)}(p: Params) -> Meta {`);
      L.push(`    return ${alias}.meta(p)`);
      L.push("}");
      L.push("");
    }
    if (paths) {
      L.push(`fn p_${alias.slice(2)}() -> []string {`);
      L.push(`    return ${alias}.paths()`);
      L.push("}");
      L.push("");
    }
  }

  L.push("fn routes() -> []Route {");
  L.push("    return []Route {");
  for (const { parts, alias, meta, paths } of builders) {
    const extra: string[] = [];
    if (meta) extra.push(`meta: m_${alias.slice(2)}`);
    if (paths) extra.push(`paths: p_${alias.slice(2)}`);
    const tail = extra.length ? ", " + extra.join(", ") : "";
    L.push(`        Route { pattern: "${urlFor(parts)}", build: b_${alias.slice(2)}${tail} },`);
  }
  L.push("    }");
  L.push("}");
  L.push("");

  // The root layout is applied once around the router, so it is retained
  // across navigation. A root not-found overrides the router default.
  const props = rootNf
    ? ` RouterProps { not_found: fn() { ${rootNf}.not_found() } }`
    : "";
  const router = `router.Router(routes(),${props})`;
  const rootLay = layouts.get(key([]));
  L.push("fn app() {");
  if (rootLay) {
    L.push(`    ${rootLay}.layout(fn() {`);
    L.push(`        ${router}`);
    L.push("    })");
  } else {
    L.push(`    ${router}`);
  }
  L.push("}");

  Deno.mkdirSync(path.dirname(out), { recursive: true });
  Deno.writeTextFileSync(out, L.join("\n") + "\n");
  return out;
}

function die(msg: string): never {
  console.error(msg);
  Deno.exit(2);
}

// --- zeli build ---

type TargetName = "web" | "macos" | "server" | "ios" | "android";

const ALL_TARGETS: TargetName[] = ["web", "macos", "ios", "android", "server"];

function exists(p: string): boolean {
  try {
    Deno.statSync(p);
    return true;
  } catch {
    return false;
  }
}

/**
 * Environment for spawning `loam` from build/dev. The headless switches are
 * compile-time (they decide whether the GUI host is linked), and the test
 * runner exports them for *running* fixtures. A user build must not inherit
 * them, or `zeli build` silently produces a windowless binary.
 */
function buildEnv(): Record<string, string> | undefined {
  try {
    const env = { ...Deno.env.toObject() };
    delete env.ZEUS_HEADLESS;
    delete env.LOAM_HEADLESS;
    delete env.MAYA_HEADLESS;
    return env;
  } catch {
    return undefined; // no --allow-env: inherit as-is
  }
}

/** `<appdir>/app.loam`, else `<appdir>/<dirname>.loam`, else `main.loam`. */
function findEntry(appdir: string): string {
  const base = path.basename(appdir);
  for (const name of ["app.loam", base + ".loam", "main.loam"]) {
    const p = path.join(appdir, name);
    if (exists(p)) return p;
  }
  die(`zeli: no entry .loam in ${appdir} (looked for app.loam / ${base}.loam / main.loam)`);
}

/**
 * `loam check <entry>` — the frontend only, no codegen. Every command that
 * runs or emits an app gates on this first, so a type error fails once, up
 * front, instead of once per target (or not at all when a target's host
 * toolchain is missing and its failure looks like the same thing).
 */
function checkEntry(entry: string): boolean {
  const env = buildEnv();
  const r = new Deno.Command(LOAM, {
    args: ["check", entry],
    cwd: REPO,
    ...(env ? { env, clearEnv: true } : {}),
    stdout: "piped",
    stderr: "piped",
  }).outputSync();
  if (r.code === 0) return true;
  const err = new TextDecoder().decode(r.stderr).trim();
  const out = new TextDecoder().decode(r.stdout).trim();
  console.error(`zeli: ${path.relative(REPO, entry)} failed the compiler check`);
  if (err) console.error("  " + err.split("\n").join("\n  "));
  else if (out) console.error("  " + out.split("\n").join("\n  "));
  return false;
}

function loamArgs(t: TargetName, entry: string, out: string): string[] {
  switch (t) {
    case "web":
      return ["--target=wasm32", entry, "-o", out];
    case "ios":
      return ["--target=ios", entry, "-o", out];
    case "android":
      return ["--target=android", entry, "-o", out];
    default:
      return [entry, "-o", out]; // macos / server: native
  }
}

/**
 * `zeli build <appdir> [--targets web,macos,...]`
 *
 * Generates the route table when `routes/` exists, then compiles the app for
 * each target into `<appdir>/build/<target>/`. After a successful web build it
 * emits one HTML shell per route plus `sitemap.xml` / `robots.txt` (§5.7).
 * Targets whose host toolchain is missing (iOS/Android SDKs) fail individually;
 * the rest still build.
 */
function build(appdir: string, targets: TargetName[], base: string): number {
  if (!exists(LOAM)) die(`zeli: missing ${LOAM} — run \`make\` in ${REPO} first`);
  if (exists(path.join(appdir, "routes"))) generate(appdir);
  const entry = findEntry(appdir);
  if (!checkEntry(entry)) return 1;
  const stem = path.basename(entry, ".loam");
  let built = 0;
  for (const t of targets) {
    const dir = path.join(appdir, "build", t);
    Deno.mkdirSync(dir, { recursive: true });
    const outName = t === "web" ? stem + ".wasm" : stem;
    const out = path.join(dir, outName);
    const env = buildEnv();
    const r = new Deno.Command(LOAM, {
      args: loamArgs(t, entry, out),
      cwd: REPO,
      ...(env ? { env, clearEnv: true } : {}),
      stdout: "piped",
      stderr: "piped",
    }).outputSync();
    if (r.code === 0) {
      console.log(`zeli: ${t} ok -> ${path.relative(REPO, out)}`);
      built++;
      if (t === "web" && exists(path.join(appdir, "routes"))) {
        emitWebShell(appdir, stem, base);
      }
    } else {
      const err = new TextDecoder().decode(r.stderr).trim();
      console.error(`zeli: ${t} failed${err ? "\n  " + err.split("\n").join("\n  ") : ""}`);
    }
  }
  console.log(`zeli: built ${built}/${targets.length} target(s)`);
  return built > 0 ? 0 : 1;
}

// --- web shell (§5.7): one HTML file per route, plus sitemap + robots ---

type MetaRec = {
  path: string;
  title: string;
  description: string;
  canonical: string;
  og_image: string;
  og_type: string;
  robots: string;
  jsonld: string;
};

/** The metadata dump: a native Loam program that prints one JSON record per
 *  concrete route URL by calling each route's `meta()` (and `paths()` for
 *  dynamic routes). */
function metaDumpSource(): string {
  return [
    "// Generated by `zeli build`. Do not edit.",
    'import "std:fmt"',
    'import "std:json"',
    'import "std:router"',
    'import "../app_routes.loam" as approutes',
    "",
    "fn emit(path: string, m: Meta) {",
    '    json.enc_obj_begin()',
    '    json.enc_key("path")',
    "    json.enc_str(path)",
    '    json.enc_key("title")',
    "    json.enc_str(m.title)",
    '    json.enc_key("description")',
    "    json.enc_str(m.description)",
    '    json.enc_key("canonical")',
    "    json.enc_str(m.canonical)",
    '    json.enc_key("og_image")',
    "    json.enc_str(m.og_image)",
    '    json.enc_key("og_type")',
    "    json.enc_str(m.og_type)",
    '    json.enc_key("robots")',
    "    json.enc_str(m.robots)",
    '    json.enc_key("jsonld")',
    "    json.enc_str(m.jsonld)",
    "    json.enc_obj_end()",
    "    fmt.println(json.enc_take())",
    "}",
    "",
    "fn main() {",
    "    let rs = approutes.routes()",
    "    let mut i = 0",
    "    while i < rs.len {",
    "        let pat = rs[i].pattern",
    "        if router.is_dynamic(pat) == 1 {",
    "            let ps = rs[i].paths()",
    "            let mut j = 0",
    "            while j < ps.len {",
    "                if router.match_route(pat, ps[j]) == 1 {",
    "                    let p = Params { keys: router.match_keys(), vals: router.match_vals() }",
    "                    emit(ps[j], rs[i].meta(p))",
    "                }",
    "                j = j + 1",
    "            }",
    "        } else {",
    "            let p = Params { keys: []string {}, vals: []string {} }",
    "            emit(pat, rs[i].meta(p))",
    "        }",
    "        i = i + 1",
    "    }",
    "}",
    "",
  ].join("\n");
}

/** Run the generated metadata dump and return its JSON records. */
function collectMeta(appdir: string): MetaRec[] {
  const zdir = path.join(appdir, ".zeus");
  const src = path.join(zdir, "meta_dump.loam");
  Deno.mkdirSync(zdir, { recursive: true });
  Deno.writeTextFileSync(src, metaDumpSource());
  const bin = path.join(zdir, "meta_dump");
  // The dump only *calls* meta()/paths(); a DCE edge case can drop a needed
  // std:zeus helper while keeping its call, so compile it with DCE off.
  const cc = new Deno.Command(LOAM, {
    args: [src, "-o", bin],
    cwd: REPO,
    env: { ...Deno.env.toObject(), LOAM_NO_DCE: "1" },
    stdout: "piped",
    stderr: "piped",
  }).outputSync();
  if (cc.code !== 0) {
    const err = new TextDecoder().decode(cc.stderr).trim();
    die(`zeli: metadata dump failed to compile${err ? "\n  " + err.split("\n").join("\n  ") : ""}`);
  }
  const run = new Deno.Command(bin, { cwd: REPO, stdout: "piped", stderr: "piped" }).outputSync();
  if (run.code !== 0) {
    const err = new TextDecoder().decode(run.stderr).trim();
    die(`zeli: metadata dump failed to run${err ? "\n  " + err.split("\n").join("\n  ") : ""}`);
  }
  const text = new TextDecoder().decode(run.stdout);
  const recs: MetaRec[] = [];
  for (const line of text.split("\n")) {
    const t = line.trim();
    if (!t.startsWith("{")) continue;
    try {
      recs.push(JSON.parse(t) as MetaRec);
    } catch {
      console.error(`zeli: skipping malformed metadata record: ${t}`);
    }
  }
  return recs;
}

function esc(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

/** Join `base` and `path` into an absolute canonical URL when `base` is set. */
function canonicalUrl(base: string, pathOrUrl: string): string {
  if (/^https?:\/\//.test(pathOrUrl)) return pathOrUrl;
  if (!base) return pathOrUrl;
  return base.replace(/\/$/, "") + (pathOrUrl.startsWith("/") ? pathOrUrl : "/" + pathOrUrl);
}

function shellHtml(r: MetaRec, stem: string): string {
  const title = esc(r.title);
  const desc = esc(r.description);
  const canonical = esc(r.canonical);
  const lines: string[] = [
    "<!DOCTYPE html>",
    '<html lang="en">',
    "<head>",
    '<meta charset="utf-8" />',
    '<meta name="viewport" content="width=device-width, initial-scale=1" />',
    `<title>${title}</title>`,
    `<meta name="description" content="${desc}" />`,
    `<link rel="canonical" href="${canonical}" />`,
    `<meta name="robots" content="${esc(r.robots)}" />`,
    `<meta property="og:type" content="${esc(r.og_type)}" />`,
    `<meta property="og:title" content="${title}" />`,
    `<meta property="og:description" content="${desc}" />`,
    `<meta property="og:url" content="${canonical}" />`,
  ];
  if (r.og_image) {
    const img = esc(canonicalUrl("", r.og_image));
    lines.push(`<meta property="og:image" content="${img}" />`);
    lines.push('<meta name="twitter:card" content="summary_large_image" />');
    lines.push(`<meta name="twitter:title" content="${title}" />`);
    lines.push(`<meta name="twitter:description" content="${desc}" />`);
    lines.push(`<meta name="twitter:image" content="${img}" />`);
  } else {
    lines.push('<meta name="twitter:card" content="summary" />');
  }
  if (r.jsonld) {
    lines.push('<script type="application/ld+json">');
    lines.push(r.jsonld);
    lines.push("</script>");
  }
  lines.push(
    "<style>html,body{margin:0;height:100%;background:#fafafa}canvas#zeus{display:block;position:fixed;inset:0;width:100%;height:100%;touch-action:none}</style>",
  );
  lines.push("</head>");
  lines.push("<body>");
  lines.push(`<canvas id="zeus" data-wasm="/${stem}.wasm"></canvas>`);
  lines.push('<script src="/loader.js"></script>');
  lines.push("</body>");
  lines.push("</html>");
  return lines.join("\n") + "\n";
}

/** Write one HTML file per route, plus `sitemap.xml` and `robots.txt`. */
function emitWebShell(appdir: string, stem: string, base: string): void {
  const web = path.join(appdir, "build", "web");
  Deno.mkdirSync(web, { recursive: true });
  const recs = collectMeta(appdir);
  const urls: string[] = [];
  for (const r of recs) {
    const clean = r.canonical || r.path;
    r.canonical = canonicalUrl(base, clean);
    const rel = r.path.replace(/^\//, "").replace(/\/$/, "");
    const dir = rel ? path.join(web, rel) : web;
    Deno.mkdirSync(dir, { recursive: true });
    Deno.writeTextFileSync(path.join(dir, "index.html"), shellHtml(r, stem));
    urls.push(r.canonical);
  }
  const sm: string[] = ['<?xml version="1.0" encoding="UTF-8"?>', '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'];
  for (const u of urls) sm.push(`  <url><loc>${esc(u)}</loc></url>`);
  sm.push("</urlset>");
  Deno.writeTextFileSync(path.join(web, "sitemap.xml"), sm.join("\n") + "\n");
  const robots = ["User-agent: *", "Allow: /", ...(base ? [`Sitemap: ${base.replace(/\/$/, "")}/sitemap.xml`] : [])];
  Deno.writeTextFileSync(path.join(web, "robots.txt"), robots.join("\n") + "\n");
  console.log(`zeli: wrote ${recs.length} HTML shell(s) + sitemap.xml + robots.txt -> ${path.relative(REPO, web)}`);
}

// --- zeli pkg: vendored packages + lockfile (§1.8) ---

function removeDir(p: string): void {
  try {
    Deno.removeSync(p, { recursive: true });
  } catch {
    /* absent */
  }
}

function copyDir(src: string, dest: string): void {
  Deno.mkdirSync(dest, { recursive: true });
  for (const e of Deno.readDirSync(src)) {
    if (e.name === ".git") continue;
    const s = path.join(src, e.name);
    const d = path.join(dest, e.name);
    if (e.isDirectory) copyDir(s, d);
    else if (e.isFile) Deno.copyFileSync(s, d);
  }
}

/**
 * `zeli pkg sync <appdir>`
 *
 * Reads `<appdir>/loam.deps` (one `name source [rev]` per line; `#` comments)
 * and materializes each package under `<appdir>/vendor/<name>/`, writing
 * `<appdir>/loam.lock`. A `path:../dir` source is copied locally (no network);
 * anything else is a git URL, cloned and pinned to its HEAD (or `rev`) SHA.
 * `import "pkg:name"` then resolves to `vendor/name/name.loam`.
 */
function pkgSync(appdir: string): number {
  const depsPath = path.join(appdir, "loam.deps");
  if (!exists(depsPath)) die(`zeli: no loam.deps in ${appdir}`);
  const vendor = path.join(appdir, "vendor");
  Deno.mkdirSync(vendor, { recursive: true });
  const lock: string[] = ["# Generated by `zeli pkg sync`. Do not edit."];
  const seen = new Set<string>();
  let ok = true;
  for (const raw of Deno.readTextFileSync(depsPath).split("\n")) {
    const line = raw.replace(/#.*$/, "").trim();
    if (!line) continue;
    const toks = line.split(/\s+/);
    const name = toks[0];
    const source = toks[1] ?? "";
    const rev = toks[2] ?? "";
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) die(`zeli: bad package name '${name}'`);
    if (seen.has(name)) die(`zeli: duplicate package '${name}'`);
    seen.add(name);
    const dest = path.join(vendor, name);
    if (!source) {
      console.error(`zeli: ${name}: missing source`);
      ok = false;
      continue;
    }
    if (source.startsWith("path:")) {
      const src = path.resolve(appdir, source.slice(5));
      if (!exists(src)) {
        console.error(`zeli: ${name}: missing ${src}`);
        ok = false;
        continue;
      }
      removeDir(dest);
      copyDir(src, dest);
      lock.push(`${name}\t${source}\tlocal`);
      console.log(`zeli: ${name} <- ${path.relative(REPO, src)} (local)`);
    } else {
      removeDir(dest);
      const clone = new Deno.Command("git", {
        args: ["clone", "--quiet", source, dest],
        cwd: REPO,
        stdout: "piped",
        stderr: "piped",
      }).outputSync();
      if (clone.code !== 0) {
        console.error(`zeli: ${name}: git clone failed\n  ${new TextDecoder().decode(clone.stderr).trim()}`);
        ok = false;
        continue;
      }
      if (rev) {
        const co = new Deno.Command("git", {
          args: ["-C", dest, "checkout", "--quiet", rev],
          stdout: "piped",
          stderr: "piped",
        }).outputSync();
        if (co.code !== 0) {
          console.error(`zeli: ${name}: cannot checkout '${rev}'`);
          ok = false;
        }
      }
      const head = new Deno.Command("git", {
        args: ["-C", dest, "rev-parse", "HEAD"],
        stdout: "piped",
      }).outputSync();
      const sha = new TextDecoder().decode(head.stdout).trim();
      lock.push(`${name}\t${source}\t${sha}`);
      console.log(`zeli: ${name} <- ${source}@${sha.slice(0, 12)}`);
    }
  }
  Deno.writeTextFileSync(path.join(appdir, "loam.lock"), lock.join("\n") + "\n");
  return ok ? 0 : 1;
}

// --- zeli dev: build, watch, serve, live reload (§2.5) ---

/**
 * Does a path under one of `roots` belong to the dev build's input set?
 *
 * Only `.loam` sources count, and never anything under an output or cache dir —
 * a rebuild writes into `<appdir>/build/`, which sits *inside* a watched root,
 * so an unfiltered watch would rebuild forever. The skip list is the one the
 * mtime scan used to walk; what changes is that a *new* file is now noticed the
 * moment it lands, rather than only when a later scan happens to look.
 *
 * Pass `roots` and the decision is made on the path *relative* to the watch
 * root, so a checkout that happens to live under a dotted directory still
 * reloads. Without them the whole path is judged, which is what a path with no
 * known root can be held to.
 */
export function isWatchedSource(file: string, roots: string[] = []): boolean {
  let rel = file;
  for (const r of roots) {
    if (rel.startsWith(r + path.sep)) {
      rel = rel.slice(r.length + 1);
      break;
    }
  }
  if (!rel.endsWith(".loam")) return false;
  for (const seg of rel.split(path.sep)) {
    if (seg.startsWith(".")) return false; // .git/, .zeus/
    if (seg === "build" || seg === "node_modules" || seg === "vendor") return false;
  }
  return true;
}

/** The Server-Sent Events channel the injected snippet subscribes to. */
export const LIVE_PATH = "/.zeli-live";

/** One SSE frame. `EventSource.onmessage` receives the payload as `data`. */
export function sseFrame(rev: number): string {
  return `data: ${rev}\n\n`;
}

/**
 * The live-reload script injected into every served HTML page.
 *
 * The server pushes a frame per successful rebuild, so an idle tab holds one
 * open connection and does nothing — no `setInterval`, no request a second.
 * `EventSource` reconnects by itself if the dev server restarts. The first
 * frame on connect is the current revision: a baseline, not a reload. Only a
 * later, different revision reloads, and it saves the signal arena first so an
 * edit does not throw away app state.
 */
export function liveSnippet(): string {
  return `<script>(function(){var r=null,s=new EventSource('${LIVE_PATH}');` +
    `s.onmessage=function(e){if(r===null){r=e.data;return}if(e.data!==r){r=e.data;` +
    `try{if(window.__zeus_save)window.__zeus_save()}catch(x){}location.reload()}}})()</script>`;
}

/**
 * Regenerate `<appdir>/app_routes.loam` and report whether it changed.
 *
 * The route table is a build artifact of the `routes/` tree, not a file to
 * maintain: dev regenerates it before every build, so adding
 * `routes/pricing/page.loam` is enough — nothing to run, nothing to keep in
 * sync by hand.
 */
function generateTracked(appdir: string): boolean {
  const out = path.join(appdir, "app_routes.loam");
  const before = readTextIfPresent(out);
  generate(appdir);
  return readTextIfPresent(out) !== before;
}

function readTextIfPresent(p: string): string {
  try {
    return Deno.readTextFileSync(p);
  } catch {
    return "";
  }
}

/**
 * Call `onBatch(paths)` once per burst of source changes under `roots`.
 *
 * One `Deno.watchFs` over the roots, filtered by `isWatchedSource`, coalesced by
 * `debounceMs` — a single editor save arrives as several events (write, then
 * rename), and each of them would otherwise start its own rebuild. Returns the
 * watcher, whose `close()` ends the loop.
 */
export function watchSources(
  roots: string[],
  debounceMs: number,
  onBatch: (paths: string[]) => void,
): Deno.FsWatcher {
  /* Absolute, because `watchFs` reports nothing at all for a relative root —
     it neither throws nor watches, so a relative app path would look like a
     dev server that simply never notices an edit. */
  const resolved = roots.filter(exists).map((r) => path.resolve(r));
  const watcher = Deno.watchFs(resolved);
  let queued: string[] = [];
  let timer: number | undefined;
  (async () => {
    for await (const ev of watcher) {
      if (ev.kind === "access") continue;
      let hit = false;
      for (const p of ev.paths) {
        if (isWatchedSource(p, resolved)) {
          queued.push(p);
          hit = true;
        }
      }
      if (!hit) continue;
      if (timer !== undefined) clearTimeout(timer);
      timer = setTimeout(() => {
        timer = undefined;
        const batch = queued;
        queued = [];
        onBatch(batch);
      }, debounceMs);
    }
  })().catch((e) => console.error(`zeli: watch stopped: ${e}`));
  return watcher;
}

/**
 * Build the web target and refresh the HTML shell. `routesChanged` says whether
 * the generated route table was rewritten, so dev can say so once.
 */
type DevBuild = { ok: boolean; routesChanged: boolean };

function devBuildOnce(appdir: string): DevBuild {
  const routesChanged = exists(path.join(appdir, "routes")) ? generateTracked(appdir) : false;
  const entry = findEntry(appdir);
  if (!checkEntry(entry)) return { ok: false, routesChanged };
  const stem = path.basename(entry, ".loam");
  const dir = path.join(appdir, "build", "web");
  Deno.mkdirSync(dir, { recursive: true });
  const env = buildEnv();
  const r = new Deno.Command(LOAM, {
    args: ["--target=wasm32", entry, "-o", path.join(dir, stem + ".wasm")],
    cwd: REPO,
    ...(env ? { env, clearEnv: true } : {}),
    stdout: "piped",
    stderr: "piped",
  }).outputSync();
  if (r.code !== 0) {
    console.error(new TextDecoder().decode(r.stderr).trim());
    return { ok: false, routesChanged };
  }
  emitWebShell(appdir, stem, "");
  return { ok: true, routesChanged };
}

/**
 * `zeli dev <appdir> [--port N] [--build-only]`
 *
 * Builds the web target, watches `.loam` sources (the app plus the framework
 * `std/` trees), rebuilds on change, and serves `build/web` on localhost. Every
 * served HTML page subscribes to `/.zeli-live`, so the browser is *told* about a
 * rebuild instead of asking: on a new revision it saves the signal arena
 * (`zeus_state_snapshot` → sessionStorage) and reloads, and the loader restores
 * it on boot — an edit reflects without losing state.
 *
 * A failed compiler check is not a reload: the previous build keeps being
 * served and the revision does not move, so the tab still shows the last thing
 * that compiled while the error is on the terminal.
 */
async function dev(appdir: string, port: number, buildOnly: boolean): Promise<number> {
  if (!exists(LOAM)) die(`zeli: missing ${LOAM} — run \`make\` in ${REPO} first`);
  console.log("zeli: initial build…");
  const first = devBuildOnce(appdir);
  if (first.routesChanged) console.log("zeli: routes -> app_routes.loam");
  if (!first.ok) return 1;
  if (buildOnly) {
    console.log("zeli: dev build ok (--build-only)");
    return 0;
  }

  const watchRoots = [
    appdir,
    path.join(REPO, "packages", "zeus", "std"),
    path.join(REPO, "packages", "http", "std"),
    path.join(REPO, "packages", "loam", "std"),
  ];
  const web = path.join(appdir, "build", "web");
  /* Where the app's own backend listens. The same override `default_addr()` in
     std:http honours, so both ends agree without configuration. */
  const rpcAddr = Deno.env.get("LOAM_RPC_ADDR") ?? "127.0.0.1:8080";
  const snippet = liveSnippet();

  /* Pages subscribed to the live channel. A rebuild pushes one frame to each. */
  const clients = new Set<ReadableStreamDefaultController<Uint8Array>>();
  let rev = 1;

  const rebuild = (): void => {
    const r = devBuildOnce(appdir);
    if (r.routesChanged) console.log("zeli: routes -> app_routes.loam");
    if (!r.ok) {
      console.log(`zeli: build failed — still serving rev ${rev}`);
      return;
    }
    rev++;
    console.log(`zeli: rebuilt (rev ${rev})`);
    for (const c of clients) {
      try {
        c.enqueue(enc(sseFrame(rev)));
      } catch {
        clients.delete(c); // the page went away between frames
      }
    }
  };

  /* Change-driven, with no timer on either side. `watchFs` is what makes an edit
     reload in milliseconds, and what catches a *new* route file as it lands
     rather than when a later pass happens to look — which is the half a
     timestamp comparison cannot see. */
  watchSources(watchRoots, 50, (paths) => {
    console.log(`zeli: ${paths.length} source file(s) changed, rebuilding…`);
    rebuild();
  });

  Deno.serve({ port, hostname: "127.0.0.1" }, async (req) => {
    const u = new URL(req.url);
    if (u.pathname === LIVE_PATH) {
      let ctrl: ReadableStreamDefaultController<Uint8Array> | undefined;
      const stream = new ReadableStream<Uint8Array>({
        start(c) {
          c.enqueue(enc(sseFrame(rev))); // baseline for this page
          ctrl = c;
          clients.add(c);
        },
        cancel() {
          if (ctrl) clients.delete(ctrl);
        },
      });
      return new Response(stream, {
        headers: { "content-type": "text/event-stream", "cache-control": "no-store" },
      });
    }
    /* The app's RPC is same-origin (for wasm `default_addr()` returns ""), so a
       browser build POSTs its gRPC-Web call here rather than to the backend.
       Without this it would be answered by the SPA fallback below. */
    const ctype = req.headers.get("content-type") ?? "";
    if (req.method === "POST" && ctype.includes("grpc-web")) {
      return await proxyRpc(req, rpcAddr, u.pathname, u.search);
    }
    const reply = devReply(web, u.pathname, snippet);
    /* `readFileSync` returns a `Uint8Array<ArrayBufferLike>`, which is a valid
       body at runtime; TS 5.9's `BufferSource` just wants the narrower
       `ArrayBuffer` view, hence the cast rather than a copy. */
    return new Response(reply.body as Uint8Array<ArrayBuffer>, {
      status: reply.status,
      headers: { "content-type": reply.contentType },
    });
  });
  console.log(`zeli: dev server on http://127.0.0.1:${port}  (Ctrl-C to stop)`);
  await new Promise(() => {});
  return 0;
}

export type DevReply = { status: number; contentType: string; body: Uint8Array };

/**
 * Forward one gRPC-Web request to the app's backend, verbatim.
 *
 * A browser build calls its RPC same-origin — `default_addr()` in `std:http`
 * returns `""` for wasm — so `POST /Blog/Posts` arrives at *this* server, not at
 * the backend, and would otherwise be answered by the SPA fallback in
 * `devReply` (200 + HTML), which the client reads as a failed exchange and
 * reports as a request that never settles.
 *
 * No CORS handling is needed: the hop from the browser is same-origin, and this
 * hop is server-to-server. A missing backend is a 502, so it shows up as a
 * failure rather than as silence.
 */
export async function proxyRpc(
  req: Request,
  addr: string,
  pathname: string,
  search: string,
): Promise<Response> {
  const body = await req.arrayBuffer();
  try {
    const up = await fetch(`http://${addr}${pathname}${search}`, {
      method: "POST",
      headers: {
        "content-type": req.headers.get("content-type") ?? "application/grpc-web+proto",
        "x-grpc-web": req.headers.get("x-grpc-web") ?? "1",
      },
      body,
    });
    /* Streamed rather than buffered: the reply is already framed, and there is
       no reason to hold it in the dev server. */
    return new Response(up.body, {
      status: up.status,
      headers: {
        "content-type": up.headers.get("content-type") ?? "application/grpc-web+proto",
      },
    });
  } catch (e) {
    console.warn(`zeli: rpc ${pathname} -> ${addr} failed: ${e}`);
    return new Response(`rpc backend ${addr} unreachable\n`, {
      status: 502,
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  }
}

const enc = (s: string): Uint8Array => new TextEncoder().encode(s);

/**
 * Resolve one dev-server request against `web` (the build output dir). Pure and
 * port-free so it is unit-testable: a nested path serves its `index.html`, a
 * missing path falls back to the root shell (SPA-style), HTML gets the
 * live-reload snippet, and `.wasm`/`.js` get their content types.
 *
 * The live channel is not part of this: it is a streaming response with a
 * lifetime, so `dev` owns it (see `LIVE_PATH`).
 */
export function devReply(web: string, pathname: string, snippet: string): DevReply {
  let rel = decodeURIComponent(pathname);
  if (rel.endsWith("/")) rel += "index.html";
  let file = path.join(web, rel);
  /* A directory (or extensionless route) serves its own index.html. */
  if (!file.endsWith(".html")) {
    const idx = path.join(file, "index.html");
    if (exists(idx)) file = idx;
  }
  if (!exists(file)) file = path.join(web, "index.html");
  try {
    if (file.endsWith(".html")) {
      const body = Deno.readTextFileSync(file).replace("</body>", snippet + "</body>");
      return { status: 200, contentType: "text/html; charset=utf-8", body: enc(body) };
    }
    const ct = file.endsWith(".wasm")
      ? "application/wasm"
      : file.endsWith(".js")
      ? "text/javascript"
      : "application/octet-stream";
    return { status: 200, contentType: ct, body: Deno.readFileSync(file) };
  } catch {
    return { status: 404, contentType: "text/plain", body: enc("not found") };
  }
}

// --- zeli fmt: one command for a tree ---

/**
 * Every `.loam` under `dir`, sorted, skipping output and cache directories —
 * the same set `dev` watches, so what the formatter rewrites and what the dev
 * server rebuilds on stay the same idea of "the sources".
 */
export function loamSources(dir: string): string[] {
  const out: string[] = [];
  const walk = (d: string) => {
    let entries: Deno.DirEntry[];
    try {
      entries = [...Deno.readDirSync(d)];
    } catch {
      return;
    }
    entries.sort((a, b) => a.name.localeCompare(b.name));
    for (const e of entries) {
      if (e.name.startsWith(".") || e.name === "build" || e.name === "vendor" ||
          e.name === "node_modules") continue;
      const p = path.join(d, e.name);
      if (e.isDirectory) walk(p);
      else if (e.isFile && p.endsWith(".loam")) out.push(p);
    }
  };
  walk(dir);
  return out;
}

/**
 * `zeli fmt [dir]` — run `loam-fmt` over every `.loam` under `dir` (the current
 * directory when none is given) and write the canonical form back in place.
 *
 * One command for a tree, no flags: the formatter has exactly one style, so
 * there is nothing to choose and nothing to check separately — a file is either
 * already in that style or it is rewritten to be. Files the build generates are
 * left alone; formatting one would only be undone by the next build.
 */
function fmt(dir: string): number {
  if (!exists(FMT)) die(`zeli: missing ${FMT} — run \`make\` in ${REPO} first`);
  const files = loamSources(dir);
  const rel = (p: string) => path.relative(Deno.cwd(), p) || ".";
  if (files.length === 0) {
    console.log(`zeli: no .loam files under ${rel(dir)}`);
    return 0;
  }
  let changed = 0;
  let failed = 0;
  for (const f of files) {
    let src: string;
    try {
      src = Deno.readTextFileSync(f);
    } catch (e) {
      console.error(`zeli: ${rel(f)}: ${e}`);
      failed++;
      continue;
    }
    if (src.startsWith("// Generated by `zeli")) continue;
    const r = new Deno.Command(FMT, { args: [f], stdout: "piped", stderr: "piped" })
      .outputSync();
    if (r.code !== 0) {
      console.error(`zeli: ${rel(f)}: ${new TextDecoder().decode(r.stderr).trim()}`);
      failed++;
      continue;
    }
    const want = new TextDecoder().decode(r.stdout);
    if (want !== src) {
      Deno.writeTextFileSync(f, want);
      console.log(`zeli: formatted ${rel(f)}`);
      changed++;
    }
  }
  console.log(
    `zeli: ${files.length} file(s), ${changed} formatted${failed ? `, ${failed} failed` : ""}`,
  );
  return failed ? 1 : 0;
}

// --- zeli new: scaffold the §6.2 app layout ---

/** The scaffolded tree. Contents are filled in by `scaffold`. */
function scaffoldFiles(name: string): Record<string, string> {
  const sub = (s: string) => s.split("__NAME__").join(name);
  return {
    "zeus.toml": sub(
      `name = "__NAME__"\n` +
        `targets = ["web", "macos", "ios", "android", "server"]\n` +
        `routes = "routes"\n` +
        `theme = "theme.loam"\n` +
        `revalidate = 60\n`,
    ),
    ".gitignore": `.zeus/\nbuild/\nvendor/\napp_routes.loam\n`,

    // One entry for every host — not four copies of app.loam.
    "app.loam": sub(
      `//! __NAME__ — application entry. One entry, every host.\n` +
        `//!\n` +
        `//! \`zeli routes\` generates app_routes.loam from routes/; this file\n` +
        `//! mounts it. \`zeli build\` emits all five targets from here.\n\n` +
        `import "std:zeus"\n` +
        `import "app_routes.loam" as approutes\n` +
        `import "server/api.loam" as api\n` +
        `import "tests/smoke.loam" as smoketest\n\n` +
        `fn main() {\n` +
        `    zeus.App("__NAME__", fn() {\n` +
        `        approutes.app()\n` +
        `    })\n` +
        `}\n`,
    ),

    "theme.loam":
      `//! Theme tokens: palette roles. \`zeus.theme(ROLE.X, light, dark)\` sets\n` +
      `//! both appearances; widgets resolve the role per appearance at paint time.\n\n` +
      `import "std:zeus"\n\n` +
      `fn install() {\n` +
      `    zeus.theme(ROLE.Accent, 0x2563EB, 0x60A5FA)\n` +
      `    zeus.theme(ROLE.Paper, 0xFFFFFF, 0x111318)\n` +
      `    zeus.theme(ROLE.Ink, 0x111318, 0xF3F4F6)\n` +
      `}\n`,

    // routes/
    "routes/layout.loam":
      `//! Root shell: theme + nav. Retained across navigation, so its state\n` +
      `//! survives — only the page slot rebuilds.\n\n` +
      `import "std:zeus"\n` +
      `import "../theme.loam" as theme\n\n` +
      `fn layout(build: fn()) {\n` +
      `    theme.install()\n` +
      `    let box = zeus.Box(align_direction = DIRECTION.Column, padding = 12, spacing = 8)\n` +
      `    zeus.slot(box, fn() {\n` +
      `        zeus.Text("myapp")\n` +
      `    })\n` +
      `    zeus.slot(box, build)\n` +
      `}\n`,
    "routes/page.loam":
      `import "std:zeus"\n` +
      `import "std:router"\n` +
      `import "../components/card.loam" as card\n\n` +
      `fn page(p: Params) {\n` +
      `    card.Card("Welcome to myapp.")\n` +
      `}\n\n` +
      `fn meta(p: Params) -> Meta {\n` +
      `    return Meta { title: "myapp", description: "A Zeus app." }\n` +
      `}\n`,
    "routes/loading.loam":
      `//! Shown while a route's loaders are pending.\n\n` +
      `import "std:zeus"\n\n` +
      `fn loading() {\n` +
      `    zeus.Text("loading...")\n` +
      `}\n`,
    "routes/error.loam":
      `//! Route error page. A trap in a page's build lands here (see zeus.Boundary).\n\n` +
      `import "std:zeus"\n\n` +
      `fn error(msg: string) {\n` +
      `    zeus.Text("something went wrong")\n` +
      `}\n`,
    "routes/not-found.loam":
      `import "std:zeus"\n\n` +
      `fn not_found() {\n` +
      `    zeus.Text("not found")\n` +
      `}\n`,

    "routes/blog/layout.loam":
      `import "std:zeus"\n\n` +
      `fn layout(build: fn()) {\n` +
      `    let box = zeus.Box(align_direction = DIRECTION.Column, padding = 8, spacing = 4)\n` +
      `    zeus.slot(box, fn() {\n` +
      `        zeus.Text("blog")\n` +
      `    })\n` +
      `    zeus.slot(box, build)\n` +
      `}\n`,
    "routes/blog/loader.loam":
      `//! Blog list query. Make \`posts\` a \`#[server] fn\` to fetch it from the\n` +
      `//! backend; it stays a plain fn here so the scaffold runs on every host.\n\n` +
      `fn posts() -> []string {\n` +
      `    return []string { "hello world", "second post" }\n` +
      `}\n`,
    "routes/blog/page.loam":
      `import "std:zeus"\n` +
      `import "std:router"\n` +
      `import "loader.loam" as blog_loader\n\n` +
      `fn page(p: Params) {\n` +
      `    let ps = blog_loader.posts()\n` +
      `    for i in 0..ps.len {\n` +
      `        zeus.Text(ps[i])\n` +
      `    }\n` +
      `}\n\n` +
      `fn meta(p: Params) -> Meta {\n` +
      `    return Meta { title: "Blog" }\n` +
      `}\n`,
    "routes/blog/[slug]/loader.loam":
      `//! One post by slug. In a real app this is\n` +
      `//! \`#[server] fn get_post(slug: string) -> Post\`.\n\n` +
      `fn get_post(slug: string) -> string {\n` +
      `    return slug\n` +
      `}\n`,
    "routes/blog/[slug]/page.loam":
      `import "std:zeus"\n` +
      `import "std:router"\n` +
      `import "loader.loam" as post_loader\n\n` +
      `fn page(p: Params) {\n` +
      `    let slug = router.param(p, "slug")\n` +
      `    zeus.Text(post_loader.get_post(slug))\n` +
      `}\n\n` +
      `// Static routes are enumerated at build time from \`paths()\`.\n` +
      `fn paths() -> []string {\n` +
      `    return []string { "/blog/hello-world", "/blog/second" }\n` +
      `}\n\n` +
      `fn meta(p: Params) -> Meta {\n` +
      `    return Meta { title: router.param(p, "slug") }\n` +
      `}\n`,
    "routes/blog/[slug]/error.loam":
      `import "std:zeus"\n\n` +
      `fn error(msg: string) {\n` +
      `    zeus.Text("post not available")\n` +
      `}\n`,

    "routes/(marketing)/about/page.loam":
      `import "std:zeus"\n` +
      `import "std:router"\n\n` +
      `fn page(p: Params) {\n` +
      `    zeus.Text("about")\n` +
      `}\n\n` +
      `fn meta(p: Params) -> Meta {\n` +
      `    return Meta { title: "About" }\n` +
      `}\n`,
    "routes/(marketing)/pricing/page.loam":
      `import "std:zeus"\n` +
      `import "std:router"\n\n` +
      `fn page(p: Params) {\n` +
      `    zeus.Text("pricing")\n` +
      `}\n\n` +
      `fn meta(p: Params) -> Meta {\n` +
      `    return Meta { title: "Pricing" }\n` +
      `}\n`,

    "components/card.loam":
      `//! Shared component, no routing knowledge: a fn that builds into the\n` +
      `//! current host.\n\n` +
      `import "std:zeus"\n\n` +
      `fn Card(text: string) {\n` +
      `    let box = zeus.Box(background = "#FFFFFF", padding = 12, spacing = 4)\n` +
      `    zeus.slot(box, fn() {\n` +
      `        zeus.Text(text)\n` +
      `    })\n` +
      `}\n`,

    "server/api.loam":
      `//! RPC surface: \`#[proto]\` contracts and \`#[server]\` fns. The entry\n` +
      `//! imports this, so the same source compiles to a direct call on native\n` +
      `//! and an RPC boundary on the client.\n\n` +
      `import "std:http"\n\n` +
      `#[proto]\n` +
      `struct HelloReq {\n` +
      `    name: string,\n` +
      `}\n\n` +
      `#[proto]\n` +
      `struct HelloResp {\n` +
      `    greeting: string,\n` +
      `}\n\n` +
      `#[server]\n` +
      `fn hello(req: HelloReq) -> HelloResp {\n` +
      `    return HelloResp { greeting: req.name }\n` +
      `}\n`,
    "server/db.loam":
      `//! Database access belongs here, behind \`#[server]\` fns so handles and\n` +
      `//! secrets never reach a client binary.\n`,

    "assets/README.md":
      `# assets\n\n` +
      `Images, fonts, and icons live here. \`zeli build\` will compile them into\n` +
      `typed handles (\`assets.hero\`) once the asset pipeline lands.\n`,
    "public/robots.txt": `User-agent: *\nAllow: /\n`,

    "tests/smoke.loam":
      `//! \`#[test]\` fns. Import this from the entry and run \`loam test app.loam\`.\n\n` +
      `import "std:test"\n\n` +
      `fn add(a: int, b: int) -> int {\n` +
      `    return a + b\n` +
      `}\n\n` +
      `#[test]\n` +
      `fn adds() {\n` +
      `    assert_eq_int(add(1, 2), 3)\n` +
      `}\n`,
  };
}

/**
 * `zeli new <name> [dir]` — scaffold the §6.2 tree. Refuses a non-empty target.
 */
function scaffold(appdir: string, name: string): number {
  if (!/^[A-Za-z_][A-Za-z0-9_-]*$/.test(name)) die(`zeli: bad app name '${name}'`);
  if (exists(appdir)) {
    let nonEmpty = false;
    for (const _ of Deno.readDirSync(appdir)) {
      nonEmpty = true;
      break;
    }
    if (nonEmpty) die(`zeli: ${appdir} already exists and is not empty`);
  }
  const files = scaffoldFiles(name);
  for (const [rel, body] of Object.entries(files)) {
    const p = path.join(appdir, rel);
    Deno.mkdirSync(path.dirname(p), { recursive: true });
    Deno.writeTextFileSync(p, body);
  }
  // app.loam imports app_routes.loam, so generate it now: a fresh app must
  // typecheck under plain `loam` (and the LSP) before anyone runs a build.
  generate(appdir);
  console.log(`zeli: created ${name} (${Object.keys(files).length + 1} files)`);
  console.log(`  cd ${path.relative(Deno.cwd(), appdir) || "."} && zeli dev`);
  return 0;
}

function parseTargets(arg: string | undefined): TargetName[] {
  if (!arg) return ALL_TARGETS;
  const out: TargetName[] = [];
  for (const raw of arg.split(",")) {
    const t = raw.trim() as TargetName;
    if (!ALL_TARGETS.includes(t)) die(`zeli: unknown target '${raw}' (want ${ALL_TARGETS.join(", ")})`);
    out.push(t);
  }
  return out;
}

async function main(args: string[]): Promise<number> {
  if (args.length >= 2 && args[0] === "new") {
    const name = args[1];
    return scaffold(path.resolve(args[2] ?? name), name);
  }
  if (args.length >= 2 && args[0] === "routes") {
    const out = generate(path.resolve(args[1]));
    console.log(`zeli: wrote ${out}`);
    return 0;
  }
  if (args.length >= 2 && args[0] === "dev") {
    let port = 5173;
    let buildOnly = false;
    const rest: string[] = [];
    for (let i = 1; i < args.length; i++) {
      if (args[i] === "--port") port = Number(args[++i]);
      else if (args[i] === "--build-only") buildOnly = true;
      else rest.push(args[i]);
    }
    return await dev(path.resolve(rest[0] ?? "."), port, buildOnly);
  }
  if (args.length >= 2 && args[0] === "pkg" && args[1] === "sync") {
    const dir = args[2] ? path.resolve(args[2]) : Deno.cwd();
    return pkgSync(dir);
  }
  if (args.length >= 1 && args[0] === "fmt") {
    return fmt(path.resolve(args[1] ?? "."));
  }
  if (args.length >= 2 && args[0] === "build") {
    let targets = ALL_TARGETS;
    let base = "";
    const rest: string[] = [];
    for (let i = 1; i < args.length; i++) {
      if (args[i] === "--targets") targets = parseTargets(args[++i]);
      else if (args[i] === "--base") base = args[++i] ?? "";
      else rest.push(args[i]);
    }
    if (rest.length < 1) die("usage: zeli build <appdir> [--targets a,b] [--base https://host]");
    return build(path.resolve(rest[0]), targets, base);
  }
  console.error(
    "usage: zeli new <name> [dir] | zeli routes <appdir> | zeli build <appdir> [--targets a,b] [--base https://host] | " +
      "zeli dev <appdir> [--port N] [--build-only] | zeli fmt [dir] | zeli pkg sync <appdir>",
  );
  return 2;
}

if (import.meta.main) Deno.exit(await main(Deno.args));
