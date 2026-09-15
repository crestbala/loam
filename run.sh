#!/bin/sh
# Run any example in this repo.
#
#   ./run.sh                      list what is runnable
#   ./run.sh fizzbuzz             a language example (examples/language/)
#   ./run.sh dashboard            a Zeus app (examples/zeus/), Cocoa desktop
#   ./run.sh gallery web          Vite wasm UI (http://127.0.0.1:5174)
#   ./run.sh dashboard ios        ...on another host: native | web | wasm32 | ios | android
#   ./run.sh counter              the full-stack example (backend :8080 + web UI :5173)
#   ./run.sh counter macos        ...as a native/ios/android client
#
# Names are the file/directory stem. A bare name that matches exactly one
# example works directly. `counter` is both a language demo and the full-stack
# Zeus app: `./run.sh counter` is the Zeus stack; the language file is
# `./run.sh language/counter`. `zeus/counter` is an alias for the stack.
#
# GUI examples open a real window and servers block until Ctrl-C. `make test`
# exports ZEUS_HEADLESS=1/MAYA_HEADLESS=1 to render one frame and exit; `run.sh`
# clears those so a value left exported in your shell cannot silently build a
# window-less binary that exits before anything appears. To render one frame on
# purpose, invoke `bin/yugac` with ZEUS_HEADLESS=1 directly (see the Makefile).
set -e

unset ZEUS_HEADLESS LOAM_HEADLESS MAYA_HEADLESS

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
LANGDIR=$HERE/examples/language
ZEUSDIR=$HERE/examples/zeus
YUGAC=$HERE/bin/yugac

die() { echo "run.sh: $*" >&2; exit 1; }

list() {
  echo "language examples  (./run.sh <name>)"
  for f in "$LANGDIR"/*.loam; do
    [ -e "$f" ] || continue
    printf '  %s\n' "$(basename "$f" .loam)"
  done
  echo
  echo "zeus apps          (./run.sh <name> [native|web|wasm32|ios|android])"
  for d in "$ZEUSDIR"/*/; do
    d=${d%/}
    name=$(basename "$d")
    if [ -f "$d/zeus.toml" ]; then
      printf '  %-18s routes/ app via the zeus CLI\n' "$name"
    elif [ -f "$d/$name.loam" ]; then
      printf '  %s\n' "$name"
    fi
  done
  echo
  echo "full stack         (./run.sh counter [web|backend|macos|ios|android])"
  [ -d "$ZEUSDIR/counter" ] && echo "  counter"
  echo
  echo "playground         (./run.sh repl [web|backend|frontend|macos])"
  echo "  repl               edit Loam in the browser; Run is Playground.Run over gRPC-Web"
  echo
  echo "docs               (./run.sh www [web|backend|frontend|macos])"
  echo "  www                Zeus docs; UI :5175, Docs.Page :8082"
}

ensure_yugac() {
  if [ ! -x "$YUGAC" ]; then
    echo "run.sh: building the compiler"
    make -C "$HERE" -j4
  fi
}

# Nothing runs from this script until `yugac check` is clean. The check is the
# same frontend a build runs, so it costs one parse and catches the errors that
# would otherwise surface as a broken window, a trapping wasm module, or a
# Vite server happily serving a stale .wasm from the last good build.
check_yuga() {
  src=$1
  ensure_yugac
  if ! "$YUGAC" check "$src"; then
    die "$src failed the compiler check (nothing was run)"
  fi
}

# The full-stack example ships its own per-host launchers.
run_counter_stack() {
  variant=${1:-web}
  case $variant in
    web|"")   exec "$ZEUSDIR/counter/run.sh" ;;
    backend)  exec "$ZEUSDIR/counter/backend/run.sh" ;;
    frontend) exec "$ZEUSDIR/counter/frontend/run.sh" ;;
    macos)    exec "$ZEUSDIR/counter/macos/run.sh" ;;
    ios)      exec "$ZEUSDIR/counter/ios/run.sh" ;;
    android)  exec "$ZEUSDIR/counter/android/run.sh" ;;
    *) die "unknown counter variant '$variant' (web backend frontend macos ios android)" ;;
  esac
}

run_repl_stack() {
  variant=${1:-web}
  case $variant in
    web|"")   exec "$ZEUSDIR/repl/run.sh" ;;
    backend)  exec "$ZEUSDIR/repl/backend/run.sh" ;;
    frontend) exec "$ZEUSDIR/repl/frontend/run.sh" ;;
    macos)    exec "$ZEUSDIR/repl/macos/run.sh" ;;
    *) die "unknown repl variant '$variant' (web backend frontend macos)" ;;
  esac
}

run_gallery_stack() {
  variant=${1:-web}
  case $variant in
    web|frontend|"") exec "$ZEUSDIR/gallery/frontend/run.sh" ;;
    macos|native)    exec "$ZEUSDIR/gallery/macos/run.sh" ;;
    ios)             exec "$ZEUSDIR/gallery/ios/run.sh" ;;
    android)         exec "$ZEUSDIR/gallery/android/run.sh" ;;
    backend)         exec "$ZEUSDIR/gallery/backend/run.sh" ;;
    *) die "unknown gallery variant '$variant' (web frontend macos ios android backend)" ;;
  esac
}

