#!/bin/bash
# Parse every .loam in the repo with the Tree-sitter grammar and fail on any
# ERROR/MISSING node outside the known-gap list below.
#
# This exists because the grammar is a second, hand-maintained parser: nothing
# else in `make test` reads it, so language changes (hex literals, `import ..
# as`, bitwise operators, `fn() => expr`) landed in loam and silently rotted
# the editor experience — the Zed/VS Code extensions highlight from this
# grammar, so a gap here shows up as a file that looks like one long error.
set -e

cd "$(dirname "$0")"
REPO=$(CDPATH= cd -- ../../.. && pwd)

# tree-sitter >= 0.25 finds grammars through `parser-directories` in a config
# file and refuses to parse without one -- it still exits 0, so the ERROR scan
# below silently saw an empty list and reported every known gap as "parses now".
# Generate the config inside the repo's gitignored scratch dir instead of
# depending on the developer's ~/.config.
cfg="$REPO/out/ts-config"
rm -rf "$cfg"
mkdir -p "$cfg/tree-sitter"
printf '{"parser-directories": ["%s/packages/tooling"]}\n' "$REPO" > "$cfg/tree-sitter/config.json"

# Files the grammar is known not to parse. Each needs a real grammar feature,
# not a tweak; keep the reason with the path so the list stays honest.
#
#   intentionally-invalid: a compile_fail fixture that is not valid Loam.
KNOWN="\
packages/loam/tests/compile_fail/if_expr_no_else.loam
packages/loam/tests/compile_fail/import_from.loam
packages/loam/tests/compile_fail/match_removed.loam
packages/loam/tests/compile_fail/let_tuple_outer_mut.loam
packages/loam/tests/compile_fail/char_literal_two_bytes.loam
packages/loam/tests/loader/misc/noquote.loam"

files=$(find "$REPO/packages" "$REPO/examples" -name '*.loam' \
    -not -path '*/tmp/*' -not -path '*/build/*' | sort)

bad=$(XDG_CONFIG_HOME="$cfg" npx --yes tree-sitter-cli parse --quiet --stat $files 2>/dev/null \
    | grep -E 'ERROR|MISSING' \
    | awk '{print $1}' \
    | sed "s|$REPO/||" \
    | sort || true)

# No ERROR nodes is not enough: `let g = f` / `(1)` as `let g = f(1)` parses
# cleanly and is wrong. The corpus pins the shape of what the compiler reads.
if ! XDG_CONFIG_HOME="$cfg" npx --yes tree-sitter-cli test >"$cfg/test.out" 2>&1; then
    echo "grammar: corpus failed:"
    cat "$cfg/test.out"
    exit 1
fi

# Anything failing that is not on the known list is a regression.
new=$(comm -23 <(printf '%s\n' "$bad" | sed '/^$/d') <(printf '%s\n' "$KNOWN" | sort))
# Anything on the known list that now parses should come off it.
fixed=$(comm -13 <(printf '%s\n' "$bad" | sed '/^$/d') <(printf '%s\n' "$KNOWN" | sort))

rc=0
if [ -n "$new" ]; then
    echo "grammar: these files no longer parse:"
    printf '  %s\n' $new
    rc=1
fi
if [ -n "$fixed" ]; then
    echo "grammar: these parse now — drop them from KNOWN in $0:"
    printf '  %s\n' $fixed
    rc=1
fi
[ $rc -eq 0 ] && echo "grammar: $(printf '%s\n' $files | wc -l | tr -d ' ') files, $(printf '%s\n' "$KNOWN" | wc -l | tr -d ' ') known gaps"
exit $rc
