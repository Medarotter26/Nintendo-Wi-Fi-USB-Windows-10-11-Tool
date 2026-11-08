// SPDX-License-Identifier: GPL-2.0-or-later
/* Host self-test: a simulated Nintendo DS talks to the access point and
 * router code exactly as over the air (raw 802.11 frames in and out),
 * and reaches the real internet through the NAT. No connector needed. */
#include "common.h"
#include "ap.h"
#include "net.h"
#include "rt2570.h"

int g_verbose = 0;
config_t g_cfg;

void log_msg(int level, const char *fmt, ...)
{
    if (level > 1 && !g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    | ");
    vprintf(fmt, ap);
    va_end(ap);
}

static int fails = 0, passes = 0;
#define CHECK(c, ...) do { if (c) { passes++; printf("  PASS "); } else { fails++; printf("  FAIL "); } \
                           printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- mock radio ---- */
typedef struct { u8 d[2400]; int len, rate, ack, ts; } cap_t;
#define NCAP 8192
static cap_t *caps;
static int ncap, rcap;
static u8 beacon[1024];
static int beacon_len, beacon_updates;
static double drop_rate = 0.0;

static int mock_tx(void *ctx, const u8 *f, int len, int rate, int ack, int ts)
{
    if (ncap >= NCAP) { memmove(caps, caps + rcap, sizeof(cap_t) * (ncap - rcap)); ncap -= rcap; rcap = 0; }
    if (ncap >= NCAP) return -1;
    cap_t *c = &caps[ncap++];
    memcpy(c->d, f, len); c->len = len; c->rate = rate; c->ack = ack; c->ts = ts;
    return 0;
}
static int mock_beacon(void *ctx, const u8 *f, int len)
{
    memcpy(beacon, f, len); beacon_len = len; beacon_updates++;
    return 0;
}

static const u8 BSSID[6] = {0x00, 0x09, 0xbf, 0xaa, 0xbb, 0xcc};
static const u8 DS[6] = {0x00, 0x09, 0xbf, 0x11, 0x22, 0x33};
static const u8 DS2[6] = {0x00, 0x09, 0xbf, 0x44, 0x55, 0x66};
static const u8 BC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static int ds_pm = 0;
static u16 ds_seq = 0;

static void pump_once(int wait_ms)
{
    fd_set r, w, e;
    FD_ZERO(&r); FD_ZERO(&w); FD_ZERO(&e);
    net_fill_fdsets(&r, &w, &e);
    if (r.fd_count + w.fd_count + e.fd_count == 0) Sleep(wait_ms);
    else {
        struct timeval tv = {0, wait_ms * 1000};
        if (select(0, &r, &w, &e, &tv) > 0) net_handle_fdsets(&r, &w, &e);
    }
    u64 t = now_ms();
    net_tick(t);
    ap_tick(t);
}

static void send_mgmt(const u8 *sta, u8 fc0, const u8 *body, int blen)
{
    u8 f[600];
    f[0] = fc0; f[1] = 0; f[2] = f[3] = 0;
    memcpy(f + 4, BSSID, 6); memcpy(f + 10, sta, 6); memcpy(f + 16, BSSID, 6);
    wr16le(f + 22, (u16)(ds_seq++ << 4));
    memcpy(f + 24, body, blen);
    ap_rx_frame(f, 24 + blen, -40);
}

static void send_eth(const u8 *sta, const u8 *dst, u16 type, const u8 *p, int len)
{
    u8 f[2400];
    f[0] = 0x08; f[1] = (u8)(0x01 | (ds_pm ? 0x10 : 0)); f[2] = f[3] = 0;
    memcpy(f + 4, BSSID, 6); memcpy(f + 10, sta, 6); memcpy(f + 16, dst, 6);
    wr16le(f + 22, (u16)(ds_seq++ << 4));
    u8 snap[6] = {0xAA, 0xAA, 0x03, 0, 0, 0};
    memcpy(f + 24, snap, 6);
    wr16be(f + 30, type);
    memcpy(f + 32, p, len);
    ap_rx_frame(f, 32 + len, -40);
}

/* Waits for a captured frame matching fc0 (0xff = any data) addressed to sta. */
static cap_t *wait_frame(const u8 *sta, int fc0, int timeout_ms)
{
    u64 end = now_ms() + timeout_ms;
    for (;;) {
        while (rcap < ncap) {
            cap_t *c = &caps[rcap++];
            if (memcmp(c->d + 4, sta, 6) && !(c->d[4] & 1)) continue;
            if (fc0 == 0xff ? ((c->d[0] & 0x0c) == 0x08) : (c->d[0] == fc0)) return c;
        }
        if (now_ms() > end) return NULL;
        pump_once(10);
    }
}

static u32 csum_ok(const u8 *p, int len, u32 sum)
{
    while (len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (u32)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return sum == 0xffff;
}

static int bad_csums = 0;

/* Next IPv4 packet for sta with given protocol; returns pointer into capture. */
static const u8 *wait_ip(const u8 *sta, int proto, int *len, int timeout_ms)
{
    u64 end = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left < 0) return NULL;
        cap_t *c = wait_frame(sta, 0xff, left);
        if (!c) return NULL;
        if (c->len < 32 + 20 || rd16be(c->d + 30) != 0x0800) continue;
        if ((c->d[0] & 0xf0) == 0x40) continue;  /* null data */
        const u8 *ip = c->d + 32;
        int tot = rd16be(ip + 2);
        if (!csum_ok(ip, 20, 0)) bad_csums++;
        if (ip[9] == 6 || ip[9] == 17) {
            u8 ph[12];
            memcpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = ip[9]; wr16be(ph + 10, (u16)(tot - 20));
            u32 s = 0;
            for (int i = 0; i < 12; i += 2) s += (u32)((ph[i] << 8) | ph[i + 1]);
            if (!csum_ok(ip + 20, tot - 20, s)) bad_csums++;
        }
        if (proto && ip[9] != proto) continue;
        if (drop_rate > 0 && proto == 6 && (rand() % 1000) < (int)(drop_rate * 1000)) continue;
        *len = tot;
        return ip;
    }
}