# Canvas2D wasm via Vite (same stack as counter/frontend). Ctrl-C stops it.
# Override the port with ZEUS_WEB_PORT (default 5174).
run_zeus_web() {
  name=$1
  src=$ZEUSDIR/$name/$name.loam
  port=${ZEUS_WEB_PORT:-5174}
  web=$HERE/packages/zeus
  [ -f "$src" ] || die "no Zeus app at $src"
  if [ ! -f "$ZEUSDIR/$name/index.html" ]; then
    cat > "$ZEUSDIR/$name/index.html" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>$name</title>
  <style>
    html, body { margin: 0; height: 100%; }
    canvas { display: block; width: 100%; height: 100%; touch-action: none; }
  </style>
</head>
<body>
  <canvas id="zeus" data-wasm="/$name.wasm"></canvas>
  <script src="/loader.js"></script>
</body>
</html>
EOF
  fi
  if [ ! -d "$web/node_modules" ]; then
    echo "run.sh: npm install (vite)"
    (cd "$web" && npm install)
  fi
  echo "run.sh: $name wasm at http://127.0.0.1:$port/  (Vite, Ctrl-C to stop)"
  export ZEUS_APP=$name
  export ZEUS_WEB_PORT=$port
  cd "$web" || exit 1
  exec npx vite --config hosts/web/vite.config.js
}

# Same entry resolution as the zeus CLI's findEntry().
zeus_entry() {
  d=$1
  for f in app.loam "$(basename "$d").loam" main.loam; do
    [ -f "$d/$f" ] && { printf '%s\n' "$d/$f"; return 0; }
  done
  return 1
}

# A zeus.toml app (routes/ tree, generated route table) is driven by the zeus
# CLI, not by pointing yugac at <name>/<name>.loam — that file does not exist
# for these. Regenerate the route table first so a new routes/ file is picked
# up, then gate on `yugac check` like every other path here.
run_zeus_framework_app() {
  name=$1
  target=${2:-native}
  appdir=$ZEUSDIR/$name
  ensure_yugac
  "$HERE/bin/zeus" routes "$appdir" >/dev/null || die "zeus routes failed for $name"
  entry=$(zeus_entry "$appdir") || die "no entry .loam in $appdir"
  check_yuga "$entry"
  case $target in
    native|macos) exec "$YUGAC" --run "$entry" ;;
    wasm32|wasm|web) exec "$HERE/bin/zeus" dev "$appdir" ;;
    ios|android) exec "$YUGAC" "--target=$target" --run "$entry" ;;
    build) exec "$HERE/bin/zeus" build "$appdir" ;;
    *) die "unknown target '$target' (native macos web ios android build)" ;;
  esac
}

run_zeus_app() {
  name=$1
  target=${2:-native}
  if [ -f "$ZEUSDIR/$name/zeus.toml" ]; then
    run_zeus_framework_app "$name" "$target"
  fi
  check_yuga "$ZEUSDIR/$name/$name.loam"
  case $target in
    native) exec "$YUGAC" --run "$ZEUSDIR/$name/$name.loam" ;;
    web) run_zeus_web "$name" ;;
    wasm32|wasm)
      exec "$YUGAC" --target=wasm32 --run "$ZEUSDIR/$name/$name.loam" ;;
    ios|android)
      exec "$YUGAC" "--target=$target" --run "$ZEUSDIR/$name/$name.loam" ;;
    *) die "unknown target '$target' (native web wasm32 ios android)" ;;
  esac
}

run_language() {
  name=$1
  check_yuga "$LANGDIR/$name.loam"
  # oob.loam exists to prove the bounds check traps, so a nonzero exit from the
  # program is the expected outcome. A compile error is still a real failure,
  # so build and run as separate steps rather than using --run.
  if [ "$name" = oob ]; then
    out=$LANGDIR/build/oob
    mkdir -p "$LANGDIR/build"
    "$YUGAC" "$LANGDIR/oob.loam" -o "$out" || die "oob.loam failed to compile"
    echo "run.sh: oob.loam is expected to trap on an out-of-bounds index"
    if "$out"; then
      die "oob.loam did not trap"
    fi
    echo "run.sh: trapped as expected"
    exit 0
  fi
  exec "$YUGAC" --run "$LANGDIR/$name.loam"
}

[ $# -eq 0 ] && { list; exit 0; }

what=$1
shift

case $what in
  list|-l|--list|-h|--help) list; exit 0 ;;
  www|docs) exec "$HERE/www/run.sh" "$@" ;;
  language/*) run_language "${what#language/}" ;;
  repl|zeus/repl) run_repl_stack "$@" ;;
  counter|zeus/counter) run_counter_stack "$@" ;;
  gallery|zeus/gallery) run_gallery_stack "$@" ;;
  zeus/*)     run_zeus_app "${what#zeus/}" "$@" ;;
esac

# Bare name: resolve it, and refuse if it is ambiguous.
is_lang=0; is_zeus=0
[ -f "$LANGDIR/$what.loam" ] && is_lang=1
{ [ -f "$ZEUSDIR/$what/$what.loam" ] || [ -d "$ZEUSDIR/$what" ]; } && is_zeus=1

if [ "$is_lang" = 1 ] && [ "$is_zeus" = 1 ]; then
  die "'$what' is ambiguous; use language/$what or zeus/$what"
fi

if [ "$is_lang" = 1 ]; then
  run_language "$what"
elif [ "$is_zeus" = 1 ]; then
  if [ "$what" = counter ]; then
    run_counter_stack "$@"
  elif [ "$what" = gallery ]; then
    run_gallery_stack "$@"
  elif [ "$what" = repl ]; then
    run_repl_stack "$@"
  else
    run_zeus_app "$what" "$@"
  fi
else
  echo "run.sh: no example named '$what'" >&2
  echo >&2
  list >&2
  exit 1
fi
