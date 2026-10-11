#!/bin/sh
# Local test media for the engine: small, generated, served on localhost, so
# an engine test is a second of work and never touches the network.
#
#   sh examples/zeus/vidget/tests/fixtures.sh [dir] [port]   # builds + serves
#
# Layout (under $dir, default /tmp/vidget-fx):
#   clip.mp4                3 s H.264/AAC
#   hls/master.m3u8         two TS variants (360p, 240p), 1 s segments
#   fmp4/index.m3u8         fMP4 segments (EXT-X-MAP init)
#   enc/index.m3u8          AES-128 encrypted TS (EXT-X-KEY)
#   page.html               og: tags + media URLs in markup and escaped JSON
#   app.html + app.js       a "webpack" page: the stream URL is only in the bundle
set -e
DIR=${1:-/tmp/vidget-fx}
PORT=${2:-8765}
BASE="http://127.0.0.1:$PORT"
FF="ffmpeg -hide_banner -loglevel error -y"
SRC="-f lavfi -i testsrc2=size=640x360:rate=24 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 3"

mkdir -p "$DIR/hls" "$DIR/fmp4" "$DIR/enc"
cd "$DIR"

if [ ! -f clip.mp4 ]; then
  $FF $SRC -c:v libx264 -g 24 -keyint_min 24 -sc_threshold 0 -pix_fmt yuv420p -c:a aac clip.mp4
  $FF $SRC -map 0:v -map 1:a -map 0:v -map 1:a \
    -filter:v:1 scale=426:240 -c:v libx264 -g 24 -keyint_min 24 -sc_threshold 0 -pix_fmt yuv420p -c:a aac \
    -b:v:0 800k -b:v:1 300k -f hls -hls_time 1 -hls_playlist_type vod \
    -var_stream_map "v:0,a:0 v:1,a:1" -master_pl_name master.m3u8 \
    -hls_segment_filename "hls/v%v_%03d.ts" "hls/v%v.m3u8"
  $FF $SRC -c:v libx264 -g 24 -keyint_min 24 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -f hls -hls_time 1 \
    -hls_playlist_type vod -hls_segment_type fmp4 \
    -hls_segment_filename "fmp4/s%03d.m4s" fmp4/index.m3u8
  printf '0123456789abcdef' > enc/k.key
  printf '%s\n%s\n' "$BASE/enc/k.key" "$DIR/enc/k.key" > enc/keyinfo
  $FF $SRC -c:v libx264 -g 24 -keyint_min 24 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -f hls -hls_time 1 \
    -hls_playlist_type vod -hls_key_info_file enc/keyinfo \
    -hls_segment_filename "enc/s%03d.ts" enc/index.m3u8
fi

cat > page.html <<EOF
<!doctype html><html><head>
<title>Fallback title</title>
<meta property="og:title" content="Test Pattern &amp; Tone">
<meta property="og:description" content="Three seconds of colour bars.">
<meta property="og:image" content="$BASE/thumb.jpg">
</head><body>
<video src="/clip.mp4"></video>
<script>window.__DATA__={"stream":"http:\/\/127.0.0.1:$PORT\/hls\/master.m3u8"}</script>
</body></html>
EOF
[ -f thumb.jpg ] || $FF -i clip.mp4 -ss 1 -frames:v 1 thumb.jpg

cat > app.html <<EOF
<!doctype html><html><head><title>SPA Player</title>
<script defer src="/app.js"></script></head><body><div id="root"></div></body></html>
EOF
cat > app.js <<EOF
(()=>{var e={42:(e,t)=>{t.src="/fmp4/index.m3u8"}};window.player=e})();
EOF

echo "fixtures in $DIR, serving $BASE (Ctrl-C to stop)"
exec python3 -m http.server "$PORT" --bind 127.0.0.1
