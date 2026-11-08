// SPDX-License-Identifier: GPL-2.0-or-later
/* Router side of the hotspot.
 * Consoles get an address from our DHCP server, use us as gateway and DNS
 * server, and every TCP/UDP flow is carried by an ordinary Windows socket,
 * so no network driver or Internet Connection Sharing is needed. */
#include "net.h"
#include "ap.h"
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

#define MAX_LEASE 32
#define MAX_UDP 256
#define MAX_TCP 256
#define TXCAP 65536
#define UPCAP 65536
#define OUR_MSS 1460
#define UDP_IDLE_MS (3 * 60 * 1000)
#define DNS_IDLE_MS (20 * 1000)

static u8 gw_mac[6];
static u32 gw_ip;          /* host order */
static u16 ip_id = 1;

/* ---------------- leases ---------------- */

typedef struct { int used; u8 mac[6]; u32 ip; } lease_t;
static lease_t leases[MAX_LEASE];

static lease_t *lease_by_mac(const u8 *mac)
{
    for (int i = 0; i < MAX_LEASE; i++)
        if (leases[i].used && !memcmp(leases[i].mac, mac, 6)) return &leases[i];
    return NULL;
}

static lease_t *lease_by_ip(u32 ip)
{
    for (int i = 0; i < MAX_LEASE; i++)
        if (leases[i].used && leases[i].ip == ip) return &leases[i];
    return NULL;
}

static lease_t *lease_alloc(const u8 *mac, u32 want)
{
    lease_t *l = lease_by_mac(mac);
    if (l) return l;
    u32 base = g_cfg.net_base;
    if (want && (want & 0xffffff00u) == base && (want & 0xff) >= 100 && (want & 0xff) < 200 && !lease_by_ip(want)) {
        /* honour the address the console asked for */
    } else {
        want = 0;
        for (u32 h = 100; h < 200 && !want; h++)
            if (!lease_by_ip(base | h)) want = base | h;
    }
    if (!want) return NULL;
    for (int i = 0; i < MAX_LEASE; i++) {
        if (!leases[i].used) {
            leases[i].used = 1;
            memcpy(leases[i].mac, mac, 6);
            leases[i].ip = want;
            return &leases[i];
        }
    }
    return NULL;
}

int net_lease_for(const u8 mac[6], u32 *ip)
{
    lease_t *l = lease_by_mac(mac);
    if (!l) return 0;
    *ip = l->ip;
    return 1;
}

u32 net_gateway_ip(void) { return gw_ip; }

/* MAC for a console IP (lease table, else learned). */
typedef struct { u32 ip; u8 mac[6]; } arp_t;
static arp_t arpc[MAX_LEASE];

static void learn(u32 ip, const u8 *mac)
{
    if ((ip & 0xffffff00u) != g_cfg.net_base || ip == gw_ip) return;
    int free_i = -1;
    for (int i = 0; i < MAX_LEASE; i++) {
        if (arpc[i].ip == ip) { memcpy(arpc[i].mac, mac, 6); return; }
        if (!arpc[i].ip && free_i < 0) free_i = i;
    }
    if (free_i < 0) free_i = ip % MAX_LEASE;
    arpc[free_i].ip = ip;
    memcpy(arpc[free_i].mac, mac, 6);
}

static int mac_for(u32 ip, u8 *mac)
{
    for (int i = 0; i < MAX_LEASE; i++)
        if (arpc[i].ip == ip) { memcpy(mac, arpc[i].mac, 6); return 1; }
    lease_t *l = lease_by_ip(ip);
    if (l) { memcpy(mac, l->mac, 6); return 1; }
    return 0;
}

/* ---------------- checksums & IP output ---------------- */

