/* Zeus browser host. Canvas2D only — no WebGPU, no WebGL, no DOM widgets.
   One <canvas>, this file, and a .wasm built by `yugac --target wasm`.
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

  function roundRect(x, y, w, h, r) {
    const p = snapRect(x, y, w, h);
    x = p.x;
    y = p.y;
    w = p.w;
    h = p.h;
    r = Math.round(r * sx) / sx;
    if (r <= 0) {
      ctx.fillRect(x, y, w, h);
      return;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
    ctx.fill();
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
      if (j === s0 && d[i] !== "." ) return null;
      const v = parseFloat(d.slice(i, j));
      i = j;
      return v;
    }
    ctx.beginPath();
    let cmd = "M";
    let px = 0, py = 0, sx = 0, sy = 0;
    skip();
    while (i < n) {
      skip();
      if (i >= n) break;
      const c = d[i];
      if ((c >= "A" && c <= "Z") || (c >= "a" && c <= "z")) {
        cmd = c;
        i++;
      }
      const rel = cmd >= "a";
      const op = cmd.toUpperCase();
      if (op === "Z") {
        ctx.closePath();
        px = sx;
        py = sy;
        continue;
      }
      if (op === "M" || op === "L") {
        const x = num();
        const y = num();
        if (x == null || y == null) break;
        const nx = rel ? px + x : x;
        const ny = rel ? py + y : y;
        if (op === "M") {
          ctx.moveTo(nx, ny);
          sx = nx;
          sy = ny;
          cmd = rel ? "l" : "L";
        } else {
          ctx.lineTo(nx, ny);
        }
        px = nx;
        py = ny;
        continue;
      }
      if (op === "H") {
        const x = num();
        if (x == null) break;
        px = rel ? px + x : x;
        ctx.lineTo(px, py);
        continue;
      }
      if (op === "V") {
        const y = num();
        if (y == null) break;
        py = rel ? py + y : y;
        ctx.lineTo(px, py);
        continue;
      }
      break;
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
      fill_g: (x, y, w, h, c0, c1, axis) => {
        const p = snapRect(x, y, w, h);
        const g = axis
          ? ctx.createLinearGradient(p.x, p.y, p.x + p.w, p.y)
          : ctx.createLinearGradient(p.x, p.y, p.x, p.y + p.h);
        g.addColorStop(0, rgb(c0));
        g.addColorStop(1, rgb(c1));
        ctx.fillStyle = g;
        ctx.fillRect(p.x, p.y, p.w, p.h);
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
        ctx.restore();
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
      clip: (x, y, w, h) => {
        ctx.beginPath();
        const box = snapRect(x, y, w, h);
        ctx.rect(box.x, box.y, box.w, box.h);
        ctx.clip();
      },
      restore: () => ctx.restore(),
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
        ctx.restore();
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
        const re = /<path\b([^>]*)\/?\s*>/gi;
        let m;
        while ((m = re.exec(markup))) {
          const tag = m[1];
          const d = svgAttr(tag, "d");
          if (!d) continue;
          let fillV = svgAttr(tag, "fill");
          if (fillV == null) fillV = defFill;
          let strokeV = svgAttr(tag, "stroke");
          if (strokeV == null) strokeV = defStroke;
          const sw = svgAttr(tag, "stroke-width") || defSw;
          const cap = svgAttr(tag, "stroke-linecap") || defCap;
          const join = svgAttr(tag, "stroke-linejoin") || defJoin;
          let fillPaint = svgPaint(fillV, cur);
          let strokePaint = svgPaint(strokeV, cur);
          if (!fillPaint && !strokePaint) strokePaint = cur;
          traceSvgPath(d);
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
        ctx.restore();
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
          "pointerup",
          () => {
            exp.zeus_pointer_up();
            schedule(0);
          },
          { signal }
        );
        canvas.addEventListener(
          "wheel",
          (e) => {
            e.preventDefault();
            /* Trackpads report pixel deltas per frame (deltaMode 0) and carry
               the momentum feel; mouse wheels report lines/pages (deltaMode
               1/2) as discrete clicks. Scale the latter to points and step
               without momentum, so a notch stops where it lands. */
            const p = layoutPoint(e.clientX, e.clientY);
            if (e.deltaMode === 1) {
              const fn = exp.zeus_scroll_step || exp.zeus_scroll;
              fn(p.x, p.y, e.deltaX * 16, e.deltaY * 16);
            } else if (e.deltaMode === 2) {
              const fn = exp.zeus_scroll_step || exp.zeus_scroll;
              fn(p.x, p.y, e.deltaX * layoutW, e.deltaY * layoutH);
            } else {
              exp.zeus_scroll(p.x, p.y, e.deltaX, e.deltaY);
            }
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
            if (e.key === "Escape") key = 27;
            if (e.key === "ArrowLeft") key = 1000;
            if (e.key === "ArrowRight") key = 1001;
            if (e.key === "ArrowUp") key = 1002;
            if (e.key === "ArrowDown") key = 1003;
            if (e.key === "Home") key = 1006;
            if (e.key === "End") key = 1007;
            exp.zeus_key(key, mods);
            schedule(0);
            if (e.key === "Tab") e.preventDefault();
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
            if (e.key === "Escape") key = 27;
            if (e.key === "ArrowLeft") key = 1000;
            if (e.key === "ArrowRight") key = 1001;
            if (e.key === "ArrowUp") key = 1002;
            if (e.key === "ArrowDown") key = 1003;
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
