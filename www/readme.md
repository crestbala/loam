# Loam

The language site. One Zeus entry, the wasm web target, pages built with
zeus-components.

```
www/
  app.loam           entry
  zeus.toml          targets = ["web"]
  routes/            pages. app_routes.loam is generated
  components/        shell, prose, the editor, and the browser runner
  tests/             runner tests and a headless route drive
```

Routes: `/`, `/language` (ownership, control, errors, modules, functions),
`/types` (numbers, aggregates, text, memory), `/zeus` (signals, views,
routing, hosts), `/components` (a curated set of zeus-components, live),
`/tooling` (fmt, tests), `/playground`. A sidebar lists every section on
wide screens and folds away on narrow ones.

## Run

From the repo root:

```
./bin/zeli serve www --target web
./run.sh www
```

## Publish

```
./bin/zeli build www --targets web --base https://example.com
```

`build/web` is the static site: one HTML file per route, `app.wasm`, and
`loader.js`. Pass the public origin as `--base` so the canonical URLs and
the sitemap name that host. Do not commit `build/`.

The wasm build records source locations relative to the directory you run
the compiler from, and drops every other absolute path to a file name. After
a build, the output must not contain a home directory. The pages do not call
the filesystem. The playground runs programs in `components/runner.loam`, a tree-walking
interpreter for the everyday core of Loam: functions, recursion, closures,
structs, enums, arrays, `if` / `while` / `for` / `switch`, interpolation, and
method calls. It imports `std:fmt`, `std:str`, and `std:math`, refuses file
and host imports, traps on i32 overflow, and stops after a million steps or
160 nested calls. `components/editor.loam` is the code editor (selection,
clipboard, undo, auto-indent, Cmd/Ctrl+Enter to run).

## Tests

```
./bin/loamc test www/tests/smoke.loam
ZEUS_HEADLESS=1 ./bin/loamc www/tests/routes.loam -o /tmp/loam-www-routes
/tmp/loam-www-routes
```
