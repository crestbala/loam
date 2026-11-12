#!/bin/sh
# Zed clones a grammar from a git repository at a revision. The grammar is
# vendored as plain files in the Loam repo, so this builds a scratch repo of it
# in out/zed-grammar (gitignored) and points the Zed extension there.
#
# The scratch repo is rebuilt from scratch with a fixed author and date, so its
# revision depends only on the grammar files: extension.toml changes only when
# the grammar does.
set -e
cd "$(dirname "$0")"
grammar_dir=$(pwd)
repo_root=$(CDPATH= cd -- ../../.. && pwd)
scratch_repo="$repo_root/out/zed-grammar"
extension_toml="$repo_root/packages/tooling/editors/zed/extension.toml"

rm -rf "$scratch_repo"
mkdir -p "$scratch_repo"
for path in grammar.js package.json tree-sitter.json queries src; do
    cp -R "$grammar_dir/$path" "$scratch_repo/"
done

cd "$scratch_repo"
git init --quiet
git add -A
GIT_AUTHOR_DATE="2000-01-01T00:00:00Z" GIT_COMMITTER_DATE="2000-01-01T00:00:00Z" \
    git -c user.name=loam -c user.email=loam@local -c commit.gpgsign=false \
    commit --quiet -m "loam grammar"
rev=$(git rev-parse HEAD)

sed -e "s|^repository = \".*\"|repository = \"file://$scratch_repo\"|" \
    -e "s|^rev = \".*\"|rev = \"$rev\"|" \
    "$extension_toml" > "$extension_toml.tmp"
mv "$extension_toml.tmp" "$extension_toml"
echo "zed grammar rev $rev"
rm -rf "$repo_root/packages/tooling/editors/zed/grammars"
