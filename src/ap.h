// SPDX-License-Identifier: GPL-2.0-or-later
/* 802.11b access point logic (beacons, join handshake, data, power save). */
#ifndef AP_H
#define AP_H
#include "common.h"

typedef struct {
    void *ctx;
    int (*tx)(void *ctx, const u8 *frame, int len, int rate, int need_ack, int timestamp);
    int (*set_beacon)(void *ctx, const u8 *frame, int len);
} ap_hw_t;

void ap_init(const ap_hw_t *hw, const u8 bssid[6], const char *ssid, int channel,
             const char *allowlist_path);
/* One received 802.11 frame (FCS removed). */
void ap_rx_frame(const u8 *f, int len, int rssi);
/* Sends an Ethernet-style payload to a console (or broadcast). */
void ap_send_eth(const u8 dst[6], const u8 src[6], u16 ethertype, const u8 *payload, int len);
void ap_tick(u64 now);
int ap_station_count(void);
int ap_is_station(const u8 mac[6]);

/* Join approval. */
int ap_pending(u8 mac_out[6]);          /* 1 if a console is waiting for Y/N */
void ap_decide(const u8 mac[6], int allow);
void ap_set_approve_all(int on);
void ap_refresh_beacon(void);
int ap_list(u8 macs[][6], int max);   /* connected consoles */

/* Hooks the network layer implements. */
void net_rx_eth(const u8 src[6], const u8 dst[6], u16 ethertype, const u8 *p, int len);
void net_station_left(const u8 mac[6]);
void net_station_joined(const u8 mac[6]);

#endif
