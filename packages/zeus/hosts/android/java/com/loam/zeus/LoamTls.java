package com.loam.zeus;

import android.util.Log;

import java.nio.ByteBuffer;
import java.util.ArrayList;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.SSLEngineResult;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSession;

/**
 * Client TLS for std:net. Loam owns the socket and the poll; this class only
 * turns plaintext into records and back, through the platform SSLEngine and
 * the system trust store. Handles are 1..8. A call copies into the caller's
 * arrays and reports how many bytes it consumed and produced. Bytes that do
 * not fit are staged and the next call drains them before touching the
 * engine. A drain returns 4 and leaves consumed at 0, so the caller retries
 * the same plaintext. 0 is an engine call that succeeded.
 */
public class LoamTls {
    private static final int SLOTS = 8;
    private static final int IO_BYTES = 65536;
    private static final int APP_BYTES = 32768;
    private static final int MAX_BUF = 262144;
    private static final Conn[] table = new Conn[SLOTS];
    private static int takeConsumed;
    private static int takeProduced;
    private static int takeStaged;

    static final class Conn {
        SSLEngine engine;
        ByteBuffer netOut;
        ByteBuffer appOut;
        ByteBuffer restNet;
        ByteBuffer restPlain;
    }

    public static int takeConsumed() { return takeConsumed; }
    public static int takeProduced() { return takeProduced; }
    public static int takeStaged() { return takeStaged; }

