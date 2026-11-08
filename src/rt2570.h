// SPDX-License-Identifier: GPL-2.0-or-later
/* User-space driver for the Ralink RT2570 chip inside the connector.
 * Register sequences are ported from the Linux rt2500usb driver
 * (GPL-2.0-or-later, Ivo van Doorn / rt2x00 project). */
#ifndef RT2570_H
#define RT2570_H
#include "wfc_usb.h"

typedef struct {
    usbdev_t usb;
    u16 eeprom[0x37];
    u8 mac[6];
    int rf_type;
    int rev;
    int ant_tx, ant_rx;
    u32 rf_cache[5];
    int channel;
    u8 txpower[14];
    u16 seq;
    long tx_ok, tx_fail;
    /* Software beaconing (the hardware beacon engine does not fire on this unit). */
    CRITICAL_SECTION bcnlock;
    u8 bcn[1024];
    int bcn_len;
    HANDLE bcn_thread;
    volatile LONG bcn_stop;
    volatile LONG bcn_sent;
    u16 led_csr19, led_csr20;   /* register values before any light setting */
    CRITICAL_SECTION txlock;
} rt_t;

/* TX rates in units of 100 kbit/s (CCK only: 10, 20, 55, 110). */
enum { RATE_1M = 10, RATE_2M = 20, RATE_5M5 = 55, RATE_11M = 110 };

int rt_init(rt_t *rt, char *err, size_t errlen);
int rt_start_ap(rt_t *rt, int channel, int txpower_override);
void rt_stop(rt_t *rt);
/* Sends one 802.11 frame (no FCS). need_ack: unicast frames.
 * timestamp: beacon/probe response (hardware inserts TSF). */
int rt_tx(rt_t *rt, const u8 *frame, int len, int rate, int need_ack, int timestamp);
/* Loads a beacon frame into the hardware beacon engine. */
int rt_set_beacon(rt_t *rt, const u8 *frame, int len);
/* Parses one bulk IN transfer. Returns frame length (FCS stripped) or -1. */
int rt_parse_rx(const u8 *buf, int got, const u8 **frame, int *rssi);
const char *rt_rf_name(int rf);
void rt_read_stats(rt_t *rt, u16 out[6]);
extern int g_tx_retry, g_tx_cwmax;   /* unicast retry limit and contention window */
extern int g_beacon_variant;   /* 0 = software beacons (default); 1-4 = hardware engine tests */
int rt_set_beacon_hw(rt_t *rt, const u8 *frame, int len);
int rt_led_count(void);
void rt_led_desc(int idx, char *out, size_t n);
void rt_led_apply(rt_t *rt, int idx);   /* 0 = off, 1..rt_led_count() = candidate */
void rt_stop_beacon(rt_t *rt);

#endif
