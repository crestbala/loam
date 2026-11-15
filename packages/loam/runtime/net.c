/* net.c — the C under `std/net.loam` (`loam_net_*`).
 *
 * TCP is Loam since R5.1 (raw libc bindings in `std/net.loam`). What is left:
 *
 * - `send_at`: one `send` from an offset into a string (Loam has no pointer
 *   into a string, and a `#[c]` string argument would copy it).
 * - macOS: the TLS client (SecureTransport, R5.2). Its handle is
 *   `TLS_BASE + slot`, so `std:net` hands any handle at or above `TLS_BASE`
 *   to the `tls_handle_*` ops here and HTTPS runs over the same read path as
 *   HTTP.
 * - wasm: the browser bridges (fetch slots, WebSocket, streams), whose XHRs
 *   complete in JS and land per frame. Native builds answer them as stubs.
 */
#include "net_rt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __wasm32__

__attribute__((import_module("zeus"), import_name("fetch_rpc")))
int32_t zeus_js_fetch_rpc(const char *path, int32_t path_len, const char *body, int32_t body_len,
                         char *out, int32_t out_cap);

/* Async fetch: JS starts an XHR and later calls the `zeus_fetch_done` export
   below with this handle. `out`/`out_cap` is the slot buffer JS writes into.
   `sync` 1 asks for a blocking XHR that completes before this returns (what
   `await` needs: its pump loop never yields to the JS event loop); 0 is a
   real async XHR whose completion lands on a later frame. */
__attribute__((import_module("zeus"), import_name("fetch_rpc_async")))
int32_t zeus_js_fetch_rpc_async(const char *path, int32_t path_len, const char *body,
                               int32_t body_len, char *out, int32_t out_cap, int32_t handle,
                               int32_t sync);

#define ASYNC_FETCH_SLOTS 4
#define ASYNC_FETCH_CAP 65536

/* Browser WebSocket bridge (wasm): the JS loader owns the WebSocket and a
   per-slot queue of inbound messages; wasm polls state/count and copies one
   message out per `ws_copy`. Slot ids are shared with JS. */

__attribute__((import_module("zeus"), import_name("ws_open")))
int32_t zeus_js_ws_open(const char *url, int32_t url_len, int32_t slot);

__attribute__((import_module("zeus"), import_name("ws_state")))
int32_t zeus_js_ws_state(int32_t slot);

__attribute__((import_module("zeus"), import_name("ws_count")))
int32_t zeus_js_ws_count(int32_t slot);

__attribute__((import_module("zeus"), import_name("ws_copy")))
int32_t zeus_js_ws_copy(int32_t slot, char *out, int32_t cap);

__attribute__((import_module("zeus"), import_name("ws_close")))
void zeus_js_ws_close(int32_t slot);

#define WS_SLOTS 4
#define WS_CAP 65536

static char *g_ws_buf[WS_SLOTS];
static int g_ws_used[WS_SLOTS];

int64_t loam_net_ws_issue(loam_str url) {
    int i;
    if (url.len <= 0 || !url.ptr) return -1;
    for (i = 0; i < WS_SLOTS; i++) {
        if (g_ws_used[i]) continue;
        g_ws_buf[i] = (char *)malloc(WS_CAP);
        if (!g_ws_buf[i]) return -1;
        if (zeus_js_ws_open(url.ptr, (int32_t)url.len, (int32_t)i) < 0) {
            free(g_ws_buf[i]);
            g_ws_buf[i] = NULL;
            return -1;
        }
        g_ws_used[i] = 1;
        return (int64_t)i;
    }
    return -1;
}

int64_t loam_net_ws_state(int64_t slot) {
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) return 1;
    return (int64_t)zeus_js_ws_state((int32_t)slot);
}

int64_t loam_net_ws_count(int64_t slot) {
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) return 0;
    return (int64_t)zeus_js_ws_count((int32_t)slot);
}

loam_str loam_net_ws_copy(int64_t slot, int64_t max) {
    int32_t n;
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) {
        return (loam_str){ .ptr = "", .len = 0 };
    }
    if (max < 1) max = 1;
    if (max > WS_CAP - 1) max = WS_CAP - 1;
    n = zeus_js_ws_copy((int32_t)slot, g_ws_buf[slot], (int32_t)max);
    if (n <= 0) return (loam_str){ .ptr = "", .len = 0 };
    return loam_str_copy(g_ws_buf[slot], (int64_t)n);
}

