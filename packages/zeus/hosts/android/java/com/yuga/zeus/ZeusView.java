package com.yuga.zeus;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.util.Base64;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import android.net.Uri;
import android.view.WindowInsets;

import java.io.InputStream;
import java.net.URL;
import java.util.HashMap;
import java.util.HashSet;

/**
 * Zeus host view. Density-scaled so layout units match iOS points. Touch is
 * forwarded to the Yuga hit-test; Android widgets are not used.
 */
public class ZeusView extends View implements Choreographer.FrameCallback {
    static {
        System.loadLibrary("zeus");
    }

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final HashMap<String, Bitmap> imgCache = new HashMap<String, Bitmap>();
    private final HashSet<String> imgLoading = new HashSet<String>();
    private static final Bitmap IMG_FAIL = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
    private final Choreographer choreographer = Choreographer.getInstance();
    private boolean started;
    private boolean framed;
    private float lastX, lastY;
    private boolean tracking, scrolling;

    public ZeusView(Context context) {
        super(context);
        setFocusable(true);
        setFocusableInTouchMode(true);
        text.setTypeface(Typeface.SANS_SERIF);
        stroke.setStyle(Paint.Style.STROKE);
        fill.setStyle(Paint.Style.FILL);
    }

    native void nativeStart();
    native void nativeResize(int w, int h, int insetT, int insetR, int insetB, int insetL);
    native void nativePaint(Canvas canvas);
    native void nativePointerDown(int x, int y);
    native void nativePointerMove(int x, int y, int dx, int dy);
    native void nativePointerUp();
    native void nativeKey(int key, int mods);
    native void nativePicked(String src, int w, int h);

    static final int PICK_IMAGE = 1;

    void jniPick() {
        Context ctx = getContext();
        if (!(ctx instanceof Activity)) return;
        Intent i = new Intent(Intent.ACTION_GET_CONTENT);
        i.setType("image/*");
        i.addCategory(Intent.CATEGORY_OPENABLE);
        ((Activity) ctx).startActivityForResult(Intent.createChooser(i, "Image"), PICK_IMAGE);
    }

    void pickedUri(Uri uri) {
        if (uri == null) return;
        String src = uri.toString();
        int w = 0, h = 0;
        InputStream in = null;
        try {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            in = getContext().getContentResolver().openInputStream(uri);
            BitmapFactory.decodeStream(in, null, o);
            w = o.outWidth;
            h = o.outHeight;
        } catch (Exception e) {
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Exception e) {
                }
            }
        }
        nativePicked(src, w, h);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (!started) {
            started = true;
            new Thread(() -> nativeStart(), "zeus-main").start();
        }
        if (!framed) {
            framed = true;
            choreographer.postFrameCallback(this);
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        framed = false;
        choreographer.removeFrameCallback(this);
        super.onDetachedFromWindow();
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!framed) return;
        invalidate();
        choreographer.postFrameCallback(this);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        float d = density();
        int dw = Math.max(1, (int) (w / d));
        int dh = Math.max(1, (int) (h / d));
        int it = 0, ir = 0, ib = 0, il = 0;
        if (Build.VERSION.SDK_INT >= 23) {
            WindowInsets in = getRootWindowInsets();
            if (in != null) {
                it = (int) (in.getSystemWindowInsetTop() / d);
                ir = (int) (in.getSystemWindowInsetRight() / d);
                ib = (int) (in.getSystemWindowInsetBottom() / d);
                il = (int) (in.getSystemWindowInsetLeft() / d);
            }
        }
        nativeResize(dw, dh, it, ir, ib, il);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(0xFFF7F7F8);
        float d = density();
        canvas.save();
        canvas.scale(d, d);
        nativePaint(canvas);
        canvas.restore();
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        float d = density();
        int x = (int) (ev.getX() / d);
        int y = (int) (ev.getY() / d);
        int action = ev.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            lastX = ev.getX();
            lastY = ev.getY();
            tracking = true;
            scrolling = false;
            nativePointerDown(x, y);
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE && tracking) {
            int dx = (int) ((lastX - ev.getX()) / d);
            int dy = (int) ((lastY - ev.getY()) / d);
            if (!scrolling && dx * dx + dy * dy > 16) scrolling = true;
            nativePointerMove(x, y, scrolling ? dx : 0, scrolling ? dy : 0);
            lastX = ev.getX();
            lastY = ev.getY();
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            tracking = false;
            scrolling = false;
            nativePointerUp();
            return true;
        }
        return super.onTouchEvent(ev);
    }

    float density() {
        float d = getResources().getDisplayMetrics().density;
        return d > 0.1f ? d : 1f;
    }

    void jniFill(Canvas c, int x, int y, int w, int h, int rgb, int radius) {
        fill.setColor(0xFF000000 | (rgb & 0xFFFFFF));
        fillRound(c, x, y, w, h, radius, fill);
    }

    void jniFillA(Canvas c, int x, int y, int w, int h, int rgb, int radius, int alpha) {
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        fill.setColor(((a & 0xFF) << 24) | (rgb & 0xFFFFFF));
        fillRound(c, x, y, w, h, radius, fill);
    }

    void jniText(Canvas c, int x, int y, String s, int rgb, int font) {
        if (s == null) s = "";
        int px = font < 8 ? 8 : font;
        text.setColor(0xFF000000 | (rgb & 0xFFFFFF));
        text.setTextSize(px);
        Paint.FontMetrics fm = text.getFontMetrics();
        c.drawText(s, x, y - fm.ascent, text);
    }

    void jniTextRot(Canvas c, int x, int y, String s, int rgb, int font, int deg) {
        if (s == null) s = "";
        int px = font < 8 ? 8 : font;
        text.setColor(0xFF000000 | (rgb & 0xFFFFFF));
        text.setTextSize(px);
        Paint.FontMetrics fm = text.getFontMetrics();
        c.save();
        c.rotate(deg, x, y);
        c.drawText(s, x, y - fm.ascent, text);
        c.restore();
    }

    void jniSave(Canvas c) {
        c.save();
    }

    void jniClip(Canvas c, int x, int y, int w, int h) {
        c.clipRect(x, y, x + w, y + h);
    }

    void jniRestore(Canvas c) {
        c.restore();
    }

    void jniSvg(Canvas c, int x, int y, int w, int h, String markup, int rgb, int alpha) {
        Svg.draw(c, x, y, w, h, markup, rgb, alpha, fill, stroke);
    }

    void jniImage(Canvas c, int x, int y, int w, int h, String src, int radius, int alpha, int fit) {
        if (src == null || src.length() == 0 || w <= 0 || h <= 0) return;
        Bitmap bmp = imgGet(src);
        if (bmp == null || bmp == IMG_FAIL) return;
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        bitmapPaint.setAlpha(a);
        int iw = bmp.getWidth();
        int ih = bmp.getHeight();
        float dx = x, dy = y, dw = w, dh = h;
        if (fit == 3 && iw > 0 && ih > 0) {
            dw = iw;
            dh = ih;
        } else if ((fit == 1 || fit == 2) && iw > 0 && ih > 0) {
            if ((fit == 1 && iw * (long) h <= ih * (long) w)
                    || (fit == 2 && iw * (long) h >= ih * (long) w)) {
                dh = h;
                dw = iw * (float) h / ih;
            } else {
                dw = w;
                dh = ih * (float) w / iw;
            }
            dx = x + (w - dw) / 2f;
            dy = y + (h - dh) / 2f;
        }
        c.save();
        if (radius > 0) {
            float rad = radius;
            if (rad > w / 2f) rad = w / 2f;
            if (rad > h / 2f) rad = h / 2f;
            Path p = new Path();
            p.addRoundRect(new RectF(x, y, x + w, y + h), rad, rad, Path.Direction.CW);
            c.clipPath(p);
        } else {
            c.clipRect(x, y, x + w, y + h);
        }
        c.drawBitmap(bmp, null, new RectF(dx, dy, dx + dw, dy + dh), bitmapPaint);
        c.restore();
    }

    private Bitmap imgGet(String src) {
        Bitmap hit = imgCache.get(src);
        if (hit != null) return hit;
        if (imgLoading.contains(src)) return null;
        if (src.startsWith("data:")) {
            int comma = src.indexOf(',');
            Bitmap bmp = null;
            if (comma > 0) {
                try {
                    byte[] raw = Base64.decode(src.substring(comma + 1), Base64.DEFAULT);
                    bmp = BitmapFactory.decodeByteArray(raw, 0, raw.length);
                } catch (Exception e) {
                    bmp = null;
                }
            }
            imgCache.put(src, bmp != null ? bmp : IMG_FAIL);
            return bmp;
        }
        if (src.startsWith("content:")) {
            Bitmap bmp = null;
            InputStream in = null;
            try {
                in = getContext().getContentResolver().openInputStream(Uri.parse(src));
                bmp = BitmapFactory.decodeStream(in);
            } catch (Exception e) {
                bmp = null;
            } finally {
                if (in != null) {
                    try {
                        in.close();
                    } catch (Exception e) {
                    }
                }
            }
            imgCache.put(src, bmp != null ? bmp : IMG_FAIL);
            return bmp;
        }
        if (src.startsWith("http://") || src.startsWith("https://")) {
            imgLoading.add(src);
            final String key = src;
            new Thread(() -> {
                Bitmap bmp = null;
                InputStream in = null;
                try {
                    in = new URL(key).openStream();
                    bmp = BitmapFactory.decodeStream(in);
                } catch (Exception e) {
                    bmp = null;
                } finally {
                    if (in != null) {
                        try {
                            in.close();
                        } catch (Exception e) {
                        }
                    }
                }
                final Bitmap got = bmp;
                post(() -> {
                    imgCache.put(key, got != null ? got : IMG_FAIL);
                    imgLoading.remove(key);
                    invalidate();
                });
            }).start();
            return null;
        }
        String path = src.startsWith("file://") ? src.substring(7) : src;
        Bitmap bmp = BitmapFactory.decodeFile(path);
        imgCache.put(src, bmp != null ? bmp : IMG_FAIL);
        return bmp;
    }

    long jniMeasure(String s, int px) {
        if (s == null) s = "";
        if (px < 8) px = 8;
        text.setTextSize(px);
        Paint.FontMetrics fm = text.getFontMetrics();
        int tw = (int) (text.measureText(s) + 0.999f);
        int th = (int) (fm.descent - fm.ascent + 0.999f);
        return ((long) tw << 32) | (th & 0xffffffffL);
    }

    /** Intrinsic size of a decoded image, packed as (w << 32) | h, or 0 when
     *  it is still loading. Layout uses it so a widthless Image keeps the
     *  source aspect instead of collapsing to zero height. */
    long jniImageSize(String src) {
        Bitmap bmp = imgGet(src);
        if (bmp == null || bmp == IMG_FAIL) return 0;
        return ((long) bmp.getWidth() << 32) | (bmp.getHeight() & 0xffffffffL);
    }

    void jniInvalidate() {
        postInvalidate();
    }

    private static void fillRound(Canvas c, int x, int y, int w, int h, int radius, Paint p) {
        if (w <= 0 || h <= 0) return;
        if (radius <= 0) {
            c.drawRect(x, y, x + w, y + h, p);
            return;
        }
        float rad = radius;
        if (rad > w / 2f) rad = w / 2f;
        if (rad > h / 2f) rad = h / 2f;
        c.drawRoundRect(new RectF(x, y, x + w, y + h), rad, rad, p);
    }

    /** Solar Linear subset: svg / g / path / circle with currentColor. */
    static final class Svg {
        static void draw(Canvas c, int x, int y, int w, int h, String markup, int rgb, int alpha,
                         Paint fill, Paint stroke) {
            if (markup == null || w <= 0 || h <= 0) return;
            float vbX = 0, vbY = 0, vbW = 24, vbH = 24;
            int svgAt = markup.indexOf("<svg");
            if (svgAt < 0) svgAt = 0;
            String head = tagAt(markup, svgAt);
            String vb = attr(head, "viewBox");
            if (vb == null) vb = attr(head, "viewbox");
            if (vb != null) {
                float[] n = nums(vb, 4);
                if (n.length >= 4) {
                    vbX = n[0];
                    vbY = n[1];
                    vbW = n[2];
                    vbH = n[3];
                }
            }
            if (vbW < 1) vbW = 24;
            if (vbH < 1) vbH = 24;
            float sx = w / vbW;
            float sy = h / vbH;
            float scale = sx < sy ? sx : sy;
            float ox = x + (w - vbW * scale) * 0.5f - vbX * scale;
            float oy = y + (h - vbH * scale) * 0.5f - vbY * scale;
            int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
            walk(markup, svgAt, ox, oy, scale, rgb, a, fill, stroke, c);
        }

        static void walk(String s, int from, float ox, float oy, float scale, int rgb, int alpha,
                         Paint fill, Paint stroke, Canvas c) {
            String group = "";
            int i = from;
            while (i < s.length()) {
                int lt = s.indexOf('<', i);
                if (lt < 0) break;
                if (lt + 1 < s.length() && s.charAt(lt + 1) == '/') {
                    String close = tagAt(s, lt).replace("/", "");
                    if ("g".equals(tagName(close))) group = "";
                    i = s.indexOf('>', lt);
                    if (i < 0) break;
                    i++;
                    continue;
                }
                String tag = tagAt(s, lt);
                String name = tagName(tag);
                if ("g".equals(name)) {
                    group = tag;
                } else if ("path".equals(name)) {
                    String d = attr(tag, "d");
                    Path p = pathD(d, ox, oy, scale);
                    paintPath(c, p, merge(group, tag), rgb, alpha, scale, fill, stroke);
                } else if ("circle".equals(name)) {
                    float cx = num1(attr(tag, "cx"), 0) * scale + ox;
                    float cy = num1(attr(tag, "cy"), 0) * scale + oy;
                    float r = num1(attr(tag, "r"), 0) * scale;
                    Path p = new Path();
                    p.addCircle(cx, cy, r, Path.Direction.CW);
                    paintPath(c, p, merge(group, tag), rgb, alpha, scale, fill, stroke);
                }
                i = lt + 1;
            }
        }

        static String merge(String group, String tag) {
            if (group == null || group.length() == 0) return tag;
            return group + " " + tag;
        }

        static void paintPath(Canvas c, Path p, String tag, int rgb, int alpha, float scale,
                              Paint fill, Paint stroke) {
            if (p == null || p.isEmpty()) return;
            int hasFill = 1, hasStroke = 0;
            int fillC = rgb, strokeC = rgb;
            float sw = 1.5f;
            String f = attr(tag, "fill");
            if (f != null) {
                if ("none".equals(f) || "transparent".equals(f)) hasFill = 0;
                else {
                    hasFill = 1;
                    fillC = color(f, rgb);
                }
            }
            String st = attr(tag, "stroke");
            if (st != null) {
                if ("none".equals(st)) hasStroke = 0;
                else {
                    hasStroke = 1;
                    strokeC = color(st, rgb);
                }
            }
            String sws = attr(tag, "stroke-width");
            if (sws != null) sw = num1(sws, sw);
            if (hasFill != 0) {
                fill.setColor(((alpha & 0xFF) << 24) | (fillC & 0xFFFFFF));
                fill.setStyle(Paint.Style.FILL);
                c.drawPath(p, fill);
            }
            if (hasStroke != 0) {
                stroke.setColor(((alpha & 0xFF) << 24) | (strokeC & 0xFFFFFF));
                stroke.setStrokeWidth(sw * scale);
                String cap = attr(tag, "stroke-linecap");
                stroke.setStrokeCap("round".equals(cap) ? Paint.Cap.ROUND
                        : "square".equals(cap) ? Paint.Cap.SQUARE : Paint.Cap.BUTT);
                String join = attr(tag, "stroke-linejoin");
                stroke.setStrokeJoin("round".equals(join) ? Paint.Join.ROUND
                        : "bevel".equals(join) ? Paint.Join.BEVEL : Paint.Join.MITER);
                stroke.setStyle(Paint.Style.STROKE);
                c.drawPath(p, stroke);
            }
        }

        static int color(String s, int current) {
            if (s == null) return current;
            if ("currentColor".equals(s) || "currentcolor".equals(s)) return current;
            if (s.length() > 0 && s.charAt(0) == '#') {
                try {
                    if (s.length() == 4) {
                        int r = Character.digit(s.charAt(1), 16);
                        int g = Character.digit(s.charAt(2), 16);
                        int b = Character.digit(s.charAt(3), 16);
                        return (r * 0x11) << 16 | (g * 0x11) << 8 | (b * 0x11);
                    }
                    if (s.length() >= 7) return Integer.parseInt(s.substring(1, 7), 16);
                } catch (NumberFormatException e) {
                    return current;
                }
            }
            return current;
        }

        static String tagAt(String s, int lt) {
            int gt = s.indexOf('>', lt);
            if (gt < 0) return "";
            return s.substring(lt + 1, gt);
        }

        static String tagName(String tag) {
            int n = 0;
            while (n < tag.length() && tag.charAt(n) != ' ' && tag.charAt(n) != '/' &&
                    tag.charAt(n) != '\n' && tag.charAt(n) != '\t') n++;
            return tag.substring(0, n).toLowerCase();
        }

        static String attr(String tag, String name) {
            String key = name + "=";
            int at = indexOfAttr(tag, key);
            if (at < 0) return null;
            int q = at + key.length();
            if (q >= tag.length()) return null;
            char quote = tag.charAt(q);
            if (quote != '"' && quote != '\'') return null;
            int end = tag.indexOf(quote, q + 1);
            if (end < 0) return null;
            return tag.substring(q + 1, end);
        }

        static int indexOfAttr(String tag, String key) {
            int from = 0;
            while (true) {
                int at = tag.indexOf(key, from);
                if (at < 0) return -1;
                if (at == 0 || tag.charAt(at - 1) == ' ' || tag.charAt(at - 1) == '\t' ||
                        tag.charAt(at - 1) == '\n') return at;
                from = at + 1;
            }
        }

        static float num1(String s, float def) {
            if (s == null) return def;
            float[] n = nums(s, 1);
            return n.length > 0 ? n[0] : def;
        }

        static float[] nums(String s, int max) {
            float[] out = new float[max];
            int n = 0, i = 0;
            while (i < s.length() && n < max) {
                char ch = s.charAt(i);
                if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\n') {
                    i++;
                    continue;
                }
                int start = i;
                if (ch == '-' || ch == '+') i++;
                while (i < s.length()) {
                    char d = s.charAt(i);
                    if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E') i++;
                    else break;
                }
                if (i == start || (i == start + 1 && (s.charAt(start) == '-' || s.charAt(start) == '+'))) {
                    i++;
                    continue;
                }
                try {
                    out[n++] = Float.parseFloat(s.substring(start, i));
                } catch (NumberFormatException e) {
                    break;
                }
            }
            if (n == max) return out;
            float[] slim = new float[n];
            System.arraycopy(out, 0, slim, 0, n);
            return slim;
        }

        static Path pathD(String d, float ox, float oy, float scale) {
            Path p = new Path();
            if (d == null) return p;
            int i = 0;
            char cmd = 'M';
            float cx = 0, cy = 0, sx = 0, sy = 0;
            while (i < d.length()) {
                char ch = d.charAt(i);
                if (ch == ' ' || ch == ',' || ch == '\t' || ch == '\n') {
                    i++;
                    continue;
                }
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                    cmd = ch;
                    i++;
                    continue;
                }
                float[] args;
                if (cmd == 'H' || cmd == 'h' || cmd == 'V' || cmd == 'v') args = take(d, i, 1);
                else if (cmd == 'A' || cmd == 'a') args = take(d, i, 7);
                else if (cmd == 'C' || cmd == 'c') args = take(d, i, 6);
                else if (cmd == 'S' || cmd == 's' || cmd == 'Q' || cmd == 'q') args = take(d, i, 4);
                else if (cmd == 'T' || cmd == 't' || cmd == 'M' || cmd == 'm' || cmd == 'L' || cmd == 'l')
                    args = take(d, i, 2);
                else if (cmd == 'Z' || cmd == 'z') {
                    p.close();
                    cx = sx;
                    cy = sy;
                    continue;
                } else {
                    i++;
                    continue;
                }
                i = skipTaken(d, i, args.length);
                if (cmd == 'M' || cmd == 'm') {
                    if (args.length < 2) continue;
                    if (cmd == 'm') {
                        cx += args[0];
                        cy += args[1];
                    } else {
                        cx = args[0];
                        cy = args[1];
                    }
                    p.moveTo(cx * scale + ox, cy * scale + oy);
                    sx = cx;
                    sy = cy;
                    cmd = cmd == 'm' ? 'l' : 'L';
                } else if (cmd == 'L' || cmd == 'l') {
                    if (args.length < 2) continue;
                    if (cmd == 'l') {
                        cx += args[0];
                        cy += args[1];
                    } else {
                        cx = args[0];
                        cy = args[1];
                    }
                    p.lineTo(cx * scale + ox, cy * scale + oy);
                } else if (cmd == 'H' || cmd == 'h') {
                    if (args.length < 1) continue;
                    cx = cmd == 'h' ? cx + args[0] : args[0];
                    p.lineTo(cx * scale + ox, cy * scale + oy);
                } else if (cmd == 'V' || cmd == 'v') {
                    if (args.length < 1) continue;
                    cy = cmd == 'v' ? cy + args[0] : args[0];
                    p.lineTo(cx * scale + ox, cy * scale + oy);
                } else if (cmd == 'C' || cmd == 'c') {
                    if (args.length < 6) continue;
                    float x1 = args[0], y1 = args[1], x2 = args[0 + 2], y2 = args[3], x = args[4], y = args[5];
                    if (cmd == 'c') {
                        x1 += cx;
                        y1 += cy;
                        x2 += cx;
                        y2 += cy;
                        x += cx;
                        y += cy;
                    }
                    p.cubicTo(x1 * scale + ox, y1 * scale + oy, x2 * scale + ox, y2 * scale + oy,
                            x * scale + ox, y * scale + oy);
                    cx = x;
                    cy = y;
                } else if (cmd == 'Q' || cmd == 'q') {
                    if (args.length < 4) continue;
                    float x1 = args[0], y1 = args[1], x = args[2], y = args[3];
                    if (cmd == 'q') {
                        x1 += cx;
                        y1 += cy;
                        x += cx;
                        y += cy;
                    }
                    p.quadTo(x1 * scale + ox, y1 * scale + oy, x * scale + ox, y * scale + oy);
                    cx = x;
                    cy = y;
                } else if (cmd == 'Z' || cmd == 'z') {
                    p.close();
                    cx = sx;
                    cy = sy;
                }
            }
            return p;
        }

        static float[] take(String d, int i, int n) {
            while (i < d.length() && (d.charAt(i) == ' ' || d.charAt(i) == ',')) i++;
            return nums(d.substring(i), n);
        }

        static int skipTaken(String d, int i, int n) {
            int seen = 0;
            while (i < d.length() && seen < n) {
                char ch = d.charAt(i);
                if (ch == ' ' || ch == ',' || ch == '\t' || ch == '\n') {
                    i++;
                    continue;
                }
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) break;
                if (ch == '-' || ch == '+' || (ch >= '0' && ch <= '9') || ch == '.') {
                    if (ch == '-' || ch == '+') i++;
                    while (i < d.length()) {
                        char c2 = d.charAt(i);
                        if ((c2 >= '0' && c2 <= '9') || c2 == '.' || c2 == 'e' || c2 == 'E') i++;
                        else break;
                    }
                    seen++;
                    continue;
                }
                i++;
            }
            return i;
        }
    }
}
