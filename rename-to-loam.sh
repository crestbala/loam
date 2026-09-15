#!/bin/sh
#
# rename-to-loam.sh — the Yuga → Loam rename, one reviewable stage at a time.
#
#   sh rename-to-loam.sh                 # plan every stage (dry run)
#   sh rename-to-loam.sh 1b              # plan one stage
#   VERBOSE=1 sh rename-to-loam.sh 2     # also echo matches to stdout
#   APPLY=1 sh rename-to-loam.sh 1b      # actually write the changes
#
# Nothing is touched unless APPLY=1. A dry run writes the *complete* list of
# renames and substitutions — every path, every occurrence — to
# `out/rename-report-<stage>.txt` and prints a summary here, so a stage that
# touches thousands of lines is still reviewable.
#
# Uses plain `mv` and `sed`, never `git mv`: no git metadata writes, and git
# still detects the renames from content. Finish each stage with the gate:
#
#   make clean && make && make test      # expect 216 ok
#   git add -A && git commit -m "rename: <stage>"
#
# Order matters. Substitutions run before renames in every stage, so the file
# list stays valid for the whole stage. Stage 1b (extensions) precedes stage 2
# (symbols) so `yuga_rt.h` is renamed together with its references, and stage 4
# (binaries) precedes stage 6 (prose) so `yuga-lsp` is not caught by a bare
# `yuga`. The later sweeps are idempotent on text an earlier stage already
# rewrote, so a straggler only ever falls forward into stage 6.
set -u

cd "$(dirname "$0")" || exit 1

APPLY="${APPLY:-0}"
VERBOSE="${VERBOSE:-0}"
SELF="rename-to-loam.sh"

# Every tracked-or-untracked text file, minus this script: it holds the `.yuga`
# patterns as data, and an in-place rewrite would corrupt the rules mid-run.
LIST="$(mktemp "${TMPDIR:-/tmp}/loam-files.XXXXXX")"
git ls-files --cached --others --exclude-standard \
    | grep -v -x "$SELF" > "$LIST"

STATUS=0
say() { printf '%s\n' "$*"; }

stage_name() {
    case "$1" in
    1b) echo "1b  extensions   *.yuga -> *.loam, and every '.yuga' literal" ;;
    1c) echo "1c  extensions   stop accepting '.yuga'  (hand edit of src/ext.h)" ;;
    2)  echo "2   C symbols    yuga_ -> loam_, YUGA_ -> LOAM_, Yuga* -> Loam*, yuga_rt.h" ;;
    3)  echo "3   dirs + vars  packages/yuga -> packages/loam, yuga.deps/.lock, com.yuga." ;;
    4)  echo "4   binaries     yugac -> loam, yuga-lsp -> loam-lsp, yugafmt -> loam-fmt" ;;
    5)  echo "5   tooling      tree-sitter/zed/vscode paths and ids" ;;
    6)  echo "6   prose        everything still saying yuga/Yuga" ;;
    *)  STATUS=1; echo "unknown stage: $1" >&2 ;;
    esac
}

# ---------------------------------------------------------------- rules ------
# Each rule is `<grep pattern>@@<sed expression>`; the pattern only drives the
# report, the expression is what APPLY runs.

# Old path / new path, one per line (`mv` semantics: directories rename whole).
renames() {
    case "$1" in
    1b) git ls-files --cached --others --exclude-standard '*.yuga' \
            | while IFS= read -r f; do printf '%s %s\n' "$f" "${f%.yuga}.loam"; done ;;
    2)  echo "packages/yuga/runtime/yuga_rt.h packages/yuga/runtime/loam_rt.h" ;;
    3)  cat <<'EOF'
packages/yuga packages/loam
packages/loam/tests/pkg_app/yuga.deps packages/loam/tests/pkg_app/loam.deps
packages/zeus/hosts/android/java/com/yuga packages/zeus/hosts/android/java/com/loam
EOF
    ;;
    5)  cat <<'EOF'
packages/tooling/tree-sitter-yuga packages/tooling/tree-sitter-loam
packages/tooling/editors/vscode/syntaxes/yuga.tmLanguage.json packages/tooling/editors/vscode/syntaxes/loam.tmLanguage.json
packages/tooling/editors/zed/icon_themes/yuga.json packages/tooling/editors/zed/icon_themes/loam.json
packages/tooling/editors/zed/icons/yuga.svg packages/tooling/editors/zed/icons/loam.svg
packages/tooling/editors/zed/languages/yuga packages/tooling/editors/zed/languages/loam
EOF
    ;;
    6)  cat <<'EOF'
docs/yuga.md docs/loam.md
docs/yuga_zeus_v2.md docs/loam_zeus_v2.md
EOF
    ;;
    esac
}

substs() {
    case "$1" in
    1b) cat <<'EOF'
\.yuga@@s/\.yuga/.loam/g
EOF
    ;;
    2)  cat <<'EOF'
