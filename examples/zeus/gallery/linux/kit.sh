#!/bin/sh
# Package the gallery as a self-contained Linux build kit, on any OS with the
# loam repo: the generated C, the Zeus runtime, and the X11 host, plus a
# build.sh that needs only cc and libx11-dev on the Linux machine.
#
#   ./kit.sh              -> ../build/linux-kit/ and ../build/gallery-linux-kit.tar.gz
#   ./kit.sh OUT_DIR
set -e
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
d=$HERE
REPO=
while [ "$d" != / ]; do
  if [ -f "$d/nob.sh" ] && [ -d "$d/packages/loam/compiler" ]; then
    REPO=$d
    break
  fi
  d=$(CDPATH= cd -- "$d/.." && pwd)
done
if [ -z "$REPO" ]; then
  echo "kit.sh: could not find the loam repo (nob.sh + packages/loam/compiler/)" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/loamc" ]; then
  echo "kit.sh: building loam"
  (cd "$REPO" && ./nob.sh)
fi
OUT=${1:-$HERE/../build/linux-kit}
RT=$REPO/packages/loam/runtime
rm -rf "$OUT"
mkdir -p "$OUT/runtime"
"$REPO/bin/loamc" --emit-c "$HERE/app.loam" -o "$OUT/app.c"
cp "$RT/loam_rt.h" "$RT/zeus_rt.h" "$RT/zeus_key.h" "$RT/zeus_plat.c" "$RT/zeus_key.c" \
  "$REPO/packages/zeus/hosts/desktop/linux.c" "$OUT/runtime/"
cat > "$OUT/build.sh" <<'BUILD'
#!/bin/sh
# Build the zeus gallery for Linux/X11. Needs cc and the X11 headers:
#   Debian/Ubuntu: sudo apt install build-essential libx11-dev
#   Fedora:        sudo dnf install gcc libX11-devel
#   Arch:          sudo pacman -S base-devel libx11
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
mkdir -p obj
# The same flags `loamc --target native` uses on Linux.
for f in zeus_plat zeus_key linux; do
  $CC -std=gnu99 -O1 -c -Iruntime -DLOAM_ALLOC_TRACE "runtime/$f.c" -o "obj/$f.o"
done
$CC -std=gnu99 -O1 -fno-asynchronous-unwind-tables -fomit-frame-pointer -ffp-contract=off \
  -DLOAM_ALLOC_TRACE -DLOAM_STRING_OWNS -Iruntime -w app.c obj/zeus_plat.o obj/zeus_key.o obj/linux.o \
  -Wl,--gc-sections -lX11 -lm -o gallery
echo "built ./gallery — run it with ./gallery (needs an X11 or XWayland display)"
BUILD
chmod +x "$OUT/build.sh"
cat > "$OUT/README.txt" <<'README'
Zeus gallery — Linux/X11 build kit

  ./build.sh      compile (cc + libx11-dev)
  ./gallery       run (X11 display, or XWayland on a Wayland desktop)

The kit is generated C, so it builds for the machine it is built on
(x86_64, arm64, ...). Text uses the X server's core font.
README
tar -czf "$OUT/../gallery-linux-kit.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
echo "kit: $OUT"
echo "tarball: $(CDPATH= cd -- "$OUT/.." && pwd)/gallery-linux-kit.tar.gz"
