/* net.c — POSIX TCP trampolines for `std/net.loam` (`yuga_net_*`).
 *
 * Blocking ops (connect/read/write/…), non-blocking ops for the async
 * transport (`tcp_nb_connect` / `tcp_poll` / `tcp_send` / `tcp_so_error`),
 * and — wasm only — async fetch slots (`fetch_issue` / `fetch_ready` /
 * `fetch_take`) whose XHR completes in JS and lands per frame.
 *
 * macOS also gets a blocking TLS client (`yuga_net_tls_connect`,
 * SecureTransport): the returned handle drives `tcp_write` / `tcp_read` /
 * `tcp_close` exactly like a plain socket, so `packages/http/std/http.loam` runs an HTTPS
 * request over the same read path as HTTP. Wasm keeps browser TLS (nothing
 * to add); other hosts return -1.
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
   below with this handle. `out`/`out_cap` is the slot buffer JS writes into. */
__attribute__((import_module("zeus"), import_name("fetch_rpc_async")))
int32_t zeus_js_fetch_rpc_async(const char *path, int32_t path_len, const char *body,
                               int32_t body_len, char *out, int32_t out_cap, int32_t handle);

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

int64_t yuga_net_ws_issue(yuga_str url) {
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

int64_t yuga_net_ws_state(int64_t slot) {
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) return 1;
    return (int64_t)zeus_js_ws_state((int32_t)slot);
}

int64_t yuga_net_ws_count(int64_t slot) {
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) return 0;
    return (int64_t)zeus_js_ws_count((int32_t)slot);
}

yuga_str yuga_net_ws_copy(int64_t slot, int64_t max) {
    char *p;
    int32_t n;
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) {
        return (yuga_str){ .ptr = "", .len = 0 };
    }
    if (max < 1) max = 1;
    if (max > WS_CAP - 1) max = WS_CAP - 1;
    n = zeus_js_ws_copy((int32_t)slot, g_ws_buf[slot], (int32_t)max);
    if (n <= 0) return (yuga_str){ .ptr = "", .len = 0 };
    p = (char *)malloc((size_t)n + 1);
    if (!p) return (yuga_str){ .ptr = "", .len = 0 };
    memcpy(p, g_ws_buf[slot], (size_t)n);
    p[n] = 0;
    return (yuga_str){ .ptr = p, .len = (int64_t)n };
}

void yuga_net_ws_close(int64_t slot) {
    if (slot < 0 || slot >= WS_SLOTS || !g_ws_used[slot]) return;
    zeus_js_ws_close((int32_t)slot);
    free(g_ws_buf[slot]);
    g_ws_buf[slot] = NULL;
    g_ws_used[slot] = 0;
}

typedef struct {
    int used;
    int32_t handle;
    char *buf;
    int32_t len;
} AsyncFetch;

static AsyncFetch g_fetches[ASYNC_FETCH_SLOTS];
static int32_t g_fetch_handle;

__attribute__((export_name("zeus_fetch_done")))
void zeus_fetch_done(int32_t handle, int32_t n) {
    int i;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++) {
        if (g_fetches[i].used && g_fetches[i].handle == handle) {
            g_fetches[i].len = n;
            return;
        }
    }
}

int64_t yuga_net_fetch_issue(yuga_str path, yuga_str body) {
    int i;
    if (path.len <= 0 || !path.ptr) return -1;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++) {
        if (g_fetches[i].used) continue;
        g_fetches[i].buf = (char *)malloc(ASYNC_FETCH_CAP);
        if (!g_fetches[i].buf) return -1;
        g_fetches[i].used = 1;
        g_fetches[i].len = -1;
        g_fetches[i].handle = ++g_fetch_handle;
        if (zeus_js_fetch_rpc_async(path.ptr, (int32_t)path.len,
                                    body.ptr ? body.ptr : "",
                                    (int32_t)(body.len > 0 ? body.len : 0),
                                    g_fetches[i].buf, ASYNC_FETCH_CAP - 1,
                                    g_fetches[i].handle) < 0) {
            free(g_fetches[i].buf);
            g_fetches[i].buf = NULL;
            g_fetches[i].used = 0;
            return -1;
        }
        return (int64_t)i;
    }
    return -1;
}

