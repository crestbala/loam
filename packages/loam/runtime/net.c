/* net.c — the C under `std/net.loam` (`loam_net_*`).
 *
 * TCP and TLS are Loam over raw bindings (R5). What is left: the wasm
 * browser bridges (fetch slots, WebSocket, streams), whose XHRs complete in
 * JS and land per frame. Native builds answer them as stubs.
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
