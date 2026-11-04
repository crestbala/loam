/* Zeus browser host. Canvas2D only — no WebGPU, no WebGL, no DOM widgets.
   One <canvas>, this file, and a .wasm built by `loam --target wasm`.
   `window.attachZeus(canvas, opts)` boots from an ArrayBuffer; a canvas with
   `data-wasm` still auto-loads that URL (counter, gallery). */
(function (root) {
  /* One frame (docs/frame-format.md), decoded straight out of wasm memory into
     `paint`. `head` points at 8 int32: version stride n ops ns strs np path,
     the last of each pair a pointer; `strs` is `ns` (ptr, len) pairs. Pure: it
     reads `buffer` and calls `paint`, so a test can drive it with a recorder.
     The guards match `zeus_replay` in zeus_plat.c. Returns the op count, or -1
     for a version this decoder does not know. */
  const FRAME_VERSION = 1;
  const FRAME_STRIDE = 11;
  const utf8 = new TextDecoder();
  function decodeFrame(buffer, head, paint) {
    const h = new Int32Array(buffer, head, 8);
    const version = h[0], stride = h[1], n = h[2], ns = h[4], np = h[6];
    if (version !== FRAME_VERSION || stride !== FRAME_STRIDE) return -1;
    const ops = new Int32Array(buffer, h[3], n * stride);
    const st = new Int32Array(buffer, h[5], ns * 2);
    const path = new Int32Array(buffer, h[7], np);
    const u8 = new Uint8Array(buffer);
    const cache = new Array(ns);
    const str = (i) => {
      if (i < 0 || i >= ns) return "";
      let v = cache[i];
      if (v === undefined) {
        const p = st[i * 2];
        v = cache[i] = utf8.decode(u8.subarray(p, p + st[i * 2 + 1]));
      }
      return v;
    };
    for (let i = 0; i < n; i++) {
      const o = i * stride;
      const x = ops[o + 1], y = ops[o + 2], w = ops[o + 3], hh = ops[o + 4];
      const rgb = ops[o + 5] & 0xffffff, radius = ops[o + 6], alpha = ops[o + 7];
      const font = ops[o + 8], value = ops[o + 9], si = ops[o + 10];
      switch (ops[o]) {
        case 0: paint.fill(x, y, w, hh, rgb, radius); break;
        case 1: paint.fill_a(x, y, w, hh, rgb, radius, alpha); break;
        case 2: paint.text(x, y, str(si), rgb, font); break;
        case 6: paint.save(); break;
        case 7: paint.clip(x, y, w, hh, radius); break;
        case 8: paint.restore(); break;
        case 9: paint.text_rot(x, y, str(si), rgb, font, w); break;
        case 10: paint.image(x, y, w, hh, str(si), radius, alpha, value); break;
        case 11: paint.fill_g(x, y, w, hh, rgb, value & 0xffffff, font, radius, alpha); break;
        case 12:
          if (alpha > 0 && w > 0 && hh > 0) {
            paint.shadow(x, y, w, hh, radius, rgb, alpha, font,
                         (value % 256) - 128, Math.trunc(value / 256) - 128);
          }
          break;
        case 13: if (font > 0 && w > 0 && hh > 0) paint.stroke(x, y, w, hh, rgb, radius, font, alpha); break;
        case 14:
          paint.fill4(x, y, w, hh, rgb, alpha, radius % 65536, Math.trunc(radius / 65536),
                      value % 65536, Math.trunc(value / 65536));
          break;
        case 15: paint.xform(x, y, radius, font, w, hh); break;
        case 16:
        case 17:
          if (value >= 0 && w > 0 && value + w <= np && (ops[o] === 16 || font > 0)) {
            paint.path(path, value, w, rgb, alpha, ops[o] === 16 ? 0 : font, ops[o] === 16 ? 0 : radius);
          }
          break;
        case 18: paint.alpha(alpha); break;
        default: break;
      }
    }
    return n;
  }
  root.zeusDecodeFrame = decodeFrame;

  /* A canvas gets no OS inertia, so a released pan coasts here. Time-based, so a
     phone that drops frames travels as far as one that does not. The constant
     is iOS's normal rate (0.998 per ms). */
  const FLING_DECAY = 0.002;
  const FLING_WINDOW_MS = 100;
  const FLING_REST_MS = 80;
  const FLING_MIN = 0.1;
  const FLING_MAX = 8;
  const FLING_STOP = 0.02;
  /* Finger velocity at lift in layout units per ms, from `samples` of
     `{t, x, y}`, oldest first. The last pair alone is noise: a real lift slows
     in its final sample. A finger that rested before lifting has no velocity. */
  function flingVelocity(samples, now) {
    const last = samples[samples.length - 1];
    if (!last || now - last.t > FLING_REST_MS) return { x: 0, y: 0 };
    let first = last;
    for (let i = samples.length - 1; i >= 0 && last.t - samples[i].t <= FLING_WINDOW_MS; i--) first = samples[i];
    const dt = last.t - first.t;
    if (dt <= 0) return { x: 0, y: 0 };
    const clamp = (v) => (Math.abs(v) < FLING_MIN ? 0 : Math.max(-FLING_MAX, Math.min(FLING_MAX, v)));
    return { x: clamp((first.x - last.x) / dt), y: clamp((first.y - last.y) / dt) };
  }
  /* `dt` ms of coast from velocity `v`: the distance covered and what is left. */
  function flingStep(v, dt) {
    const f = Math.exp(-FLING_DECAY * dt);
    return { d: (v * (1 - f)) / FLING_DECAY, v: v * f };
  }
  root.zeusFling = { velocity: flingVelocity, step: flingStep };
  /* The light / dark choice. `zeli`'s page shell reads this key too, before the
     first paint. */
  const APPEARANCE_KEY = "zeus-appearance";

  function attachZeus(canvas, opts) {
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("Canvas2D unavailable");
    opts = opts || {};

    /* A phone's 3x canvas is 2.25 times the pixels of 2x, all repainted on every
       scroll frame, for detail nobody sees at arm's length. Rastering fewer
       keeps a pan on the finger on the phones that need it most. */
    const coarse = !!(window.matchMedia && matchMedia("(pointer: coarse)").matches);
    const dpr = Math.min(window.devicePixelRatio || 1, coarse ? 2 : Infinity);
    let layoutW = 1;
    let layoutH = 1;
    let sx = dpr;
    let sy = dpr;
    let mem = null;
    let stopped = false;
    let raf = 0;
    let wakeTimer = 0;
    /* `start` rebinds this so `request_frame` / image onload can wake rAF. */
    let schedule = function (_ms) {};
    /* Set once input is wired: applies the frame's pan and fling, and says
       whether a fling wants another frame. */
    let touchFrame = null;
    /* The page text `zeli build` put in the HTML. The canvas fades in over it on
       its first frame, and the text goes once it is covered. */
    let prepaint = document.getElementById("zeus-prepaint");
    function revealCanvas() {
      if (!prepaint) return;
      const text = prepaint;
      prepaint = null;
      canvas.style.opacity = "1";
      canvas.style.pointerEvents = "auto";
      setTimeout(() => text.remove(), 200);
    }
    /* True while a touch reports its hover. See `preloadRoute`. */
    let touching = false;
    /* Instance exports; imports that finish asynchronously (fetch_rpc_async)
       call back into these once `boot` has instantiated the module. */
    let exp = null;
    /* Browser WebSockets (wasm `http.ws_open`): the socket and an inbound
       message queue live here per slot; wasm polls and copies out. */
    let wsSlots = new Array(8).fill(null);
    /* Streaming gRPC-Web replies (`rs_*`): one open XHR per slot. */
    let rsSlots = new Array(4).fill(null);
    /* Console text waiting for its newline (see `write`). */
    let consoleLine = "";
    /* Log lines go to the terminal running the dev server (`zeli serve`, or an
       app's Vite config), posted in batches to `/__zeus/log`, not to the
       console. Without such a server (a static deploy) the first post fails
       and the lines, that batch included, fall back to the console. */
    const logRelay = { on: true, queue: [], timer: 0 };
    const consoleOut = (line) => {
      if (/^\+\d+\.\d{3}s ERROR /.test(line)) console.error(line);
      else if (/^\+\d+\.\d{3}s WARN /.test(line)) console.warn(line);
      else console.log(line);
    };
    const relayFlush = () => {
      logRelay.timer = 0;
      const batch = logRelay.queue;
      logRelay.queue = [];
      if (!batch.length) return;
      const back = () => {
        logRelay.on = false;
        for (const l of batch) consoleOut(l);
        for (const l of logRelay.queue.splice(0)) consoleOut(l);
      };
      try {
        fetch("/__zeus/log", {
          method: "POST",
          headers: { "Content-Type": "text/plain; charset=utf-8" },
          body: batch.join("\n") + "\n",
          keepalive: true,
        }).then((r) => {
          if (!r.ok) back();
        }, back);
      } catch (e) {
        back();
      }
    };
    const logLine = (line) => {
      if (!logRelay.on || typeof fetch !== "function") {
        consoleOut(line);
        return;
      }
      logRelay.queue.push(line);
      if (!logRelay.timer) logRelay.timer = setTimeout(relayFlush, 100);
    };
    const imgCache = new Map();
    let fileInput = null;
    /* Media players (`plat_media_*`): one <video> per slot, never in the DOM.
       Its frames reach the canvas through `drawImage` when the draw list hands
       `image` a `media:<slot>` src, and `frame` keeps rAF running while any
       of them plays, so the picture moves with no signal write. Audio-only
       sources play through the same element. */
    const media = new Array(8).fill(null);
    const mediaPicked = { seq: 0, name: "", src: "" };
    let mediaInput = null;
    const MEDIA_ACCEPT =
      "video/*,audio/*,.mp4,.m4v,.mov,.webm,.mkv,.ogv,.ogg,.mp3,.m4a,.aac,.wav,.flac,.opus,.m3u8,.ts,.3gp";
    function mediaEl(slot) {
      let m = media[slot];
      if (m) return m;
      const v = document.createElement("video");
      v.preload = "auto";
      v.playsInline = true;
      v.crossOrigin = "anonymous";
      m = { el: v, error: "", waiting: false, seeking: false, src: "" };
      const wake = () => schedule(0);
      for (const ev of ["loadedmetadata", "loadeddata", "canplay", "play", "pause", "ended",
                        "seeked", "progress", "ratechange", "volumechange", "durationchange",
                        "emptied"]) {
        v.addEventListener(ev, wake);
      }
      v.addEventListener("waiting", () => { m.waiting = true; wake(); });
      v.addEventListener("playing", () => { m.waiting = false; wake(); });
      v.addEventListener("seeking", () => { m.seeking = true; wake(); });
      v.addEventListener("seeked", () => { m.seeking = false; wake(); });
      v.addEventListener("error", () => {
        const c = v.error ? v.error.code : 0;
        m.error = c === 4 ? "This browser cannot play this format."
          : c === 3 ? "The file could not be decoded."
          : c === 2 ? "A network error stopped the download."
          : "The media could not be loaded.";
        /* Cross-origin hosts without CORS headers refuse an anonymous load;
           retry once without it (the frame still paints, the canvas is
           merely tainted, which this host never reads back). */
        if (v.crossOrigin && m.src && /^https?:/.test(m.src) && c !== 4) {
          m.error = "";
          v.removeAttribute("crossorigin");
          v.src = m.src;
          v.load();
        }
        wake();
      });
      media[slot] = m;
      return m;
    }
    function mediaState(m) {
      if (!m || !m.src) return 0;
      const v = m.el;
      if (m.error) return 5;
      if (v.readyState < 1) return 1;
      if (v.ended) return 4;
      if (v.paused) return 2;
      if (m.waiting || m.seeking || v.readyState < 3) return 6;
      return 3;
    }
    /* A frame is wanted while a player plays or settles a seek. */
    function mediaLive() {
      for (const m of media) {
        if (m && m.src && !m.error && ((!m.el.paused && !m.el.ended) || m.seeking)) return true;
      }
      return false;
    }
    function mediaPick(f) {
      if (!f) return;
      mediaPicked.seq++;
      mediaPicked.name = f.name || "media";
      mediaPicked.src = URL.createObjectURL(f);
      schedule(0);
    }
    function mediaFile(dt) {
      if (!dt || !dt.files) return null;
      for (const f of dt.files) {
        if (/^(video|audio)\//.test(f.type) || /\.(mkv|m3u8|ts|flac|opus|ogv|m4v|mov)$/i.test(f.name)) return f;
      }
      return null;
    }
    canvas.addEventListener("dragover", (e) => {
      if (e.dataTransfer && Array.from(e.dataTransfer.types || []).includes("Files")) {
        e.preventDefault();
        e.dataTransfer.dropEffect = "copy";
      }
    });
    canvas.addEventListener("drop", (e) => {
      const f = mediaFile(e.dataTransfer);
      if (!f) return;
      e.preventDefault();
      mediaPick(f);
    });
    const te = typeof TextEncoder !== "undefined" ? new TextEncoder() : null;
    const ac = new AbortController();
    const signal = ac.signal;

  function sizeCanvas() {
    const cssW = canvas.clientWidth || window.innerWidth;
    const cssH = canvas.clientHeight || window.innerHeight;
    layoutW = Math.max(1, Math.round(cssW));
    layoutH = Math.max(1, Math.round(cssH));
    const bw = Math.max(1, Math.round(cssW * dpr));
    const bh = Math.max(1, Math.round(cssH * dpr));
    canvas.width = bw;
    canvas.height = bh;
    sx = bw / layoutW;
    sy = bh / layoutH;
    applyLayoutTransform();
    return { w: layoutW, h: layoutH };
  }

  function applyLayoutTransform() {
    ctx.setTransform(sx, 0, 0, sy, 0, 0);
  }

  /* Map a layout unit onto a device-pixel edge so fills and type stay sharp
     at fractional `devicePixelRatio` (1.25 / 1.5 / 2.25). */
  function snapX(n) {
    return Math.round(n * sx) / sx;
  }
  function snapY(n) {
    return Math.round(n * sy) / sy;
  }
  function snapRect(x, y, w, h) {
    const x0 = Math.round(x * sx);
    const y0 = Math.round(y * sy);
    const x1 = Math.round((x + w) * sx);
    const y1 = Math.round((y + h) * sy);
    return { x: x0 / sx, y: y0 / sy, w: (x1 - x0) / sx, h: (y1 - y0) / sy };
  }
  function layoutPoint(clientX, clientY) {
    const r = canvas.getBoundingClientRect();
    const rw = r.width || 1;
    const rh = r.height || 1;
    return {
      x: Math.round((clientX - r.left) * (layoutW / rw)),
      y: Math.round((clientY - r.top) * (layoutH / rh)),
    };
  }

  function rgb(c) {
    const n = c >>> 0;
    return "rgb(" + ((n >> 16) & 255) + "," + ((n >> 8) & 255) + "," + (n & 255) + ")";
  }
  function rgba(c, a) {
    const n = c >>> 0;
    return "rgba(" + ((n >> 16) & 255) + "," + ((n >> 8) & 255) + "," + (n & 255) + "," + (a / 255) + ")";
  }

  /* Match Cocoa: line box is ascent+descent, paint origin is the top of that
     box. Canvas `textBaseline = "top"` is the em square and `size+4` is taller
     than the glyphs, so labels sit high in buttons, chips, and avatars. */
  /* One global family (see zeus_plat.c). `set_font_family` swaps it and
     drops every cached metric, because ascent, descent and every measured
     width belong to the face that produced them. */
  const fontFallback = "system-ui, sans-serif";
  let fontFace = fontFallback;
  const fontMetrics = new Map();
  /* Measured text widths per (px, string): layout re-measures every label
     on every frame, and measureText is the priciest JS import. */
  const textWidths = new Map();
  let lastFontPx = 0;
  function setFont(px) {
    if (px === lastFontPx) return;
    ctx.font = px + "px " + fontFace;
    lastFontPx = px;
  }
  /* `ctx.restore()` puts back the WHOLE drawing state, `font` included, so a
     cached size can no longer be trusted after one — the next `setFont(13)`
     would early-return and leave whatever the restored state had (the canvas
     default, which is much larger than our small UI sizes). That is what made a
     just-shown tooltip paint one frame at the default size. Every restore in
     this file goes through here so the cache is dropped with the state. */
  /* Cached nine-slice shadows, as the macOS host does (mac.m
     `mac_shadow_template`). A live `shadowBlur` re-blurs the whole rect every
     frame, and a frame repaints everything: a drawer or sheet (Modal
     elevation, blur 52) sliding over the page re-ran a full-panel Gaussian on
     each of its frames. A shadow depends only on (radius, color, alpha, blur,
     offset) once the rect is large enough, so it is drawn once into a
     template whose corners land 1:1 and whose edges are a band that is
     constant along the stretch. A template too big to keep at the device
     scale is built at a coarser one — a wide blur is a smooth ramp, so
     upscaling it loses nothing visible, where falling back to the live blur
     cost the frame. */
  const SHADOW_TEMPLATE_MAX_PX = 768;
  const shadowCache = new Map();
  function shadowGeom(radius, blur, dx, dy) {
    const d = Math.max(Math.abs(dx), Math.abs(dy));
    const reach = blur * 2 + 2;
    const m = reach + d;
    const k = m + radius + reach + d;
    return { m, k, side: 2 * k + 2 };
  }
  function roundRectPath(t, x, y, w, h, r) {
    const rr = Math.max(0, Math.min(r, w / 2, h / 2));
    t.beginPath();
    t.moveTo(x + rr, y);
    t.arcTo(x + w, y, x + w, y + h, rr);
    t.arcTo(x + w, y + h, x, y + h, rr);
    t.arcTo(x, y + h, x, y, rr);
    t.arcTo(x, y, x + w, y, rr);
    t.closePath();
  }
  function shadowTemplate(tsc, radius, color, a, blur, dx, dy, g) {
    const key = `${radius},${color},${a},${blur},${dx},${dy},${tsc}`;
    const hit = shadowCache.get(key);
    if (hit) return hit;
    const px = Math.max(1, Math.round(g.side * tsc));
    const c = document.createElement("canvas");
    c.width = px;
    c.height = px;
    const t = c.getContext("2d");
    if (!t) return null;
    /* The live path's trick, in the template: fill the shape off the left /
       top edge so only its blur lands, shifted back by the shadow offset
       (device pixels, so scaled by `tsc`). */
    const off = g.side + 2 * blur + 64;
    t.setTransform(tsc, 0, 0, tsc, 0, 0);
    t.shadowColor = rgba(color, a);
    t.shadowBlur = blur * tsc;
    t.shadowOffsetX = tsc * (off + dx);
    t.shadowOffsetY = tsc * (off + dy);
    t.fillStyle = "#000";
    roundRectPath(t, g.m - off, g.m - off, g.side - 2 * g.m, g.side - 2 * g.m, radius);
    t.fill();
    /* Elevations come from a short token list; the bound is for an app that
       animates a blur or an offset. */
    if (shadowCache.size >= 64) shadowCache.clear();
    shadowCache.set(key, c);
    return c;
  }
  /* Draws the shadow from a template and answers true, or answers false and
     leaves it to the live path: a rotated or skewed paint transform, a rect
     narrower than the two corner regions, or a template too large even at
     one pixel per unit. */
  function cachedShadow(p, radius, color, a, blur, dx, dy) {
    if (blur <= 0 || a <= 0) return false;
    const mt = ctx.getTransform();
    const sc = mt.a;
    if (!(sc > 0) || mt.b !== 0 || mt.c !== 0 || Math.abs(mt.d - sc) > 1e-6) return false;
    /* A corner is no rounder than half the short side — pill controls carry
       a nominal 999, which would key a huge template for nothing. */
    const r = Math.max(0, Math.min(radius, Math.floor(p.w / 2), Math.floor(p.h / 2)));
    const g = shadowGeom(r, blur, dx, dy);
    if (p.w + 2 * g.m < 2 * g.k + 2 || p.h + 2 * g.m < 2 * g.k + 2) return false;
    let tsc = sc;
    if (g.side * tsc > SHADOW_TEMPLATE_MAX_PX) {
      tsc = Math.floor(SHADOW_TEMPLATE_MAX_PX / g.side);
      if (tsc < 1) return false;
    }
    const tpl = shadowTemplate(tsc, r, color, a, blur, dx, dy, g);
    if (!tpl) return false;
    const K = g.k, S = g.side;
    /* Only a ring `T` deep: past the margin and the rounded corner the shadow
       sits under the opaque panel, so the rest of each slice is pixels the
       panel paints over (and the macOS host never draws them at all). */
    const T = g.m + r;
    const X0 = p.x - g.m, Y0 = p.y - g.m;
    const W = p.w + 2 * g.m, H = p.h + 2 * g.m;
    const X1 = X0 + W - K, Y1 = Y0 + H - K;
    const sl = (sx0, sy0, sw, sh, x0, y0, w0, h0) => {
      if (w0 <= 0 || h0 <= 0) return;
      ctx.drawImage(tpl, sx0 * tsc, sy0 * tsc, sw * tsc, sh * tsc, x0, y0, w0, h0);
    };
    ctx.save();
    /* 1:1 slices need no filtering; a coarser template is upscaled smoothly. */
    ctx.imageSmoothingEnabled = tsc !== sc;
    /* top and bottom strips: corners 1:1, a 1-unit column stretched between */
    sl(0, 0, K, T, X0, Y0, K, T);
    sl(K, 0, 1, T, X0 + K, Y0, W - 2 * K, T);
    sl(S - K, 0, K, T, X1, Y0, K, T);
    sl(0, S - T, K, T, X0, Y0 + H - T, K, T);
    sl(K, S - T, 1, T, X0 + K, Y0 + H - T, W - 2 * K, T);
    sl(S - K, S - T, K, T, X1, Y0 + H - T, K, T);
    /* left and right strips between them: corner tails 1:1, a row stretched */
    sl(0, T, T, K - T, X0, Y0 + T, T, K - T);
    sl(0, K, T, 1, X0, Y0 + K, T, H - 2 * K);
    sl(0, S - K, T, K - T, X0, Y1, T, K - T);
    sl(S - T, T, T, K - T, X0 + W - T, Y0 + T, T, K - T);
    sl(S - T, K, T, 1, X0 + W - T, Y0 + K, T, H - 2 * K);
    sl(S - T, S - K, T, K - T, X0 + W - T, Y1, T, K - T);
    popState();
    return true;
  }
  function popState() {
    ctx.restore();
    lastFontPx = 0;
  }
  function resetFontCaches() {
    fontMetrics.clear();
    textWidths.clear();
    lastFontPx = 0;
  }
  function lineBox(px) {
    let m = fontMetrics.get(px);
    if (m) return m;
    setFont(px);
    ctx.textBaseline = "alphabetic";
    const t = ctx.measureText("Hg");
    let ascent = t.fontBoundingBoxAscent;
    let descent = t.fontBoundingBoxDescent;
    if (!Number.isFinite(ascent) || !Number.isFinite(descent)) {
      const inkA = t.actualBoundingBoxAscent;
      const inkD = t.actualBoundingBoxDescent;
      if (Number.isFinite(inkA) && Number.isFinite(inkD)) {
        ascent = inkA;
        descent = inkD;
      } else {
        ascent = px * 0.8;
        descent = px * 0.25;
      }
    }
    ascent = Math.round(ascent);
    descent = Math.round(descent);
    m = { ascent: ascent, height: Math.max(1, ascent + descent) };
    fontMetrics.set(px, m);
    return m;
  }

  /* Trace a rounded rect as the current path, in already-snapped device
     space. Kept separate from the fill so clip, stroke, shadow, and the
     per-corner fill can all reuse one corner geometry — four traversals
     that disagreed by a fraction of a pixel would show up as seams where a
     border meets its fill. */
  function traceRoundRect(x, y, w, h, r) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    ctx.beginPath();
    if (r <= 0) {
      ctx.rect(x, y, w, h);
      return;
    }
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  function roundRect(x, y, w, h, r) {
    const p = snapRect(x, y, w, h);
    r = Math.round(r * sx) / sx;
    if (r <= 0) {
      ctx.fillRect(p.x, p.y, p.w, p.h);
      return;
    }
    traceRoundRect(p.x, p.y, p.w, p.h, r);
    ctx.fill();
  }

  /* Per-corner rounded rect, clockwise from the top-left. */
  function traceRect4(x, y, w, h, tl, tr, br, bl) {
    const lim = Math.min(w, h) / 2;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    tl = Math.min(tl, lim);
    tr = Math.min(tr, lim);
    br = Math.min(br, lim);
    bl = Math.min(bl, lim);
    ctx.beginPath();
    ctx.moveTo(x + tl, y);
    ctx.arcTo(x + w, y, x + w, y + h, tr);
    ctx.arcTo(x + w, y + h, x, y + h, br);
    ctx.arcTo(x, y + h, x, y, bl);
    ctx.arcTo(x, y, x + w, y, tl);
    ctx.closePath();
  }

  function u8() {
    return new Uint8Array(mem.buffer);
  }
  function cstr(ptr) {
    if (!ptr) return "";
    const b = u8();
    let e = ptr;
    while (b[e]) e++;
    return new TextDecoder().decode(b.subarray(ptr, e));
  }
  function bytes(ptr, len) {
    if (len <= 0) return "";
    return new TextDecoder().decode(u8().subarray(ptr, ptr + len));
  }

  /* Canvas2D for each primitive of a frame (docs/frame-format.md). Called only
     by `decodeFrame`, in op order, with strings already decoded; units are
     layout points under the scaled CTM. */
  const paint = {
    fill: (x, y, w, h, color, r) => {
      ctx.fillStyle = rgb(color);
      roundRect(x, y, w, h, r);
    },
    fill_a: (x, y, w, h, color, r, a) => {
      ctx.fillStyle = rgba(color, a);
      roundRect(x, y, w, h, r);
    },
    fill_g: (x, y, w, h, c0, c1, axis, radius, a) => {
      const p = snapRect(x, y, w, h);
      const g = axis
        ? ctx.createLinearGradient(p.x, p.y, p.x + p.w, p.y)
        : ctx.createLinearGradient(p.x, p.y, p.x, p.y + p.h);
      g.addColorStop(0, rgb(c0));
      g.addColorStop(1, rgb(c1));
      ctx.save();
      if (a < 255) ctx.globalAlpha = Math.max(0, a) / 255;
      ctx.fillStyle = g;
      /* The radius used to be dropped, which is why every gradient
         background painted square-cornered. */
      traceRoundRect(p.x, p.y, p.w, p.h, Math.round(radius * sx) / sx);
      ctx.fill();
      popState();
    },
    /* Soft drop shadow. Canvas2D's shadow applies to the next fill, so the
       rounded path is filled offscreen-left and only its blur lands inside
       the real rect — filling in place would paint over whatever the
       shadow is meant to sit under.
       Units: the path is in layout (CSS) space under the scaled CTM, but
       `shadowOffset*` / `shadowBlur` are device pixels and ignore the CTM.
       So the path moves by `off` and the shadow by `off * sx`; mixing the
       two put the fill on screen at 2x (a black card-sized block). `off`
       also has to clear the canvas' left edge from wherever the rect is,
       so it includes `p.x` — a rect at x = 1000 needs more than its own
       width to leave the screen. */
    shadow: (x, y, w, h, radius, color, a, blur, dx, dy) => {
      const p = snapRect(x, y, w, h);
      if (cachedShadow(p, radius, color, a, blur, dx, dy)) return;
      /* Canvas2D's shadow applies to the next fill, so the rounded path is
         filled past the LEFT edge of the canvas and only its blur lands where
         the rect is — filling in place would paint over whatever the shadow
         sits under.

         How far "past the left edge" depends on the CURRENT transform, which
         is why this reads the matrix instead of assuming the layout origin is
         the device origin. A paint transform (the drawer sliding out) moves
         that origin, so the old fixed margin stopped clearing the canvas and
         the black fill itself showed up as a block on the left — the bug you
         saw while a drawer closed. In device terms the path must end before
         device x = 0: `end = sc * (p.x + p.w - off) + originDev <= 0`. */
      const m = ctx.getTransform();
      const sc = m.a || 1; /* device pixels per layout unit */
      const originX = m.e / sc; /* device origin, in layout units */
      const originY = m.f / (m.d || sc);
      const offX = p.x + p.w + originX + 2 * blur + 64;
      const offY = p.y + p.h + originY + 2 * blur + 64;
      ctx.save();
      ctx.shadowColor = rgba(color, a);
      /* shadow* values are DEVICE pixels and ignore the CTM, so the path's
         leftward shift has to be cancelled out in device terms and the blur
         scales with the device-per-layout factor. */
      ctx.shadowBlur = blur * sc;
      ctx.shadowOffsetX = sc * offX + dx * sc;
      ctx.shadowOffsetY = sc * offY + dy * (m.d || sc);
      ctx.fillStyle = "#000";
      traceRoundRect(p.x - offX, p.y - offY, p.w, p.h,
                     Math.round(radius * sc) / sc);
      ctx.fill();
      popState();
    },
    /* Ring stroke inset by half the width, so the line paints inside the
       rect — the box model layout already assumes when it insets content
       by `border_w`. */
    stroke: (x, y, w, h, color, radius, width, a) => {
      const p = snapRect(x, y, w, h);
      /* Whole device pixels, expressed in layout units: `lineWidth` is in
         user space under the scaled CTM, so a device-pixel count here drew
         a 1dp border 2dp wide at 2x. */
      const lw = Math.max(1, Math.round(width * sx)) / sx;
      if (p.w - lw <= 0 || p.h - lw <= 0) return;
      ctx.save();
      ctx.lineWidth = lw;
      ctx.strokeStyle = rgba(color, a);
      traceRoundRect(p.x + lw / 2, p.y + lw / 2, p.w - lw, p.h - lw,
                     Math.max(0, Math.round(radius * sx) / sx - lw / 2));
      ctx.stroke();
      popState();
    },
    fill4: (x, y, w, h, color, a, tl, tr, br, bl) => {
      const p = snapRect(x, y, w, h);
      const k = (v) => Math.round(v * sx) / sx;
      ctx.fillStyle = rgba(color, a);
      traceRect4(p.x, p.y, p.w, p.h, k(tl), k(tr), k(br), k(bl));
      ctx.fill();
    },
    /* Paint-space transform: translate, then scale and rotate about
       (ox, oy). Composes with the enclosing clip and unwinds with the
       enclosing restore, so it never escapes its save level. */
    xform: (dx, dy, scale, rot, ox, oy) => {
      /* LAYOUT units, like every other callback here: `applyLayoutTransform`
         has already put `sx, sy` on the CTM, so scaling these translates
         again moved the content by sx^2 (4x at dpr 2 — the spinner lands far
         from its box) and swung the rotation about a pivot that no longer
         coincided with the node's centre (so it "rotates differently").
         Translate in user space and let the CTM do the device mapping. */
      ctx.translate(ox + dx, oy + dy);
      if (rot) ctx.rotate((rot * Math.PI) / 180);
      if (scale !== 100) ctx.scale(scale / 100, scale / 100);
      ctx.translate(-ox, -oy);
    },
    text: (x, y, s, color, font) => {
      const px = font > 0 ? font : 13;
      const box = lineBox(px);
      ctx.fillStyle = rgb(color);
      setFont(px);
      ctx.textBaseline = "alphabetic";
      ctx.fillText(s, snapX(x), snapY(y + box.ascent));
    },
    /* Rotated text, clockwise around the top-left of the line box. */
    text_rot: (x, y, s, color, font, deg) => {
      const px = font > 0 ? font : 13;
      const box = lineBox(px);
      ctx.fillStyle = rgb(color);
      setFont(px);
      ctx.textBaseline = "alphabetic";
      ctx.save();
      ctx.translate(snapX(x), snapY(y + box.ascent));
      ctx.rotate((deg * Math.PI) / 180);
      ctx.fillText(s, 0, 0);
      popState();
    },
    save: () => ctx.save(),
    alpha: (a) => {
      ctx.save();
      ctx.globalAlpha = Math.max(0, Math.min(255, a)) / 255;
    },
    clip: (x, y, w, h, radius) => {
      const box = snapRect(x, y, w, h);
      traceRoundRect(box.x, box.y, box.w, box.h,
                     radius > 0 ? Math.round(radius * sx) / sx : 0);
      ctx.clip();
    },
    restore: () => popState(),
    /* PNG / JPEG / WebP / GIF via the browser decoder. Cache by src.
       fit: 0 stretch, 1 contain, 2 cover, 3 none. */
    image: (x, y, w, h, src, radius, alpha, fit) => {
      if (!src || w <= 0 || h <= 0) return;
      let rec = null;
      let iw = 0;
      let ih = 0;
      if (src.startsWith("media:")) {
        /* A player's current frame (see `media`). Nothing until the first
           frame decodes; the poster is the app's business. */
        const m = media[parseInt(src.slice(6), 10) | 0];
        if (!m || !m.src || m.error || m.el.readyState < 2 || !m.el.videoWidth) return;
        rec = m.el;
        iw = rec.videoWidth;
        ih = rec.videoHeight;
      } else {
        rec = imgCache.get(src);
        if (rec === "fail") return;
        if (!rec) {
          const im = new Image();
          /* Tiny `data:` catalog shots should decode on this paint, not the
             next rAF. `decoding = "sync"` is a hint; we still draw if
             `complete` after setting `src`. */
          if ("decoding" in im) im.decoding = "sync";
          im.onload = () => schedule(0);
          im.onerror = () => {
            imgCache.set(src, "fail");
          };
          im.src = src;
          imgCache.set(src, im);
          rec = im;
        }
        if (!rec.complete || !rec.naturalWidth) return;
        iw = rec.naturalWidth;
        ih = rec.naturalHeight;
      }
      const p = snapRect(x, y, w, h);
      let dx = p.x, dy = p.y, dw = p.w, dh = p.h;
      if (fit === 3) {
        dw = iw;
        dh = ih;
      } else if (fit === 1 || fit === 2) {
        if ((fit === 1 && iw * p.h <= ih * p.w) || (fit === 2 && iw * p.h >= ih * p.w)) {
          dh = p.h;
          dw = ih ? (iw * p.h) / ih : p.w;
        } else {
          dw = p.w;
          dh = iw ? (ih * p.w) / iw : p.h;
        }
        dx = p.x + (p.w - dw) / 2;
        dy = p.y + (p.h - dh) / 2;
      }
      ctx.save();
      if (radius > 0) {
        let r = radius;
        if (r > p.w / 2) r = p.w / 2;
        if (r > p.h / 2) r = p.h / 2;
        ctx.beginPath();
        ctx.moveTo(p.x + r, p.y);
        ctx.arcTo(p.x + p.w, p.y, p.x + p.w, p.y + p.h, r);
        ctx.arcTo(p.x + p.w, p.y + p.h, p.x, p.y + p.h, r);
        ctx.arcTo(p.x, p.y + p.h, p.x, p.y, r);
        ctx.arcTo(p.x, p.y, p.x + p.w, p.y, r);
        ctx.closePath();
        ctx.clip();
      } else {
        ctx.beginPath();
        ctx.rect(p.x, p.y, p.w, p.h);
        ctx.clip();
      }
      ctx.globalAlpha = alpha >= 0 && alpha < 255 ? alpha / 255 : 1;
      ctx.drawImage(rec, dx, dy, dw, dh);
      popState();
    },
    /* A vector path the engine parsed (icons, charts, `Svg`): `n` int32 of
       the frame's path stream `c` from `start`, commands 0 move x y / 1 line x y / 2 cubic x1 y1 x2 y2 x y /
       3 close, in 1/64 px. `width` 0 fills (non-zero); otherwise a stroke
       `width`/64 wide, `style` = cap + 4 * join. */
    path: (c, start, n, color, a, width, style) => {
      const u = 1 / 64;
      const end = start + n;
      ctx.beginPath();
      let i = start;
      while (i < end) {
        const k = c[i++];
        if (k === 0) {
          ctx.moveTo(c[i] * u, c[i + 1] * u);
          i += 2;
        } else if (k === 1) {
          ctx.lineTo(c[i] * u, c[i + 1] * u);
          i += 2;
        } else if (k === 2) {
          ctx.bezierCurveTo(c[i] * u, c[i + 1] * u, c[i + 2] * u, c[i + 3] * u,
                            c[i + 4] * u, c[i + 5] * u);
          i += 6;
        } else if (k === 3) {
          ctx.closePath();
        } else {
          break;
        }
      }
      if (width === 0) {
        ctx.fillStyle = rgba(color, a);
        ctx.fill();
        return;
      }
      ctx.save();
      ctx.strokeStyle = rgba(color, a);
      ctx.lineWidth = width * u;
      ctx.lineCap = ["butt", "round", "square"][style & 3] || "butt";
      ctx.lineJoin = ["miter", "round", "bevel"][style >> 2] || "miter";
      ctx.stroke();
      ctx.restore();
    },
  };

  const imports = {
    /* clang wasm32 libcalls (`__multi3`, …) land in `env` unless linked in C. */
    env: {
      __multi3: (out, a0, a1, b0, b1) => {
        const a =
          BigInt.asUintN(64, BigInt(a0)) + (BigInt.asUintN(64, BigInt(a1)) << 64n);
        const b =
          BigInt.asUintN(64, BigInt(b0)) + (BigInt.asUintN(64, BigInt(b1)) << 64n);
        const r = a * b;
        const view = new DataView(mem.buffer);
        view.setBigUint64(out, BigInt.asUintN(64, r), true);
        view.setBigUint64(out + 8, BigInt.asUintN(64, r >> 64n), true);
      },
    },
    zeus: {
      /* Wake the idle rAF loop (image decode, plat_redraw, async fetch). */
      request_frame: () => schedule(0),
      /* Monotonic milliseconds (std:async `now_ms`). i64: return a BigInt. */
      now_ms: () => BigInt(Math.floor(performance.now())),
      /* Window title from the router's `Meta.title`. */
      set_title: (t) => {
        document.title = cstr(t);
      },
      /* `CopyButton`: best effort — the browser may refuse without a gesture. */
      clipboard_write: (t) => {
        const s = cstr(t);
        if (navigator.clipboard) navigator.clipboard.writeText(s).catch(() => {});
      },
      /* ⌘-style shortcuts on Apple platforms (`KbdCombo`). */
      apple_keys: () =>
        /Mac|iPhone|iPad|iPod/.test(navigator.platform || navigator.userAgent) ? 1 : 0,
      /* Router history mirror: the Loam stack is authoritative; these keep the
         browser's own back/forward in step and make URLs shareable. */
      /* The URL the app starts on, written to `buf`, length returned. Asked for
         while `main` builds, so a deep link builds only its own page. */
      boot_path: (buf, cap) => {
        const enc = new TextEncoder().encode(location.pathname + location.search);
        const n = enc.length < cap ? enc.length : 0;
        new Uint8Array(mem.buffer, buf, n).set(enc.subarray(0, n));
        return n;
      },
      set_head: (keyPtr, keyLen, valuePtr, valueLen) => {
        setHead(bytes(keyPtr, keyLen), bytes(valuePtr, valueLen));
      },
      /* localStorage throws when storage is blocked: then the choice lasts as
         long as the page. */
      appearance_load: () => {
        try {
          const v = localStorage.getItem(APPEARANCE_KEY);
          return v === "0" ? 0 : v === "1" ? 1 : -1;
        } catch (e) {
          return -1;
        }
      },
      appearance_save: (dark) => {
        try {
          localStorage.setItem(APPEARANCE_KEY, String(dark));
        } catch (e) {}
        const root = document.documentElement;
        const paper = dark ? root.dataset.paperDark : root.dataset.paperLight;
        if (!paper) return;
        root.style.setProperty("--zeus-paper", paper);
        const bar = document.querySelector('meta[name="theme-color"]');
        if (bar) bar.setAttribute("content", paper);
      },
      /* Not the `noopener` feature: with it `open` always returns null, which
         reads as blocked. A really blocked popup leaves in this tab instead. */
      open_external: (ptr, len) => {
        const url = bytes(ptr, len);
        const tab = window.open(url, "_blank");
        if (tab) tab.opener = null;
        else location.href = url;
      },
      chunks_ready: (index) => (routeReady(index) ? 1 : 0),
      chunks_load: (index) => loadRoute(index),
      chunks_preload: (index) => preloadRoute(index),
      history_push: (p) => {
        const path = cstr(p);
        if (location.pathname + location.search !== path) {
          history.pushState(null, "", path);
        }
      },
      history_replace: (p) => {
        history.replaceState(null, "", cstr(p));
      },
      history_back: () => {
        history.back();
      },
      /* WebSocket bridge: open one same-origin socket; inbound text messages
         queue in JS until wasm drains them per frame. */
      ws_open: (urlPtr, urlLen, slot) => {
        try {
          if (slot < 0 || slot >= wsSlots.length) return -1;
          const url = bytes(urlPtr, urlLen);
          if (!url) return -1;
          const sock = new WebSocket(url);
          wsSlots[slot] = { sock: sock, queue: [] };
          sock.onmessage = (ev) => {
            const q = wsSlots[slot] ? wsSlots[slot].queue : null;
            if (!q) return;
            q.push(String(ev.data));
            if (q.length > 256) q.shift();
          };
          return 0;
        } catch (e) {
          console.warn("ws_open", e);
          wsSlots[slot] = null;
          return -1;
        }
      },
      ws_state: (slot) => {
        const r = wsSlots[slot];
        if (!r || !r.sock) return 1;
        return r.sock.readyState === WebSocket.OPEN ? 0 : 1;
      },
      ws_count: (slot) => {
        const r = wsSlots[slot];
        return r && r.queue ? r.queue.length : 0;
      },
      ws_copy: (slot, ptr, cap) => {
        const r = wsSlots[slot];
        if (!r || !r.queue || !r.queue.length) return -1;
        const s = r.queue.shift();
        const b = te ? te.encode(s) : Array.from(s).map((c) => c.charCodeAt(0) & 0xff);
        const n = Math.min(b.length, Math.max(cap, 0));
        const dst = u8();
        for (let i = 0; i < n; i++) dst[ptr + i] = b[i];
        return n;
      },
      ws_close: (slot) => {
        const r = wsSlots[slot];
        if (r && r.sock) {
          try {
            r.sock.close();
          } catch (e) {}
        }
        wsSlots[slot] = null;
      },
      /* Streaming gRPC-Web bridge: POST once, keep the reply open, queue the
         bytes as they arrive (`onprogress`); wasm drains them per frame and
         splits the frames itself. State: 0 open, 1 ended, 2 failed. An XHR
         keeps its whole `responseText`, so a long stream is ended past a cap
         and the caller reopens it. */
      rs_open: (pathPtr, pathLen, bodyPtr, bodyLen, slot) => {
        try {
          if (slot < 0 || slot >= rsSlots.length) return -1;
          const path = bytes(pathPtr, pathLen) || "/";
          const body =
            bodyLen > 0 ? u8().slice(bodyPtr, bodyPtr + bodyLen) : new Uint8Array(0);
          const xhr = new XMLHttpRequest();
          const r = { xhr: xhr, seen: 0, pending: "", state: 0 };
          rsSlots[slot] = r;
          const pull = () => {
            const t = xhr.responseText || "";
            if (t.length > r.seen) {
              r.pending += t.slice(r.seen);
              r.seen = t.length;
            }
            if (r.seen > 4194304 && r.state === 0) {
              r.state = 1;
              xhr.abort();
            }
          };
          xhr.open("POST", path, true);
          xhr.overrideMimeType("text/plain; charset=x-user-defined");
          xhr.setRequestHeader("Content-Type", "application/grpc-web+proto");
          xhr.setRequestHeader("Accept", "application/grpc-web+proto");
          xhr.onprogress = pull;
          xhr.onloadend = () => {
            if (r.state !== 0) return;
            if (xhr.status === 200) {
              pull();
              r.state = 1;
            } else {
              r.state = 2;
            }
          };
          xhr.send(body);
          return 0;
        } catch (e) {
          console.warn("rs_open", e);
          rsSlots[slot] = null;
          return -1;
        }
      },
      rs_state: (slot) => {
        const r = rsSlots[slot];
        if (!r) return 2;
        /* Ended still reads as open until wasm has drained the last bytes. */
        if (r.state !== 0 && r.pending.length > 0) return 0;
        return r.state;
      },
      rs_copy: (slot, ptr, cap) => {
        const r = rsSlots[slot];
        if (!r || !r.pending.length) return 0;
        const n = Math.min(r.pending.length, Math.max(cap, 0));
        const dst = u8();
        for (let i = 0; i < n; i++) dst[ptr + i] = r.pending.charCodeAt(i) & 0xff;
        r.pending = r.pending.slice(n);
        return n;
      },
      rs_close: (slot) => {
        const r = rsSlots[slot];
        if (r && r.xhr && r.state === 0) {
          r.state = 1;
          try {
            r.xhr.abort();
          } catch (e) {}
        }
        rsSlots[slot] = null;
      },
      write: (p, n) => {
        const t = bytes(p, n);
        if (opts.onWrite) {
          opts.onWrite(t);
          return;
        }
        /* `fmt.println` arrives in pieces: buffer to the newline so a line is
           one log line (see `logLine`). */
        consoleLine += t;
        let nl;
        while ((nl = consoleLine.indexOf("\n")) >= 0) {
          const line = consoleLine.slice(0, nl);
          consoleLine = consoleLine.slice(nl + 1);
          logLine(line);
        }
      },
      panic: (p, n) => {
        const e = new Error(bytes(p, n) || cstr(p) || "zeus panic");
        /* Marks a Loam trap for `guardExports` (a RuntimeError is the other kind). */
        e.zeusPanic = true;
        throw e;
      },
      view_w: () => Math.max(1, Math.round(canvas.clientWidth || window.innerWidth || 1)),
      view_h: () => Math.max(1, Math.round(canvas.clientHeight || window.innerHeight || 1)),
      /* The whole frame, once per paint: see `decodeFrame`. */
      frame: (head) => decodeFrame(mem.buffer, head, paint),
      /* "" restores the host default. Quoting the family lets names with
         spaces ("Universal Sans") survive the CSS shorthand. */
      set_font_family: (ptr) => {
        const name = cstr(ptr);
        fontFace = name ? '"' + name + '", ' + fontFallback : fontFallback;
        resetFontCaches();
      },
      /* Loads asynchronously; 1 means "accepted", not "ready". Metrics are
         dropped again on arrival so the first frames using the fallback are
         re-measured once the real face lands. */
      load_font: (famPtr, srcPtr) => {
        const family = cstr(famPtr);
        const src = cstr(srcPtr);
        if (!family || !src || typeof FontFace === "undefined") return 0;
        try {
          const face = new FontFace(family, 'url("' + src + '")');
          face.load().then((f) => {
            document.fonts.add(f);
            resetFontCaches();
            if (exp && exp.zeus_fonts_changed) exp.zeus_fonts_changed();
            schedule(0);
          }).catch(() => {});
          return 1;
        } catch (e) {
          return 0;
        }
      },
      /* In-tree metrics need the font's bytes, which wasm cannot read from a
         URL. Fetch them, copy into the wasm buffer, and bind — async like
         load_font, so layout keeps measureText until this lands. */
      font_fetch: (ptr) => {
        const src = cstr(ptr);
        if (!src) return 0;
        fetch(src).then((r) => r.arrayBuffer()).then((buf) => {
          if (!exp || !mem) return;
          const n = buf.byteLength;
          if (!n) return;
          const dst = exp.zeus_font_reserve ? exp.zeus_font_reserve(n) : 0;
          if (!dst) return;
          new Uint8Array(mem.buffer, dst, n).set(new Uint8Array(buf));
          if (exp.zeus_font_set) exp.zeus_font_set(n);
          schedule(0);
        }).catch(() => {});
        return 1;
      },
      measure: (ptr, px, wPtr, hPtr) => {
        const s = cstr(ptr);
        const size = px > 0 ? px : 13;
        const box = lineBox(size);
        setFont(size);
        let m = textWidths.get(size);
        let w = m ? m.get(s) : undefined;
        if (w === undefined) {
          w = ctx.measureText(s).width;
          if (!m) {
            m = new Map();
            textWidths.set(size, m);
          }
          if (m.size >= 4096) {
            /* Bound memory for live-edited text: drop the oldest entry. */
            m.delete(m.keys().next().value);
          }
          m.set(s, w);
        }
        const view = new DataView(mem.buffer);
        view.setInt32(wPtr, Math.ceil(w), true);
        view.setInt32(hPtr, box.height, true);
      },
      pick_image: () => {
        if (!fileInput) {
          fileInput = document.createElement("input");
          fileInput.type = "file";
          fileInput.accept = "image/png,image/jpeg,image/webp,image/gif";
          fileInput.style.display = "none";
          document.body.appendChild(fileInput);
          fileInput.addEventListener("change", () => {
            const f = fileInput.files && fileInput.files[0];
            fileInput.value = "";
            if (!f || !exp || !te || !mem) return;
            const url = URL.createObjectURL(f);
            const im = new Image();
            im.onload = () => {
              imgCache.set(url, im);
              const bytes = te.encode(url);
              const cap = exp.zeus_text_buf_cap ? exp.zeus_text_buf_cap() : 0;
              const ptr = exp.zeus_text_buf ? exp.zeus_text_buf() : 0;
              if (!ptr || cap < 2) return;
              const n = Math.min(bytes.length, cap - 1);
              new Uint8Array(mem.buffer, ptr, n).set(bytes.subarray(0, n));
              if (exp.zeus_picked) exp.zeus_picked(n, im.naturalWidth, im.naturalHeight);
              schedule(0);
            };
            im.onerror = () => {
              imgCache.set(url, "fail");
            };
            im.src = url;
          });
        }
        fileInput.click();
      },
      /* Intrinsic size for layout (`platform.plat_image_size`): the two i32
         out-params. Decode is shared with `image`; a not-yet-decoded source
         reports 0 and the load's `schedule` re-lays out on the next frame. */
      image_size: (ptr, wPtr, hPtr) => {
        const view = new DataView(mem.buffer);
        view.setInt32(wPtr, 0, true);
        view.setInt32(hPtr, 0, true);
        const src = cstr(ptr);
        if (!src) return;
        if (src.startsWith("media:")) {
          const m = media[parseInt(src.slice(6), 10) | 0];
          if (!m || !m.src || !m.el.videoWidth) return;
          view.setInt32(wPtr, m.el.videoWidth, true);
          view.setInt32(hPtr, m.el.videoHeight, true);
          return;
        }
        let rec = imgCache.get(src);
        if (rec === "fail") return;
        if (!rec) {
          const im = new Image();
          if ("decoding" in im) im.decoding = "sync";
          im.onload = () => schedule(0);
          im.onerror = () => {
            imgCache.set(src, "fail");
          };
          im.src = src;
          imgCache.set(src, im);
          rec = im;
        }
        if (!rec.complete || !rec.naturalWidth) return;
        view.setInt32(wPtr, rec.naturalWidth, true);
        view.setInt32(hPtr, rec.naturalHeight, true);
      },
      /* `plat_media_*`: see `media` above. Codes match zeus_plat.c. */
      media_open: (slot, ptr) => {
        if (slot <= 0 || slot >= media.length) return 0;
        const src = cstr(ptr);
        const m = mediaEl(slot);
        m.error = "";
        m.waiting = false;
        m.seeking = false;
        m.src = src;
        if (!src) {
          m.el.removeAttribute("src");
          m.el.load();
          return 0;
        }
        /* Blob URLs and same-origin files never need CORS; keep the
           anonymous mode only where it can matter. */
        if (/^https?:/.test(src)) m.el.crossOrigin = "anonymous";
        else m.el.removeAttribute("crossorigin");
        m.el.src = src;
        m.el.load();
        schedule(0);
        return 1;
      },
      media_cmd: (slot, cmd, value) => {
        const m = media[slot];
        if (!m) return;
        const v = m.el;
        if (cmd === 1) {
          if (v.ended) v.currentTime = 0;
          const pr = v.play();
          /* Autoplay policy: a play outside a gesture is refused with
             sound; retry muted rather than leave the UI saying "playing". */
          if (pr && pr.catch) pr.catch((e) => {
            if (e && e.name === "NotAllowedError" && !v.muted) {
              v.muted = true;
              v.play().catch(() => {});
            }
            schedule(0);
          });
        } else if (cmd === 2) v.pause();
        else if (cmd === 3) {
          const d = Number.isFinite(v.duration) ? v.duration : Infinity;
          const t = Math.max(0, Math.min(value / 1000, d));
          v.currentTime = t;
        } else if (cmd === 4) {
          const r = Math.max(0.0625, Math.min(16, value / 100));
          v.playbackRate = r;
          v.defaultPlaybackRate = r;
          v.preservesPitch = true;
        } else if (cmd === 5) v.volume = Math.max(0, Math.min(1, value / 100));
        else if (cmd === 6) v.muted = !!value;
        else if (cmd === 7) v.loop = !!value;
        else if (cmd === 8) {
          v.pause();
          v.removeAttribute("src");
          v.load();
          m.src = "";
          m.error = "";
        }
        schedule(0);
      },
      media_get: (slot, what) => {
        if (what === 7) return mediaPicked.seq;
        const m = media[slot];
        if (!m) return 0;
        const v = m.el;
        if (what === 1) return Math.round((v.currentTime || 0) * 1000);
        if (what === 2) return Number.isFinite(v.duration) ? Math.round(v.duration * 1000) : 0;
        if (what === 3) return mediaState(m);
        if (what === 4) return v.videoWidth | 0;
        if (what === 5) return v.videoHeight | 0;
        if (what === 6) {
          const b = v.buffered;
          const t = v.currentTime || 0;
          for (let i = 0; i < b.length; i++) {
            if (b.start(i) <= t + 0.5 && b.end(i) >= t) return Math.round(b.end(i) * 1000);
          }
          return Math.round(t * 1000);
        }
        return 0;
      },
      media_text: (slot, what, out, cap) => {
        let s = "";
        if (what === 1) {
          if (mediaPicked.seq) s = mediaPicked.name + "\n" + mediaPicked.src;
        } else if (media[slot]) s = media[slot].error;
        if (!s || !te || cap <= 1) return 0;
        const b = te.encode(s);
        const n = Math.min(b.length, cap - 1);
        u8().set(b.subarray(0, n), out);
        return n;
      },
      media_pick: () => {
        if (!mediaInput) {
          mediaInput = document.createElement("input");
          mediaInput.type = "file";
          mediaInput.accept = MEDIA_ACCEPT;
          mediaInput.style.display = "none";
          document.body.appendChild(mediaInput);
          mediaInput.addEventListener("change", () => {
            const f = mediaInput.files && mediaInput.files[0];
            mediaInput.value = "";
            mediaPick(f);
          });
        }
        mediaInput.click();
      },
      fullscreen: (on) => {
        const d = document;
        const el = d.documentElement;
        if (on === 1 && !d.fullscreenElement && el.requestFullscreen) {
          el.requestFullscreen().catch(() => {});
          return 1;
        }
        if (on === 0 && d.fullscreenElement && d.exitFullscreen) {
          d.exitFullscreen().catch(() => {});
          return 0;
        }
        return d.fullscreenElement ? 1 : 0;
      },
      fetch_rpc_async: (pathPtr, pathLen, bodyPtr, bodyLen, outPtr, cap, handle, sync) => {
        /* `sync` 1: a blocking XHR, for `await` (its pump loop holds this
           thread, so an async XHR would never complete under it). `sync` 0:
           a real async XHR, so the page keeps painting while the server
           works; the reply lands in the slot on a later frame. `zeus_fetch_done`
           gets the byte count, -1 for no response, or -(1000 + status). */
        const finish = (xhr) => {
          if (!exp || !exp.zeus_fetch_done) return;
          if (xhr.status !== 200) {
            exp.zeus_fetch_done(handle, xhr.status > 0 ? -(1000 + xhr.status) : -1);
            return;
          }
          const t = xhr.responseText || "";
          const n = Math.min(t.length, Math.max(cap, 0));
          const dst = u8();
          for (let i = 0; i < n; i++) dst[outPtr + i] = t.charCodeAt(i) & 0xff;
          exp.zeus_fetch_done(handle, n);
        };
        try {
          const path = bytes(pathPtr, pathLen) || "/";
          const body =
            bodyLen > 0 ? u8().slice(bodyPtr, bodyPtr + bodyLen) : new Uint8Array(0);
          const xhr = new XMLHttpRequest();
          xhr.open("POST", path, !sync);
          xhr.overrideMimeType("text/plain; charset=x-user-defined");
          xhr.setRequestHeader("Content-Type", "application/grpc-web+proto");
          xhr.setRequestHeader("Accept", "application/grpc-web+proto");
          if (sync) {
            xhr.send(body);
            finish(xhr);
            return 0;
          }
          xhr.onloadend = () => {
            try {
              finish(xhr);
            } catch (e) {
              console.warn("fetch_rpc_async", e);
            }
          };
          xhr.send(body);
          return 0;
        } catch (e) {
          console.warn("fetch_rpc_async", e);
          return -1;
        }
      },
      fetch_rpc: (pathPtr, pathLen, bodyPtr, bodyLen, outPtr, cap) => {
        try {
          const path = bytes(pathPtr, pathLen) || "/";
          const body =
            bodyLen > 0 ? u8().slice(bodyPtr, bodyPtr + bodyLen) : new Uint8Array(0);
          const xhr = new XMLHttpRequest();
          xhr.open("POST", path, false);
          /* Sync XHR forbids responseType = "arraybuffer". x-user-defined
             keeps each byte as a Latin-1 code unit in responseText. */
          xhr.overrideMimeType("text/plain; charset=x-user-defined");
          xhr.setRequestHeader("Content-Type", "application/grpc-web+proto");
          xhr.setRequestHeader("Accept", "application/grpc-web+proto");
          xhr.send(body);
          if (xhr.status !== 200) return 0;
          const t = xhr.responseText || "";
          const n = Math.min(t.length, Math.max(cap, 0));
          const dst = u8();
          for (let i = 0; i < n; i++) dst[outPtr + i] = t.charCodeAt(i) & 0xff;
          return n;
        } catch (e) {
          console.warn("fetch_rpc", e);
          return 0;
        }
      },
    },
  };

    function stop() {
      stopped = true;
      if (raf) cancelAnimationFrame(raf);
      raf = 0;
      if (wakeTimer) clearTimeout(wakeTimer);
      wakeTimer = 0;
      ac.abort();
      for (const m of media) {
        if (m) m.el.pause();
      }
    }

    /* Wasm has no setjmp, so a Loam panic (bounds, overflow, `panic`) cannot be
       caught inside the module: it traps, and the RuntimeError unwinds out of
       whichever export JS called. Every export goes through this wrapper. At the
       outermost call it puts the shadow stack pointer back (the trap skipped
       every epilogue that would have) and asks the program to recover: the
       innermost `Boundary` that was at work takes the trap and shows its
       fallback on the next frame. A trap no boundary covers is rethrown. */
    /* Boundaries that trapped during startup, carried across re-instantiation. */
    /* Page metadata. The build writes each route's tags into its HTML, and a
       route change has no new HTML, so these keep the head matching the page.
       An empty value removes a tag, so one page's tags never outlive it. */
    function setTag(attr, name, value) {
      let el = document.head.querySelector(`meta[${attr}="${name}"]`);
      if (!value) {
        if (el) el.remove();
        return;
      }
      if (!el) {
        el = document.createElement("meta");
        el.setAttribute(attr, name);
        document.head.appendChild(el);
      }
      el.setAttribute("content", value);
    }

    /* `--base` is only known to the build, so it comes from the canonical URL
       the page was served with. */
    function canonicalOrigin(link) {
      try {
        return new URL(link.href).origin;
      } catch (e) {
        return location.origin;
      }
    }

    function setCanonical(value) {
      let link = document.head.querySelector('link[rel="canonical"]');
      const base = link ? canonicalOrigin(link) : location.origin;
      const url = new URL(value, base).href;
      if (!link) {
        link = document.createElement("link");
        link.rel = "canonical";
        document.head.appendChild(link);
      }
      link.href = url;
      setTag("property", "og:url", url);
    }

    function setJsonLd(value) {
      let el = document.head.querySelector('script[type="application/ld+json"]');
      if (!value) {
        if (el) el.remove();
        return;
      }
      if (!el) {
        el = document.createElement("script");
        el.type = "application/ld+json";
        document.head.appendChild(el);
      }
      el.textContent = value;
    }

    /* A card takes its text from Open Graph, and only a page with an image gets
       the large card, matching what the build writes. */
    function setImage(value) {
      const og = (n) => {
        const el = document.head.querySelector(`meta[property="og:${n}"]`);
        return el ? el.getAttribute("content") : "";
      };
      setTag("property", "og:image", value);
      setTag("name", "twitter:card", value ? "summary_large_image" : "summary");
      setTag("name", "twitter:image", value);
      setTag("name", "twitter:title", value ? og("title") : "");
      setTag("name", "twitter:description", value ? og("description") : "");
    }

    function setHead(key, value) {
      if (key === "title") {
        if (value) setTag("property", "og:title", value);
      } else if (key === "description") {
        setTag("name", "description", value);
        setTag("property", "og:description", value);
      } else if (key === "canonical") {
        setCanonical(value);
      } else if (key === "robots") {
        setTag("name", "robots", value);
      } else if (key === "og_type") {
        setTag("property", "og:type", value);
      } else if (key === "jsonld") {
        setJsonLd(value);
      } else if (key === "og_image") {
        setImage(value);
      }
    }

    const bootPresets = [];
    let wasmModule = null;

    /* Route chunks. A split build keeps each route's code in its own module,
       sharing the primary's memory, table and stack pointer. A chunk has no
       start function: instantiating it writes its functions into the table.
       The router loads what a route needs before building it, so nothing below
       `placeholders` runs in normal use. */
    const chunkUrls = opts.chunks || {};
    const routeChunks = opts.routes || [];
    const chunkLinked = new Set();
    /* Compiled modules, kept so a restarted instance can link them again. */
    const chunkModules = new Map();
    const chunkPending = new Map();
    let primaryExports = null;

    function linkChunk(name, mod) {
      chunkModules.set(name, mod);
      if (chunkLinked.has(name) || !primaryExports) return;
      new WebAssembly.Instance(mod, { primary: primaryExports, zeus: imports.zeus });
      chunkLinked.add(name);
    }

    /* Fetch and link one chunk, once, with two retries for a flaky network. */
    function loadChunk(name, tries = 3) {
      if (chunkModules.has(name)) {
        linkChunk(name, chunkModules.get(name));
        return Promise.resolve();
      }
      if (chunkPending.has(name)) return chunkPending.get(name);
      const p = fetch(chunkUrls[name])
        .then((r) => {
          if (!r.ok) throw new Error("chunk " + name + ": " + r.status);
          return WebAssembly.compileStreaming ? WebAssembly.compileStreaming(r) : r.arrayBuffer().then((b) => WebAssembly.compile(b));
        })
        .then((mod) => {
          chunkPending.delete(name);
          linkChunk(name, mod);
        })
        .catch((e) => {
          chunkPending.delete(name);
          if (tries > 1) return new Promise((r) => setTimeout(r, 300)).then(() => loadChunk(name, tries - 1));
          throw e;
        });
      chunkPending.set(name, p);
      return p;
    }

    function routeReady(index) {
      return (routeChunks[index] || []).every((n) => chunkLinked.has(n));
    }

    /* A chunk that cannot be had usually means a new deploy replaced it, so
       load this URL for real, once. A page that fails again stops here. */
    function reloadForChunks() {
      const key = "zeus-chunk-reload";
      if (sessionStorage.getItem(key) === location.href) return;
      sessionStorage.setItem(key, location.href);
      location.reload();
    }

    /* Fetching ahead is a guess, so it never spends a metered connection, and a
       failure is left for the real navigation to retry and report. */
    function canFetchAhead() {
      const net = navigator.connection;
      return !(net && (net.saveData || /2g/.test(net.effectiveType || "")));
    }

    /* Wait out the pointer: a sweep down a nav list settles on one link, and
       only that one should cost a download. */
    let preloadTimer = 0;
    function preloadRoute(index) {
      clearTimeout(preloadTimer);
      if (!canFetchAhead()) return;
      const fetchAll = () => {
        for (const n of routeChunks[index] || []) loadChunk(n).catch(() => {});
      };
      /* A finger is already on the link, with a tap about 100 ms away. */
      if (touching) fetchAll();
      else preloadTimer = setTimeout(fetchAll, 80);
    }

    /* After the page is up, fetch the chunks most other pages share, one at a
       time so a visitor's own requests always come first. A screen without hover
       never gets a head start on a link, so it takes the build's next few too. */
    function preloadIdle() {
      const noHover = window.matchMedia && matchMedia("(hover: none)").matches;
      const names = (opts.idle || []).concat(noHover ? opts.idleTouch || [] : []);
      if (!canFetchAhead()) return;
      const later = window.requestIdleCallback || ((f) => setTimeout(f, 1000));
      const next = () => {
        const name = names.shift();
        if (name) loadChunk(name).catch(() => {}).then(() => later(next));
      };
      later(next);
    }

    function loadRoute(index) {
      /* Under 150 ms a wait reads as instant, and a spinner would only flash. */
      const slow = setTimeout(() => {
        if (exp.zeus_chunks_slow) exp.zeus_chunks_slow(index);
        schedule(0);
      }, 150);
      Promise.all((routeChunks[index] || []).map((n) => loadChunk(n))).then(
        () => {
          clearTimeout(slow);
          exp.zeus_chunks_done(index);
          schedule(0);
        },
        (e) => {
          clearTimeout(slow);
          reloadForChunks(e);
        }
      );
    }

    /* Sync XHR refuses `responseType` on a page, so the bytes come back as
       text with one code unit per byte. */
    function fetchChunkSync(name) {
      const xhr = new XMLHttpRequest();
      xhr.open("GET", chunkUrls[name], false);
      xhr.overrideMimeType("text/plain; charset=x-user-defined");
      xhr.send();
      if (xhr.status !== 200) throw new Error("failed to load chunk " + name + " (" + xhr.status + ")");
      const text = xhr.responseText;
      const bytes = new Uint8Array(text.length);
      for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i) & 255;
      return new WebAssembly.Module(bytes);
    }

    /* The safety net. Each table slot of a function in an unloaded chunk holds
       one of these: the module is `placeholder.<chunk>`, the field its slot.
       A call means the router missed a dependency, so load the chunk now, which
       overwrites the slot, and call the real function. Slow, but correct. */
    function placeholder(chunk, slot) {
      return function (...args) {
        console.warn("zeus: chunk " + chunk + " loaded late, by a call");
        linkChunk(chunk, fetchChunkSync(chunk));
        return splitTable().get(slot)(...args);
      };
    }

    /* The table `wasm-split` adds for calls into chunks, beside the original. */
    function splitTable() {
      const name = Object.keys(primaryExports).find((k) => k.startsWith("%table_"));
      return primaryExports[name];
    }

    /* Short, as every function in a chunk has an import naming it (`zeli`'s
       `--placeholder-namespace-prefix`). */
    const PLACEHOLDER_NS = "p.";

    function placeholders(mod) {
      const chunk = mod.slice(PLACEHOLDER_NS.length);
      return new Proxy({}, { get: (_, field) => placeholder(chunk, Number(field)) });
    }

    const moduleImports = new Proxy(imports, {
      get: (t, k) => (typeof k === "string" && k.startsWith(PLACEHOLDER_NS) ? placeholders(k) : t[k]),
    });

    function cstrIn(raw, ptr) {
      if (!ptr) return "";
      const u8 = new Uint8Array(raw.memory.buffer);
      let end = ptr;
      while (end < u8.length && u8[end] !== 0) end++;
      return new TextDecoder().decode(u8.subarray(ptr, end));
    }

    function guardExports(raw) {
      const sp = raw.__stack_pointer;
      const spBase = sp ? sp.value : 0;
      let depth = 0;
      const out = {};
      for (const k of Object.keys(raw)) {
        const v = raw[k];
        if (typeof v !== "function" || k === "zeus_recover") {
          out[k] = v;
          continue;
        }
        out[k] = function (...args) {
          depth++;
          try {
            return v.apply(null, args);
          } catch (e) {
            const trap = e instanceof WebAssembly.RuntimeError || (e && e.zeusPanic);
            if (depth > 1 || !trap) throw e;
            if (sp) sp.value = spBase;
            let ok = 0;
            try {
              ok = raw.zeus_recover ? raw.zeus_recover() | 0 : 0;
            } catch (e2) {
              ok = 0;
            }
            if (!ok) throw e;
            if (ok === 2) {
              /* The trap hit a Boundary while `main` was building the app: the
                 rest of that build is gone, so start over from a fresh
                 instance with this boundary pre-seeded as failed. */
              bootPresets.push({ seq: raw.zeus_trap_seq() | 0, msg: cstrIn(raw, raw.zeus_trap_msg()) });
              const r = new Error("zeus: restart after a trap during startup");
              r.zeusRestart = true;
              throw r;
            }
            console.warn("zeus: a Boundary caught a trap in " + k, e);
            if (schedule) schedule(0);
            return 0;
          } finally {
            depth--;
          }
        };
      }
      return out;
    }

    function start(instance) {
        if (stopped) return;
        primaryExports = instance.exports;
        chunkLinked.clear();
        for (const [name, mod] of chunkModules) linkChunk(name, mod);
        exp = guardExports(instance.exports);
        mem = exp.memory;
        const sz = sizeCanvas();
        function frame() {
          raf = 0;
          if (stopped) return;
          ctx.setTransform(1, 0, 0, 1, 0, 0);
          ctx.clearRect(0, 0, canvas.width, canvas.height);
          applyLayoutTransform();
          let next = 0;
          try {
            const coasting = touchFrame ? touchFrame() : false;
            next = exp.zeus_paint() | 0;
            if (coasting) next = 1;
            revealCanvas();
            syncIme();
            syncA11y();
          } catch (e) {
            console.error("zeus_paint", e);
            return;
          }
          if (mediaLive()) next = 1;
          if (next) schedule(next <= 1 ? 0 : next);
        }
        schedule = function (ms) {
          if (stopped) return;
          if (!ms) {
            if (wakeTimer) {
              clearTimeout(wakeTimer);
              wakeTimer = 0;
            }
            if (!raf) raf = requestAnimationFrame(frame);
            return;
          }
          if (raf) return;
          if (wakeTimer) return;
          wakeTimer = setTimeout(() => {
            wakeTimer = 0;
            schedule(0);
          }, ms);
        };
        /* Tree build only — layout waits for the first paint so the tab can
           finish loading and input listeners can attach. */
        /* Pre-seed the boundaries a previous instance trapped in at startup. */
        if (exp.zeus_boundary_preset && exp.zeus_text_buf) {
          for (const p of bootPresets) {
            const ptr = exp.zeus_text_buf();
            const cap = exp.zeus_text_buf_cap ? exp.zeus_text_buf_cap() : 0;
            const b = new TextEncoder().encode(p.msg);
            const n = Math.min(b.length, Math.max(0, cap - 1));
            new Uint8Array(mem.buffer, ptr, n).set(b.subarray(0, n));
            new Uint8Array(mem.buffer, ptr + n, 1)[0] = 0;
            exp.zeus_boundary_preset(p.seq, ptr);
          }
        }
        try {
          exp.zeus_start();
        } catch (e) {
          if (e && e.zeusRestart && wasmModule && bootPresets.length <= 16) {
            return WebAssembly.instantiate(wasmModule, moduleImports).then((inst) => start(inst));
          }
          throw e;
        }
        if (exp.zeus_resize) exp.zeus_resize(sz.w, sz.h);
        /* Reduced motion (§3.2): animations jump to their final value, and a
           change of preference takes effect immediately. */
        if (exp.zeus_reduced_motion && window.matchMedia) {
          const rm = window.matchMedia("(prefers-reduced-motion: reduce)");
          exp.zeus_reduced_motion(rm.matches ? 1 : 0);
          if (rm.addEventListener) {
            rm.addEventListener("change", (e) => {
              exp.zeus_reduced_motion(e.matches ? 1 : 0);
              schedule(0);
            });
          }
        }
        /* Hot reload: the dev server's reload script stashes a signal snapshot
           in sessionStorage before reloading; restore it into the fresh arena
           so state survives a component edit. */
        const saveState = () => {
          try {
            if (exp && exp.zeus_state_snapshot) {
              sessionStorage.setItem("zeus.state", cstr(exp.zeus_state_snapshot()));
            }
          } catch (e) {
            /* sessionStorage may be unavailable; state simply isn't kept. */
          }
        };
        const restoreState = () => {
          let st = null;
          try {
            st = sessionStorage.getItem("zeus.state");
          } catch (e) {
            return;
          }
          if (!st || !exp.zeus_state_restore || !exp.zeus_text_buf) return;
          const ptr = exp.zeus_text_buf();
          const cap = exp.zeus_text_buf_cap ? exp.zeus_text_buf_cap() : 0;
          const enc = new TextEncoder().encode(st);
          const n = Math.min(enc.length, Math.max(0, cap - 1));
          new Uint8Array(mem.buffer, ptr, n).set(enc.subarray(0, n));
          new Uint8Array(mem.buffer, ptr + n, 1)[0] = 0;
          exp.zeus_state_restore(ptr);
        };
        restoreState();
        const domWin = canvas.ownerDocument && canvas.ownerDocument.defaultView;
        if (domWin) domWin.__zeus_save = saveState;
        /* Deep link + browser history: route to the URL this shell was served
           for, and re-route on back/forward. The app owns the stack; this is
           the browser's mirror of it. */
        const openUrl = (p) => {
          if (!exp.zeus_open_url || !exp.zeus_text_buf) return;
          const ptr = exp.zeus_text_buf();
          const cap = exp.zeus_text_buf_cap ? exp.zeus_text_buf_cap() : 0;
          const enc = new TextEncoder().encode(p);
          const n = Math.min(enc.length, Math.max(0, cap - 1));
          const out = new Uint8Array(mem.buffer, ptr, n);
          out.set(enc.subarray(0, n));
          new Uint8Array(mem.buffer, ptr + n, 1)[0] = 0;
          exp.zeus_open_url(ptr);
        };
        const win = canvas.ownerDocument && canvas.ownerDocument.defaultView;
        if (win) {
          win.addEventListener("popstate", () => {
            openUrl(location.pathname + location.search);
            schedule(0);
          });
        }
        /* 16px: iOS zooms the page to focus anything smaller. */
        const ime = document.createElement("textarea");
        ime.setAttribute("autocapitalize", "off");
        ime.setAttribute("autocomplete", "off");
        ime.setAttribute("autocorrect", "off");
        ime.setAttribute("spellcheck", "false");
        ime.setAttribute("aria-hidden", "true");
        ime.style.cssText =
          "position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0;margin:0;resize:none;z-index:-1;font-size:16px;";
        document.body.appendChild(ime);
        function syncIme() {
          if (!exp) return;
          if (exp.zeus_captures_text && exp.zeus_captures_text()) {
            if (document.activeElement !== ime) ime.focus({ preventScroll: true });
          } else if (document.activeElement === ime) {
            ime.blur();
          }
        }
        /* Accessibility mirror (§2.3): the UI stays canvas. A browser exposes
           a canvas to assistive tech as one opaque image, so this visually
           hidden, never-painted DOM mirrors the semantic tree — roles, labels,
           checked / disabled state — for screen readers. It is an a11y
           surface, not a render target.

           Kept in sync by a keyed diff on the engine node id: an unchanged
           node keeps its element (and the screen reader's place on it), and
           only what changed is touched. A screen reader's "press" or focus
           move on an element is sent back to the engine, and the engine's own
           focus moves (Tab, arrows on the canvas) move DOM focus here, so the
           reader announces them. Mirror elements are never in the Tab order:
           the engine owns Tab. */
        canvas.setAttribute("aria-hidden", "true");
        const a11y = document.createElement("div");
        a11y.id = "zeus-a11y";
        a11y.style.cssText =
          "position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);" +
          "clip-path:inset(50%);white-space:nowrap;border:0;padding:0;margin:-1px;";
        document.body.appendChild(a11y);
        const a11yEls = new Map(); // engine node id -> element
        let a11yLast = "";
        let a11yForceAt = 0;
        let a11yFocusId = 0;
        let a11yQuiet = false; // set while the loader itself moves DOM focus
        // Roles whose on / off state is `aria-checked`; tabs and options use
        // `aria-selected`; a button with a state is a toggle (`aria-pressed`).
        const A11Y_CHECKED = new Set(["checkbox", "radio", "switch", "menuitemcheckbox", "menuitemradio"]);
        const A11Y_SELECTED = new Set(["tab", "option", "row", "gridcell"]);
        function a11yAttr(el, name, value) {
          if (value === null) {
            if (el.hasAttribute(name)) el.removeAttribute(name);
          } else if (el.getAttribute(name) !== value) {
            el.setAttribute(name, value);
          }
        }
        function a11yElement(id, role) {
          let el = a11yEls.get(id);
          const tag = role === "button" ? "BUTTON" : "DIV";
          if (el && el.tagName === tag && el.dataset.role === role) return el;
          if (el) el.remove();
          el = document.createElement(tag);
          el.dataset.role = role;
          if (tag === "BUTTON") el.type = "button";
          else if (role !== "text") el.setAttribute("role", role);
          el.addEventListener(
            "click",
            (e) => {
              e.preventDefault();
              if (!exp || !exp.zeus_a11y_activate) return;
              exp.zeus_a11y_activate(id);
              schedule(0);
            },
            { signal }
          );
          el.addEventListener(
            "focus",
            () => {
              if (a11yQuiet || !exp || !exp.zeus_a11y_focus) return;
              a11yFocusId = id;
              exp.zeus_a11y_focus(id);
              schedule(0);
            },
            { signal }
          );
          a11yEls.set(id, el);
          return el;
        }
        function syncA11y() {
          if (!exp || !exp.zeus_a11y_sync) return;
          // A checkbox flip or a focus move changes the semantics without a
          // layout, so re-dump a few times a second regardless.
          const now = performance.now();
          const force = now - a11yForceAt > 250;
          if (force) a11yForceAt = now;
          const ptr = exp.zeus_a11y_sync(force ? 1 : 0);
          if (ptr) {
            const s = cstr(ptr);
            if (s !== a11yLast) {
              a11yLast = s;
              a11yApply(s);
            }
          }
          a11yFollowFocus();
        }
        function a11yApply(s) {
          const seen = new Set();
          let k = 0;
          for (const line of s.split("\n")) {
            if (!line) continue;
            // depth, role, label, x, y, w, h, id, state
            const parts = line.split("\t");
            if (parts.length < 9) continue;
            const role = parts[1] || "text";
            const label = parts[2];
            const id = parts[7] | 0;
            const state = parts[8] | 0;
            if (!id || seen.has(id)) continue;
            seen.add(id);
            const el = a11yElement(id, role);
            if (el.textContent !== label) el.textContent = label;
            a11yAttr(el, "aria-label", role === "text" ? null : label);
            // Focusable nodes take programmatic focus (so the engine's focus
            // can follow); none are Tab stops.
            a11yAttr(el, "tabindex", state & 1 || el.tagName === "BUTTON" ? "-1" : null);
            const hasState = (state & 2) !== 0;
            const on = (state & 4) !== 0 ? "true" : "false";
            a11yAttr(el, "aria-checked", hasState && A11Y_CHECKED.has(role) ? on : null);
            a11yAttr(el, "aria-selected", hasState && A11Y_SELECTED.has(role) ? on : null);
            a11yAttr(el, "aria-pressed", hasState && role === "button" ? on : null);
            a11yAttr(el, "aria-disabled", state & 8 ? "true" : null);
            if (a11y.children[k] !== el) a11y.insertBefore(el, a11y.children[k] || null);
            k++;
          }
          for (const [id, el] of a11yEls) {
            if (!seen.has(id)) {
              el.remove();
              a11yEls.delete(id);
            }
          }
        }
        /* The engine's focus moved (Tab or arrows on the canvas, a click):
           move DOM focus to the mirror element so a screen reader announces
           it. Never while a text field holds focus — the IME textarea owns DOM
           focus then — and never away from some other page element. */
        function a11yFollowFocus() {
          if (!exp.zeus_focus_id) return;
          const id = exp.zeus_focus_id() | 0;
          if (id === a11yFocusId) return;
          a11yFocusId = id;
          if (exp.zeus_captures_text && exp.zeus_captures_text()) return;
          const el = a11yEls.get(id);
          if (!el) return;
          const active = document.activeElement;
          if (active && active !== document.body && active !== canvas && !a11y.contains(active)) return;
          a11yQuiet = true;
          el.focus({ preventScroll: true });
          a11yQuiet = false;
        }
        ime.addEventListener(
          "input",
          (e) => {
            if (e.isComposing) return;
            if (ime.value) sendWasmText(ime.value, false);
            ime.value = "";
          },
          { signal }
        );
        ime.addEventListener(
          "compositionupdate",
          (e) => {
            if (e.data) sendWasmText(e.data, true);
          },
          { signal }
        );
        ime.addEventListener(
          "compositionend",
          (e) => {
            sendWasmText("", true);
            if (e.data) sendWasmText(e.data, false);
            ime.value = "";
          },
          { signal }
        );
        window.addEventListener(
          "resize",
          () => {
            const s = sizeCanvas();
            if (exp.zeus_resize) exp.zeus_resize(s.w, s.h);
            schedule(0);
          },
          { signal }
        );
        /* A finger on the canvas. `touch-action: none` means the browser never
           scrolls this page, and a phone has no wheel, so a touch is handled
           here: a tap clicks on release, so a pan that starts on a link scrolls
           instead of navigating. A slider or scrollbar thumb keeps the gesture.
           An `on_drag` region (the playground editor) keeps a horizontal drag
           and yields a vertical one to the page.
           The pan follows the finger exactly, through `zeus_scroll_step`: the
           engine's own coast runs on every frame without a touch event, which
           is most of them on a 120 Hz screen, and moved the page at twice the
           finger's speed. Moves are added up and applied once per frame. The
           fling then runs from here (see `flingStep`). */
        const PAN_SLOP2 = 64;
        const AXIS_BOTH = 0;
        const AXIS_X = 1;
        const AXIS_Y = 2;
        const pan = { id: -1, on: false, scroll: false, grab: 0, tap: false, axis: AXIS_BOTH };
        const panDown = { x: 0, y: 0, ox: 0, oy: 0, rect: null };
        const panLast = { x: 0, y: 0 };
        const panRem = { x: 0, y: 0 };
        const panWant = { x: 0, y: 0 };
        let panSamples = [];
        let fling = null;
        /* Velocity a new touch cut short, so swiping again keeps speed. */
        let flingCarry = null;
        function directPointer(e) {
          return e.pointerType === "touch" || e.pointerType === "pen";
        }
        function panPoint(e) {
          const r = panDown.rect;
          return {
            x: (e.clientX - r.left) * (layoutW / (r.width || 1)),
            y: (e.clientY - r.top) * (layoutH / (r.height || 1)),
          };
        }
        /* Whole points go to the engine, and the fraction waits for the next
           frame, so a slow pan neither freezes nor loses distance. Returns
           whether anything moved. */
        function panScrollBy(dx, dy) {
          if (panRem.x * dx < 0) panRem.x = 0;
          if (panRem.y * dy < 0) panRem.y = 0;
          dx += panRem.x;
          dy += panRem.y;
          const ix = Math.trunc(dx);
          const iy = Math.trunc(dy);
          panRem.x = dx - ix;
          panRem.y = dy - iy;
          if (!ix && !iy) return true;
          return !!(exp.zeus_scroll_step(panDown.x, panDown.y, ix, iy) | 0);
        }
        function releaseLatch() {
          if (exp.zeus_scroll_latch) exp.zeus_scroll_latch(-1, -1);
        }
        function stopFling() {
          if (fling) flingCarry = { x: fling.vx, y: fling.vy, t: performance.now() };
          fling = null;
        }
        function startFling(now) {
          let v = flingVelocity(panSamples, now);
          const axis = pan.axis;
          if (axis === AXIS_X) v = { x: v.x, y: 0 };
          if (axis === AXIS_Y) v = { x: 0, y: v.y };
          if (!v.x && !v.y) return false;
          if (window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches) return false;
          const c = flingCarry;
          if (c && now - c.t < 400) {
            if (c.x * v.x > 0) v.x = Math.max(-FLING_MAX * 2, Math.min(FLING_MAX * 2, v.x + c.x));
            if (c.y * v.y > 0) v.y = Math.max(-FLING_MAX * 2, Math.min(FLING_MAX * 2, v.y + c.y));
          }
          fling = { vx: v.x, vy: v.y, t: now };
          return true;
        }
        /* Runs before each paint, so a frame draws the finger where it is now. */
        touchFrame = function () {
          if (panWant.x || panWant.y) {
            panScrollBy(panWant.x, panWant.y);
            panWant.x = 0;
            panWant.y = 0;
          }
          if (!fling) return false;
          const now = performance.now();
          const dt = Math.min(now - fling.t, 100);
          fling.t = now;
          const fx = flingStep(fling.vx, dt);
          const fy = flingStep(fling.vy, dt);
          fling.vx = fx.v;
          fling.vy = fy.v;
          const moved = panScrollBy(fx.d, fy.d);
          if (!moved || Math.hypot(fling.vx, fling.vy) < FLING_STOP) {
            fling = null;
            flingCarry = null;
            releaseLatch();
            return false;
          }
          return true;
        };
        function trackPan(e) {
          const raw = panPoint(e);
          if (pan.grab === 1) {
            exp.zeus_pointer_move(Math.round(raw.x), Math.round(raw.y));
            schedule(0);
            return;
          }
          /* Kept from the moment the finger lands, slop included: the browser
             merges moves, so a hard flick may be only a sample or two past it. */
          panSamples.push({ t: e.timeStamp, x: raw.x, y: raw.y });
          if (panSamples.length > 24) panSamples.shift();
          if (!pan.scroll) {
            const adx = panDown.ox - raw.x;
            const ady = panDown.oy - raw.y;
            if (adx * adx + ady * ady <= PAN_SLOP2) return;
            /* A selection keeps a drag along the line. */
            if (pan.grab === 2 && Math.abs(adx) > Math.abs(ady)) {
              exp.zeus_pointer_move(Math.round(raw.x), Math.round(raw.y));
              schedule(0);
              return;
            }
            pan.scroll = true;
            pan.tap = false;
            /* A pan that leans on one axis stays on it, so reading down a page
               does not nudge the code block a thumb passes over sideways. */
            const ax = Math.abs(adx);
            const ay = Math.abs(ady);
            pan.axis = ay > ax * 2 ? AXIS_Y : ax > ay * 2 ? AXIS_X : AXIS_BOTH;
            if (pan.grab) exp.zeus_pointer_up();
            /* The press lit whatever it landed on: this was never a press. */
            exp.zeus_pointer_move(-1, -1);
            if (exp.zeus_scroll_latch) exp.zeus_scroll_latch(panDown.x, panDown.y);
            panLast.x = raw.x;
            panLast.y = raw.y;
          }
          if (pan.axis !== AXIS_Y) panWant.x += panLast.x - raw.x;
          if (pan.axis !== AXIS_X) panWant.y += panLast.y - raw.y;
          panLast.x = raw.x;
          panLast.y = raw.y;
          schedule(0);
        }
        function finishPan(e, cancel) {
          if (!pan.on || e.pointerId !== pan.id) return;
          const scrolled = pan.scroll;
          const flung = scrolled && !cancel && startFling(e.timeStamp);
          if (pan.tap && !cancel) exp.zeus_pointer_down(panDown.x, panDown.y);
          if (!scrolled) exp.zeus_pointer_up();
          /* No finger means no hover: a tapped link must not stay lit. */
          exp.zeus_pointer_move(-1, -1);
          if (!flung) releaseLatch();
          pan.on = false;
          pan.scroll = false;
          pan.grab = 0;
          pan.tap = false;
          pan.id = -1;
          schedule(0);
        }
        canvas.addEventListener(
          "touchstart",
          (e) => {
            if (e.cancelable) e.preventDefault();
          },
          { passive: false, signal }
        );
        canvas.addEventListener(
          "touchmove",
          (e) => {
            if (e.cancelable) e.preventDefault();
          },
          { passive: false, signal }
        );
        /* iOS opens its keyboard only for a focus made inside the gesture, so
           the frame's `syncIme` is too late. This runs after the tap. */
        canvas.addEventListener("touchend", syncIme, { signal });
        canvas.addEventListener(
          "pointerdown",
          (e) => {
            if (directPointer(e)) {
              if (pan.on) return;
              const wasFlinging = !!fling;
              stopFling();
              const r = canvas.getBoundingClientRect();
              panDown.rect = r;
              const raw = panPoint(e);
              panDown.ox = raw.x;
              panDown.oy = raw.y;
              panDown.x = Math.round(raw.x);
              panDown.y = Math.round(raw.y);
              panLast.x = raw.x;
              panLast.y = raw.y;
              panRem.x = 0;
              panRem.y = 0;
              panWant.x = 0;
              panWant.y = 0;
              panSamples = [{ t: e.timeStamp, x: raw.x, y: raw.y }];
              releaseLatch();
              /* Hover runs here because a touch has no other chance to: `on_hover`
                 is what fetches a link's code ahead of the tap. */
              touching = true;
              exp.zeus_pointer_move(panDown.x, panDown.y);
              touching = false;
              const grab = exp.zeus_grab ? exp.zeus_grab(panDown.x, panDown.y) | 0 : 0;
              pan.on = true;
              pan.id = e.pointerId;
              pan.scroll = false;
              pan.axis = AXIS_BOTH;
              pan.grab = grab === 1 || grab === 2 ? grab : 0;
              /* A touch that stops a fling only stops it. */
              pan.tap = pan.grab === 0 && !wasFlinging;
              if (pan.grab) exp.zeus_pointer_down(panDown.x, panDown.y);
              if (canvas.setPointerCapture) {
                try {
                  canvas.setPointerCapture(e.pointerId);
                } catch (err) {
                  /* The gesture still works while the finger is over the canvas. */
                }
              }
            } else {
              const p = layoutPoint(e.clientX, e.clientY);
              exp.zeus_pointer_down(p.x, p.y);
            }
            schedule(0);
          },
          { signal }
        );
        canvas.addEventListener(
          "pointermove",
          (e) => {
            if (directPointer(e)) {
              if (pan.on && e.pointerId === pan.id) trackPan(e);
              return;
            }
            const p = layoutPoint(e.clientX, e.clientY);
            exp.zeus_pointer_move(p.x, p.y);
            const cp = exp.zeus_cursor_sync ? exp.zeus_cursor_sync() : 0;
            canvas.style.cursor = cstr(cp) || "default";
            schedule(0);
          },
          { signal }
        );
        canvas.addEventListener(
          "contextmenu",
          (e) => {
            const p = layoutPoint(e.clientX, e.clientY);
            if (exp.zeus_context_click && exp.zeus_context_click(p.x, p.y)) {
              e.preventDefault();
              /* A long press opened a menu: its release is not also a tap. */
              pan.tap = false;
              schedule(0);
            }
          },
          { signal }
        );
        canvas.addEventListener(
          "pointerup",
          (e) => {
            if (directPointer(e)) {
              finishPan(e, false);
              return;
            }
            exp.zeus_pointer_up();
            schedule(0);
          },
          { signal }
        );
        canvas.addEventListener(
          "pointercancel",
          (e) => {
            if (directPointer(e)) finishPan(e, true);
            else {
              exp.zeus_pointer_up();
              schedule(0);
            }
          },
          { signal }
        );
        /* Sub-point wheel remainder per axis (see the wheel handler). */
        let wheelRemX = 0;
        let wheelRemY = 0;
        canvas.addEventListener(
          "wheel",
          (e) => {
            e.preventDefault();
            /* No engine coast: a trackpad's inertia already arrives as a
               stream of wheel events from the OS, so precise deltas step
               exactly where they land. A mouse notch eases to its target
               instead (the browser's own smooth wheel scroll; bounded, and
               retargeted by the next notch). Chromium reports a notch as a
               `wheelDeltaY` multiple of 120; line / page modes (1 / 2) are
               notches too, scaled to points. */
            const p = layoutPoint(e.clientX, e.clientY);
            let notch = e.deltaMode !== 0;
            if (!notch) {
              if (typeof e.wheelDeltaY === "number" && e.wheelDeltaY !== 0) {
                notch = e.wheelDeltaY % 120 === 0;
              } else {
                notch = Number.isInteger(e.deltaY) && Math.abs(e.deltaY) >= 48;
              }
            }
            const step = exp.zeus_scroll_step || exp.zeus_scroll;
            const fn = notch ? (exp.zeus_scroll_smooth || step) : step;
            let dx = e.deltaX;
            let dy = e.deltaY;
            if (e.deltaMode === 1) {
              dx *= 16;
              dy *= 16;
            } else if (e.deltaMode === 2) {
              dx *= layoutW;
              dy *= layoutH;
            }
            /* The exports take whole points, but a trackpad on a HiDPI screen
               reports fractions (0.5, 1.33…). Passing them straight through
               truncated every one — a slow scroll froze then jumped, and fast
               ones lost distance. Carry the remainder into the next event;
               a direction change starts fresh. */
            if (wheelRemX * dx < 0) wheelRemX = 0;
            if (wheelRemY * dy < 0) wheelRemY = 0;
            dx += wheelRemX;
            dy += wheelRemY;
            const ix = Math.trunc(dx);
            const iy = Math.trunc(dy);
            wheelRemX = dx - ix;
            wheelRemY = dy - iy;
            if (ix === 0 && iy === 0) return;
            fn(p.x, p.y, ix, iy);
            schedule(0);
          },
          { passive: false, signal }
        );
        function keyMods(e) {
          let mods = 0;
          if (e.shiftKey) mods |= 1;
          if (e.ctrlKey) mods |= 2;
          if (e.altKey) mods |= 4;
          if (e.metaKey) mods |= 8;
          return mods;
        }
        const KEY_CODES = {
          Enter: 13, Tab: 9, Backspace: 8, Delete: 127, Escape: 27,
          ArrowLeft: 1000, ArrowRight: 1001, ArrowUp: 1002, ArrowDown: 1003,
          PageUp: 1004, PageDown: 1005, Home: 1006, End: 1007,
        };
        function keyCode(e) {
          if (e.key in KEY_CODES) return KEY_CODES[e.key];
          return e.key.length === 1 ? e.key.charCodeAt(0) : 0;
        }
        /* Whether a key event goes to the engine. A page form control keeps its
           keys. The IME textarea is different: it holds DOM focus while a Zeus
           text field is being edited, but only to receive typed text (`input`)
           and compositions. Its printable keys arrive through `input`, so they
           stay out (they would insert twice); everything else — Backspace,
           Delete, arrows, Home / End, Enter, Tab, Escape, Cmd / Ctrl chords such
           as select-all — must reach the engine, or a field could be typed into
           but never edited or cleared. Nothing goes mid-composition. */
        function keyForEngine(e) {
          const active = document.activeElement;
          if (active === ime) {
            if (e.isComposing || e.keyCode === 229) return false;
            if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) return false;
            return true;
          }
          const tag = (active && active.tagName) || "";
          return !(tag === "TEXTAREA" || tag === "INPUT" || tag === "SELECT");
        }
        window.addEventListener(
          "keydown",
          (e) => {
            if (!keyForEngine(e)) return;
            const mods = keyMods(e);
            const key = keyCode(e);
            exp.zeus_key(key, mods);
            schedule(0);
            if (e.key === "Tab") e.preventDefault();
            // Focus on a mirror element: the engine just handled the key, so
            // the browser must not also turn Enter / Space into a click on it.
            if ((e.key === "Enter" || e.key === " ") && a11y.contains(document.activeElement))
              e.preventDefault();
            if (e.key === "Enter" && exp.zeus_captures_text && exp.zeus_captures_text())
              e.preventDefault();
          },
          { signal }
        );
        window.addEventListener(
          "keyup",
          (e) => {
            if (!keyForEngine(e)) return;
            const mods = keyMods(e);
            const key = keyCode(e);
            if (exp.zeus_key_up) exp.zeus_key_up(key, mods);
            schedule(0);
          },
          { signal }
        );
        function sendWasmText(s, marked) {
          if (!s || !exp || !te || !mem) return;
          const bytes = te.encode(s);
          const cap = exp.zeus_text_buf_cap ? exp.zeus_text_buf_cap() : 0;
          const ptr = exp.zeus_text_buf ? exp.zeus_text_buf() : 0;
          if (!ptr || cap < 2) return;
          const n = Math.min(bytes.length, cap - 1);
          new Uint8Array(mem.buffer, ptr, n).set(bytes.subarray(0, n));
          if (marked && exp.zeus_marked) exp.zeus_marked(n);
          else if (exp.zeus_text) exp.zeus_text(n);
          schedule(0);
        }
        window.addEventListener(
          "paste",
          (e) => {
            if (!exp.zeus_captures_text || !exp.zeus_captures_text()) return;
            const t = e.clipboardData && e.clipboardData.getData("text");
            if (!t) return;
            e.preventDefault();
            sendWasmText(t, false);
          },
          { signal }
        );
        /* IME composition is owned by the hidden textarea (CJK / dead keys). */
        schedule(0);
    }

    function boot(wasmBuffer) {
      return WebAssembly.instantiate(wasmBuffer, moduleImports).then((r) => {
        wasmModule = r.module;
        return start(r.instance);
      });
    }

    function bootUrl(url) {
      /* This page's chunks download alongside the primary, and link just before
         `main`, so the first frame is the real page. */
      const chunksReady = Promise.all((opts.boot || []).map((n) => loadChunk(n)));
      const started = (x) => {
        wasmModule = x.module;
        return chunksReady.then(() => {
          start(x.instance);
          preloadIdle();
        });
      };
      return fetch(url).then((r) => {
        if (!r.ok) throw new Error("failed to load " + url + " (" + r.status + ")");
        const ct = (r.headers.get("content-type") || "").toLowerCase();
        if (typeof WebAssembly.instantiateStreaming === "function" && ct.indexOf("wasm") >= 0) {
          return WebAssembly.instantiateStreaming(r, moduleImports).then(started);
        }
        return r.arrayBuffer().then((buf) => WebAssembly.instantiate(buf, moduleImports).then(started));
      });
    }

    return { boot: boot, bootUrl: bootUrl, stop: stop, sizeCanvas: sizeCanvas };
  }

  root.attachZeus = attachZeus;

  const canvas = typeof document !== "undefined" ? document.getElementById("zeus") : null;
  if (canvas && canvas.getAttribute("data-wasm")) {
    const chunkList = document.getElementById("zeus-chunks");
    const host = attachZeus(canvas, chunkList ? JSON.parse(chunkList.textContent) : undefined);
    host.bootUrl(canvas.getAttribute("data-wasm") || "app.wasm").catch((err) => {
      console.error(err);
      document.body.appendChild(document.createTextNode(String(err)));
    });
  }
})(typeof window !== "undefined" ? window : globalThis);