int64_t yuga_net_fetch_ready(void) {
    int i, n = 0;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++)
        if (g_fetches[i].used && g_fetches[i].len >= 0) n++;
    return (int64_t)n;
}

/* Hands one completed response to Yuga (ownership of the slot buffer moves;
   Yuga frees it when the string drops). Errors come back empty. */
yuga_str yuga_net_fetch_take(void) {
    int i;
    for (i = 0; i < ASYNC_FETCH_SLOTS; i++) {
        if (!g_fetches[i].used || g_fetches[i].len < 0) continue;
        g_fetches[i].used = 0;
        if (g_fetches[i].len > 0) {
            g_fetches[i].buf[g_fetches[i].len] = 0;
            return (yuga_str){ .ptr = g_fetches[i].buf, .len = g_fetches[i].len };
        }
        free(g_fetches[i].buf);
        g_fetches[i].buf = NULL;
        return (yuga_str){ .ptr = "", .len = 0 };
    }
    return (yuga_str){ .ptr = "", .len = 0 };
}

int64_t yuga_net_tcp_connect(yuga_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

/* Browser TLS: the wasm fetch/ws bridges never speak TLS themselves. */
int64_t yuga_net_tls_connect(yuga_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t yuga_net_tls_nb_connect(yuga_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t yuga_net_tls_nb_ready(int64_t fd) {
    (void)fd;
    return -1;
}

int64_t yuga_net_tcp_wouldblock(void) { return 0; }

int64_t yuga_net_tcp_nb_connect(yuga_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t yuga_net_tcp_poll(int64_t fd, int64_t want, int64_t ms) {
    (void)fd;
    (void)want;
    (void)ms;
    return -1;
}

int64_t yuga_net_tcp_send(int64_t fd, yuga_str data, int64_t off) {
    (void)fd;
    (void)data;
    (void)off;
    return -1;
}

int64_t yuga_net_tcp_so_error(int64_t fd) {
    (void)fd;
    return 1;
}

int64_t yuga_net_tcp_write(int64_t fd, yuga_str data) {
    (void)fd;
    (void)data;
    return -1;
}

yuga_str yuga_net_tcp_read(int64_t fd, int64_t max) {
    (void)fd;
    (void)max;
    return (yuga_str){ .ptr = "", .len = 0 };
}

void yuga_net_tcp_close(int64_t fd) { (void)fd; }

int64_t yuga_net_tcp_listen(int64_t port) {
    (void)port;
    return -1;
}

int64_t yuga_net_tcp_accept(int64_t fd) {
    (void)fd;
    return -1;
}

int64_t yuga_net_tcp_bound_port(int64_t fd) {
    (void)fd;
    return 0;
}

yuga_str yuga_net_tcp_peek(int64_t fd, int64_t max) {
    (void)fd;
    (void)max;
    return (yuga_str){ .ptr = "", .len = 0 };
}

yuga_str yuga_net_fetch_rpc(yuga_str path, yuga_str body) {
    char *buf;
    int32_t n;
    yuga_str out;
    if (path.len <= 0 || !path.ptr) return (yuga_str){ .ptr = "", .len = 0 };
    buf = (char *)malloc(65536);
    if (!buf) return (yuga_str){ .ptr = "", .len = 0 };
    n = zeus_js_fetch_rpc(path.ptr, (int32_t)path.len, body.ptr ? body.ptr : "",
                          (int32_t)(body.len > 0 ? body.len : 0), buf, 65535);
    if (n < 0) n = 0;
    out.ptr = buf;
    out.len = (int64_t)n;
    buf[n] = 0;
    return out;
}

#else

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#if defined(__APPLE__)
#include <netdb.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecureTransport.h>

/* Blocking TLS client (SecureTransport). Each conn is a slot in `g_tls`; its
   handle is TLS_BASE + slot, far above any raw fd, so `tcp_write` / `tcp_read`
   / `tcp_close` can dispatch on it and the Yuga HTTP client treats a TLS conn
   exactly like a plain socket. */
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
static int tls_slot_of(int64_t fd);
static int g_tls_wouldblock;
#endif

#if defined(__APPLE__) && defined(__clang__)
/* SecureTransport is deprecated in favor of Network.framework, but it is the
   only synchronous TLS API on macOS and matches this blocking-socket design. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

int64_t yuga_net_tcp_connect(yuga_str host, int64_t port) {
    char name[256];
    int fd;
    struct sockaddr_in sa;
    if (port <= 0 || port > 65535 || host.len <= 0 || host.len >= 256 || !host.ptr)
        return -1;
    memcpy(name, host.ptr, (size_t)host.len);
    name[host.len] = '\0';
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, name, &sa.sin_addr) != 1) {
        if (strcmp(name, "localhost") == 0)
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else {
            close(fd);
            return -1;
        }
    }
    int yes = 1;
#ifdef SO_NOSIGPIPE
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#else
    (void)yes;
#endif
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return (int64_t)fd;
}

int64_t yuga_net_tcp_write(int64_t fd, yuga_str data) {
    size_t len;
    size_t off;
    if (fd < 0) return -1;
#if defined(__APPLE__)
    {
        int slot = tls_slot_of(fd);
        if (slot >= 0) {
            len = data.len > 0 ? (size_t)data.len : 0;
            if (len == 0) return 0;
            if (!data.ptr) return -1;
            off = 0;
            while (off < len) {
                size_t n = 0;
                OSStatus st = SSLWrite(g_tls[slot].ctx, data.ptr + off, len - off, &n);
                if (st != 0 || n == 0) return -1;
                off += n;
            }
            return (int64_t)off;
        }
    }
#endif
    len = data.len > 0 ? (size_t)data.len : 0;
    if (len == 0) return 0;
    if (!data.ptr) return -1;
    off = 0;
    while (off < len) {
        ssize_t n;
#if defined(MSG_NOSIGNAL)
        n = send((int)fd, data.ptr + off, len - off, MSG_NOSIGNAL);
#else
        n = write((int)fd, data.ptr + off, len - off);
#endif
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (int64_t)off;
}

yuga_str yuga_net_tcp_read(int64_t fd, int64_t max) {
    char *p;
    if (fd < 0 || max <= 0) return (yuga_str){ .ptr = "", .len = 0 };
    if (max > 65536) max = 65536;
#if defined(__APPLE__)
    {
        int slot = tls_slot_of(fd);
        if (slot >= 0) {
            size_t got = 0;
            OSStatus st;
            p = (char *)malloc((size_t)max + 1);
            if (!p) return (yuga_str){ .ptr = "", .len = 0 };
            g_tls_wouldblock = 0;
            st = SSLRead(g_tls[slot].ctx, p, (size_t)max, &got);
            if (st == errSSLWouldBlock) {
                g_tls_wouldblock = 1;
                free(p);
                return (yuga_str){ .ptr = "", .len = 0 };
            }
            if (got > 0) {
                p[got] = 0;
                return (yuga_str){ .ptr = p, .len = (int64_t)got };
            }
            free(p);
            return (yuga_str){ .ptr = "", .len = 0 };
        }
    }
#endif
    p = (char *)malloc((size_t)max + 1);
    if (!p) return (yuga_str){ .ptr = "", .len = 0 };
    {
        ssize_t n = read((int)fd, p, (size_t)max);
        if (n <= 0) {
            free(p);
            return (yuga_str){ .ptr = "", .len = 0 };
        }
        p[n] = 0;
        return (yuga_str){ .ptr = p, .len = n };
    }
}

void yuga_net_tcp_close(int64_t fd) {
    if (fd < 0) return;
#if defined(__APPLE__)
    {
        int slot = tls_slot_of(fd);
        if (slot >= 0) {
            SSLContextRef ctx = g_tls[slot].ctx;
            int rfd = g_tls[slot].fd;
            (void)SSLClose(ctx); /* best-effort close_notify */
            g_tls[slot].used = 0;
            CFRelease(ctx);
            if (rfd >= 0) close(rfd);
            return;
        }
    }
#endif
    close((int)fd);
}

int64_t yuga_net_tcp_listen(int64_t port) {
    int fd;
    struct sockaddr_in sa;
    int one = 1;
    uint16_t p = 0;
    if (port > 0 && port < 65536) p = (uint16_t)port;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(p);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return (int64_t)fd;
}

int64_t yuga_net_tcp_accept(int64_t fd) {
    int c;
    struct timeval tv;
    if (fd < 0) return -1;
    c = accept((int)fd, NULL, NULL);
    if (c < 0) return -1;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    {
        int yes = 1;
        (void)setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    }
#endif
    return (int64_t)c;
}

int64_t yuga_net_tcp_bound_port(int64_t fd) {
    struct sockaddr_in sa;
    socklen_t n = sizeof sa;
    if (fd < 0) return 0;
    memset(&sa, 0, sizeof sa);
    if (getsockname((int)fd, (struct sockaddr *)&sa, &n) != 0) return 0;
    return (int64_t)ntohs(sa.sin_port);
}

yuga_str yuga_net_tcp_peek(int64_t fd, int64_t max) {
    char *p;
    ssize_t n;
    if (fd < 0 || max <= 0) return (yuga_str){ .ptr = "", .len = 0 };
    if (max > 65536) max = 65536;
    p = (char *)malloc((size_t)max + 1);
    if (!p) return (yuga_str){ .ptr = "", .len = 0 };
    n = recv((int)fd, p, (size_t)max, MSG_PEEK);
    if (n <= 0) {
        free(p);
        return (yuga_str){ .ptr = "", .len = 0 };
    }
    p[n] = 0;
    return (yuga_str){ .ptr = p, .len = n };
}

#if defined(__APPLE__)

/* --- Blocking TLS client (SecureTransport) --- */

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

int64_t yuga_net_tls_connect(yuga_str host, int64_t port) {
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
int64_t yuga_net_tls_nb_connect(yuga_str host, int64_t port) {
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

int64_t yuga_net_tls_nb_ready(int64_t fd) {
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

int64_t yuga_net_tcp_wouldblock(void) { return g_tls_wouldblock; }

#else

/* No SecureTransport/OpenSSL on this host: TLS is unavailable. */
int64_t yuga_net_tls_connect(yuga_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t yuga_net_tls_nb_connect(yuga_str host, int64_t port) {
    (void)host;
    (void)port;
    return -1;
}

int64_t yuga_net_tls_nb_ready(int64_t fd) {
    (void)fd;
    return -1;
}

int64_t yuga_net_tcp_wouldblock(void) { return 0; }

#endif

/* --- Async transport (UI never blocks): non-blocking connect + poll. --- */

/* Non-blocking connect. Returns a non-blocking fd that is either connected
   already or connecting in the background, or -1 on immediate failure. After
   `tcp_poll(fd, 2, ...)` reports ready, `tcp_so_error(fd)` says whether the
   connect actually succeeded. */
int64_t yuga_net_tcp_nb_connect(yuga_str host, int64_t port) {
    char name[256];
    int fd, rc, flags;
    struct sockaddr_in sa;
    if (port <= 0 || port > 65535 || host.len <= 0 || host.len >= 256 || !host.ptr)
        return -1;
    memcpy(name, host.ptr, (size_t)host.len);
    name[host.len] = '\0';
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, name, &sa.sin_addr) != 1) {
        if (strcmp(name, "localhost") == 0)
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else {
            close(fd);
            return -1;
        }
    }
    {
        int yes = 1;
#ifdef SO_NOSIGPIPE
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#else
        (void)yes;
#endif
    }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR && errno != EALREADY) {
        close(fd);
        return -1;
    }
    return (int64_t)fd;
}

/* Single-fd poll. `want`: 1 = readable, 2 = writable. `ms`: timeout
   (0 = immediate). Returns 1 when ready (including HUP/ERR so the caller can
   observe EOF or a failed connect), 0 on timeout, -1 on error. */
int64_t yuga_net_tcp_poll(int64_t fd, int64_t want, int64_t ms) {
    struct pollfd p;
    int ev, rc;
    if (fd < 0) return -1;
#if defined(__APPLE__)
    {
        int slot = tls_slot_of(fd);
        if (slot >= 0) fd = (int64_t)g_tls[slot].fd;
    }
#endif
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000;
    p.fd = (int)fd;
    p.events = (want == 2) ? POLLOUT : POLLIN;
    p.revents = 0;
    for (;;) {
        rc = poll(&p, 1, (int)ms);
        if (rc >= 0) break;
        if (errno != EINTR) return -1;
    }
    if (rc == 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return 1;
    if (p.revents & POLLHUP) return 1;
    if (p.revents & (POLLIN | POLLOUT)) return 1;
    return 0;
}

/* One non-blocking send of `data[off..]`. Returns bytes written (> 0),
   0 when the socket would block (poll for writable, then resume), -1 on
   error. Offsets past the end are an error; an empty remainder writes 0. */
int64_t yuga_net_tcp_send(int64_t fd, yuga_str data, int64_t off) {
    ssize_t n;
    size_t left;
    if (fd < 0) return -1;
    if (off < 0 || !data.ptr || off > data.len) return -1;
    left = (size_t)(data.len - off);
    if (left == 0) return 0;
#if defined(__APPLE__)
    {
        int slot = tls_slot_of(fd);
        if (slot >= 0) {
            size_t got = 0;
            OSStatus st;
            g_tls_wouldblock = 0;
            st = SSLWrite(g_tls[slot].ctx, data.ptr + off, left, &got);
            if (st == errSSLWouldBlock) {
                g_tls_wouldblock = 1;
                return 0;
            }
            if (st != noErr) return -1;
            return (int64_t)got;
        }
    }
#endif
    for (;;) {
#if defined(MSG_NOSIGNAL)
        n = send((int)fd, data.ptr + off, left, MSG_NOSIGNAL);
#else
        n = send((int)fd, data.ptr + off, left, 0);
#endif
        if (n >= 0) return (int64_t)n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

/* Socket error after a non-blocking connect polled writable: 0 = connected,
   otherwise the errno the connect failed with. */
int64_t yuga_net_tcp_so_error(int64_t fd) {
    int err = 0;
    socklen_t n = sizeof err;
    if (fd < 0) return 1;
#if defined(__APPLE__)
    {
        int slot = tls_slot_of(fd);
        if (slot >= 0) {
            if (g_tls[slot].hs != 0) return 1;
            fd = (int64_t)g_tls[slot].fd;
        }
    }
#endif
    if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0) return 1;
    return (int64_t)err;
}

/* Async fetch is wasm-only (browser XHR). Native: issue fails, nothing is
   ever ready, take returns empty. */
int64_t yuga_net_fetch_issue(yuga_str path, yuga_str body) {
    (void)path;
    (void)body;
    return -1;
}

int64_t yuga_net_fetch_ready(void) { return 0; }

yuga_str yuga_net_fetch_take(void) {
    return (yuga_str){ .ptr = "", .len = 0 };
}

/* Browser WebSocket bridge is wasm-only. */
int64_t yuga_net_ws_issue(yuga_str url) {
    (void)url;
    return -1;
}

int64_t yuga_net_ws_state(int64_t slot) {
    (void)slot;
    return 1;
}

int64_t yuga_net_ws_count(int64_t slot) {
    (void)slot;
    return 0;
}

yuga_str yuga_net_ws_copy(int64_t slot, int64_t max) {
    (void)slot;
    (void)max;
    return (yuga_str){ .ptr = "", .len = 0 };
}

void yuga_net_ws_close(int64_t slot) { (void)slot; }

yuga_str yuga_net_fetch_rpc(yuga_str path, yuga_str body) {
    (void)path;
    (void)body;
    return (yuga_str){ .ptr = "", .len = 0 };
}

#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif
