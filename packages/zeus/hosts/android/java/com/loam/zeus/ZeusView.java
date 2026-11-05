package com.loam.zeus;

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
import android.provider.Settings;
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
 * forwarded to the Loam hit-test; Android widgets are not used.
 */
public class ZeusView extends View implements Choreographer.FrameCallback {
    static {
        System.loadLibrary("zeus");
    }

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pathPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path pathBuf = new Path();
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    /* Shadow needs its own Paint: setShadowLayer is sticky on the Paint, and
       sharing `fill` would leave a blur on every later fill. */
    private final Paint shadowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path clipPath = new Path();
    private final Path shadowPath = new Path();
    private final Path rect4 = new Path();
    private final float[] corners = new float[8];
    private final HashMap<String, Bitmap> imgCache = new HashMap<String, Bitmap>();
    private final HashSet<String> imgLoading = new HashSet<String>();
    private static final Bitmap IMG_FAIL = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
    private final Choreographer choreographer = Choreographer.getInstance();
    private boolean started;
    private boolean framed;
    private float lastX, lastY;
    private boolean tracking, scrolling;
    private float startX, startY, remX, remY;

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
    native void nativeReducedMotion(int on);

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
        /* Reduced motion (§3.2): the system's animator duration scale of 0
           means "no animation"; report it once. */
        try {
            float scale = Settings.Global.getFloat(getContext().getContentResolver(),
                                                   Settings.Global.ANIMATOR_DURATION_SCALE, 1f);
            nativeReducedMotion(scale == 0f ? 1 : 0);
        } catch (Exception e) {
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
            startX = lastX;
            startY = lastY;
            remX = 0;
            remY = 0;
            tracking = true;
            scrolling = false;
            nativePointerDown(x, y);
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE && tracking) {
            // The engine takes whole dp. Carry the fraction, or a slow drag
            // loses a piece of every move and falls behind the finger.
            float fx = (lastX - ev.getX()) / d + remX;
            float fy = (lastY - ev.getY()) / d + remY;
            int dx = (int) fx;
            int dy = (int) fy;
            remX = fx - dx;
            remY = fy - dy;
            // Measured from the press: moves are small, so a slow start judged
            // one move at a time would never become a scroll.
            if (!scrolling) {
                float sx = (startX - ev.getX()) / d;
                float sy = (startY - ev.getY()) / d;
                if (sx * sx + sy * sy > 64) scrolling = true;
            }
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

    void jniFillG(Canvas c, int x, int y, int w, int h, int c0, int c1, int axis,
                  int radius, int alpha) {
        if (w <= 0 || h <= 0) return;
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        int col0 = ((a & 0xFF) << 24) | (c0 & 0xFFFFFF);
        int col1 = ((a & 0xFF) << 24) | (c1 & 0xFFFFFF);
        fill.setShader(new android.graphics.LinearGradient(
                x, y, axis != 0 ? x + w : x, axis != 0 ? y : y + h,
                col0, col1, android.graphics.Shader.TileMode.CLAMP));
        fill.setColor(col0);
        // The radius used to be dropped on every host, which is why gradient
        // backgrounds painted square-cornered.
        fillRound(c, x, y, w, h, radius, fill);
        fill.setShader(null);
    }

    /** Soft drop shadow.
     *
     *  setShadowLayer blurs the shape that is drawn, so the shape itself has
     *  to be drawn — and would paint over whatever the card sits on. The
     *  rect's own area is clipped out first (clipOutPath, API 26+), so the
     *  opaque path lands entirely outside the clip and only its blur
     *  survives. Same construction as the Cocoa and iOS hosts.
     *
     *  Shape shadow layers are honoured on a hardware canvas from API 28. On
     *  26/27 the blur is dropped and the surface simply reads flat, which is
     *  the documented degradation for a host that cannot blur — never a
     *  hard-edged rectangle. */
    void jniShadow(Canvas c, int x, int y, int w, int h, int radius, int rgb, int alpha,
                   int blur, int dx, int dy) {
        if (w <= 0 || h <= 0 || alpha <= 0) return;
        int a = alpha > 255 ? 255 : alpha;
        float rad = radius;
        if (rad > w / 2f) rad = w / 2f;
        if (rad > h / 2f) rad = h / 2f;
        if (rad < 0) rad = 0;
        shadowPath.reset();
        shadowPath.addRoundRect(new RectF(x, y, x + w, y + h), rad, rad, Path.Direction.CW);
        c.save();
        c.clipOutPath(shadowPath);
        shadowPaint.setColor(0xFF000000);
        shadowPaint.setShadowLayer(blur <= 0 ? 0.01f : blur, dx, dy,
                                   ((a & 0xFF) << 24) | (rgb & 0xFFFFFF));
        c.drawPath(shadowPath, shadowPaint);
        shadowPaint.clearShadowLayer();
        c.restore();
    }

    /** Ring stroke inset by half the width, so the line paints inside the
     *  rect — the box model layout assumes when it insets content by the
     *  border width. */
    void jniStroke(Canvas c, int x, int y, int w, int h, int rgb, int radius, int width,
                   int alpha) {
        if (width <= 0 || w - width <= 0 || h - width <= 0) return;
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        float lw = width;
        stroke.setColor(((a & 0xFF) << 24) | (rgb & 0xFFFFFF));
        stroke.setStrokeWidth(lw);
        float rad = radius - lw / 2f;
        if (rad < 0) rad = 0;
        RectF r = new RectF(x + lw / 2f, y + lw / 2f, x + w - lw / 2f, y + h - lw / 2f);
        if (rad > r.width() / 2f) rad = r.width() / 2f;
        if (rad > r.height() / 2f) rad = r.height() / 2f;
        if (rad <= 0) c.drawRect(r, stroke);
        else c.drawRoundRect(r, rad, rad, stroke);
    }

    void jniFill4(Canvas c, int x, int y, int w, int h, int rgb, int alpha,
                  int tl, int tr, int br, int bl) {
        if (w <= 0 || h <= 0) return;
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        float lim = Math.min(w, h) / 2f;
        float a1 = Math.min(tl, lim), a2 = Math.min(tr, lim);
        float a3 = Math.min(br, lim), a4 = Math.min(bl, lim);
        fill.setColor(((a & 0xFF) << 24) | (rgb & 0xFFFFFF));
        rect4.reset();
        // Android takes (rx, ry) per corner, clockwise from the top-left.
        corners[0] = a1; corners[1] = a1;
        corners[2] = a2; corners[3] = a2;
        corners[4] = a3; corners[5] = a3;
        corners[6] = a4; corners[7] = a4;
        rect4.addRoundRect(new RectF(x, y, x + w, y + h), corners, Path.Direction.CW);
        c.drawPath(rect4, fill);
    }

    /** Translate, then scale and rotate about (ox, oy). Composes with the
     *  enclosing clip and unwinds with the enclosing restore. */
    void jniXform(Canvas c, int dx, int dy, int scale, int rot, int ox, int oy) {
        // dx, dy are 1/16 dp and scale permille.
        c.translate(ox + dx / 16f, oy + dy / 16f);
        if (rot != 0) c.rotate(rot);
        if (scale != 1000) c.scale(scale / 1000f, scale / 1000f);
        c.translate(-ox, -oy);
    }

    void jniSave(Canvas c) {
        c.save();
    }

    void jniAlpha(Canvas c, int a) {
        int alpha = a < 0 ? 0 : (a > 255 ? 255 : a);
        c.saveLayerAlpha(null, alpha);
    }

    void jniClip(Canvas c, int x, int y, int w, int h, int radius) {
        if (radius > 0) {
            float rad = radius;
            if (rad > w / 2f) rad = w / 2f;
            if (rad > h / 2f) rad = h / 2f;
            clipPath.reset();
            clipPath.addRoundRect(new RectF(x, y, x + w, y + h), rad, rad, Path.Direction.CW);
            c.clipPath(clipPath);
            return;
        }
        c.clipRect(x, y, x + w, y + h);
    }

    void jniRestore(Canvas c) {
        c.restore();
    }

    /** A vector path the engine parsed (icons, charts, `Svg`): commands 0 move
     *  x y / 1 line x y / 2 cubic x1 y1 x2 y2 x y / 3 close, in 1/64 px.
     *  `width` 0 fills (non-zero winding); otherwise a stroke `width`/64 wide,
     *  `style` = cap + 4 * join. Its own Paint, so a path's cap and join never
     *  leak into `jniStroke`'s rectangles. */
    void jniPath(Canvas c, int[] cmds, int rgb, int alpha, int width, int style) {
        Path p = pathBuf;
        p.reset();
        final float u = 1f / 64f;
        int n = cmds.length;
        int i = 0;
        while (i < n) {
            int k = cmds[i++];
            if (k == 0 && i + 2 <= n) {
                p.moveTo(cmds[i] * u, cmds[i + 1] * u);
                i += 2;
            } else if (k == 1 && i + 2 <= n) {
                p.lineTo(cmds[i] * u, cmds[i + 1] * u);
                i += 2;
            } else if (k == 2 && i + 6 <= n) {
                p.cubicTo(cmds[i] * u, cmds[i + 1] * u, cmds[i + 2] * u, cmds[i + 3] * u,
                          cmds[i + 4] * u, cmds[i + 5] * u);
                i += 6;
            } else if (k == 3) {
                p.close();
            } else {
                break;
            }
        }
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        Paint q = pathPaint;
        q.setColor(((a & 0xFF) << 24) | (rgb & 0xFFFFFF));
        if (width == 0) {
            q.setStyle(Paint.Style.FILL);
        } else {
            int cap = style & 3;
            int join = style >> 2;
            q.setStyle(Paint.Style.STROKE);
            q.setStrokeWidth(width * u);
            q.setStrokeCap(cap == 1 ? Paint.Cap.ROUND : cap == 2 ? Paint.Cap.SQUARE : Paint.Cap.BUTT);
            q.setStrokeJoin(join == 1 ? Paint.Join.ROUND
                            : join == 2 ? Paint.Join.BEVEL : Paint.Join.MITER);
        }
        c.drawPath(p, q);
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
}