static u16 ipid = 1;
static u32 ds_ip, gw_ip;

static void send_ip(const u8 *sta, u32 src, u32 dst, u8 proto, const u8 *l4, int l4len)
{
    u8 p[2000];
    p[0] = 0x45; p[1] = 0; wr16be(p + 2, (u16)(20 + l4len)); wr16be(p + 4, ipid++);
    wr16be(p + 6, 0x4000); p[8] = 64; p[9] = proto; p[10] = p[11] = 0;
    wr32be(p + 12, src); wr32be(p + 16, dst);
    u32 s = 0;
    for (int i = 0; i < 20; i += 2) s += (u32)((p[i] << 8) | p[i + 1]);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    wr16be(p + 10, (u16)~s);
    memcpy(p + 20, l4, l4len);
    if (proto == 6 || proto == 17) {
        u8 *seg = p + 20;
        int co = proto == 6 ? 16 : 6;
        seg[co] = seg[co + 1] = 0;
        u32 t = 0;
        u8 ph[12];
        wr32be(ph, src); wr32be(ph + 4, dst); ph[8] = 0; ph[9] = proto; wr16be(ph + 10, (u16)l4len);
        for (int i = 0; i < 12; i += 2) t += (u32)((ph[i] << 8) | ph[i + 1]);
        for (int i = 0; i + 1 < l4len; i += 2) t += (u32)((seg[i] << 8) | seg[i + 1]);
        if (l4len & 1) t += (u32)(seg[l4len - 1] << 8);
        while (t >> 16) t = (t & 0xffff) + (t >> 16);
        wr16be(seg + co, (u16)~t);
    }
    send_eth(sta, BSSID, 0x0800, p, 20 + l4len);
}