static u32 csum_add(u32 sum, const u8 *p, int len)
{
    while (len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (u32)(p[0] << 8);
    return sum;
}

static u16 csum_fold(u32 sum)
{
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

static u16 l4_csum(u32 src, u32 dst, u8 proto, const u8 *seg, int len)
{
    u8 ph[12];
    wr32be(ph, src);
    wr32be(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    wr16be(ph + 10, (u16)len);
    u32 s = csum_add(0, ph, 12);
    s = csum_add(s, seg, len);
    return csum_fold(s);
}

/* Sends an IP packet whose payload (L4) is given, fragmenting if needed. */
static void ip_send(u32 src, u32 dst, u8 proto, const u8 *l4, int l4len)
{
    u8 mac[6];
    if (dst == 0xffffffffu || dst == (g_cfg.net_base | 0xff))
        memset(mac, 0xff, 6);
    else if (!mac_for(dst, mac))
        return;
    const int mtu_payload = 1480;     /* 1500 - IP header, multiple of 8 */
    u16 id = ip_id++;
    int off = 0;
    do {
        int chunk = l4len - off;
        int more = 0;
        if (chunk > mtu_payload) { chunk = mtu_payload; more = 1; }
        u8 pkt[1600];
        u8 *h = pkt;
        h[0] = 0x45; h[1] = 0;
        wr16be(h + 2, (u16)(20 + chunk));
        wr16be(h + 4, id);
        wr16be(h + 6, (u16)((more ? 0x2000 : 0) | (off / 8)));
        h[8] = 64; h[9] = proto;
        h[10] = h[11] = 0;
        wr32be(h + 12, src);
        wr32be(h + 16, dst);
        wr16be(h + 10, csum_fold(csum_add(0, h, 20)));
        memcpy(pkt + 20, l4 + off, chunk);
        ap_send_eth(mac, gw_mac, ETH_IP, pkt, 20 + chunk);
        off += chunk;
    } while (off < l4len);
}

static void udp_send(u32 src, u16 sport, u32 dst, u16 dport, const u8 *data, int len)
{
    if (len > 65507) return;
    u8 *seg = malloc(8 + len);
    wr16be(seg, sport);
    wr16be(seg + 2, dport);
    wr16be(seg + 4, (u16)(8 + len));
    seg[6] = seg[7] = 0;
    memcpy(seg + 8, data, len);
    u16 c = l4_csum(src, dst, 17, seg, 8 + len);
    wr16be(seg + 6, c ? c : 0xffff);
    ip_send(src, dst, 17, seg, 8 + len);
    free(seg);
}

/* ---------------- ARP ---------------- */

static void handle_arp(const u8 *src_mac, const u8 *p, int len)
{
    if (len < 28 || rd16be(p) != 1 || rd16be(p + 2) != ETH_IP || p[4] != 6 || p[5] != 4)
        return;
    u16 op = rd16be(p + 6);
    u32 spa = rd32be(p + 14), tpa = rd32be(p + 24);
    if (spa) learn(spa, p + 8);
    if (op == 1 && tpa == gw_ip) {
        u8 r[28];
        wr16be(r, 1); wr16be(r + 2, ETH_IP); r[4] = 6; r[5] = 4; wr16be(r + 6, 2);
        memcpy(r + 8, gw_mac, 6); wr32be(r + 14, gw_ip);
        memcpy(r + 18, p + 8, 6); wr32be(r + 24, spa);
        ap_send_eth(src_mac, gw_mac, ETH_ARP, r, 28);
    }
}

/* ---------------- DHCP ---------------- */

static void dhcp_reply(const u8 *req, const u8 *chaddr, u8 type, u32 yiaddr)
{
    u8 b[548];
    memset(b, 0, sizeof(b));
    b[0] = 2; b[1] = 1; b[2] = 6; b[3] = 0;
    memcpy(b + 4, req + 4, 4);            /* xid */
    memcpy(b + 10, req + 10, 2);          /* flags */
    if (type != 6) wr32be(b + 16, yiaddr);
    wr32be(b + 20, gw_ip);
    memcpy(b + 28, chaddr, 6);
    wr32be(b + 236, 0x63825363);
    int o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = type;
    b[o++] = 54; b[o++] = 4; wr32be(b + o, gw_ip); o += 4;
    if (type != 6) {
        b[o++] = 51; b[o++] = 4; wr32be(b + o, 86400); o += 4;
        b[o++] = 58; b[o++] = 4; wr32be(b + o, 43200); o += 4;
        b[o++] = 1;  b[o++] = 4; wr32be(b + o, 0xffffff00u); o += 4;
        b[o++] = 28; b[o++] = 4; wr32be(b + o, g_cfg.net_base | 0xff); o += 4;
        b[o++] = 3;  b[o++] = 4; wr32be(b + o, gw_ip); o += 4;
        b[o++] = 6;  b[o++] = 4; wr32be(b + o, gw_ip); o += 4;
    }
    b[o++] = 255;
    if (o < 300) o = 300;
    udp_send(gw_ip, 67, 0xffffffffu, 68, b, o);
}

static void handle_dhcp(const u8 *src_mac, const u8 *d, int len)
{
    if (len < 240 || d[0] != 1 || rd32be(d + 236) != 0x63825363)
        return;
    const u8 *chaddr = d + 28;
    if (memcmp(chaddr, src_mac, 6))
        return;
    int type = 0;
    u32 req_ip = 0, server = 0;
    for (int o = 240; o < len;) {
        u8 code = d[o];
        if (code == 255) break;
        if (code == 0) { o++; continue; }
        if (o + 1 >= len) break;
        u8 l = d[o + 1];
        if (o + 2 + l > len) break;
        const u8 *v = d + o + 2;
        if (code == 53 && l >= 1) type = v[0];
        if (code == 50 && l == 4) req_ip = rd32be(v);
        if (code == 54 && l == 4) server = rd32be(v);
        o += 2 + l;
    }
    u32 ciaddr = rd32be(d + 12);
    if (type == 1) {                      /* DISCOVER */
        lease_t *l = lease_alloc(src_mac, req_ip);
        if (l) dhcp_reply(d, chaddr, 2, l->ip);
    } else if (type == 3) {               /* REQUEST */
        if (server && server != gw_ip) return;
        u32 want = req_ip ? req_ip : ciaddr;
        lease_t *l = lease_by_mac(src_mac);
        if (!l) l = lease_alloc(src_mac, want);
        if (l && (!want || want == l->ip)) {
            dhcp_reply(d, chaddr, 5, l->ip);
            learn(l->ip, src_mac);
            LOG("Console " MACFMT " got address %u.%u.%u.%u.\n", MACARG(src_mac),
                l->ip >> 24, (l->ip >> 16) & 255, (l->ip >> 8) & 255, l->ip & 255);
        } else {
            dhcp_reply(d, chaddr, 6, 0);  /* NAK: it will start over */
        }
    } else if (type == 8) {               /* INFORM */
        dhcp_reply(d, chaddr, 5, 0);
    }
}

/* ---------------- UDP NAT + DNS forwarder ---------------- */

typedef struct {
    int used;
    int dns;          /* 1 = console asked the gateway for DNS */
    u32 cip;
    u16 cport;
    SOCKET s;
    u64 last;
    u16 bound_port;
    u32 peer_ip[8];        /* remote endpoints seen, for the diagnostic log */
    u16 peer_port[8];
    int npeers;
    long pkts_out, pkts_in;
} udpmap_t;

static udpmap_t umap[MAX_UDP];

/* ---- diagnostic helpers (log file detail) ---- */

static const char *ipstr(u32 h)
{
    static char b[4][20];
    static int k;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof(b[k]), "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
    return b[k];
}

/* Returns 1 the first time this endpoint is seen on the mapping. */
static int udp_peer_new(udpmap_t *m, u32 ip, u16 port)
{
    for (int i = 0; i < m->npeers; i++)
        if (m->peer_ip[i] == ip && m->peer_port[i] == port) return 0;
    if (m->npeers < 8) { m->peer_ip[m->npeers] = ip; m->peer_port[m->npeers] = port; m->npeers++; }
    return 1;
}

static void dns_name(const u8 *d, int len, char *out, int outlen)
{
    int k = 12, o = 0;
    out[0] = 0;
    if (len < 13) return;
    while (k < len && d[k] && o < outlen - 2) {
        int l = d[k++];
        if ((l & 0xc0) || k + l > len) break;
        if (o) out[o++] = '.';
        for (int i = 0; i < l && o < outlen - 1; i++) out[o++] = (char)d[k + i];
        k += l;
    }
    out[o] = 0;
}

static void dns_log_answer(const u8 *r, int rl)
{
    char name[128];
    dns_name(r, rl, name, sizeof(name));
    int rcode = rl > 3 ? (r[3] & 0x0f) : -1;
    int an = rl > 7 ? rd16be(r + 6) : 0;
    int k = 12;
    while (k < rl && r[k]) k += r[k] + 1;
    k += 5;
    char ips[160] = "";
    for (int i = 0; i < an && k + 12 <= rl; i++) {
        if ((r[k] & 0xc0) == 0xc0) k += 2; else { while (k < rl && r[k]) k += r[k] + 1; k++; }
        if (k + 10 > rl) break;
        u16 type = rd16be(r + k), rdl = rd16be(r + k + 8);
        if (type == 1 && rdl == 4 && k + 14 <= rl && strlen(ips) < sizeof(ips) - 20) {
            strcat(ips, " ");
            strcat(ips, ipstr(rd32be(r + k + 10)));
        }
        k += 10 + rdl;
    }
    DBG("NET dns answer %s rcode=%d ->%s\n", name, rcode, ips[0] ? ips : " (no address)");
}

static void sock_nonblock(SOCKET s)
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

static udpmap_t *udp_map(u32 cip, u16 cport, int dns)
{
    udpmap_t *freeslot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_UDP; i++) {
        udpmap_t *m = &umap[i];
        if (m->used && m->cip == cip && m->cport == cport && m->dns == dns) return m;
        if (!m->used && !freeslot) freeslot = m;
        if (m->used && (!oldest || m->last < oldest->last)) oldest = m;
    }
    if (!freeslot) {
        closesocket(oldest->s);
        oldest->used = 0;
        freeslot = oldest;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return NULL;
    BOOL off = FALSE;
    DWORD br;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &br, NULL, NULL);
    sock_nonblock(s);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    /* Keep the console's own port when possible: helps games' NAT traversal. */
    a.sin_port = dns ? 0 : htons(cport);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        a.sin_port = 0;
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); return NULL; }
    }
    memset(freeslot, 0, sizeof(*freeslot));
    freeslot->used = 1;
    freeslot->dns = dns;
    freeslot->cip = cip;
    freeslot->cport = cport;
    freeslot->s = s;
    freeslot->last = now_ms();
    int al = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &al);
    freeslot->bound_port = ntohs(a.sin_port);
    if (!dns)
        DBG("NET udp mapping %s:%u -> PC port %u%s\n", ipstr(cip), cport, freeslot->bound_port,
            freeslot->bound_port == cport ? " (same port)" : " (port changed)");
    return freeslot;
}

