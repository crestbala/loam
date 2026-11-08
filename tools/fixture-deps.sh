# Shared by tools/cross-check.sh and tools/win-bundle.sh (sourced, not run).
# Which fixtures are GUI-only, and what extra C each fixture links: the
# `#![c_source]` / `#![c_link]` directives of the fixture and of the packages
# it imports. `runtime/net.c` is added by the callers when the C calls std:net.

skip='^(zeus_|maya_|raygui_|zui_|counter_panel)'
gui='import "(zeus|maya|raygui|zui)'

# `#![c_source("p", "-flag"...)]` lines of $1 as `dir|path|flags`, and
# `#![c_link(...)]` flags as `link||flags`.
directives() {
    d=$(dirname "$1")
    grep -hE '^#!\[c_(source|link)\(' "$1" 2>/dev/null | while IFS= read -r line; do
        args=$(printf '%s\n' "$line" | grep -oE '"[^"]*"' | tr -d '"' | tr '\n' ' ')
        case $line in
        '#![c_source'*) set -- $args; p=$1; shift; echo "$d|$p|$*" ;;
        *) echo "link||$args" ;;
        esac
    done
}

# The directives of a fixture and of every package it imports (`import "pkg"`
# or `import "pkg:mod"`, a directory under packages/).
deps_of() {
    directives "$1"
    for pkg in $(grep -oE '^import "[a-z_]+(:[^"]*)?"' "$1" | sed -E 's/import "([a-z_]+).*/\1/' | sort -u); do
        [ -d "packages/$pkg/src" ] || continue
        for m in $(find "packages/$pkg/src" -name '*.loam'); do directives "$m"; done
    done
}

