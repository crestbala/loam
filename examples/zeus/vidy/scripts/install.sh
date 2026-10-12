#!/bin/sh
# Build Vidy.app and install it, like any Mac app.
#
#   sh examples/zeus/vidy/scripts/install.sh               # /Applications/Vidy.app
#   sh examples/zeus/vidy/scripts/install.sh --user        # ~/Applications/Vidy.app
#   sh examples/zeus/vidy/scripts/install.sh --dest DIR    # DIR/Vidy.app
#   sh examples/zeus/vidy/scripts/install.sh --uninstall   # remove it (same flags)
#
# `zeli build` writes the bundle (build/macos/Vidy.app: icon from
# assets/icon.svg, document types from macos/Info.plist.part, ad-hoc signed);
# this copies it into place and registers it, so Finder offers
# "Open With > Vidy". An existing Vidy.app is replaced only when it is this app.
set -eu

ID=com.loam.vidy
NAME=Vidy
DEST=/Applications
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --user) DEST="$HOME/Applications" ;;
        --dest) shift; DEST="${1:?--dest needs a directory}" ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $1 (try --help)"; exit 2 ;;
    esac
    shift
done

[ "$(uname -s)" = Darwin ] || { echo "install.sh: Vidy.app is macOS only"; exit 1; }

APP=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$APP/../../.." && pwd)
TARGET="$DEST/$NAME.app"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

# Refuse to touch a Vidy.app that someone else shipped.
ours() {
    [ -f "$1/Contents/Info.plist" ] &&
        [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$1/Contents/Info.plist" 2>/dev/null)" = "$ID" ]
}

if [ -e "$TARGET" ] && ! ours "$TARGET"; then
    echo "install.sh: $TARGET exists and is not $ID; leaving it alone"
    exit 1
fi

if [ "$UNINSTALL" = 1 ]; then
    if [ -e "$TARGET" ]; then
        "$LSREGISTER" -u "$TARGET" 2>/dev/null || true
        rm -rf "$TARGET"
        echo "removed $TARGET"
    else
        echo "nothing at $TARGET"
    fi
    exit 0
fi

cd "$ROOT"
[ -x ./bin/zeli ] || { echo "install.sh: missing bin/zeli: run make in $ROOT first"; exit 1; }
./bin/zeli build "$APP" --targets macos
BUNDLE="$APP/build/macos/$NAME.app"
ours "$BUNDLE" || { echo "install.sh: the build wrote no $BUNDLE"; exit 1; }

mkdir -p "$DEST"
[ -w "$DEST" ] || { echo "install.sh: $DEST is not writable; rerun with --user, or with sudo"; exit 1; }
# Quit a running copy first, so the new one is what opens next.
osascript -e "tell application id \"$ID\" to quit" >/dev/null 2>&1 || true
rm -rf "$TARGET"
ditto "$BUNDLE" "$TARGET"
"$LSREGISTER" -f "$TARGET" 2>/dev/null || true

echo "installed $TARGET"
echo "  open -a $NAME                 # launch"
echo "  open -a $NAME movie.mp4       # open a file"
