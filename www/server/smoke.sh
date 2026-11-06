#!/usr/bin/env bash
# Runs a built server through a beacon, a login, and a report, as a real process.
# It proves a binary works on the machine it is on, with throwaway secrets.
#
#     www/server/smoke.sh ./analytics ./hash-password
set -euo pipefail

server=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
hasher=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
port=${SMOKE_PORT:-18080}
base=http://127.0.0.1:$port
work=$(mktemp -d)
email=owner@example.com
password=smoke-test-password
pid=

finish() {
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    rm -rf "$work"
}
trap finish EXIT

die() {
    echo "smoke: FAIL: $1"
    [ -f "$work/log" ] && tail -20 "$work/log"
    exit 1
}

# A gRPC-Web frame around one protobuf message, read from standard input.
frame() {
    local body=$work/message
    cat > "$body"
    local n
    n=$(wc -c < "$body" | tr -d ' ')
    printf "\\0\\0\\0\\0\\$(printf %03o "$n")"
    cat "$body"
}

# The `status` field of a reply in `$work/reply`: field 1 straight after the frame header.
reply_status() {
    local tag value
    read -r tag value < <(od -An -tu1 -j5 -N2 "$work/reply")
    [ "$tag" = 8 ] && echo "$value"
}

rpc() {
    local path=$1 cookie=${2:-}
    curl -s -m 10 -D "$work/head" -o "$work/reply" \
        -H 'Content-Type: application/grpc-web+proto' \
        ${cookie:+-H "Cookie: $cookie"} \
        --data-binary @"$work/request" "$base$path"
}

login_request() {
    {
        printf "\\012\\$(printf %03o "${#email}")%s" "$email"
        printf "\\022\\$(printf %03o "${#1}")%s" "$1"
    } | frame > "$work/request"
}

hash=$(printf %s "$password" | "$hasher")
case $hash in
    '$argon2id$'*) ;;
    *) die "hash_password printed: $hash" ;;
esac

PORT=$port DATABASE_PATH=$work/analytics.db VAULT_EMAIL=$email VAULT_PASSWORD_HASH=$hash \
    VAULT_SESSION_SECRET=0123456789abcdef0123456789abcdef "$server" > "$work/log" 2>&1 &
pid=$!

for _ in $(seq 1 50); do
    curl -s -m 1 -o /dev/null "$base/healthz" && break
    kill -0 "$pid" 2>/dev/null || die "the server stopped"
    sleep 0.1
done
[ "$(curl -s -m 5 "$base/healthz")" = '{"ok":true}' ] || die "healthz"

id=0123456789abcdef0123456789abcdef
event="{\"k\":\"page\",\"sid\":\"$id\",\"pv\":\"$id\",\"path\":\"/\",\"w\":1400}"
code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' -H 'Content-Type: text/plain' \
    --data "{\"v\":1,\"vid\":\"$id\",\"events\":[$event]}" "$base/api/track")
[ "$code" = 204 ] || die "beacon answered $code"

# A frame that lies about its length must not stop the server.
printf '\0\377\377\377\377' > "$work/request"
rpc /Vault/Stats
curl -s -m 5 -o /dev/null "$base/healthz" || die "a lying frame stopped the server"

printf '\010\036' | frame > "$work/request"
rpc /Vault/Stats
[ "$(reply_status)" = 2 ] || die "a report without a session"

login_request wrong-password
rpc /Vault/Login
[ "$(reply_status)" = 3 ] || die "a wrong password"
grep -qi '^set-cookie:' "$work/head" && die "a wrong password got a cookie"

login_request "$password"
rpc /Vault/Login
[ "$(reply_status)" = 1 ] || die "the right password"
cookie=$(grep -i '^set-cookie:' "$work/head" | sed 's/^[^:]*: //; s/;.*//')
[ -n "$cookie" ] || die "no session cookie"

printf '\010\036' | frame > "$work/request"
rpc /Vault/Stats "$cookie"
[ "$(reply_status)" = 1 ] || die "a report with a session"

kill -0 "$pid" 2>/dev/null || die "the server stopped"
echo "smoke: ok on $(uname -sm)"
