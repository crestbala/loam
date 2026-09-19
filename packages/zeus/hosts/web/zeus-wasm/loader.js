/* Zeus browser host. Canvas2D only — no WebGPU, no WebGL, no DOM widgets.
   One <canvas>, this file, and a .wasm built by `loam --target wasm`. */
(function () {
  const canvas = document.getElementById("zeus");
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("Canvas2D unavailable");

  const dpr = window.devicePixelRatio || 1;
  function sizeCanvas() {
    const w = canvas.clientWidth || window.innerWidth;
    const h = canvas.clientHeight || window.innerHeight;
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { w: w, h: h };
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
  const fontFace = "system-ui, sans-serif";
  const fontMetrics = new Map();
  function setFont(px) {
    ctx.font = px + "px " + fontFace;
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

  let mem = null;
  const imgCache = new Map();
  let fileInput = null;
  let exp = null;
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
      request_frame: () => {},
      now_ms: () => BigInt(Math.floor(performance.now())),
      write: (p, n) => {
        const t = bytes(p, n);
        if (t) console.log(t);
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
      text: (x, y, ptr, color, font) => {
        const s = cstr(ptr);
        const px = font > 0 ? font : 13;
        const box = lineBox(px);
        ctx.fillStyle = rgb(color);
        setFont(px);
        ctx.textBaseline = "alphabetic";
        ctx.fillText(s, x, y + box.ascent);
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
        ctx.translate(x, y + box.ascent);
        ctx.rotate((deg * Math.PI) / 180);
        ctx.fillText(s, 0, 0);
        ctx.restore();
      },
      measure: (ptr, px, wPtr, hPtr) => {
        const s = cstr(ptr);
        const size = px > 0 ? px : 13;
        const box = lineBox(size);
        setFont(size);
        const m = ctx.measureText(s);
        const view = new DataView(mem.buffer);
        view.setInt32(wPtr, Math.ceil(m.width), true);
        view.setInt32(hPtr, box.height, true);
      },
      save: () => ctx.save(),
      alpha: (a) => {
        ctx.save();
        ctx.globalAlpha = Math.max(0, Math.min(255, a)) / 255;
      },
      clip: (x, y, w, h) => {
        ctx.beginPath();
        ctx.rect(x, y, w, h);
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
            if (!f || !exp || !mem) return;
            const url = URL.createObjectURL(f);
            const im = new Image();
            im.onload = () => {
              imgCache.set(url, im);
              const bytes = new TextEncoder().encode(url);
              const cap = exp.zeus_text_buf_cap ? exp.zeus_text_buf_cap() : 0;
              const ptr = exp.zeus_text_buf ? exp.zeus_text_buf() : 0;
              if (!ptr || cap < 2) return;
              const n = Math.min(bytes.length, cap - 1);
              new Uint8Array(mem.buffer, ptr, n).set(bytes.subarray(0, n));
              if (exp.zeus_picked) exp.zeus_picked(n, im.naturalWidth, im.naturalHeight);
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
          im.onload = () => {};
          im.onerror = () => {
            imgCache.set(src, "fail");
          };
          im.src = src;
          imgCache.set(src, im);
          rec = im;
        }
        if (!rec.complete || !rec.naturalWidth) return;
        const iw = rec.naturalWidth;
        const ih = rec.naturalHeight;
        let dx = x, dy = y, dw = w, dh = h;
        if (fit === 3) {
          dw = iw;
          dh = ih;
        } else if (fit === 1 || fit === 2) {
          if ((fit === 1 && iw * h <= ih * w) || (fit === 2 && iw * h >= ih * w)) {
            dh = h;
            dw = ih ? (iw * h) / ih : w;
          } else {
            dw = w;
            dh = iw ? (ih * w) / iw : h;
          }
          dx = x + (w - dw) / 2;
          dy = y + (h - dh) / 2;
        }
        ctx.save();
        if (radius > 0) {
          let r = radius;
          if (r > w / 2) r = w / 2;
          if (r > h / 2) r = h / 2;
          ctx.beginPath();
          ctx.moveTo(x + r, y);
          ctx.arcTo(x + w, y, x + w, y + h, r);
          ctx.arcTo(x + w, y + h, x, y + h, r);
          ctx.arcTo(x, y + h, x, y, r);
          ctx.arcTo(x, y, x + w, y, r);
          ctx.closePath();
          ctx.clip();
        } else {
          ctx.beginPath();
          ctx.rect(x, y, w, h);
          ctx.clip();
        }
        ctx.globalAlpha = alpha >= 0 && alpha < 255 ? alpha / 255 : 1;
        ctx.drawImage(rec, dx, dy, dw, dh);
        ctx.restore();
      },
      /* Intrinsic size for layout (`platform.plat_image_size`): the two i32
         out-params. Decode is shared with `image`; a not-yet-decoded source
         reports 0 and the load's callback re-lays out on the next frame. */
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
          im.onload = () => {};
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
        const sx = w / (vw || 1);
        const sy = h / (vh || 1);
        ctx.save();
        ctx.translate(x, y);
        ctx.scale(sx, sy);
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
      fetch_get: (ptr, len, outPtr, cap) => {
        try {
          const path = bytes(ptr, len) || "/";
          const xhr = new XMLHttpRequest();
          xhr.open("GET", path, false);
          xhr.send(null);
          if (xhr.status !== 200) return 0;
          const t = xhr.responseText || "";
          const enc = new TextEncoder().encode(t);
          const n = Math.min(enc.length, cap);
          u8().set(enc.subarray(0, n), outPtr);
          return n;
        } catch (e) {
          console.warn("fetch_get", e);
          return 0;
        }
      },
    },
  };

  const wasmUrl = canvas.getAttribute("data-wasm") || "app.wasm";
  fetch(wasmUrl)
    .then((r) => r.arrayBuffer())
    .then((buf) => WebAssembly.instantiate(buf, imports))
    .then((r) => {
      exp = r.instance.exports;
      mem = exp.memory;
      const sz = sizeCanvas();
      exp.zeus_start();
      if (exp.zeus_resize) exp.zeus_resize(sz.w, sz.h);
      function frame() {
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        try {
          exp.zeus_paint();
        } catch (e) {
          console.error("zeus_paint", e);
          return;
        }
        requestAnimationFrame(frame);
      }
      requestAnimationFrame(frame);
      window.addEventListener("resize", () => {
        const s = sizeCanvas();
        exp.zeus_resize(s.w, s.h);
      });
      canvas.addEventListener("pointerdown", (e) => {
        const r = canvas.getBoundingClientRect();
        exp.zeus_pointer_down(e.clientX - r.left, e.clientY - r.top);
      });
      canvas.addEventListener("pointermove", (e) => {
        const r = canvas.getBoundingClientRect();
        exp.zeus_pointer_move(e.clientX - r.left, e.clientY - r.top);
        const cp = exp.zeus_cursor_sync ? exp.zeus_cursor_sync() : 0;
        canvas.style.cursor = cstr(cp) || "default";
      });
      canvas.addEventListener("pointerup", () => exp.zeus_pointer_up());
      canvas.addEventListener(
        "wheel",
        (e) => {
          e.preventDefault();
          const r = canvas.getBoundingClientRect();
          const x = e.clientX - r.left;
          const y = e.clientY - r.top;
          /* Trackpads: pixel deltas per frame, momentum applies. Mouse
             wheels: line/page clicks — scale to points, step without coast. */
          if (e.deltaMode === 1) {
            const fn = exp.zeus_scroll_step || exp.zeus_scroll;
            fn(x, y, e.deltaX * 16, e.deltaY * 16);
          } else if (e.deltaMode === 2) {
            const fn = exp.zeus_scroll_step || exp.zeus_scroll;
            fn(x, y, e.deltaX * window.innerWidth, e.deltaY * window.innerHeight);
          } else {
            exp.zeus_scroll(x, y, e.deltaX, e.deltaY);
          }
        },
        { passive: false }
      );
      window.addEventListener("keydown", (e) => {
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
        if (e.key === "Tab") e.preventDefault();
        if (e.key === "Enter" && exp.zeus_captures_text && exp.zeus_captures_text())
          e.preventDefault();
      });
      function sendWasmText(s, marked) {
        if (!s || !exp.zeus_text_buf) return;
        const bytes = new TextEncoder().encode(s);
        const cap = exp.zeus_text_buf_cap();
        const ptr = exp.zeus_text_buf();
        const n = Math.min(bytes.length, cap - 1);
        new Uint8Array(mem.buffer, ptr, n).set(bytes.subarray(0, n));
        if (marked && exp.zeus_marked) exp.zeus_marked(n);
        else if (exp.zeus_text) exp.zeus_text(n);
      }
      window.addEventListener("paste", (e) => {
        if (!exp.zeus_captures_text || !exp.zeus_captures_text()) return;
        const t = e.clipboardData && e.clipboardData.getData("text");
        if (!t) return;
        e.preventDefault();
        sendWasmText(t, false);
      });
      window.addEventListener("compositionupdate", (e) => {
        if (e.data) sendWasmText(e.data, true);
      });
      window.addEventListener("compositionend", (e) => {
        sendWasmText("", true);
        if (e.data) sendWasmText(e.data, false);
      });
    })
    .catch((err) => {
      console.error(err);
      document.body.appendChild(document.createTextNode(String(err)));
    });
})();
