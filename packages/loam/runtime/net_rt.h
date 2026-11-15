/* net_rt.h — declarations for `std/net.loam` (`loam_net_*`). */
#ifndef NET_RT_H
#define NET_RT_H

#ifdef LOAM_RT_H
#else
#include "loam_rt.h"
#endif

int64_t loam_net_tls_connect(loam_str host, int64_t port);
int64_t loam_net_tls_nb_connect(loam_str host, int64_t port);
int64_t loam_net_tls_nb_ready(int64_t fd);
/* The pieces of `std:net` that stay C (R5.1): one `send` from an offset into
   a string, and the macOS TLS client's handle ops. */
int64_t loam_net_send_at(int64_t fd, loam_str data, int64_t off, int64_t flags);
int64_t loam_net_tls_handle_send(int64_t fd, loam_str data, int64_t off);
int64_t loam_net_tls_handle_write(int64_t fd, loam_str data);
loam_str loam_net_tls_handle_read(int64_t fd, int64_t max);
void loam_net_tls_handle_close(int64_t fd);
int64_t loam_net_tls_handle_socket(int64_t fd);
int64_t loam_net_tls_handle_handshaking(int64_t fd);
int64_t loam_net_tls_handle_would_block(void);
loam_str loam_net_fetch_rpc(loam_str path, loam_str body);
int64_t loam_net_fetch_issue(loam_str path, loam_str body);
int64_t loam_net_fetch_ready(void);
loam_str loam_net_fetch_take(void);
int64_t loam_net_fetch_start(loam_str path, loam_str body, int64_t sync);
int64_t loam_net_fetch_slot_done(int64_t slot);
int64_t loam_net_fetch_slot_status(int64_t slot);
loam_str loam_net_fetch_slot_take(int64_t slot);
void loam_net_fetch_slot_drop(int64_t slot);
int64_t loam_net_ws_issue(loam_str url);
int64_t loam_net_ws_state(int64_t slot);
int64_t loam_net_ws_count(int64_t slot);
loam_str loam_net_ws_copy(int64_t slot, int64_t max);
void loam_net_ws_close(int64_t slot);
int64_t loam_net_stream_open(loam_str path, loam_str body);
int64_t loam_net_stream_state(int64_t slot);
loam_str loam_net_stream_take(int64_t slot, int64_t max);
void loam_net_stream_close(int64_t slot);

#endif