void loam_net_ws_close(int64_t slot) {
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) return;
    zeus_js_ws_close((int32_t)slot);
    free(g_ws_buf[slot]);
    g_ws_buf[slot] = NULL;
    g_ws_used[slot] = 0;
}

/* Streaming gRPC-Web bridge (wasm): JS posts the request and keeps the reply
   open, queueing each chunk that arrives; wasm copies out whatever has come
   in since the last copy, once per frame, and frames the messages itself. */

__attribute__((import_module("zeus"), import_name("rs_open")))
int32_t zeus_js_rs_open(const char *path, int32_t path_len, const char *body, int32_t body_len,
                        int32_t slot);

__attribute__((import_module("zeus"), import_name("rs_state")))
int32_t zeus_js_rs_state(int32_t slot);

__attribute__((import_module("zeus"), import_name("rs_copy")))
int32_t zeus_js_rs_copy(int32_t slot, char *out, int32_t cap);

__attribute__((import_module("zeus"), import_name("rs_close")))
void zeus_js_rs_close(int32_t slot);

#define RS_SLOTS 4
#define RS_CAP 65536

static char *g_rs_buf[RS_SLOTS];
static int g_rs_used[RS_SLOTS];

int64_t loam_net_stream_open(loam_str path, loam_str body) {
    int i;
    if (path.len <= 0 || !path.ptr) return -1;
    for (i = 0; i < RS_SLOTS; i++) {
        if (g_rs_used[i]) continue;
        g_rs_buf[i] = (char *)malloc(RS_CAP);
        if (!g_rs_buf[i]) return -1;
        if (zeus_js_rs_open(path.ptr, (int32_t)path.len, body.ptr ? body.ptr : "",
                            (int32_t)(body.len > 0 ? body.len : 0), (int32_t)i) < 0) {
            free(g_rs_buf[i]);
            g_rs_buf[i] = NULL;
            return -1;
        }
        g_rs_used[i] = 1;
        return (int64_t)i;
    }
    return -1;
}

int64_t loam_net_stream_state(int64_t slot) {
    if (slot < 0 || slot >= RS_SLOTS || !g_rs_used[slot]) return 2;
    return (int64_t)zeus_js_rs_state((int32_t)slot);
}

loam_str loam_net_stream_take(int64_t slot, int64_t max) {
    int32_t n;
    if (slot < 0 || slot >= RS_SLOTS || !g_rs_used[slot]) {
        return (loam_str){ .ptr = "", .len = 0 };
    }
    if (max < 1) max = 1;
    if (max > RS_CAP - 1) max = RS_CAP - 1;
    n = zeus_js_rs_copy((int32_t)slot, g_rs_buf[slot], (int32_t)max);
    if (n <= 0) return (loam_str){ .ptr = "", .len = 0 };
    return loam_str_copy(g_rs_buf[slot], (int64_t)n);
}

void loam_net_stream_close(int64_t slot) {
    if (slot < 0 || slot >= RS_SLOTS || !g_rs_used[slot]) return;
    zeus_js_rs_close((int32_t)slot);
    free(g_rs_buf[slot]);
    g_rs_buf[slot] = NULL;
    g_rs_used[slot] = 0;
}

/* One async fetch slot. `done` flips when JS reports back; `len` is the body
   length (0 on failure) and `status` the HTTP status (0: no response at all).
   An `abandoned` slot (its caller gave up) keeps its buffer until JS is done
   writing into it, then frees itself. */
typedef struct {
    int used;
    int done;
    int abandoned;
    int32_t handle;
    char *buf;
    int32_t len;
    int32_t status;
} AsyncFetch;

static AsyncFetch g_fetches[ASYNC_FETCH_SLOTS];
static int32_t g_fetch_handle;

static void fetch_slot_free(int i) {
    free(g_fetches[i].buf);
    g_fetches[i].buf = NULL;
    g_fetches[i].used = 0;
    g_fetches[i].done = 0;
    g_fetches[i].abandoned = 0;
}

