# Shared by tools/cross-check.sh and tools/win-bundle.sh (sourced, not run).
# Which fixtures are GUI-only, and what extra C each fixture links: the
# `#![c_source]` / `#![c_link]` directives of the fixture and of the packages
# it imports.

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


# Bindings that name their libraries (`#[c("sym", "link -lws2_32")]`), one
# line each: `oslist|sym|libs`, where `oslist` is the `#[os(...)]` just above
# the binding (empty for every OS).
binding_lib_table() {
    for f in packages/loam/std/*.loam packages/*/src/*.loam; do
        awk '
            /^#\[os\(/ { os = $0; gsub(/^#\[os\(|\)\]$|"| /, "", os); next }
            /^#\[c\("[A-Za-z0-9_]+", "link [^"]*"\)\]/ {
                sym = $0; sub(/^#\[c\("/, "", sym); sub(/".*/, "", sym)
                libs = $0; sub(/^[^,]*, "link /, "", libs); sub(/"\)\]$/, "", libs)
                print os "|" sym "|" libs
            }
            !/^#\[/ { os = "" }
        ' "$f"
    done
}

# The libraries `$3` (a fixture's C) needs on OS `$1` (`linux`, `windows`):
# those of every binding in table `$2` for that OS whose symbol the C names.
# `posix` covers linux. Each library once.
binding_libs_for() {
    want_os=$1 table=$2 c=$3
    while IFS='|' read -r oses sym libs; do
        case ",$oses," in
        ,,|*,"$want_os",*) ;;
        *,posix,*) [ "$want_os" = windows ] && continue ;;
        *) continue ;;
        esac
        grep -qF "LOAM_CSYM(\"$sym\")" "$c" && echo "$libs"
    done < "$table" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' '
}