static void send_udp(const u8 *sta, u32 src, u16 sp, u32 dst, u16 dp, const u8 *d, int n)
{
    u8 seg[1600];
    wr16be(seg, sp); wr16be(seg + 2, dp); wr16be(seg + 4, (u16)(8 + n)); seg[6] = seg[7] = 0;
    memcpy(seg + 8, d, n);
    send_ip(sta, src, dst, 17, seg, 8 + n);
}

/* ---- DHCP ---- */
static int dhcp(const u8 *sta, u32 *ip_out, u32 *dns_out)
{
    u8 b[300];
    memset(b, 0, sizeof(b));
    b[0] = 1; b[1] = 1; b[2] = 6; wr32be(b + 4, 0x12345678); memcpy(b + 28, sta, 6);
    wr32be(b + 236, 0x63825363);
    int o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = 1; b[o++] = 255;
    send_udp(sta, 0, 68, 0xffffffff, 67, b, 300);
    int len;
    const u8 *ip = wait_ip(sta, 17, &len, 2000);
    if (!ip) return 0;
    const u8 *d = ip + 28;
    u32 offer = rd32be(d + 16);
    o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = 3;
    b[o++] = 50; b[o++] = 4; wr32be(b + o, offer); o += 4;
    b[o++] = 255;
    send_udp(sta, 0, 68, 0xffffffff, 67, b, 300);
    ip = wait_ip(sta, 17, &len, 2000);
    if (!ip) return 0;
    d = ip + 28;
    int dlen = len - 28;
    int type = 0;
    u32 router = 0, dns = 0;
    for (int k = 240; k < dlen && d[k] != 255;) {
        if (d[k] == 0) { k++; continue; }
        if (d[k] == 53) type = d[k + 2];
        if (d[k] == 3) router = rd32be(d + k + 2);
        if (d[k] == 6) dns = rd32be(d + k + 2);
        k += 2 + d[k + 1];
    }
    if (type != 5) return 0;
    *ip_out = rd32be(d + 16);
    *dns_out = dns;
    gw_ip = router;
    return 1;
}

/* ---- DNS ---- */
static u32 dns_lookup(const u8 *sta, u32 server, const char *name, int timeout)
{
    u8 q[300];
    int n = 0;
    u16 id = (u16)(rand() & 0xffff);
    wr16be(q, id); wr16be(q + 2, 0x0100); wr16be(q + 4, 1); wr16be(q + 6, 0); wr16be(q + 8, 0); wr16be(q + 10, 0);
    n = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int l = dot ? (int)(dot - p) : (int)strlen(p);
        q[n++] = (u8)l; memcpy(q + n, p, l); n += l;
        p += l; if (*p == '.') p++;
    }
    q[n++] = 0; wr16be(q + n, 1); wr16be(q + n + 2, 1); n += 4;
    send_udp(sta, ds_ip, 50000 + (id % 1000), server, 53, q, n);
    int len;
    u64 end = now_ms() + timeout;
    while (now_ms() < end) {
        const u8 *ip = wait_ip(sta, 17, &len, (int)(end - now_ms()));
        if (!ip) break;
        if (rd32be(ip + 12) != server || rd16be(ip + 20) != 53) continue;
        const u8 *r = ip + 28;
        int rl = len - 28;
        if (rd16be(r) != id) continue;
        int an = rd16be(r + 6);
        int k = 12;
        while (k < rl && r[k]) k += r[k] + 1;
        k += 5;
        for (int i = 0; i < an && k + 12 <= rl; i++) {
            if ((r[k] & 0xc0) == 0xc0) k += 2; else { while (k < rl && r[k]) k += r[k] + 1; k++; }
            u16 type = rd16be(r + k), rdl = rd16be(r + k + 8);
            if (type == 1 && rdl == 4) return rd32be(r + k + 10);
            k += 10 + rdl;
        }
        return 0;
    }
    return 0;
}

/* ---- client TCP ---- */
typedef struct { u32 rip; u16 lport, rport; u32 snd_nxt, rcv_nxt; u8 *buf; int len, cap; int fin; } ctcp_t;

