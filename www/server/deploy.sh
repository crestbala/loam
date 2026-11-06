#!/usr/bin/env bash
# Puts a Linux build of the server on the Fly volume, and restarts onto it. The
# binary comes from the analytics-server job. `fly deploy` is only for fly.toml.
#
#     www/server/deploy.sh ./analytics
set -euo pipefail

app=loam-analytics
binary=${1:?usage: deploy.sh <linux binary>}
url=https://$app.fly.dev/healthz

case $(file -b "$binary") in
    ELF*x86-64*) ;;
    *) echo "deploy: $binary is not a Linux x86-64 binary"; exit 1 ;;
esac

remote() {
    fly ssh console -q -a "$app" -C "sh -c '$1'"
}

remote "rm -f /data/server.new"
fly ssh sftp put -a "$app" "$binary" /data/server.new

want=$(shasum -a 256 "$binary" | cut -d' ' -f1)
got=$(remote "sha256sum /data/server.new" | tr -d '\r' | grep -o '^[0-9a-f]\{64\}')
if [ "$want" != "$got" ]; then
    echo "deploy: the upload does not match: $want against $got"
    exit 1
fi

# A rename, so a crash at any point leaves a whole binary on the volume.
remote "chmod +x /data/server.new && mv /data/server.new /data/server"
fly apps restart "$app"

for _ in $(seq 1 30); do
    if [ "$(curl -s -m 5 "$url")" = '{"ok":true}' ]; then
        echo "deploy: healthy, $want"
        exit 0
    fi
    sleep 2
done
echo "deploy: $url did not come up"
exit 1