static void udp_out(u32 src, u16 sport, u32 dst, u16 dport, const u8 *data, int len)
{
    int dns = (dst == gw_ip && dport == 53);
    if (dst == gw_ip && !dns) return;
    if (dns) {
        if (!g_cfg.dns1) return;
        dst = ntohl(g_cfg.dns1);
    }
    udpmap_t *m = udp_map(src, sport, dns);
    if (!m) { DBG("NET udp from %s:%u dropped: no socket\n", ipstr(src), sport); return; }
    m->last = now_ms();
    m->pkts_out++;
    if (dns) {
        char name[128];
        dns_name(data, len, name, sizeof(name));
        DBG("NET dns query %s (via %s)\n", name, ipstr(dst));
    } else if (udp_peer_new(m, dst, dport)) {
        DBG("NET udp %s:%u -> %s:%u (%d bytes)\n", ipstr(src), sport, ipstr(dst), dport, len);
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(dst);
    to.sin_port = htons(dport);
    sendto(m->s, (const char *)data, len, 0, (struct sockaddr *)&to, sizeof(to));
    if (dns && g_cfg.dns2) {
        to.sin_addr.s_addr = g_cfg.dns2;
        sendto(m->s, (const char *)data, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void udp_in(udpmap_t *m)
{
    static u8 buf[65536];
    for (int k = 0; k < 64; k++) {
        struct sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(m->s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        m->last = now_ms();
        u32 fip = ntohl(from.sin_addr.s_addr);
        u16 fport = ntohs(from.sin_port);
        m->pkts_in++;
        if (m->dns) {
            if (fport != 53) continue;
            dns_log_answer(buf, n);
            udp_send(gw_ip, 53, m->cip, m->cport, buf, n);
        } else {
            if (udp_peer_new(m, fip, fport))
                DBG("NET udp %s:%u <- %s:%u (%d bytes, new sender)\n", ipstr(m->cip), m->cport, ipstr(fip), fport, n);
            udp_send(fip, fport, m->cip, m->cport, buf, n);
        }
    }
}

/* ---------------- TCP proxy ---------------- */

enum { T_FREE = 0, T_CONNECTING, T_SYNRCVD, T_EST };
#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10

typedef struct {
    int state;
    u32 cip, rip;
    u16 cport, rport;
    SOCKET s;
    u32 rcv_nxt;
    u32 iss, snd_una, snd_nxt;
    u32 snd_wnd;
    u16 mss;
    u8 *tx; int tx_len;          /* remote -> console, tx[0] is at snd_una */
    u8 *up; int up_len;          /* console -> remote, not yet sent */
    int client_fin, remote_fin, fin_sent, shut_sent;
    u32 fin_seq;
    u64 rto_at;
    int rto, retries, dupacks, probe;
    u64 last;
    u32 last_adv;
    long up_total, down_total;
    const char *why;
} tcpc_t;

static tcpc_t tconn[MAX_TCP];

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)

static u32 rcv_window(tcpc_t *c)
{
    u32 w = (u32)(UPCAP - c->up_len);
    return w > 65535 ? 65535 : w;
}

static void tcp_raw(u32 src, u16 sport, u32 dst, u16 dport, u32 seq, u32 ack, u8 flags,
                    u16 wnd, const u8 *data, int len, int mss_opt)
{
    u8 seg[1600];
    int hl = mss_opt ? 24 : 20;
    wr16be(seg, sport);
    wr16be(seg + 2, dport);
    wr32be(seg + 4, seq);
    wr32be(seg + 8, ack);
    seg[12] = (u8)((hl / 4) << 4);
    seg[13] = flags;
    wr16be(seg + 14, wnd);
    seg[16] = seg[17] = 0;
    seg[18] = seg[19] = 0;
    if (mss_opt) { seg[20] = 2; seg[21] = 4; wr16be(seg + 22, (u16)mss_opt); }
    if (len) memcpy(seg + hl, data, len);
    wr16be(seg + 16, l4_csum(src, dst, 6, seg, hl + len));
    ip_send(src, dst, 6, seg, hl + len);
}

static void tcp_send(tcpc_t *c, u8 flags, u32 seq, const u8 *data, int len, int syn)
{
    u32 w = rcv_window(c);
    c->last_adv = w;
    tcp_raw(c->rip, c->rport, c->cip, c->cport, seq, c->rcv_nxt, flags, (u16)w, data, len,
            syn ? OUR_MSS : 0);
}

static void tcp_free(tcpc_t *c, int send_rst)
{
    if (c->state != T_FREE)
        DBG("NET tcp %s:%u -> %s:%u closed: %s (console sent %ld bytes, server sent %ld)\n", ipstr(c->cip), c->cport,
            ipstr(c->rip), c->rport, c->why ? c->why : "?", c->up_total, c->down_total);
    if (send_rst && c->state != T_FREE)
        tcp_send(c, TF_RST | TF_ACK, c->snd_nxt, NULL, 0, 0);
    if (c->s != INVALID_SOCKET && c->s) closesocket(c->s);
    free(c->tx);
    free(c->up);
    memset(c, 0, sizeof(*c));
    c->s = INVALID_SOCKET;
}

static tcpc_t *tcp_find(u32 cip, u16 cport, u32 rip, u16 rport)
{
    for (int i = 0; i < MAX_TCP; i++) {
        tcpc_t *c = &tconn[i];
        if (c->state != T_FREE && c->cip == cip && c->cport == cport && c->rip == rip && c->rport == rport)
            return c;
    }
    return NULL;
}

int net_conn_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_TCP; i++) if (tconn[i].state != T_FREE) n++;
    for (int i = 0; i < MAX_UDP; i++) if (umap[i].used) n++;
    return n;
}

static void arm(tcpc_t *c)
{
    c->rto_at = now_ms() + (u64)c->rto;
}

static void tcp_output(tcpc_t *c)
{
    if (c->state != T_EST)
        return;
    u32 wnd = c->snd_wnd;
    if (wnd == 0 && c->probe) wnd = 1;
    u32 mss = c->mss ? c->mss : 536;
    if (mss > OUR_MSS) mss = OUR_MSS;
    int sent_any = 0;
    if (!c->fin_sent) {
        u32 off = c->snd_nxt - c->snd_una;
        while ((int)off < c->tx_len && off < wnd) {
            u32 n = (u32)c->tx_len - off;
            if (n > mss) n = mss;
            if (n > wnd - off) n = wnd - off;
            tcp_send(c, TF_ACK | TF_PSH, c->snd_una + off, c->tx + off, (int)n, 0);
            off += n;
            sent_any = 1;
        }
        c->snd_nxt = c->snd_una + off;
        if (c->remote_fin && (int)off == c->tx_len) {
            c->fin_seq = c->snd_una + (u32)c->tx_len;
            tcp_send(c, TF_FIN | TF_ACK, c->fin_seq, NULL, 0, 0);
            c->snd_nxt = c->fin_seq + 1;
            c->fin_sent = 1;
            sent_any = 1;
        }
    }
    c->probe = 0;
    if (sent_any && !c->rto_at)
        arm(c);
}

static void flush_up(tcpc_t *c)
{
    while (c->up_len > 0) {
        int n = send(c->s, (const char *)c->up, c->up_len, 0);
        if (n <= 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) { c->why = "sending to server failed"; tcp_free(c, 1); return; }
            break;
        }
        memmove(c->up, c->up + n, c->up_len - n);
        c->up_len -= n;
    }
    if (c->client_fin && c->up_len == 0 && !c->shut_sent) {
        shutdown(c->s, SD_SEND);
        c->shut_sent = 1;
    }
    /* Window update if we had been close to full. */
    if (c->last_adv < (u32)c->mss * 2 && rcv_window(c) >= (u32)c->mss * 2)
        tcp_send(c, TF_ACK, c->snd_nxt, NULL, 0, 0);
}

static void maybe_finish(tcpc_t *c)
{
    if (c->state == T_EST && c->client_fin && c->fin_sent && c->snd_una == c->fin_seq + 1 && c->up_len == 0)
        { c->why = "finished normally"; tcp_free(c, 0); }
}

static void tcp_new(u32 cip, u16 cport, u32 rip, u16 rport, u32 seq, const u8 *opts, int optlen, u16 wnd)
{
    tcpc_t *c = NULL;
    for (int i = 0; i < MAX_TCP && !c; i++)
        if (tconn[i].state == T_FREE) c = &tconn[i];
    if (!c) {
        tcp_raw(rip, rport, cip, cport, 0, seq + 1, TF_RST | TF_ACK, 0, NULL, 0, 0);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->cip = cip; c->cport = cport; c->rip = rip; c->rport = rport;
    c->rcv_nxt = seq + 1;
    c->iss = (u32)rand() << 16 ^ (u32)rand() ^ (u32)now_ms();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss;
    c->snd_wnd = wnd;
    c->mss = 536;
    for (int o = 0; o < optlen;) {
        if (opts[o] == 0) break;
        if (opts[o] == 1) { o++; continue; }
        if (o + 1 >= optlen || opts[o + 1] < 2) break;
        if (opts[o] == 2 && opts[o + 1] == 4 && o + 4 <= optlen) c->mss = rd16be(opts + o + 2);
        o += opts[o + 1];
    }
    c->rto = 500;
    c->last = now_ms();
    c->tx = malloc(TXCAP);
    c->up = malloc(UPCAP);
    c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->s == INVALID_SOCKET) { c->state = T_EST; tcp_free(c, 1); return; }
    sock_nonblock(c->s);
    BOOL nd = TRUE;
    setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(rip);
    to.sin_port = htons(rport);
    c->state = T_CONNECTING;
    if (connect(c->s, (struct sockaddr *)&to, sizeof(to)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        tcp_raw(rip, rport, cip, cport, 0, seq + 1, TF_RST | TF_ACK, 0, NULL, 0, 0);
        c->state = T_EST;
        tcp_free(c, 0);
    }
    DBG("TCP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u\n", cip >> 24, (cip >> 16) & 255, (cip >> 8) & 255, cip & 255,
        cport, rip >> 24, (rip >> 16) & 255, (rip >> 8) & 255, rip & 255, rport);
}

/* Diagnostic: write the first ~700 bytes of plain-text conversations
 * (web on port 80, the Plaza's chat on 6667) to the log. */
static void payload_log(tcpc_t *c, const char *who, const u8 *d, int n, long already)
{
    if ((c->rport != 80 && c->rport != 6667) || already >= 700 || n <= 0)
        return;
    if (n > 700 - already) n = (int)(700 - already);
    char out[3000];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(out) - 6; i++) {
        u8 ch = d[i];
        if (ch == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (ch == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (ch >= 32 && ch < 127 && ch != '\\') out[o++] = (char)ch;
        else o += snprintf(out + o, sizeof(out) - o, "\\x%02x", ch);
    }
    out[o] = 0;
    DBG("NET data %s:%u %s says: %s\n", ipstr(c->rip), c->rport, who, out);
}

static void handle_tcp(u32 src, u32 dst, const u8 *seg, int len)
{
    if (len < 20) return;
    int hl = (seg[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    u16 sport = rd16be(seg), dport = rd16be(seg + 2);
    u32 seq = rd32be(seg + 4), ack = rd32be(seg + 8);
    u8 fl = seg[13];
    u16 wnd = rd16be(seg + 14);
    const u8 *data = seg + hl;
    int dlen = len - hl;

    if (dst == gw_ip) {                     /* nothing listens on the gateway */
        if (!(fl & TF_RST))
            tcp_raw(dst, dport, src, sport, (fl & TF_ACK) ? ack : 0, seq + dlen + ((fl & TF_SYN) ? 1 : 0),
                    TF_RST | TF_ACK, 0, NULL, 0, 0);
        return;
    }
    tcpc_t *c = tcp_find(src, sport, dst, dport);
    if (!c) {
        if ((fl & (TF_SYN | TF_ACK | TF_RST)) == TF_SYN)
            tcp_new(src, sport, dst, dport, seq, seg + 20, hl - 20, wnd);
        else if (!(fl & TF_RST))
            tcp_raw(dst, dport, src, sport, (fl & TF_ACK) ? ack : 0,
                    seq + dlen + ((fl & TF_SYN) ? 1 : 0) + ((fl & TF_FIN) ? 1 : 0),
                    TF_RST | ((fl & TF_ACK) ? 0 : TF_ACK), 0, NULL, 0, 0);
        return;
    }
    c->last = now_ms();
    if (fl & TF_RST) { c->why = "console reset it"; tcp_free(c, 0); return; }
    if (fl & TF_SYN) {
        if (c->state == T_SYNRCVD) tcp_send(c, TF_SYN | TF_ACK, c->iss, NULL, 0, 1);
        else if (c->state == T_EST) tcp_send(c, TF_ACK, c->snd_nxt, NULL, 0, 0);
        return;
    }
    if (c->state == T_CONNECTING) return;
    if (c->state == T_SYNRCVD) {
        if (!(fl & TF_ACK) || ack != c->iss + 1) return;
        c->state = T_EST;
        c->snd_una = c->snd_nxt = c->iss + 1;
        c->rto_at = 0;
        c->retries = 0;
    }
    if (fl & TF_ACK) {
        if (SEQ_LT(c->snd_una, ack) && SEQ_LE(ack, c->snd_nxt)) {
            u32 n = ack - c->snd_una;
            u32 data_acked = n;
            if (c->fin_sent && ack == c->fin_seq + 1) data_acked--;
            if ((int)data_acked > c->tx_len) data_acked = (u32)c->tx_len;
            memmove(c->tx, c->tx + data_acked, c->tx_len - data_acked);
            c->tx_len -= (int)data_acked;
            c->snd_una = ack;
            c->retries = 0;
            c->dupacks = 0;
            c->rto = 500;
            c->rto_at = 0;
            if (c->snd_una != c->snd_nxt) arm(c);
        } else if (ack == c->snd_una && dlen == 0 && !(fl & TF_FIN) && c->snd_nxt != c->snd_una &&
                   wnd == c->snd_wnd) {
            if (++c->dupacks == 3) {     /* fast retransmit */
                if (!c->fin_sent || c->tx_len) { c->snd_nxt = c->snd_una; c->fin_sent = 0; }
            }
        }
        c->snd_wnd = wnd;
    }
    int need_ack = 0;
    if (dlen > 0) {
        if (seq == c->rcv_nxt && !c->client_fin) {
            int room = UPCAP - c->up_len;
            int take = dlen < room ? dlen : room;
            memcpy(c->up + c->up_len, data, take);
            payload_log(c, "console", data, take, c->up_total);
            c->up_len += take;
            c->up_total += take;
            c->rcv_nxt += (u32)take;
            flush_up(c);
            if (c->state == T_FREE) return;
        }
        need_ack = 1;
    }
    if ((fl & TF_FIN) && seq + (u32)dlen == c->rcv_nxt && !c->client_fin) {
        c->rcv_nxt++;
        c->client_fin = 1;
        need_ack = 1;
        if (c->up_len == 0 && !c->shut_sent) { shutdown(c->s, SD_SEND); c->shut_sent = 1; }
    } else if ((fl & TF_FIN) && c->client_fin) {
        need_ack = 1;
    }
    if (need_ack)
        tcp_send(c, TF_ACK, c->snd_nxt, NULL, 0, 0);
    tcp_output(c);
    maybe_finish(c);
}

static void tcp_sock_events(tcpc_t *c, int rd, int wr, int ex)
{
    if (c->state == T_CONNECTING) {
        if (ex) {
            int err = 0, el = sizeof(err);
            getsockopt(c->s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
            static char why[64];
            snprintf(why, sizeof(why), "server refused or unreachable (Windows error %d)", err);
            c->why = why;
            tcp_raw(c->rip, c->rport, c->cip, c->cport, 0, c->rcv_nxt, TF_RST | TF_ACK, 0, NULL, 0, 0);
            c->state = T_EST;
            tcp_free(c, 0);
            return;
        }
        if (wr) {
            DBG("NET tcp %s:%u -> %s:%u connected\n", ipstr(c->cip), c->cport, ipstr(c->rip), c->rport);
            c->state = T_SYNRCVD;
            tcp_send(c, TF_SYN | TF_ACK, c->iss, NULL, 0, 1);
            c->snd_nxt = c->iss + 1;
            arm(c);
        }
        return;
    }
    if (c->state != T_EST) return;
    if (wr && c->up_len) {
        flush_up(c);
        if (c->state == T_FREE) return;
    }
    if (rd && !c->remote_fin && c->tx_len < TXCAP) {
        int n = recv(c->s, (char *)c->tx + c->tx_len, TXCAP - c->tx_len, 0);
        if (n > 0) {
            payload_log(c, "server", c->tx + c->tx_len, n, c->down_total);
            c->tx_len += n;
            c->down_total += n;
            c->last = now_ms();
        } else if (n == 0) {
            c->remote_fin = 1;
        } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
            c->why = "server connection broke";
            tcp_free(c, 1);
            return;
        }
        if (n == 0)
            DBG("NET tcp %s:%u -> %s:%u server finished sending\n", ipstr(c->cip), c->cport, ipstr(c->rip), c->rport);
        tcp_output(c);
    }
}

static void tcp_timers(u64 t)
{
    for (int i = 0; i < MAX_TCP; i++) {
        tcpc_t *c = &tconn[i];
        if (c->state == T_FREE) continue;
        u64 idle = t - c->last;
        if ((c->state == T_CONNECTING && idle > 30000) ||
            (c->state == T_SYNRCVD && idle > 60000) ||
            (c->state == T_EST && (idle > 2 * 3600 * 1000ull ||
                                   (c->remote_fin && c->fin_sent && c->snd_una == c->fin_seq + 1 && idle > 120000)))) {
            c->why = c->state == T_CONNECTING ? "server did not answer in 30 s"
                   : c->state == T_SYNRCVD ? "console never completed the connection" : "idle timeout";
            tcp_free(c, c->state != T_CONNECTING);
            continue;
        }
        if (c->rto_at && t >= c->rto_at) {
            c->rto_at = 0;
            if (++c->retries > 12) { c->why = "console stopped answering"; tcp_free(c, 1); continue; }
            c->rto = c->rto * 2 > 8000 ? 8000 : c->rto * 2;
            if (c->state == T_SYNRCVD) {
                tcp_send(c, TF_SYN | TF_ACK, c->iss, NULL, 0, 1);
                arm(c);
                continue;
            }
            if (c->snd_nxt != c->snd_una) {
                c->snd_nxt = c->snd_una;
                c->fin_sent = 0;
            }
            if (c->snd_wnd == 0) c->probe = 1;
            tcp_output(c);
            if (!c->rto_at && (c->snd_nxt != c->snd_una)) arm(c);
        } else if (!c->rto_at && c->state == T_EST && c->snd_wnd == 0 && c->tx_len > 0) {
            arm(c);                      /* persist timer */
        }
    }
}

/* ---------------- ICMP (gateway ping only) ---------------- */

static void handle_icmp(u32 src, u32 dst, const u8 *p, int len)
{
    if (dst != gw_ip && len >= 1) DBG("NET dropped ICMP type %u %s -> %s\n", p[0], ipstr(src), ipstr(dst));
    if (dst != gw_ip || len < 8 || p[0] != 8) return;
    u8 *r = malloc(len);
    memcpy(r, p, len);
    r[0] = 0;
    r[2] = r[3] = 0;
    wr16be(r + 2, csum_fold(csum_add(0, r, len)));
    ip_send(gw_ip, src, 1, r, len);
    free(r);
}

/* ---------------- IP input ---------------- */

static void handle_ip(const u8 *src_mac, const u8 *p, int len)
{
    if (len < 20 || (p[0] >> 4) != 4) return;
    int hl = (p[0] & 0x0f) * 4;
    int tot = rd16be(p + 2);
    if (hl < 20 || tot < hl || tot > len) return;
    u16 frag = rd16be(p + 6);
    if (frag & 0x3fff) {                  /* fragments are not forwarded */
        DBG("NET dropped IP fragment %s -> %s proto %u\n", ipstr(rd32be(p + 12)), ipstr(rd32be(p + 16)), p[9]);
        return;
    }
    u8 proto = p[9];
    u32 src = rd32be(p + 12), dst = rd32be(p + 16);
    const u8 *l4 = p + hl;
    int l4len = tot - hl;
    if (src) learn(src, src_mac);

    if (proto == 17 && l4len >= 8) {
        u16 sport = rd16be(l4), dport = rd16be(l4 + 2);
        int ulen = rd16be(l4 + 4);
        if (ulen < 8 || ulen > l4len) return;
        if (dport == 67) { handle_dhcp(src_mac, l4 + 8, ulen - 8); return; }
        if (dst == 0xffffffffu || dst == (g_cfg.net_base | 0xff) || (dst >> 28) == 0xe) {
            DBG("NET dropped broadcast/multicast UDP %s:%u -> %s:%u\n", ipstr(src), sport, ipstr(dst), dport);
            return;
        }
        if ((dst & 0xffffff00u) == g_cfg.net_base && dst != gw_ip) return;
        udp_out(src, sport, dst, dport, l4 + 8, ulen - 8);
    } else if (proto == 6) {
        if ((dst & 0xffffff00u) == g_cfg.net_base && dst != gw_ip) return;
        if ((dst >> 28) == 0xe || dst == 0xffffffffu) return;
        handle_tcp(src, dst, l4, l4len);
    } else if (proto == 1) {
        handle_icmp(src, dst, l4, l4len);
    } else {
        DBG("NET dropped IP protocol %u %s -> %s\n", proto, ipstr(src), ipstr(dst));
    }
}

void net_rx_eth(const u8 src[6], const u8 dst[6], u16 ethertype, const u8 *p, int len)
{
    if (!(dst[0] & 1) && memcmp(dst, gw_mac, 6))
        return;
    if (ethertype == ETH_ARP) handle_arp(src, p, len);
    else if (ethertype == ETH_IP) handle_ip(src, p, len);
}

void net_station_joined(const u8 mac[6]) { (void)mac; }

void net_station_left(const u8 mac[6])
{
    lease_t *l = lease_by_mac(mac);
    if (!l) return;
    for (int i = 0; i < MAX_TCP; i++)
        if (tconn[i].state != T_FREE && tconn[i].cip == l->ip) { tconn[i].why = "console left"; tcp_free(&tconn[i], 0); }
    for (int i = 0; i < MAX_UDP; i++)
        if (umap[i].used && umap[i].cip == l->ip) { closesocket(umap[i].s); umap[i].used = 0; }
}

/* ---------------- socket loop integration ---------------- */

void net_init(const u8 mac[6])
{
    memcpy(gw_mac, mac, 6);
    gw_ip = g_cfg.net_base | 1;
    memset(leases, 0, sizeof(leases));
    memset(umap, 0, sizeof(umap));
    for (int i = 0; i < MAX_TCP; i++) { memset(&tconn[i], 0, sizeof(tconn[i])); tconn[i].s = INVALID_SOCKET; }
    srand((unsigned)GetTickCount());
}

void net_fill_fdsets(fd_set *r, fd_set *w, fd_set *e)
{
    for (int i = 0; i < MAX_UDP; i++)
        if (umap[i].used && r->fd_count < FD_SETSIZE) FD_SET(umap[i].s, r);
    for (int i = 0; i < MAX_TCP; i++) {
        tcpc_t *c = &tconn[i];
        if (c->state == T_FREE) continue;
        if (c->state == T_CONNECTING) {
            if (w->fd_count < FD_SETSIZE) FD_SET(c->s, w);
            if (e->fd_count < FD_SETSIZE) FD_SET(c->s, e);
            continue;
        }
        if (c->state != T_EST) continue;
        if (!c->remote_fin && c->tx_len < TXCAP && r->fd_count < FD_SETSIZE) FD_SET(c->s, r);
        if (c->up_len && w->fd_count < FD_SETSIZE) FD_SET(c->s, w);
    }
}

void net_handle_fdsets(fd_set *r, fd_set *w, fd_set *e)
{
    for (int i = 0; i < MAX_UDP; i++)
        if (umap[i].used && FD_ISSET(umap[i].s, r)) udp_in(&umap[i]);
    for (int i = 0; i < MAX_TCP; i++) {
        tcpc_t *c = &tconn[i];
        if (c->state == T_FREE) continue;
        int rd = FD_ISSET(c->s, r), wr = FD_ISSET(c->s, w), ex = FD_ISSET(c->s, e);
        if (rd || wr || ex) tcp_sock_events(c, rd, wr, ex);
    }
}

void net_tick(u64 t)
{
    tcp_timers(t);
    for (int i = 0; i < MAX_UDP; i++) {
        udpmap_t *m = &umap[i];
        if (m->used && t - m->last > (u64)(m->dns ? DNS_IDLE_MS : UDP_IDLE_MS)) {
            closesocket(m->s);
            m->used = 0;
        }
    }
}

void net_shutdown(void)
{
    for (int i = 0; i < MAX_TCP; i++) if (tconn[i].state != T_FREE) tcp_free(&tconn[i], 1);
    for (int i = 0; i < MAX_UDP; i++) if (umap[i].used) { closesocket(umap[i].s); umap[i].used = 0; }
}
