/* net_rt.h — declarations for `std/net.loam` (`loam_net_*`). */
#ifndef NET_RT_H
#define NET_RT_H

#ifdef LOAM_RT_H
#else
#include "loam_rt.h"
#endif

/* `std:net`'s one C helper (native): a string's byte address. */
int64_t loam_net_string_bytes_at(loam_str s, int64_t off);
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
