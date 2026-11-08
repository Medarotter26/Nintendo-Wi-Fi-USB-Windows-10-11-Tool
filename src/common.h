// SPDX-License-Identifier: GPL-2.0-or-later
/* Shared definitions for the Windows 11 Nintendo Wi-Fi USB Connector program. */
#ifndef COMMON_H
#define COMMON_H

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Monotonic milliseconds. */
static inline u64 now_ms(void) { return GetTickCount64(); }

/* Logging: level 0 = always, 1 = info, 2 = verbose/debug. */
extern int g_verbose;
void log_msg(int level, const char *fmt, ...);
#define LOG(...)  log_msg(0, __VA_ARGS__)
#define INFO(...) log_msg(1, __VA_ARGS__)
#define DBG(...)  log_msg(2, __VA_ARGS__)

static inline u16 rd16be(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline void wr16be(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr32be(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static inline u16 rd16le(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32le(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline void wr16le(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void wr32le(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

#define MACFMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MACARG(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

/* Runtime configuration (connector.ini). */
typedef struct {
    char ssid[33];
    int channel;
    u32 dns1, dns2;        /* network byte order */
    int approve_all;       /* 1 = let any console join without asking */
    u32 net_base;          /* e.g. 192.168.77.0, host order */
    int tx_power;          /* -1 = EEPROM default, else 0..31 */
    int led;               /* status light method, 0 = off (see rt_led_apply) */
} config_t;

extern config_t g_cfg;

#endif
