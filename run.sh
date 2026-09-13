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

unset ZEUS_HEADLESS YUGA_HEADLESS MAYA_HEADLESS

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
LANGDIR=$HERE/examples/language
ZEUSDIR=$HERE/examples/zeus
YUGAC=$HERE/bin/yugac

die() { echo "run.sh: $*" >&2; exit 1; }

list() {
  echo "language examples  (./run.sh <name>)"
  for f in "$LANGDIR"/*.yuga; do
    [ -e "$f" ] || continue
    printf '  %s\n' "$(basename "$f" .yuga)"
  done
  echo
  echo "zeus apps          (./run.sh <name> [native|web|wasm32|ios|android])"
  for d in "$ZEUSDIR"/*/; do
    d=${d%/}
    name=$(basename "$d")
    [ -f "$d/$name.yuga" ] || continue
    printf '  %s\n' "$name"
  done
  echo
  echo "full stack         (./run.sh counter [web|backend|macos|ios|android])"
  [ -d "$ZEUSDIR/counter" ] && echo "  counter"
  echo
  echo "playground         (./run.sh repl [web|backend|frontend|macos])"
  echo "  repl               edit Yuga in the browser; Run is Playground.Run over gRPC-Web"
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
  src=$ZEUSDIR/$name/$name.yuga
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
  exec npx vite --config web/vite.config.js
}

run_zeus_app() {
  name=$1
  target=${2:-native}
  ensure_yugac
  case $target in
    native) exec "$YUGAC" --run "$ZEUSDIR/$name/$name.yuga" ;;
    web) run_zeus_web "$name" ;;
    wasm32|wasm)
      exec "$YUGAC" --target=wasm32 --run "$ZEUSDIR/$name/$name.yuga" ;;
    ios|android)
      exec "$YUGAC" "--target=$target" --run "$ZEUSDIR/$name/$name.yuga" ;;
    *) die "unknown target '$target' (native web wasm32 ios android)" ;;
  esac
}

run_language() {
  name=$1
  ensure_yugac
  # oob.yuga exists to prove the bounds check traps, so a nonzero exit from the
  # program is the expected outcome. A compile error is still a real failure,
  # so build and run as separate steps rather than using --run.
  if [ "$name" = oob ]; then
    out=$LANGDIR/build/oob
    mkdir -p "$LANGDIR/build"
    "$YUGAC" "$LANGDIR/oob.yuga" -o "$out" || die "oob.yuga failed to compile"
    echo "run.sh: oob.yuga is expected to trap on an out-of-bounds index"
    if "$out"; then
      die "oob.yuga did not trap"
    fi
    echo "run.sh: trapped as expected"
    exit 0
  fi
  exec "$YUGAC" --run "$LANGDIR/$name.yuga"
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
[ -f "$LANGDIR/$what.yuga" ] && is_lang=1
{ [ -f "$ZEUSDIR/$what/$what.yuga" ] || [ -d "$ZEUSDIR/$what" ]; } && is_zeus=1

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