yuga_@@s/yuga_/loam_/g
YUGA_@@s/YUGA_/LOAM_/g
Yuga@@s/Yuga/Loam/g
EOF
    ;;
    3)  cat <<'EOF'
packages/yuga@@s#packages/yuga#packages/loam#g
com/yuga/@@s#com/yuga/#com/loam/#g
yuga\.deps@@s/yuga\.deps/loam.deps/g
yuga\.lock@@s/yuga\.lock/loam.lock/g
com\.yuga\.@@s/com\.yuga\./com.loam./g
EOF
    ;;
    4)  cat <<'EOF'
YUGAC@@s/YUGAC/LOAM/g
yugac@@s/yugac/loam/g
yuga-lsp@@s/yuga-lsp/loam-lsp/g
yugafmt@@s/yugafmt/loam-fmt/g
EOF
    ;;
    5)  cat <<'EOF'
tree-sitter-yuga@@s#tree-sitter-yuga#tree-sitter-loam#g
zed_yuga@@s/zed_yuga/zed_loam/g
yuga\.tmLanguage\.json@@s/yuga\.tmLanguage\.json/loam.tmLanguage.json/g
yuga\.json@@s/yuga\.json/loam.json/g
yuga\.svg@@s/yuga\.svg/loam.svg/g
editors/zed/languages/yuga@@s#editors/zed/languages/yuga#editors/zed/languages/loam#g
"file-types"@@s/"file-types": \["yuga"\]/"file-types": ["loam", "loa"]/g
"extensions": \["\.loam"\]@@s/"extensions": \["\.loam"\]/"extensions": [".loam", ".loa"]/g
EOF
    ;;
    6)  cat <<'EOF'
YUGA@@s/YUGA/LOAM/g
Yuga@@s/Yuga/Loam/g
yuga@@s/yuga/loam/g
EOF
    ;;
    esac
}

# ----------------------------------------------------------------- run -------
stage="${1:-all}"
report="out/rename-report-${stage}.txt"
mkdir -p out
: > "$report"

plan_substs() {
    printf '%s\n' "$(substs "$1")" | grep . | while IFS= read -r rule; do
        pat="${rule%%@@*}"
        expr="${rule#*@@}"
        files="$(xargs grep -Iln -e "$pat" < "$LIST" 2>/dev/null)"
        [ -n "$files" ] || continue
        nfiles=$(printf '%s\n' "$files" | grep -c .)
        nocc=$(printf '%s\n' "$files" | xargs grep -Ih -e "$pat" 2>/dev/null | grep -c .)
        say "  $pat   ->   $nocc occurrence(s) in $nfiles file(s)"
        printf '\n## %s   (%s occurrences in %s files)\n' "$expr" "$nocc" "$nfiles" >> "$report"
        printf '%s\n' "$files" | while IFS= read -r f; do
            printf -- '--- %s\n' "$f" >> "$report"
            grep -In -e "$pat" "$f" 2>/dev/null | sed 's/^/    /' >> "$report"
            if [ "$VERBOSE" = 1 ]; then
                grep -In -e "$pat" "$f" 2>/dev/null | sed 's/^/      /' >&2
            fi
        done
        if [ "$APPLY" = 1 ]; then
            printf '%s\n' "$files" | while IFS= read -r f; do
                tmp="$f.loamtmp"
                if sed -e "$expr" "$f" > "$tmp"; then
                    mv "$tmp" "$f"
                else
                    rm -f "$tmp"; echo "sed FAILED: $expr in $f" >&2
                fi
            done
        fi
    done
}

plan_renames() {
    out="$(renames "$1")"
    [ -n "$out" ] || return 0
    n=$(printf '%s\n' "$out" | grep -c .)
    say "renames ($n):"
    printf '%s\n' "$out" | sed 's/^/    /'
    printf '\n## renames (%s)\n' "$n" >> "$report"
    printf '%s\n' "$out" | sed 's/^/    /' >> "$report"
    if [ "$APPLY" = 1 ]; then
        printf '%s\n' "$out" | while IFS=' ' read -r src dst; do
            [ -n "${src:-}" ] || continue
            mv "$src" "$dst" || echo "mv FAILED: $src -> $dst" >&2
        done
    fi
}

run_stage() {
    printf '\n=== %s ===\n' "$(stage_name "$1")"
    plan_substs "$1"
    plan_renames "$1"
}

case "$stage" in
all) for s in 1b 2 3 4 5 6; do run_stage "$s"; done ;;
*)   run_stage "$stage" ;;
esac

rm -f "$LIST"
say ""
say "APPLY=$APPLY   full detail: $report"
if [ "$APPLY" = 1 ]; then
    say "gate:  make clean && make && make test     # expect 216 ok"
    say "then:  git add -A && git commit -m 'rename: <stage>'"
else
    say "review, then re-run with APPLY=1"
fi
exit "$STATUS"
