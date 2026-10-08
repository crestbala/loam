/* Zeus browser host. Canvas2D only — no WebGPU, no WebGL, no DOM widgets.
   One <canvas>, this file, and a .wasm built by `loam --target wasm`.
   `window.attachZeus(canvas, opts)` boots from an ArrayBuffer; a canvas with
   `data-wasm` still auto-loads that URL (counter, gallery). */
(function (root) {
  function attachZeus(canvas, opts) {
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("Canvas2D unavailable");
    opts = opts || {};

    const dpr = window.devicePixelRatio || 1;
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
    /* Instance exports; imports that finish asynchronously (fetch_rpc_async)
       call back into these once `boot` has instantiated the module. */
    let exp = null;
    /* Browser WebSockets (wasm `http.ws_open`): the socket and an inbound
       message queue live here per slot; wasm polls and copies out. */
    let wsSlots = new Array(8).fill(null);
    const imgCache = new Map();
    let fileInput = null;
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

  function svgAttr(tag, name) {
    const m = new RegExp("(?:^|\\s)" + name + "=['\"]([^'\"]+)['\"]", "i").exec(tag || "");
    return m ? m[1] : null;
  }

  function svgPaint(v, cur) {
    if (!v || v === "none" || v === "transparent") return null;
    if (v === "currentColor" || v === "currentcolor") return cur;
    return v;
  }

  /* Path2D rejects some SVG number runs (`m0-6.5`). Trace onto ctx instead. */
  /* Path2D rejects some SVG number runs (`m0-6.5`). Trace onto ctx instead.
     Covers every command the icons and charts emit: M/L/H/V/C/S/Q/T/Z, absolute
     and relative, with the implicit-repeat rule (a run of numbers after `M` is
     `L`, after anything else repeats the command). An unknown command stops, so
     an `A` arc segment is skipped rather than misdrawn. */
  function traceSvgPath(d) {
    let i = 0;
    const n = d.length;
    function skip() {
      while (i < n && (d[i] === " " || d[i] === "\t" || d[i] === "\n" || d[i] === "\r" || d[i] === ",")) i++;
    }
    function num() {
      skip();
      if (i >= n) return null;
      let j = i;
      if (d[j] === "+" || d[j] === "-") j++;
      const s0 = j;
      while (j < n && d[j] >= "0" && d[j] <= "9") j++;
      if (j < n && d[j] === ".") {
        j++;
        while (j < n && d[j] >= "0" && d[j] <= "9") j++;
      }
      if (j < n && (d[j] === "e" || d[j] === "E")) {
        j++;
        if (d[j] === "+" || d[j] === "-") j++;
        while (j < n && d[j] >= "0" && d[j] <= "9") j++;
      }
      if (j === s0 && d[i] !== ".") return null;
      const v = parseFloat(d.slice(i, j));
      i = j;
      return v;
    }
    ctx.beginPath();
    let cmd = "";
    let prev = "";
    let px = 0, py = 0, sx = 0, sy = 0, cx1 = 0, cy1 = 0;
    let started = false;
    while (i < n) {
      skip();
      if (i >= n) break;
      const c = d[i];
      if ((c >= "A" && c <= "Z") || (c >= "a" && c <= "z")) {
        cmd = c;
        i++;
      } else if (!cmd) {
        break;
      } else if (prev === "M") {
        cmd = "L";
      } else if (prev === "m") {
        cmd = "l";
      } else {
        cmd = prev;
      }
      const rel = cmd >= "a";
      const op = cmd.toUpperCase();
      if (op === "Z") {
        ctx.closePath();
        px = sx;
        py = sy;
        started = false;
        prev = cmd;
        continue;
      }
      if (op === "M") {
        const x = num();
        const y = num();
        if (x == null || y == null) break;
        px = rel ? px + x : x;
        py = rel ? py + y : y;
        sx = px;
        sy = py;
        ctx.moveTo(px, py);
        started = true;
      } else if (op === "L") {
        const x = num();
        const y = num();
        if (x == null || y == null) break;
        px = rel ? px + x : x;
        py = rel ? py + y : y;
        if (!started) {
          ctx.moveTo(px, py);
          started = true;
        } else {
          ctx.lineTo(px, py);
        }
      } else if (op === "H") {
        const x = num();
        if (x == null) break;
        px = rel ? px + x : x;
        ctx.lineTo(px, py);
      } else if (op === "V") {
        const y = num();
        if (y == null) break;
        py = rel ? py + y : y;
        ctx.lineTo(px, py);
      } else if (op === "C" || op === "S" || op === "Q" || op === "T") {
        /* Relative coordinates are relative to the point BEFORE this segment,
           so the raw numbers are converted first and `px`/`py` updated after. */
        if (!started) break;
        let a1, b1, a2, b2, ax, ay;
        if (op === "C" || op === "S") {
          if (op === "C") {
            a1 = num();
            b1 = num();
          } else {
            const refl = prev === "C" || prev === "c" || prev === "S" || prev === "s";
            a1 = refl ? 2 * px - cx1 : px;
            b1 = refl ? 2 * py - cy1 : py;
          }
          a2 = num();
          b2 = num();
          ax = num();
          ay = num();
          if (a1 == null || b1 == null || a2 == null || b2 == null || ax == null || ay == null) break;
          if (rel) {
            if (op === "C") {
              a1 += px;
              b1 += py;
            }
            a2 += px;
            b2 += py;
            ax += px;
            ay += py;
          }
          ctx.bezierCurveTo(a1, b1, a2, b2, ax, ay);
          cx1 = a2;
          cy1 = b2;
        } else {
          if (op === "Q") {
            a1 = num();
            b1 = num();
          } else {
            const refl = prev === "Q" || prev === "q" || prev === "T" || prev === "t";
            a1 = refl ? 2 * px - cx1 : px;
            b1 = refl ? 2 * py - cy1 : py;
          }
          ax = num();
          ay = num();
          if (a1 == null || b1 == null || ax == null || ay == null) break;
          if (rel) {
            if (op === "Q") {
              a1 += px;
              b1 += py;
            }
            ax += px;
            ay += py;
          }
          ctx.quadraticCurveTo(a1, b1, ax, ay);
          cx1 = a1;
          cy1 = b1;
        }
        px = ax;
        py = ay;
      } else {
        break;
      }
      prev = cmd;
    }
  }

  /* Fill and/or stroke the path `traceSvgPath` / the shape helpers just built. */
  function paintSvgShape(fillPaint, strokePaint, sw, cap, join) {
    if (fillPaint) {
      ctx.fillStyle = fillPaint;
      ctx.fill();
    }
    if (strokePaint) {
      ctx.strokeStyle = strokePaint;
      ctx.lineWidth = sw ? parseFloat(sw) : 1;
      if (cap) ctx.lineCap = cap;
      if (join) ctx.lineJoin = join;
      ctx.stroke();
    }
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
      write: (p, n) => {
        const t = bytes(p, n);
        if (opts.onWrite) opts.onWrite(t);
        else if (t) console.log(t);
      },
      panic: (p, n) => {
        throw new Error(bytes(p, n) || cstr(p) || "zeus panic");
      },
      view_w: () => Math.max(1, Math.round(canvas.clientWidth || window.innerWidth || 1)),
      view_h: () => Math.max(1, Math.round(canvas.clientHeight || window.innerHeight || 1)),
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
      text: (x, y, ptr, color, font) => {
        const s = cstr(ptr);
        const px = font > 0 ? font : 13;
        const box = lineBox(px);
        ctx.fillStyle = rgb(color);
        setFont(px);
        ctx.textBaseline = "alphabetic";
        ctx.fillText(s, snapX(x), snapY(y + box.ascent));
      },
      /* Rotated text, clockwise around the top-left of the line box. */
      text_rot: (x, y, ptr, color, font, deg) => {
        const s = cstr(ptr);
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
      /* PNG / JPEG / WebP / GIF via the browser decoder. Cache by src.
         fit: 0 stretch, 1 contain, 2 cover, 3 none. */
      image: (x, y, w, h, ptr, radius, alpha, fit) => {
        const src = cstr(ptr);
        if (!src || w <= 0 || h <= 0) return;
        let rec = imgCache.get(src);
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
        const p = snapRect(x, y, w, h);
        const iw = rec.naturalWidth;
        const ih = rec.naturalHeight;
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
      /* Intrinsic size for layout (`platform.plat_image_size`): the two i32
         out-params. Decode is shared with `image`; a not-yet-decoded source
         reports 0 and the load's `schedule` re-lays out on the next frame. */
      image_size: (ptr, wPtr, hPtr) => {
        const view = new DataView(mem.buffer);
        view.setInt32(wPtr, 0, true);
        view.setInt32(hPtr, 0, true);
        const src = cstr(ptr);
        if (!src) return;
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
      svg: (x, y, w, h, ptr, color, alpha) => {
        const markup = cstr(ptr);
        if (!markup || w <= 0 || h <= 0) return;
        const svgTag = (/<svg\b([^>]*)>/i.exec(markup) || [])[1] || "";
        const vb = /viewBox=['"]([^'"]+)['"]/i.exec(markup);
        let vx = 0, vy = 0, vw = 24, vh = 24;
        if (vb) {
          const p = vb[1].trim().split(/[\s,]+/).map(Number);
          if (p.length >= 4) {
            vx = p[0];
            vy = p[1];
            vw = p[2] || 24;
            vh = p[3] || 24;
          }
        }
        const p = snapRect(x, y, w, h);
        const isx = p.w / (vw || 1);
        const isy = p.h / (vh || 1);
        ctx.save();
        ctx.translate(p.x, p.y);
        ctx.scale(isx, isy);
        ctx.translate(-vx, -vy);
        ctx.globalAlpha = (alpha >= 0 && alpha < 255) ? alpha / 255 : 1;
        const cur = rgb(color);
        const defFill = svgAttr(svgTag, "fill");
        const defStroke = svgAttr(svgTag, "stroke");
        const defSw = svgAttr(svgTag, "stroke-width");
        const defCap = svgAttr(svgTag, "stroke-linecap");
        const defJoin = svgAttr(svgTag, "stroke-linejoin");
        /* Every shape element, in document order — not just `path`. The Alert
           and Info icons are a `<circle>` plus two paths, so ignoring the circle
           drew the exclamation mark and the dot with no ring around them. */
        const re = /<(path|circle|rect)\b([^>]*?)\/?\s*>/gi;
        let m;
        while ((m = re.exec(markup))) {
          const el = m[1].toLowerCase();
          const tag = m[2];
          let fillV = svgAttr(tag, "fill");
          if (fillV == null) fillV = defFill;
          let strokeV = svgAttr(tag, "stroke");
          if (strokeV == null) strokeV = defStroke;
          const sw = svgAttr(tag, "stroke-width") || defSw;
          const cap = svgAttr(tag, "stroke-linecap") || defCap;
          const join = svgAttr(tag, "stroke-linejoin") || defJoin;
          const fillPaint = svgPaint(fillV, cur);
          let strokePaint = svgPaint(strokeV, cur);
          if (!fillPaint && !strokePaint) strokePaint = cur;
          if (el === "path") {
            const d = svgAttr(tag, "d");
            if (!d) continue;
            traceSvgPath(d);
          } else if (el === "circle") {
            const cx = parseFloat(svgAttr(tag, "cx") || "0");
            const cy = parseFloat(svgAttr(tag, "cy") || "0");
            const r = parseFloat(svgAttr(tag, "r") || "0");
            if (!(r > 0)) continue;
            ctx.beginPath();
            ctx.arc(cx, cy, r, 0, Math.PI * 2);
          } else {
            const x = parseFloat(svgAttr(tag, "x") || "0");
            const y = parseFloat(svgAttr(tag, "y") || "0");
            const rw = parseFloat(svgAttr(tag, "width") || "0");
            const rh = parseFloat(svgAttr(tag, "height") || "0");
            if (!(rw > 0) || !(rh > 0)) continue;
            let rr = parseFloat(svgAttr(tag, "rx") || "0");
            if (!(rr > 0)) rr = 0;
            if (rr > rw / 2) rr = rw / 2;
            if (rr > rh / 2) rr = rh / 2;
            ctx.beginPath();
            if (rr > 0) {
              ctx.moveTo(x + rr, y);
              ctx.arcTo(x + rw, y, x + rw, y + rh, rr);
              ctx.arcTo(x + rw, y + rh, x, y + rh, rr);
              ctx.arcTo(x, y + rh, x, y, rr);
              ctx.arcTo(x, y, x + rw, y, rr);
              ctx.closePath();
            } else {
              ctx.rect(x, y, rw, rh);
            }
          }
          paintSvgShape(fillPaint, strokePaint, sw, cap, join);
        }
        popState();
      },
      fetch_rpc_async: (pathPtr, pathLen, bodyPtr, bodyLen, outPtr, cap, handle) => {
        /* Sync XHR: `await` pumps on this thread (`tick` + `sleep`). An async
           XHR never completes while that loop holds the JS event loop. */
        try {
          const path = bytes(pathPtr, pathLen) || "/";
          const body =
            bodyLen > 0 ? u8().slice(bodyPtr, bodyPtr + bodyLen) : new Uint8Array(0);
          const xhr = new XMLHttpRequest();
          xhr.open("POST", path, false);
          xhr.overrideMimeType("text/plain; charset=x-user-defined");
          xhr.setRequestHeader("Content-Type", "application/grpc-web+proto");
          xhr.setRequestHeader("Accept", "application/grpc-web+proto");
          xhr.send(body);
          if (xhr.status !== 200) {
            if (exp && exp.zeus_fetch_done) exp.zeus_fetch_done(handle, -1);
            return 0;
          }
          const t = xhr.responseText || "";
          const n = Math.min(t.length, Math.max(cap, 0));
          const dst = u8();
          for (let i = 0; i < n; i++) dst[outPtr + i] = t.charCodeAt(i) & 0xff;
          if (exp && exp.zeus_fetch_done) exp.zeus_fetch_done(handle, n);
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
    }

    function start(instance) {
        if (stopped) return;
        exp = instance.exports;
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
            next = exp.zeus_paint() | 0;
            syncIme();
            syncA11y();
          } catch (e) {
            console.error("zeus_paint", e);
            return;
          }
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
        exp.zeus_start();
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
        openUrl(location.pathname + location.search);
        const win = canvas.ownerDocument && canvas.ownerDocument.defaultView;
        if (win) {
          win.addEventListener("popstate", () =>
            openUrl(location.pathname + location.search)
          );
        }
        const ime = document.createElement("textarea");
        ime.setAttribute("autocapitalize", "off");
        ime.setAttribute("autocomplete", "off");
        ime.setAttribute("autocorrect", "off");
        ime.setAttribute("spellcheck", "false");
        ime.setAttribute("aria-hidden", "true");
        ime.style.cssText =
          "position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0;margin:0;resize:none;z-index:-1;";
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
        canvas.addEventListener(
          "pointerdown",
          (e) => {
            const p = layoutPoint(e.clientX, e.clientY);
            exp.zeus_pointer_down(p.x, p.y);
            schedule(0);
          },
          { signal }
        );
        canvas.addEventListener(
          "pointermove",
          (e) => {
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
              schedule(0);
            }
          },
          { signal }
        );
        canvas.addEventListener(
          "pointerup",
          () => {
            exp.zeus_pointer_up();
            schedule(0);
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
        window.addEventListener(
          "keydown",
          (e) => {
            const tag = (document.activeElement && document.activeElement.tagName) || "";
            if (tag === "TEXTAREA" || tag === "INPUT" || tag === "SELECT") return;
            let mods = 0;
            if (e.shiftKey) mods |= 1;
            if (e.ctrlKey) mods |= 2;
            if (e.altKey) mods |= 4;
            if (e.metaKey) mods |= 8;
            let key = e.key.length === 1 ? e.key.charCodeAt(0) : 0;
            if (e.key === "Enter") key = 13;
            if (e.key === "Tab") key = 9;
            if (e.key === "Backspace") key = 8;
            if (e.key === "Delete") key = 127;
            if (e.key === "Escape") key = 27;
            if (e.key === "ArrowLeft") key = 1000;
            if (e.key === "ArrowRight") key = 1001;
            if (e.key === "ArrowUp") key = 1002;
            if (e.key === "ArrowDown") key = 1003;
            if (e.key === "PageUp") key = 1004;
            if (e.key === "PageDown") key = 1005;
            if (e.key === "Home") key = 1006;
            if (e.key === "End") key = 1007;
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
            const tag = (document.activeElement && document.activeElement.tagName) || "";
            if (tag === "TEXTAREA" || tag === "INPUT" || tag === "SELECT") return;
            let mods = 0;
            if (e.shiftKey) mods |= 1;
            if (e.ctrlKey) mods |= 2;
            if (e.altKey) mods |= 4;
            if (e.metaKey) mods |= 8;
            let key = e.key.length === 1 ? e.key.charCodeAt(0) : 0;
            if (e.key === "Enter") key = 13;
            if (e.key === "Tab") key = 9;
            if (e.key === "Backspace") key = 8;
            if (e.key === "Delete") key = 127;
            if (e.key === "Escape") key = 27;
            if (e.key === "ArrowLeft") key = 1000;
            if (e.key === "ArrowRight") key = 1001;
            if (e.key === "ArrowUp") key = 1002;
            if (e.key === "ArrowDown") key = 1003;
            if (e.key === "PageUp") key = 1004;
            if (e.key === "PageDown") key = 1005;
            if (e.key === "Home") key = 1006;
            if (e.key === "End") key = 1007;
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
      return WebAssembly.instantiate(wasmBuffer, imports).then((r) => start(r.instance));
    }

    function bootUrl(url) {
      return fetch(url).then((r) => {
        if (!r.ok) throw new Error("failed to load " + url + " (" + r.status + ")");
        const ct = (r.headers.get("content-type") || "").toLowerCase();
        if (typeof WebAssembly.instantiateStreaming === "function" && ct.indexOf("wasm") >= 0) {
          return WebAssembly.instantiateStreaming(r, imports).then((x) => start(x.instance));
        }
        return r.arrayBuffer().then(boot);
      });
    }

    return { boot: boot, bootUrl: bootUrl, stop: stop, sizeCanvas: sizeCanvas };
  }

  root.attachZeus = attachZeus;

  const canvas = document.getElementById("zeus");
  if (canvas && canvas.getAttribute("data-wasm")) {
    const host = attachZeus(canvas);
    host.bootUrl(canvas.getAttribute("data-wasm") || "app.wasm").catch((err) => {
      console.error(err);
      document.body.appendChild(document.createTextNode(String(err)));
    });
  }
})(typeof window !== "undefined" ? window : globalThis);