    /** A free slot's handle, or -1. TLS 1.2 at least, SNI from the host, and the certificate name checked as HTTPS. */
    public static int open(String host, int port) {
        if (host == null || host.length() == 0 || port <= 0 || port > 65535) return -1;
        int slot = -1;
        for (int i = 0; i < SLOTS; i++) {
            if (table[i] == null) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return -1;
        try {
            SSLEngine engine = SSLContext.getDefault().createSSLEngine(host, port);
            engine.setUseClientMode(true);
            if (!enableModern(engine)) return -1;
            /* Read the parameters after the protocol filter, then write them back with the name check. setSSLParameters replaces the enabled protocols. */
            SSLParameters params = engine.getSSLParameters();
            params.setEndpointIdentificationAlgorithm("HTTPS");
            engine.setSSLParameters(params);
            SSLSession session = engine.getSession();
            int packet = session.getPacketBufferSize();
            int app = session.getApplicationBufferSize();
            if (packet < IO_BYTES) packet = IO_BYTES;
            if (app < APP_BYTES) app = APP_BYTES;
            Conn c = new Conn();
            c.engine = engine;
            c.netOut = ByteBuffer.allocate(packet);
            c.appOut = ByteBuffer.allocate(app);
            table[slot] = c;
            engine.beginHandshake();
            return slot + 1;
        } catch (Exception e) {
            table[slot] = null;
            logFail("open", e);
            return -1;
        }
    }

    /** 0 done, 1 NEED_WRAP, 2 NEED_UNWRAP, 3 NEED_UNWRAP_AGAIN, -1 failed. Delegated tasks run here. */
    public static int status(int handle) {
        Conn c = live(handle);
        if (c == null) return -1;
        try {
            for (int spins = 0; spins < 8; spins++) {
                String name = c.engine.getHandshakeStatus().name();
                if (name.equals("NEED_TASK")) {
                    runTasks(c.engine);
                    continue;
                }
                if (name.equals("NOT_HANDSHAKING") || name.equals("FINISHED")) return 0;
                if (name.equals("NEED_WRAP")) return 1;
                if (name.equals("NEED_UNWRAP")) return 2;
                if (name.equals("NEED_UNWRAP_AGAIN")) return 3;
                return -1;
            }
            return -1;
        } catch (Exception e) {
            logFail("status", e);
            return -1;
        }
    }

    /** 0 ok, 1 the record would not fit, 3 closed, 4 staged bytes only, -1 failed. */
    public static int wrap(int handle, byte[] in, int inLen, byte[] out) {
        resetTakes();
        Conn c = buffers(handle, in, inLen, out);
        if (c == null) return -1;
        try {
            if (c.restNet != null && c.restNet.hasRemaining()) {
                takeProduced = copyRemaining(c.restNet, out);
                if (!c.restNet.hasRemaining()) c.restNet = null;
                takeStaged = remaining(c.restNet);
                return 4;
            }
            ByteBuffer src = ByteBuffer.wrap(in, 0, inLen);
            for (int guard = 0; guard < 4; guard++) {
                c.netOut.clear();
                SSLEngineResult result = c.engine.wrap(src, c.netOut);
                runTasks(c.engine);
                SSLEngineResult.Status s = result.getStatus();
                if (s == SSLEngineResult.Status.BUFFER_OVERFLOW) {
                    ByteBuffer bigger = grow(c.netOut);
                    if (bigger == c.netOut) return 1;
                    c.netOut = bigger;
                    continue;
                }
                takeConsumed = src.position();
                if (s == SSLEngineResult.Status.OK || s == SSLEngineResult.Status.CLOSED) {
                    takeProduced = emit(c, c.netOut, out, true);
                    takeStaged = remaining(c.restNet);
                    return s == SSLEngineResult.Status.CLOSED ? 3 : 0;
                }
                return -1;
            }
            return -1;
        } catch (Exception e) {
            logFail("wrap", e);
            return -1;
        }
    }

    /** 0 ok, 1 the plaintext would not fit, 2 need more ciphertext, 3 closed, 4 staged bytes only, -1 failed. */
    public static int unwrap(int handle, byte[] in, int inLen, byte[] out) {
        resetTakes();
        Conn c = buffers(handle, in, inLen, out);
        if (c == null) return -1;
        try {
            if (c.restPlain != null && c.restPlain.hasRemaining()) {
                takeProduced = copyRemaining(c.restPlain, out);
                if (!c.restPlain.hasRemaining()) c.restPlain = null;
                takeStaged = remaining(c.restPlain);
                return 4;
            }
            ByteBuffer src = ByteBuffer.wrap(in, 0, inLen);
            for (int guard = 0; guard < 4; guard++) {
                c.appOut.clear();
                SSLEngineResult result = c.engine.unwrap(src, c.appOut);
                runTasks(c.engine);
                SSLEngineResult.Status s = result.getStatus();
                if (s == SSLEngineResult.Status.BUFFER_OVERFLOW) {
                    ByteBuffer bigger = grow(c.appOut);
                    if (bigger == c.appOut) return 1;
                    c.appOut = bigger;
                    continue;
                }
                takeConsumed = src.position();
                if (s == SSLEngineResult.Status.BUFFER_UNDERFLOW) return 2;
                if (s == SSLEngineResult.Status.OK || s == SSLEngineResult.Status.CLOSED) {
                    takeProduced = emit(c, c.appOut, out, false);
                    takeStaged = remaining(c.restPlain);
                    return s == SSLEngineResult.Status.CLOSED ? 3 : 0;
                }
                return -1;
            }
            return -1;
        } catch (Exception e) {
            logFail("unwrap", e);
            return -1;
        }
    }

    /** Plaintext staged for the caller. */
    public static int pending(int handle) {
        Conn c = live(handle);
        if (c == null) return 0;
        return remaining(c.restPlain);
    }

    /** Ask for a close_notify. The caller wraps once to produce it. */
    public static int close(int handle) {
        Conn c = live(handle);
        if (c == null) return -1;
        try {
            c.engine.closeOutbound();
            return 0;
        } catch (Exception e) {
            logFail("close", e);
            return -1;
        }
    }

    public static void free(int handle) {
        int i = handle - 1;
        if (i < 0 || i >= SLOTS) return;
        table[i] = null;
    }

    private static void resetTakes() {
        takeConsumed = 0;
        takeProduced = 0;
        takeStaged = 0;
    }

    private static Conn live(int handle) {
        int i = handle - 1;
        if (i < 0 || i >= SLOTS) return null;
        Conn c = table[i];
        if (c == null || c.engine == null) return null;
        return c;
    }

    private static Conn buffers(int handle, byte[] in, int inLen, byte[] out) {
        if (in == null || out == null || inLen < 0 || inLen > in.length) return null;
        return live(handle);
    }

    /** Drop SSLv3, TLS 1.0 and TLS 1.1. An empty keep-list fails the connect. */
    private static boolean enableModern(SSLEngine engine) {
        String[] supported = engine.getSupportedProtocols();
        if (supported == null) return false;
        ArrayList<String> keep = new ArrayList<String>();
        for (int i = 0; i < supported.length; i++) {
            String protocol = supported[i];
            if (protocol == null) continue;
            if (protocol.equals("SSLv2Hello") || protocol.equals("SSLv3")) continue;
            if (protocol.equals("TLSv1") || protocol.equals("TLSv1.1")) continue;
            keep.add(protocol);
        }
        if (keep.isEmpty()) return false;
        String[] enabled = new String[keep.size()];
        for (int i = 0; i < keep.size(); i++) enabled[i] = keep.get(i);
        engine.setEnabledProtocols(enabled);
        return true;
    }

    private static void runTasks(SSLEngine engine) {
        Runnable task = engine.getDelegatedTask();
        while (task != null) {
            task.run();
            task = engine.getDelegatedTask();
        }
    }

    private static ByteBuffer grow(ByteBuffer buf) {
        int next = buf.capacity() * 2;
        if (next > MAX_BUF) next = MAX_BUF;
        if (next <= buf.capacity()) return buf;
        return ByteBuffer.allocate(next);
    }

    private static int copyRemaining(ByteBuffer src, byte[] out) {
        int n = src.remaining();
        if (n > out.length) n = out.length;
        if (n > 0) src.get(out, 0, n);
        return n;
    }

    private static int remaining(ByteBuffer buf) {
        if (buf == null || !buf.hasRemaining()) return 0;
        return buf.remaining();
    }

    /** Copy what fits, and park the tail for the next call. */
    private static int emit(Conn c, ByteBuffer produced, byte[] out, boolean net) {
        produced.flip();
        int copy = produced.remaining();
        if (copy > out.length) copy = out.length;
        if (copy > 0) produced.get(out, 0, copy);
        ByteBuffer rest = null;
        if (produced.hasRemaining()) {
            rest = ByteBuffer.allocate(produced.remaining());
            rest.put(produced);
            rest.flip();
        }
        if (net) c.restNet = rest;
        else c.restPlain = rest;
        return copy;
    }

    private static void logFail(String where, Exception e) {
        String message = e.getMessage();
        if (message == null) message = "";
        Log.e("zeus", "loam tls " + where + ": " + e.getClass().getSimpleName() + ": " + message);
    }
}