/* `n` >= 0: a 200 reply of `n` bytes. -1: no response (network error).
   <= -1000: HTTP status `-n - 1000`. */
__attribute__((export_name("zeus_fetch_done")))
void zeus_fetch_done(int32_t handle, int32_t n) {
    int i;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++) {
        if (!g_fetches[i].used || g_fetches[i].handle != handle) continue;
        if (g_fetches[i].abandoned) {
            fetch_slot_free(i);
            return;
        }
        g_fetches[i].done = 1;
        if (n >= 0) {
            g_fetches[i].len = n;
            g_fetches[i].status = 200;
        } else {
            g_fetches[i].len = 0;
            g_fetches[i].status = n <= -1000 ? -n - 1000 : 0;
        }
        return;
    }
}

int64_t loam_net_fetch_start(loam_str path, loam_str body, int64_t sync) {
    int i;
    if (path.len <= 0 || !path.ptr) return -1;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++) {
        if (g_fetches[i].used) continue;
        g_fetches[i].buf = (char *)malloc(ASYNC_FETCH_CAP);
        if (!g_fetches[i].buf) return -1;
        g_fetches[i].used = 1;
        g_fetches[i].done = 0;
        g_fetches[i].abandoned = 0;
        g_fetches[i].len = 0;
        g_fetches[i].status = 0;
        g_fetches[i].handle = ++g_fetch_handle;
        if (zeus_js_fetch_rpc_async(path.ptr, (int32_t)path.len,
                                    body.ptr ? body.ptr : "",
                                    (int32_t)(body.len > 0 ? body.len : 0),
                                    g_fetches[i].buf, ASYNC_FETCH_CAP - 1,
                                    g_fetches[i].handle, sync ? 1 : 0) < 0) {
            fetch_slot_free(i);
            return -1;
        }
        return (int64_t)i;
    }
    return -1;
}

int64_t loam_net_fetch_issue(loam_str path, loam_str body) {
    return loam_net_fetch_start(path, body, 1);
}

static int fetch_slot_live(int64_t slot) {
    return slot >= 0 && slot < ASYNC_FETCH_SLOTS && g_fetches[slot].used && !g_fetches[slot].abandoned;
}

int64_t loam_net_fetch_slot_done(int64_t slot) {
    return fetch_slot_live(slot) && g_fetches[slot].done ? 1 : 0;
}

int64_t loam_net_fetch_slot_status(int64_t slot) {
    return fetch_slot_live(slot) && g_fetches[slot].done ? (int64_t)g_fetches[slot].status : 0;
}

/* Hands slot `slot`'s response to Loam as an owned string and frees the
   slot. Empty on failure. */
loam_str loam_net_fetch_slot_take(int64_t slot) {
    if (!fetch_slot_live(slot) || !g_fetches[slot].done) return (loam_str){ .ptr = "", .len = 0 };
    if (g_fetches[slot].len > 0) {
        loam_str r = loam_str_copy(g_fetches[slot].buf, g_fetches[slot].len);
        fetch_slot_free((int)slot);
        return r;
    }
    fetch_slot_free((int)slot);
    return (loam_str){ .ptr = "", .len = 0 };
}

/* The caller gave up (timeout): free now if JS is done, else when it is. */
void loam_net_fetch_slot_drop(int64_t slot) {
    if (!fetch_slot_live(slot)) return;
    if (g_fetches[slot].done) {
        fetch_slot_free((int)slot);
        return;
    }
    g_fetches[slot].abandoned = 1;
}

int64_t loam_net_fetch_ready(void) {
    int i, n = 0;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++)
        if (fetch_slot_live(i) && g_fetches[i].done) n++;
    return (int64_t)n;
}

/* Any one completed response, for callers with a single fetch in flight;
   per-call code uses `fetch_slot_take`. */
loam_str loam_net_fetch_take(void) {
    int i;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++)
        if (fetch_slot_live(i) && g_fetches[i].done) return loam_net_fetch_slot_take(i);
    return (loam_str){ .ptr = "", .len = 0 };
}

/* Browser TLS: the wasm fetch/ws bridges never speak TLS themselves. TCP
   itself is `std:net` Loam, whose wasm variants answer without C. */
