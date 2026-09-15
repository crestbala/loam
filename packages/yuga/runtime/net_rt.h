/* net_rt.h — declarations for `std/net.loam` (`loam_net_*`). */
#ifndef NET_RT_H
#define NET_RT_H

#ifdef LOAM_RT_H
#else
#include "loam_rt.h"
#endif

int64_t loam_net_tcp_connect(loam_str host, int64_t port);
int64_t loam_net_tls_connect(loam_str host, int64_t port);
int64_t loam_net_tls_nb_connect(loam_str host, int64_t port);
int64_t loam_net_tls_nb_ready(int64_t fd);
int64_t loam_net_tcp_wouldblock(void);
int64_t loam_net_tcp_nb_connect(loam_str host, int64_t port);
int64_t loam_net_tcp_poll(int64_t fd, int64_t want, int64_t ms);
int64_t loam_net_tcp_send(int64_t fd, loam_str data, int64_t off);
int64_t loam_net_tcp_so_error(int64_t fd);
int64_t loam_net_tcp_write(int64_t fd, loam_str data);
loam_str loam_net_tcp_read(int64_t fd, int64_t max);
void loam_net_tcp_close(int64_t fd);
int64_t loam_net_tcp_listen(int64_t port);
int64_t loam_net_tcp_accept(int64_t fd);
int64_t loam_net_tcp_bound_port(int64_t fd);
loam_str loam_net_tcp_peek(int64_t fd, int64_t max);
loam_str loam_net_fetch_rpc(loam_str path, loam_str body);
int64_t loam_net_fetch_issue(loam_str path, loam_str body);
int64_t loam_net_fetch_ready(void);
loam_str loam_net_fetch_take(void);
int64_t loam_net_ws_issue(loam_str url);
int64_t loam_net_ws_state(int64_t slot);
int64_t loam_net_ws_count(int64_t slot);
loam_str loam_net_ws_copy(int64_t slot, int64_t max);
void loam_net_ws_close(int64_t slot);

#endif