static void ctcp_seg(ctcp_t *c, u8 flags, const u8 *d, int n, u16 wnd)
{
    u8 s[1600];
    int hl = (flags & 0x02) ? 24 : 20;
    wr16be(s, c->lport); wr16be(s + 2, c->rport); wr32be(s + 4, c->snd_nxt); wr32be(s + 8, c->rcv_nxt);
    s[12] = (u8)((hl / 4) << 4); s[13] = flags; wr16be(s + 14, wnd); s[16] = s[17] = s[18] = s[19] = 0;
    if (hl == 24) { s[20] = 2; s[21] = 4; wr16be(s + 22, 1360); }
    memcpy(s + hl, d, n);
    send_ip(DS, ds_ip, c->rip, 6, s, hl + n);
}

static int http_get(u32 rip, const char *host, const char *path, int wnd, int *body_len, int *status, int timeout)
{
    ctcp_t c = {0};
    c.rip = rip; c.lport = (u16)(40000 + rand() % 20000); c.rport = 80;
    c.snd_nxt = 1000;
    c.cap = 4 << 20; c.buf = malloc(c.cap);
    ctcp_seg(&c, 0x02, NULL, 0, (u16)wnd);
    c.snd_nxt++;
    int len;
    const u8 *ip;
    u64 end = now_ms() + timeout;
    for (;;) {
        ip = wait_ip(DS, 6, &len, 5000);
        if (!ip) { free(c.buf); return -1; }
        const u8 *t = ip + 20;
        if (rd16be(t + 2) != c.lport) continue;
        if ((t[13] & 0x12) == 0x12) { c.rcv_nxt = rd32be(t + 4) + 1; break; }
        if (t[13] & 0x04) { free(c.buf); return -2; }
    }
    ctcp_seg(&c, 0x10, NULL, 0, (u16)wnd);
    char req[512];
    int rn = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: WFC-selftest\r\nConnection: close\r\n\r\n", path, host);
    ctcp_seg(&c, 0x18, (const u8 *)req, rn, (u16)wnd);
    c.snd_nxt += (u32)rn;
    while (now_ms() < end) {
        ip = wait_ip(DS, 6, &len, 8000);
        if (!ip) break;
        const u8 *t = ip + 20;
        if (rd16be(t + 2) != c.lport) continue;
        int hl = (t[12] >> 4) * 4;
        int dl = len - 20 - hl;
        u32 seq = rd32be(t + 4);
        if (t[13] & 0x04) break;
        if (seq == c.rcv_nxt) {
            if (dl > 0 && c.len + dl <= c.cap) { memcpy(c.buf + c.len, t + hl, dl); c.len += dl; c.rcv_nxt += (u32)dl; }
            if (t[13] & 0x01) { c.rcv_nxt++; c.fin = 1; }
        }
        ctcp_seg(&c, 0x10, NULL, 0, (u16)wnd);
        if (c.fin) {
            ctcp_seg(&c, 0x11, NULL, 0, (u16)wnd);
            c.snd_nxt++;
            break;
        }
    }
    *status = 0;
    *body_len = -1;
    if (c.len > 12) sscanf((char *)c.buf + 9, "%d", status);
    for (int i = 0; i + 3 < c.len; i++) {
        if (!memcmp(c.buf + i, "\r\n\r\n", 4)) { *body_len = c.len - i - 4; break; }
    }
    int clen = -1;
    for (int i = 0; i + 16 < c.len && i < 4096; i++)
        if (!_strnicmp((char *)c.buf + i, "\r\nContent-Length:", 17)) { clen = atoi((char *)c.buf + i + 17); break; }
    free(c.buf);
    if (!c.fin) return -3;
    return clen;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-v")) g_verbose = 1;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    caps = calloc(NCAP, sizeof(cap_t));
    memset(&g_cfg, 0, sizeof(g_cfg));
    strcpy(g_cfg.ssid, "WFC-Connector");
    g_cfg.channel = 6;
    g_cfg.dns1 = inet_addr("178.62.43.212");
    g_cfg.net_base = 0xC0A84D00u;
    char allow[MAX_PATH];
    GetTempPathA(sizeof(allow), allow);
    strcat(allow, "wfc_selftest_allow.txt");
    DeleteFileA(allow);

    ap_hw_t hw = { NULL, mock_tx, mock_beacon };
    ap_init(&hw, BSSID, g_cfg.ssid, 6, allow);
    ap_set_approve_all(0);
    net_init(BSSID);

    printf("1. Beacon\n");
    CHECK(beacon_len > 40 && beacon[0] == 0x80, "beacon uploaded (%d bytes)", beacon_len);
    CHECK(beacon_len > 38 + 13 && beacon[36] == 0 && beacon[37] == 13 && !memcmp(beacon + 38, "WFC-Connector", 13),
          "beacon carries SSID");

    printf("2. Probe\n");
    u8 probe[2] = {0, 0};
    send_mgmt(DS, 0x40, probe, 2);
    cap_t *c = wait_frame(DS, 0x50, 500);
    CHECK(c && c->ts == 1 && c->ack == 1, "probe response sent with timestamp");

    printf("3. Approval (ask mode)\n");
    u8 auth[6] = {0, 0, 1, 0, 0, 0};
    send_mgmt(DS, 0xB0, auth, 6);
    c = wait_frame(DS, 0xB0, 300);
    u8 pm[6];
    CHECK(!c && ap_pending(pm) && !memcmp(pm, DS, 6), "unknown console waits for Y/N");
    ap_decide(DS, 1);
    send_mgmt(DS, 0xB0, auth, 6);
    c = wait_frame(DS, 0xB0, 500);
    CHECK(c && rd16le(c->d + 28) == 0, "after Y, authentication succeeds");
    send_mgmt(DS2, 0xB0, auth, 6);
    ap_pending(pm);
    ap_decide(DS2, 0);
    send_mgmt(DS2, 0xB0, auth, 6);
    c = wait_frame(DS2, 0xB0, 500);
    CHECK(c && rd16le(c->d + 28) != 0, "after N, the other console is refused");

    printf("4. Association\n");
    u8 as[64];
    int an = 0;
    wr16le(as, 0x0021); wr16le(as + 2, 10); an = 4;
    as[an++] = 0; as[an++] = 13; memcpy(as + an, "WFC-Connector", 13); an += 13;
    as[an++] = 1; as[an++] = 2; as[an++] = 0x82; as[an++] = 0x84;
    send_mgmt(DS, 0x00, as, an);
    c = wait_frame(DS, 0x10, 500);
    CHECK(c && rd16le(c->d + 26) == 0 && (rd16le(c->d + 28) & 0x3fff) == 1, "association accepted, AID 1");
    CHECK(ap_station_count() == 1, "one console connected");

    printf("5. DHCP\n");
    u32 dns = 0;
    int ok = dhcp(DS, &ds_ip, &dns);
    CHECK(ok && (ds_ip & 0xffffff00u) == 0xC0A84D00u, "console got 192.168.77.x (%u)", ds_ip & 255);
    CHECK(gw_ip == 0xC0A84D01u && dns == gw_ip, "gateway and DNS are 192.168.77.1");

    printf("6. ARP\n");
    u8 arp[28];
    wr16be(arp, 1); wr16be(arp + 2, 0x0800); arp[4] = 6; arp[5] = 4; wr16be(arp + 6, 1);
    memcpy(arp + 8, DS, 6); wr32be(arp + 14, ds_ip); memset(arp + 18, 0, 6); wr32be(arp + 24, gw_ip);
    send_eth(DS, BC, 0x0806, arp, 28);
    c = wait_frame(DS, 0xff, 500);
    CHECK(c && rd16be(c->d + 30) == 0x0806 && !memcmp(c->d + 32 + 8, BSSID, 6), "gateway answers ARP");

    printf("7. DNS through the hotspot (Kaeru WFC, 178.62.43.212)\n");
    u32 nas = dns_lookup(DS, gw_ip, "conntest.nintendowifi.net", 4000);
    CHECK(nas != 0, "conntest.nintendowifi.net -> %u.%u.%u.%u", nas >> 24, (nas >> 16) & 255, (nas >> 8) & 255, nas & 255);
    u32 ex = dns_lookup(DS, gw_ip, "example.com", 4000);
    CHECK(ex != 0, "example.com resolves too");

    printf("8. Nintendo connection test over TCP\n");
    int blen, st;
    if (nas) {
        int clen = http_get(nas, "conntest.nintendowifi.net", "/", 8192, &blen, &st, 15000);
        CHECK(clen >= -1 && st == 200, "HTTP %d from conntest.nintendowifi.net (body %d bytes)", st, blen);
    } else CHECK(0, "skipped: no address");

    printf("9. Web page over TCP\n");
    if (ex) {
        int clen = http_get(ex, "example.com", "/", 8192, &blen, &st, 15000);
        CHECK(st == 200 && blen > 0 && (clen < 0 || clen == blen), "HTTP %d, %d bytes received in order", st, blen);
    }

    printf("10. 1 MB download with 5%% packet loss and a 4 KB window\n");
    const char *bighost = "ipv4.download.thinkbroadband.com", *bigpath = "/1MB.zip";
    u32 big = dns_lookup(DS, gw_ip, bighost, 4000);
    if (big) {
        drop_rate = 0.05;
        u64 t0 = now_ms();
        int clen = http_get(big, bighost, bigpath, 4096, &blen, &st, 120000);
        drop_rate = 0;
        CHECK(st == 200 && clen > 0 && blen == clen, "HTTP %d, %d of %d bytes in %.1f s", st, blen, clen,
              (now_ms() - t0) / 1000.0);
    } else CHECK(0, "could not resolve %s", bighost);

    printf("11. Power save buffering\n");
    ds_pm = 1;
    u8 nul[1] = {0};
    send_eth(DS, BSSID, 0x0800, nul, 0);   /* any frame with PM set */
    int before = beacon_updates;
    u32 r = dns_lookup(DS, gw_ip, "example.com", 1500);
    CHECK(r == 0, "while asleep, the reply is held back");
    pump_once(150);
    CHECK(beacon_updates > before && (beacon[beacon_len - 1] & 0x02), "beacon TIM flags AID 1");
    u8 pp[16];
    pp[0] = 0xA4; pp[1] = 0x10; wr16le(pp + 2, 0xC001); memcpy(pp + 4, BSSID, 6); memcpy(pp + 10, DS, 6);
    ap_rx_frame(pp, 16, -40);
    c = wait_frame(DS, 0xff, 300);
    CHECK(c && rd16be(c->d + 30) == 0x0800, "PS-Poll releases the buffered reply");
    ds_pm = 0;
    send_eth(DS, BSSID, 0x0800, nul, 0);
    pump_once(150);

    printf("12. Changing the DNS while connected\n");
    g_cfg.dns1 = inet_addr("1.1.1.1");
    u32 r2 = dns_lookup(DS, gw_ip, "example.com", 4000);
    CHECK(r2 != 0, "new DNS (1.1.1.1) answers with no reconnect");

    printf("13. Direct DNS (console with DNS typed in by hand)\n");
    u32 r3 = dns_lookup(DS, 0xB23E2BD4u /* 178.62.43.212 */, "conntest.nintendowifi.net", 4000);
    CHECK(r3 != 0, "query straight to 178.62.43.212 is passed through");

    printf("14. Leaving\n");
    u8 rs[2] = {3, 0};
    send_mgmt(DS, 0xC0, rs, 2);
    CHECK(ap_station_count() == 0, "deauthentication removes the console");

    CHECK(bad_csums == 0, "every IP/TCP/UDP checksum the router sent was valid (%d bad)", bad_csums);
    printf("\n%d passed, %d failed\n", passes, fails);
    DeleteFileA(allow);
    return fails ? 1 : 0;
}
