#!/usr/bin/env bash
# Deploys the www site to Vercel and the analytics server to Fly.io. Run it from
# anywhere. It needs `vercel`, `fly`, and `gh`, each signed in.
#
#     www/deploy.sh site      build the site, and publish it to Vercel
#     www/deploy.sh server    put the latest Linux build of the server on Fly
#     www/deploy.sh all       the server, then the site
#     www/deploy.sh check     test that a deploy could run, and change nothing
#
# The server cannot be built on a Mac. Commit and push, run the analytics server job
# with `gh workflow run analytics-server.yml --ref <branch>`, wait for it, then run
# this. It refuses a build older than your server code.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
web=$root/www/build/web
project=loam-lang
site=https://loam-lang.vercel.app
fly_app=loam-analytics
workflow=analytics-server.yml
# What the server binary is built from.
server_code=('www/server/*.loam' packages/crypto packages/http packages/loam packages/sqlite Makefile nob.loam)

say() { printf '\ndeploy: %s\n' "$1"; }
die() {
    printf '\ndeploy: FAILED: %s\n' "$1" >&2
    exit 1
}

need_tools() {
    local tool
    for tool in "$@"; do
        command -v "$tool" > /dev/null || die "$tool is not installed"
    done
}

check_site() {
    need_tools vercel curl
    vercel whoami > /dev/null 2>&1 || die "not signed in: run vercel login"
    if pgrep -f 'zeli serve' > /dev/null; then
        die "stop zeli serve first: it rewrites the folder a build uses"
    fi
}

# Sets `run_id` to the newest passing build whose code matches this checkout.
find_server_build() {
    local branch sha
    need_tools fly gh git file shasum curl
    fly auth whoami > /dev/null 2>&1 || die "not signed in: run fly auth login"
    gh auth status > /dev/null 2>&1 || die "not signed in: run gh auth login"
    cd "$root"
    branch=$(git rev-parse --abbrev-ref HEAD)
    run_id=$(gh run list --workflow "$workflow" --branch "$branch" --status success \
        --limit 1 --json databaseId -q '.[0].databaseId')
    [ -n "$run_id" ] || die "no passing $workflow run on $branch: push, run gh workflow run $workflow --ref $branch, and wait for the job"
    sha=$(gh run view "$run_id" --json headSha -q .headSha)
    git cat-file -e "$sha" 2> /dev/null || die "fetch first: this checkout lacks $sha"
    if ! git diff --quiet "$sha" -- "${server_code[@]}"; then
        git diff --stat "$sha" -- "${server_code[@]}" | tail -8
        die "the server code above is newer than the last build: commit, push, run gh workflow run $workflow --ref $branch, and wait for the job"
    fi
}

deploy_server() {
    local tmp
    find_server_build
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    say "fetching the Linux build from run $run_id"
    gh run download "$run_id" -n analytics-linux-x86_64 -D "$tmp" ||
        die "that build's artifact has expired: run gh run rerun $run_id, then try again"
    say "applying fly.toml"
    (cd "$root/www/server" && fly deploy --ha=false --wait-timeout 180s)
    say "putting the server on the volume"
    "$root/www/server/deploy.sh" "$tmp/analytics"
}

# A batch with no events stores nothing, so this leaves no trace in the numbers.
check_live_site() {
    local loader code id=0123456789abcdef0123456789abcdef
    loader=$(basename "$(ls "$web"/_zeus/loader.*.js)")
    curl -s -m 20 "$site/" | grep -q "$loader" || die "$site is not serving this build ($loader)"
    code=$(curl -s -m 20 -o /dev/null -w '%{http_code}' -H "Origin: $site" \
        -H 'Content-Type: text/plain' --data "{\"v\":1,\"vid\":\"$id\",\"events\":[]}" \
        "$site/api/track")
    [ "$code" = 204 ] || die "/api/track answered $code through Vercel, not 204"
    code=$(curl -s -m 20 -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/grpc-web+proto' "$site/Vault/Stats")
    [ "$code" = 200 ] || die "/Vault/Stats answered $code through Vercel, not 200"
}

deploy_site() {
    check_site
    cd "$root"
    say "building the site"
    make
    ./bin/zeli build www --targets web --release
    say "publishing to Vercel"
    # A build wipes this folder, so it has to be linked again each time.
    cd "$web"
    vercel link --yes --project "$project"
    vercel deploy --prod --yes
    say "checking $site"
    check_live_site
    say "live: $site"
}

case ${1:-} in
    site) deploy_site ;;
    server) deploy_server ;;
    all)
        deploy_server
        deploy_site
        ;;
    check)
        check_site
        find_server_build
        say "ready: the server would come from run $run_id"
        ;;
    *)
        sed -n 2,12p "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