int64_t loam_net_tls_connect(loam_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t loam_net_tls_nb_connect(loam_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t loam_net_tls_nb_ready(int64_t fd) {
    (void)fd;
    return -1;
}

loam_str loam_net_fetch_rpc(loam_str path, loam_str body) {
    char *buf;
    int32_t n;
    loam_str out;
    if (path.len <= 0 || !path.ptr) return (loam_str){ .ptr = "", .len = 0 };
    buf = (char *)malloc(65536);
    if (!buf) return (loam_str){ .ptr = "", .len = 0 };
    n = zeus_js_fetch_rpc(path.ptr, (int32_t)path.len, body.ptr ? body.ptr : "",
                          (int32_t)(body.len > 0 ? body.len : 0), buf, 65535);
    if (n < 0) n = 0;
    out = loam_str_copy(buf, (int64_t)n);
    free(buf);
    return out;
}

#else

/* Windows has no socket port yet: `std:net`'s TCP answers -1 there in Loam,
   and only the TLS and bridge stubs below are compiled. */
#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* One `send` of `data[off..]`: the bytes sent, or -1 with `errno` set for
   `std:net` to read. Bounds are Loam's; this only moves the bytes, so a
   string is never copied to reach the kernel. */
int64_t loam_net_send_at(int64_t fd, loam_str data, int64_t off, int64_t flags) {
    if (fd < 0 || off < 0 || off > data.len || !data.ptr) return -1;
    return (int64_t)send((int)fd, data.ptr + off, (size_t)(data.len - off), (int)flags);
}
#endif

#if defined(__APPLE__)
#include <netdb.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecureTransport.h>

/* Every socket is close-on-exec: a child the program spawns must not inherit
   it, or the port stays bound after the program exits. */
static void net_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

/* TLS client (SecureTransport). Each conn is a slot in `g_tls`; its handle
   is TLS_BASE + slot, far above any raw fd, so `std:net` sends a handle at
   or above it here and the HTTP client treats a TLS conn like a socket. */
#define TLS_SLOTS 8
#define TLS_BASE 0x40000000

typedef struct {
    int used;
    int fd;
    SSLContextRef ctx;
    /* 0 = ready (handshake done), 1 = TCP connecting, 2 = TLS handshake. */
    int hs;
} TlsConn;

static TlsConn g_tls[TLS_SLOTS];
/* The latest TLS read or send would have blocked. */
static int g_tls_wouldblock;

#if defined(__clang__)
/* SecureTransport is deprecated in favor of Network.framework, but it is the
   only synchronous TLS API on macOS and matches this blocking-socket design. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif


/* IO callbacks: SecureTransport drives the raw socket itself. The fd rides
   in the connection ref. EAGAIN maps to errSSLWouldBlock so a handshake on a
   non-blocking socket could be polled, but sockets here are blocking. */
static OSStatus tls_io_read(SSLConnectionRef conn, void *buf, size_t *len) {
    int fd = (int)(intptr_t)conn;
    ssize_t n;
    if (!buf || !len || *len == 0) return errSSLIllegalParam;
    for (;;) {
        n = recv(fd, buf, *len, 0);
        if (n > 0) {
            *len = (size_t)n;
            return noErr;
        }
        if (n == 0) {
            /* Clean TCP EOF: report it as a graceful TLS close. */
            *len = 0;
            return errSSLClosedGraceful;
        }
        if (errno == EINTR) continue;
        *len = 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        if (errno == ECONNRESET || errno == EPIPE) return errSSLClosedAbort;
        return errSSLInternal;
    }
}

static OSStatus tls_io_write(SSLConnectionRef conn, const void *buf, size_t *len) {
    int fd = (int)(intptr_t)conn;
    ssize_t n;
    if (!buf || !len || *len == 0) return errSSLIllegalParam;
    for (;;) {
#if defined(MSG_NOSIGNAL)
        n = send(fd, buf, *len, MSG_NOSIGNAL);
#else
        n = send(fd, buf, *len, 0);
#endif
        if (n >= 0) {
            *len = (size_t)n;
            return noErr;
        }
        if (errno == EINTR) continue;
        *len = 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        if (errno == ECONNRESET || errno == EPIPE) return errSSLClosedAbort;
        return errSSLInternal;
    }
}

static int tls_slot_of(int64_t fd) {
    int64_t s = fd - TLS_BASE;
    if (fd < TLS_BASE || s < 0 || s >= TLS_SLOTS) return -1;
    return g_tls[s].used ? (int)s : -1;
}

/* Resolve `name` (DNS or an address literal) and open one blocking TCP
   connection. Tries every address getaddrinfo returns. */
static int tls_connect_tcp(const char *name, uint16_t port) {
    struct addrinfo hints, *res = NULL, *ai;
    char sp[8];
    int fd = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(sp, sizeof sp, "%u", (unsigned)port);
    if (getaddrinfo(name, sp, &hints, &res) != 0) return -1;
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
        int f = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (f < 0) continue;
        net_cloexec(f);
        {
            int yes = 1;
#ifdef SO_NOSIGPIPE
            (void)setsockopt(f, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#else
            (void)yes;
#endif
            /* Bound a silent peer (never speaks TLS) instead of blocking the
               caller forever. Well-behaved servers answer far sooner. */
            {
                struct timeval tv;
                tv.tv_sec = 10;
                tv.tv_usec = 0;
                (void)setsockopt(f, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                (void)setsockopt(f, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            }
        }
        if (connect(f, ai->ai_addr, ai->ai_addrlen) == 0)
            fd = f;
        else
            close(f);
    }
    freeaddrinfo(res);
    return fd;
}

int64_t loam_net_tls_connect(loam_str host, int64_t port) {
    char name[256];
    int fd = -1, i;
    SSLContextRef ctx = NULL;
    OSStatus st = noErr;
    if (port <= 0 || port > 65535 || host.len <= 0 || host.len >= 256 || !host.ptr)
        return -1;
    memcpy(name, host.ptr, (size_t)host.len);
    name[host.len] = '\0';
    for (i = 0; i < TLS_SLOTS; i++)
        if (!g_tls[i].used) break;
    if (i >= TLS_SLOTS) return -1;
    fd = tls_connect_tcp(name, (uint16_t)port);
    if (fd < 0) return -1;
    ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!ctx) {
        close(fd);
        return -1;
    }
    SSLSetIOFuncs(ctx, tls_io_read, tls_io_write);
    SSLSetConnection(ctx, (SSLConnectionRef)(intptr_t)fd);
    /* SNI, and the server identity check verifies this name against the
       certificate. SecureTransport validates chain + name by default. */
    SSLSetPeerDomainName(ctx, name, strlen(name));
    SSLSetProtocolVersionMin(ctx, kTLSProtocol12);
    g_tls[i].used = 1;
    g_tls[i].fd = fd;
    g_tls[i].ctx = ctx;
    g_tls[i].hs = 0;
    {
        int idle = 0;
        for (;;) {
            st = SSLHandshake(ctx);
            if (st != errSSLWouldBlock) break;
            {
                struct pollfd p;
                int rc;
                p.fd = fd;
                p.events = POLLIN | POLLOUT;
                p.revents = 0;
                rc = poll(&p, 1, 50);
                if (rc <= 0) {
                    /* The peer went idle after a would-block: give up before
                       the read timeout retries pile up. */
                    idle++;
                    if (idle >= 3) {
                        st = errSSLInternal;
                        break;
                    }
                } else {
                    idle = 0;
                }
            }
        }
    }
    if (st != noErr) {
        g_tls[i].used = 0;
        CFRelease(ctx);
        close(fd);
        return -1;
    }
    return TLS_BASE + (int64_t)i;
}

/* Non-blocking TLS: TCP connect + SSLHandshake polled per frame so the UI
   thread never waits on a socket. `hs` 1 = TCP connecting, 2 = handshake. */
int64_t loam_net_tls_nb_connect(loam_str host, int64_t port) {
    char name[256];
    int fd = -1, i, flags, rc;
    SSLContextRef ctx = NULL;
    struct addrinfo hints, *res = NULL, *ai;
    char sp[8];
    if (port <= 0 || port > 65535 || host.len <= 0 || host.len >= 256 || !host.ptr)
        return -1;
    memcpy(name, host.ptr, (size_t)host.len);
    name[host.len] = '\0';
    for (i = 0; i < TLS_SLOTS; i++)
        if (!g_tls[i].used) break;
    if (i >= TLS_SLOTS) return -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(sp, sizeof sp, "%u", (unsigned)port);
    if (getaddrinfo(name, sp, &hints, &res) != 0) return -1;
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
        int f = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (f < 0) continue;
        net_cloexec(f);
        flags = fcntl(f, F_GETFL, 0);
        if (flags < 0 || fcntl(f, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(f);
            continue;
        }
        {
            int yes = 1;
#ifdef SO_NOSIGPIPE
            (void)setsockopt(f, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
        }
        rc = connect(f, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS && errno != EINTR && errno != EALREADY) {
            close(f);
            continue;
        }
        fd = f;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!ctx) {
        close(fd);
        return -1;
    }
    SSLSetIOFuncs(ctx, tls_io_read, tls_io_write);
    SSLSetConnection(ctx, (SSLConnectionRef)(intptr_t)fd);
    SSLSetPeerDomainName(ctx, name, strlen(name));
    SSLSetProtocolVersionMin(ctx, kTLSProtocol12);
    g_tls[i].used = 1;
    g_tls[i].fd = fd;
    g_tls[i].ctx = ctx;
    g_tls[i].hs = 1;
    return TLS_BASE + (int64_t)i;
}

int64_t loam_net_tls_nb_ready(int64_t fd) {
    int slot = tls_slot_of(fd);
    OSStatus st;
    if (slot < 0) return -1;
    if (g_tls[slot].hs == 0) return 1;
    if (g_tls[slot].hs == 1) {
        struct pollfd p;
        int err = 0;
        socklen_t n = sizeof err;
        p.fd = g_tls[slot].fd;
        p.events = POLLOUT;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0) return 0;
        if (getsockopt(g_tls[slot].fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0 || err != 0) {
            g_tls[slot].used = 0;
            CFRelease(g_tls[slot].ctx);
            close(g_tls[slot].fd);
            g_tls[slot].ctx = NULL;
            g_tls[slot].fd = -1;
            return -1;
        }
        g_tls[slot].hs = 2;
    }
    {
        struct pollfd p;
        p.fd = g_tls[slot].fd;
        p.events = POLLIN | POLLOUT;
        p.revents = 0;
        (void)poll(&p, 1, 0);
    }
    st = SSLHandshake(g_tls[slot].ctx);
    if (st == noErr) {
        g_tls[slot].hs = 0;
        return 1;
    }
    if (st == errSSLWouldBlock) return 0;
    g_tls[slot].used = 0;
    CFRelease(g_tls[slot].ctx);
    if (g_tls[slot].fd >= 0) close(g_tls[slot].fd);
    g_tls[slot].ctx = NULL;
    g_tls[slot].fd = -1;
    return -1;
}

/* --- TLS handle ops for `std:net` (only ever called with a handle) --- */

int64_t loam_net_tls_handle_write(int64_t fd, loam_str data) {
    int slot = tls_slot_of(fd);
    size_t len, off = 0;
    if (slot < 0) return -1;
    len = data.len > 0 ? (size_t)data.len : 0;
    if (len == 0) return 0;
    if (!data.ptr) return -1;
    while (off < len) {
        size_t n = 0;
        OSStatus st = SSLWrite(g_tls[slot].ctx, data.ptr + off, len - off, &n);
        if (st != 0 || n == 0) return -1;
        off += n;
    }
    return (int64_t)off;
}

int64_t loam_net_tls_handle_send(int64_t fd, loam_str data, int64_t off) {
    int slot = tls_slot_of(fd);
    size_t got = 0;
    OSStatus st;
    g_tls_wouldblock = 0;
    if (slot < 0 || off < 0 || off > data.len || !data.ptr) return -1;
    if (off == data.len) return 0;
    st = SSLWrite(g_tls[slot].ctx, data.ptr + off, (size_t)(data.len - off), &got);
    if (st == errSSLWouldBlock) {
        g_tls_wouldblock = 1;
        return 0;
    }
    if (st != noErr) return -1;
    return (int64_t)got;
}

loam_str loam_net_tls_handle_read(int64_t fd, int64_t max) {
    int slot = tls_slot_of(fd);
    size_t got = 0;
    OSStatus st;
    char *p;
    g_tls_wouldblock = 0;
    if (slot < 0 || max <= 0) return (loam_str){ .ptr = "", .len = 0 };
    p = (char *)malloc((size_t)max + 1);
    if (!p) return (loam_str){ .ptr = "", .len = 0 };
    st = SSLRead(g_tls[slot].ctx, p, (size_t)max, &got);
    if (st == errSSLWouldBlock) g_tls_wouldblock = 1;
    if (st == errSSLWouldBlock || got == 0) {
        free(p);
        return (loam_str){ .ptr = "", .len = 0 };
    }
    {
        loam_str r = loam_str_copy(p, (int64_t)got);
        free(p);
        return r;
    }
}

void loam_net_tls_handle_close(int64_t fd) {
    int slot = tls_slot_of(fd);
    SSLContextRef ctx;
    int rfd;
    if (slot < 0) return;
    ctx = g_tls[slot].ctx;
    rfd = g_tls[slot].fd;
    (void)SSLClose(ctx); /* best-effort close_notify */
    g_tls[slot].used = 0;
    CFRelease(ctx);
    if (rfd >= 0) close(rfd);
}

int64_t loam_net_tls_handle_socket(int64_t fd) {
    int slot = tls_slot_of(fd);
    return slot < 0 ? -1 : (int64_t)g_tls[slot].fd;
}

int64_t loam_net_tls_handle_handshaking(int64_t fd) {
    int slot = tls_slot_of(fd);
    return slot < 0 ? 1 : (int64_t)(g_tls[slot].hs != 0);
}

int64_t loam_net_tls_handle_would_block(void) { return g_tls_wouldblock; }

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#else

/* No TLS library bound on this host: TLS is unavailable. */
int64_t loam_net_tls_connect(loam_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t loam_net_tls_nb_connect(loam_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t loam_net_tls_nb_ready(int64_t fd) {
    (void)fd;
    return -1;
}

#endif

/* Async fetch is wasm-only (browser XHR). Native: issue fails, nothing is
   ever ready, take returns empty. */
int64_t loam_net_fetch_issue(loam_str path, loam_str body) {
    (void)path;
    (void)body;
    return -1;
}

int64_t loam_net_fetch_ready(void) { return 0; }

loam_str loam_net_fetch_take(void) {
    return (loam_str){ .ptr = "", .len = 0 };
}

int64_t loam_net_fetch_start(loam_str path, loam_str body, int64_t sync) {
    (void)path;
    (void)body;
    (void)sync;
    return -1;
}

int64_t loam_net_fetch_slot_done(int64_t slot) {
    (void)slot;
    return 0;
}

int64_t loam_net_fetch_slot_status(int64_t slot) {
    (void)slot;
    return 0;
}

loam_str loam_net_fetch_slot_take(int64_t slot) {
    (void)slot;
    return (loam_str){ .ptr = "", .len = 0 };
}

void loam_net_fetch_slot_drop(int64_t slot) {
    (void)slot;
}

/* Browser WebSocket bridge is wasm-only. */
int64_t loam_net_ws_issue(loam_str url) {
    (void)url;
    return -1;
}

int64_t loam_net_ws_state(int64_t slot) {
    (void)slot;
    return 1;
}

int64_t loam_net_ws_count(int64_t slot) {
    (void)slot;
    return 0;
}

loam_str loam_net_ws_copy(int64_t slot, int64_t max) {
    (void)slot;
    (void)max;
    return (loam_str){ .ptr = "", .len = 0 };
}

void loam_net_ws_close(int64_t slot) { (void)slot; }

/* Native streams use a plain socket (`std:http` `watch`); the bridge is wasm-only. */
int64_t loam_net_stream_open(loam_str path, loam_str body) {
    (void)path;
    (void)body;
    return -1;
}

int64_t loam_net_stream_state(int64_t slot) {
    (void)slot;
    return 2;
}

loam_str loam_net_stream_take(int64_t slot, int64_t max) {
    (void)slot;
    (void)max;
    return (loam_str){ .ptr = "", .len = 0 };
}

void loam_net_stream_close(int64_t slot) { (void)slot; }

loam_str loam_net_fetch_rpc(loam_str path, loam_str body) {
    (void)path;
    (void)body;
    return (loam_str){ .ptr = "", .len = 0 };
}

#endif
