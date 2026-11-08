// SPDX-License-Identifier: GPL-2.0-or-later
/* Tiny router: ARP, DHCP, DNS forwarding and NAT through Windows sockets. */
#ifndef NET_H
#define NET_H
#include "common.h"

void net_init(const u8 gw_mac[6]);
void net_fill_fdsets(fd_set *r, fd_set *w, fd_set *e);
void net_handle_fdsets(fd_set *r, fd_set *w, fd_set *e);
void net_tick(u64 now);
void net_shutdown(void);
/* Address helpers (host byte order). */
u32 net_gateway_ip(void);
int net_lease_for(const u8 mac[6], u32 *ip);
int net_conn_count(void);

#endif
