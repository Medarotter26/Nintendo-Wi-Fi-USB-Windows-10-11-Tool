// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal open-system 802.11b access point for Nintendo DS and Wii consoles. */
#include "ap.h"
#include "rt2570.h"   /* RATE_* */

#define MAX_STA 8
#define PSQ_MAX 48
#define STA_TIMEOUT_MS (10 * 60 * 1000)

enum { ST_FREE = 0, ST_AUTH, ST_ASSOC };

typedef struct {
    u8 *data;
    int len;
} pkt_t;

typedef struct {
    int state;
    u8 mac[6];
    u16 aid;
    int ps;
    int rate;
    u64 last_seen;
    u16 last_seq;
    int have_seq;
    pkt_t psq[PSQ_MAX];
    int psq_n;
} sta_t;

static struct {
    ap_hw_t hw;
    u8 bssid[6];
    char ssid[33];
    int ssid_len;
    int channel;
    sta_t sta[MAX_STA];
    int beacon_dirty;
    char allow_path[MAX_PATH];
    int approve_all;
    u8 pending_mac[6];
    int pending;
    u64 pending_since;
    u8 denied[16][6];
    int ndenied;
    u64 last_deauth_sent;
} ap;

static const u8 BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* ---- allow list ---- */

static int allow_contains(const u8 mac[6])
{
    FILE *f = fopen(ap.allow_path, "r");
    if (!f)
        return 0;
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned m[6];
        if (sscanf(line, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            int eq = 1;
            for (int i = 0; i < 6; i++) if ((u8)m[i] != mac[i]) eq = 0;
            if (eq) { found = 1; break; }
        }
    }
    fclose(f);
    return found;
}

static void allow_add(const u8 mac[6])
{
    if (allow_contains(mac))
        return;
    FILE *f = fopen(ap.allow_path, "a");
    if (!f)
        return;
    fprintf(f, MACFMT "\n", MACARG(mac));
    fclose(f);
}

static int is_denied(const u8 mac[6])
{
    for (int i = 0; i < ap.ndenied; i++)
        if (!memcmp(ap.denied[i], mac, 6)) return 1;
    return 0;
}

/* ---- helpers ---- */

static sta_t *sta_find(const u8 mac[6])
{
    for (int i = 0; i < MAX_STA; i++)
        if (ap.sta[i].state != ST_FREE && !memcmp(ap.sta[i].mac, mac, 6))
            return &ap.sta[i];
    return NULL;
}

static sta_t *sta_new(const u8 mac[6])
{
    for (int i = 0; i < MAX_STA; i++) {
        if (ap.sta[i].state == ST_FREE) {
            sta_t *s = &ap.sta[i];
            memset(s, 0, sizeof(*s));
            memcpy(s->mac, mac, 6);
            s->aid = (u16)(i + 1);
            s->rate = RATE_1M;
            s->state = ST_AUTH;
            return s;
        }
    }
    return NULL;
}

static void psq_clear(sta_t *s)
{
    for (int i = 0; i < s->psq_n; i++)
        free(s->psq[i].data);
    s->psq_n = 0;
}

static void sta_remove(sta_t *s, const char *why)
{
    if (s->state == ST_ASSOC) {
        LOG("Console " MACFMT " left (%s).\n", MACARG(s->mac), why);
        net_station_left(s->mac);
    }
    psq_clear(s);
    s->state = ST_FREE;
    ap.beacon_dirty = 1;
}

static int hdr_init(u8 *f, u8 fc0, u8 fc1, const u8 *a1, const u8 *a2, const u8 *a3, u16 dur)
{
    f[0] = fc0;
    f[1] = fc1;
    wr16le(f + 2, dur);
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    f[22] = f[23] = 0;
    return 24;
}

/* NAV for one ACK at 1 Mbit with long preamble: SIFS + 192 us + 112 us. */
#define ACK_DUR 314

static int add_ie(u8 *p, u8 id, const u8 *d, int n)
{
    p[0] = id;
    p[1] = (u8)n;
    memcpy(p + 2, d, n);
    return 2 + n;
}

static const u8 RATES[4] = {0x82, 0x84, 0x0b, 0x16}; /* 1,2 basic; 5.5, 11 */

static int build_common_body(u8 *p)
{
    int n = 0;
    memset(p, 0, 8);            /* timestamp (hardware fills in) */
    n += 8;
    wr16le(p + n, 100);         /* beacon interval, TU */
    n += 2;
    wr16le(p + n, 0x0001);      /* capability: ESS */
    n += 2;
    n += add_ie(p + n, 0, (const u8 *)ap.ssid, ap.ssid_len);
    n += add_ie(p + n, 1, RATES, 4);
    u8 ch = (u8)ap.channel;
    n += add_ie(p + n, 3, &ch, 1);
    return n;
}

static void update_beacon(void)
{
    u8 f[512];
    int n = hdr_init(f, 0x80, 0x00, BCAST, ap.bssid, ap.bssid, 0);
    n += build_common_body(f + n);
    /* TIM: DTIM count 0, period 1, bitmap control 0, one bitmap byte for AIDs 0..7, second for 8. */
    u8 tim[5] = {0, 1, 0, 0, 0};
    int any = 0;
    for (int i = 0; i < MAX_STA; i++) {
        sta_t *s = &ap.sta[i];
        if (s->state == ST_ASSOC && s->psq_n > 0) {
            tim[3 + s->aid / 8] |= (u8)(1 << (s->aid % 8));
            any = 1;
        }
    }
    int timlen = (any && tim[4]) ? 5 : 4;
    n += add_ie(f + n, 5, tim, timlen);
    ap.hw.set_beacon(ap.hw.ctx, f, n);
    ap.beacon_dirty = 0;
}

static void send_mgmt(u8 fc0, const u8 *da, const u8 *body, int blen)
{
    DBG("TX mgmt %02x to " MACFMT "\n", fc0, MACARG(da));
    u8 f[600];
    int n = hdr_init(f, fc0, 0x00, da, ap.bssid, ap.bssid, ACK_DUR);
    memcpy(f + n, body, blen);
    n += blen;
    ap.hw.tx(ap.hw.ctx, f, n, RATE_1M, 1, 0);
}

static void send_deauth(const u8 *da, u16 reason)
{
    u8 b[2];
    wr16le(b, reason);
    send_mgmt(0xC0, da, b, 2);
}

static void send_probe_resp(const u8 *da)
{
    DBG("TX probe-resp to " MACFMT "\n", MACARG(da));
    u8 f[512];
    int n = hdr_init(f, 0x50, 0x00, da, ap.bssid, ap.bssid, ACK_DUR);
    n += build_common_body(f + n);
    ap.hw.tx(ap.hw.ctx, f, n, RATE_1M, 1, 1);
}

/* Highest CCK rate in a supported-rates list. */
static int pick_rate(const u8 *ies, int len)
{
    int best = RATE_1M;
    while (len >= 2) {
        int id = ies[0], l = ies[1];
        if (l + 2 > len) break;
        if (id == 1 || id == 50) {
            for (int i = 0; i < l; i++) {
                int r = (ies[2 + i] & 0x7f) * 5;   /* units of 100 kbit/s */
                if ((r == RATE_2M || r == RATE_5M5 || r == RATE_11M) && r > best)
                    best = r;
            }
        }
        ies += 2 + l;
        len -= 2 + l;
    }
    return best;
}

static const u8 *find_ie(const u8 *ies, int len, int want, int *outlen)
{
    while (len >= 2) {
        int id = ies[0], l = ies[1];
        if (l + 2 > len) break;
        if (id == want) { *outlen = l; return ies + 2; }
        ies += 2 + l;
        len -= 2 + l;
    }
    return NULL;
}

/* ---- data path ---- */

static void tx_data_raw(sta_t *s, const u8 *frame, int len)
{
    ap.hw.tx(ap.hw.ctx, frame, len, s ? s->rate : RATE_1M, s != NULL, 0);
}

static int build_data(u8 *f, const u8 *da, const u8 *sa, u16 type, const u8 *p, int len, int ucast)
{
    int n = hdr_init(f, 0x08, 0x02 /* FromDS */, da, ap.bssid, sa, ucast ? ACK_DUR : 0);
    static const u8 snap[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
    memcpy(f + n, snap, 6);
    n += 6;
    wr16be(f + n, type);
    n += 2;
    memcpy(f + n, p, len);
    return n + len;
}

static void deliver_to_sta(sta_t *s, const u8 *frame, int len)
{
    if (s->ps) {
        if (s->psq_n >= PSQ_MAX) {
            free(s->psq[0].data);
            memmove(&s->psq[0], &s->psq[1], sizeof(pkt_t) * (PSQ_MAX - 1));
            s->psq_n--;
        }
        pkt_t *pk = &s->psq[s->psq_n++];
        pk->data = malloc(len);
        memcpy(pk->data, frame, len);
        pk->len = len;
        if (s->psq_n == 1)
            ap.beacon_dirty = 1;
        return;
    }
    tx_data_raw(s, frame, len);
}

void ap_send_eth(const u8 dst[6], const u8 src[6], u16 ethertype, const u8 *payload, int len)
{
    if (len > 2304 - 8)
        return;
    u8 f[2400];
    if (dst[0] & 1) {
        int n = build_data(f, dst, src, ethertype, payload, len, 0);
        ap.hw.tx(ap.hw.ctx, f, n, RATE_1M, 0, 0);
        return;
    }
    sta_t *s = sta_find(dst);
    if (!s || s->state != ST_ASSOC)
        return;
    int n = build_data(f, dst, src, ethertype, payload, len, 1);
    deliver_to_sta(s, f, n);
}

static void flush_psq(sta_t *s)
{
    for (int i = 0; i < s->psq_n; i++) {
        tx_data_raw(s, s->psq[i].data, s->psq[i].len);
        free(s->psq[i].data);
    }
    if (s->psq_n)
        ap.beacon_dirty = 1;
    s->psq_n = 0;
}

static void set_ps(sta_t *s, int ps)
{
    if (s->ps == ps)
        return;
    s->ps = ps;
    DBG("Console " MACFMT " power save %s\n", MACARG(s->mac), ps ? "on" : "off");
    if (!ps)
        flush_psq(s);
}

static void handle_pspoll(const u8 *f, int len)
{
    if (len < 16)
        return;
    sta_t *s = sta_find(f + 10);
    if (!s || s->state != ST_ASSOC)
        return;
    s->last_seen = now_ms();
    if (s->psq_n == 0) {
        /* Nothing buffered: send a null-data frame so it can sleep again. */
        u8 nf[24];
        hdr_init(nf, 0x48, 0x02, s->mac, ap.bssid, ap.bssid, ACK_DUR);
        tx_data_raw(s, nf, 24);
        return;
    }
    pkt_t pk = s->psq[0];
    memmove(&s->psq[0], &s->psq[1], sizeof(pkt_t) * (s->psq_n - 1));
    s->psq_n--;
    if (s->psq_n > 0)
        pk.data[1] |= 0x20;   /* More Data */
    else
        ap.beacon_dirty = 1;
    tx_data_raw(s, pk.data, pk.len);
    free(pk.data);
}

static void handle_data(const u8 *f, int len)
{
    u8 fc0 = f[0], fc1 = f[1];
    if ((fc1 & 0x03) != 0x01)            /* only ToDS frames from stations */
        return;
    if (memcmp(f + 4, ap.bssid, 6))
        return;
    const u8 *sa = f + 10, *da = f + 16;
    sta_t *s = sta_find(sa);
    if (!s || s->state != ST_ASSOC) {
        u64 t = now_ms();
        if (t - ap.last_deauth_sent > 500) {
            send_deauth(sa, 7);          /* class 3 frame from non-associated station */
            ap.last_deauth_sent = t;
        }
        return;
    }
    s->last_seen = now_ms();
    u16 seq = rd16le(f + 22);
    if ((fc1 & 0x08) && s->have_seq && seq == s->last_seq)
        return;                          /* retransmitted duplicate */
    s->last_seq = seq;
    s->have_seq = 1;
    set_ps(s, (fc1 & 0x10) != 0);

    int subtype = (fc0 >> 4) & 0x0f;
    int hdr = 24;
    if (subtype & 0x08) hdr += 2;        /* QoS */
    if (subtype & 0x04) return;          /* null-data: PS signalling only */
    if (fc1 & 0x40) return;              /* encrypted: not supported */
    if (fc1 & 0x04) return;              /* fragments: not supported */
    if (len < hdr + 8)
        return;
    const u8 *p = f + hdr;
    if (!(p[0] == 0xAA && p[1] == 0xAA && p[2] == 0x03))
        return;
    u16 type = rd16be(p + 6);
    const u8 *payload = p + 8;
    int plen = len - hdr - 8;

    if (da[0] & 1) {
        /* Broadcast/multicast: give to the router and repeat on the air. */
        net_rx_eth(sa, da, type, payload, plen);
        if (ap_station_count() > 1) {
            u8 out[2400];
            int n = build_data(out, da, sa, type, payload, plen, 0);
            ap.hw.tx(ap.hw.ctx, out, n, RATE_1M, 0, 0);
        }
        return;
    }
    sta_t *peer = sta_find(da);
    if (peer && peer->state == ST_ASSOC && peer != s) {
        u8 out[2400];
        int n = build_data(out, da, sa, type, payload, plen, 1);
        deliver_to_sta(peer, out, n);
        return;
    }
    net_rx_eth(sa, da, type, payload, plen);
}

/* ---- management ---- */

static void handle_auth(const u8 *f, int len)
{
    if (len < 30)
        return;
    const u8 *sa = f + 10;
    u16 alg = rd16le(f + 24), seq = rd16le(f + 26);
    if (seq != 1)
        return;
    u8 body[6];
    wr16le(body, alg);
    wr16le(body + 2, 2);
    if (alg != 0) {
        wr16le(body + 4, 13);            /* algorithm not supported */
        send_mgmt(0xB0, sa, body, 6);
        return;
    }
    int allowed = ap.approve_all || allow_contains(sa);
    if (!allowed) {
        if (is_denied(sa)) {
            wr16le(body + 4, 1);
            send_mgmt(0xB0, sa, body, 6);
            return;
        }
        if (!ap.pending || memcmp(ap.pending_mac, sa, 6)) {
            if (!ap.pending) {
                memcpy(ap.pending_mac, sa, 6);
                ap.pending = 1;
                ap.pending_since = now_ms();
                LOG("A new console (" MACFMT ") wants to connect.\n", MACARG(sa));
            }
        }
        return;                          /* stay silent; the console retries */
    }
    sta_t *s = sta_find(sa);
    if (s && s->state == ST_ASSOC)
        sta_remove(s, "reconnecting");
    if (!s || s->state == ST_FREE)
        s = sta_new(sa);
    if (!s) {
        wr16le(body + 4, 17);            /* AP full */
        send_mgmt(0xB0, sa, body, 6);
        return;
    }
    s->state = ST_AUTH;
    s->last_seen = now_ms();
    wr16le(body + 4, 0);
    send_mgmt(0xB0, sa, body, 6);
}

static void handle_assoc(const u8 *f, int len, int reassoc)
{
    int fixed = reassoc ? 10 : 4;
    if (len < 24 + fixed)
        return;
    const u8 *sa = f + 10;
    sta_t *s = sta_find(sa);
    u8 body[64];
    int n = 0;
    wr16le(body, 0x0001);
    if (!s) {
        send_deauth(sa, 6);              /* class 2 frame from non-authenticated station */
        return;
    }
    const u8 *ies = f + 24 + fixed;
    int ielen = len - 24 - fixed;
    int sl = 0;
    const u8 *ssid = find_ie(ies, ielen, 0, &sl);
    if (ssid && (sl != ap.ssid_len || memcmp(ssid, ap.ssid, sl))) {
        wr16le(body + 2, 1);
        wr16le(body + 4, 0);
        n = 6;
        n += add_ie(body + n, 1, RATES, 4);
        send_mgmt(reassoc ? 0x30 : 0x10, sa, body, n);
        return;
    }
    int was_assoc = s->state == ST_ASSOC;
    s->rate = pick_rate(ies, ielen);
    s->state = ST_ASSOC;
    s->ps = 0;
    s->have_seq = 0;
    s->last_seen = now_ms();
    wr16le(body + 2, 0);
    wr16le(body + 4, (u16)(s->aid | 0xC000));
    n = 6;
    n += add_ie(body + n, 1, RATES, 4);
    send_mgmt(reassoc ? 0x30 : 0x10, sa, body, n);
    if (!was_assoc) {
        LOG("Console " MACFMT " connected (%d.%d Mbit/s).\n", MACARG(sa), s->rate / 10, s->rate % 10);
        net_station_joined(sa);
    }
}

void ap_rx_frame(const u8 *f, int len, int rssi)
{
    if (len < 10)
        return;
    int type = (f[0] >> 2) & 3, subtype = (f[0] >> 4) & 0xf;
    if (len >= 16 && !(type == 0 && subtype == 8)) {   /* everything except other APs' beacons */
        static const char *mg[16] = {"assoc-req", "assoc-resp", "reassoc-req", "reassoc-resp", "probe-req",
            "probe-resp", "?", "?", "beacon", "atim", "disassoc", "auth", "deauth", "action", "?", "?"};
        const char *name = type == 0 ? mg[subtype] : type == 1 ? (subtype == 10 ? "ps-poll" : "control")
                                                               : "data";
        if (type != 1 || subtype == 10)
            DBG("RX %-12s from " MACFMT " to " MACFMT " len %d rssi %d\n", name, MACARG(f + 10), MACARG(f + 4),
                len, rssi);
    }
    if (type == 1) {                      /* control */
        if (subtype == 10)
            handle_pspoll(f, len);
        return;
    }
    if (len < 24)
        return;
    if (type == 2) {
        handle_data(f, len);
        return;
    }
    if (type != 0)
        return;
    const u8 *a1 = f + 4, *sa = f + 10, *a3 = f + 16;
    if (subtype == 4) {                   /* probe request */
        int sl = 0;
        const u8 *ssid = find_ie(f + 24, len - 24, 0, &sl);
        if (!ssid || sl == 0 || (sl == ap.ssid_len && !memcmp(ssid, ap.ssid, sl)))
            send_probe_resp(sa);
        return;
    }
    if (memcmp(a1, ap.bssid, 6) || memcmp(a3, ap.bssid, 6))
        return;
    sta_t *s = sta_find(sa);
    if (s) s->last_seen = now_ms();
    switch (subtype) {
    case 11: handle_auth(f, len); break;
    case 0: handle_assoc(f, len, 0); break;
    case 2: handle_assoc(f, len, 1); break;
    case 10: /* disassoc */
        if (s) { if (s->state == ST_ASSOC) net_station_left(s->mac); s->state = ST_AUTH; psq_clear(s); }
        if (s) LOG("Console " MACFMT " disconnected.\n", MACARG(sa));
        break;
    case 12: /* deauth */
        if (s) sta_remove(s, "deauthenticated");
        break;
    default: break;
    }
}

/* ---- public ---- */

void ap_init(const ap_hw_t *hw, const u8 bssid[6], const char *ssid, int channel,
             const char *allowlist_path)
{
    memset(&ap, 0, sizeof(ap));
    ap.hw = *hw;
    memcpy(ap.bssid, bssid, 6);
    snprintf(ap.ssid, sizeof(ap.ssid), "%s", ssid);
    ap.ssid_len = (int)strlen(ap.ssid);
    ap.channel = channel;
    snprintf(ap.allow_path, sizeof(ap.allow_path), "%s", allowlist_path);
    update_beacon();
}

void ap_tick(u64 t)
{
    for (int i = 0; i < MAX_STA; i++) {
        sta_t *s = &ap.sta[i];
        if (s->state == ST_FREE)
            continue;
        u64 limit = s->state == ST_ASSOC ? STA_TIMEOUT_MS : 30000;
        if (t - s->last_seen > limit) {
            if (s->state == ST_ASSOC)
                send_deauth(s->mac, 4);  /* inactivity */
            sta_remove(s, "inactive");
        }
    }
    if (ap.pending && t - ap.pending_since > 120000) {
        LOG("No answer for " MACFMT "; ignoring it for now.\n", MACARG(ap.pending_mac));
        ap.pending = 0;
    }
    if (ap.beacon_dirty)
        update_beacon();
}

int ap_station_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_STA; i++)
        if (ap.sta[i].state == ST_ASSOC) n++;
    return n;
}

int ap_is_station(const u8 mac[6])
{
    sta_t *s = sta_find(mac);
    return s && s->state == ST_ASSOC;
}

int ap_pending(u8 mac_out[6])
{
    if (!ap.pending)
        return 0;
    memcpy(mac_out, ap.pending_mac, 6);
    return 1;
}

void ap_decide(const u8 mac[6], int allow)
{
    if (!ap.pending || memcmp(ap.pending_mac, mac, 6))
        return;
    ap.pending = 0;
    if (allow) {
        allow_add(mac);
        LOG("Allowed " MACFMT ". It will connect on its next try.\n", MACARG(mac));
    } else {
        if (ap.ndenied < 16)
            memcpy(ap.denied[ap.ndenied++], mac, 6);
        LOG("Refused " MACFMT " until the program restarts.\n", MACARG(mac));
    }
}

void ap_set_approve_all(int on) { ap.approve_all = on; }
void ap_refresh_beacon(void) { update_beacon(); }

int ap_list(u8 macs[][6], int max)
{
    int n = 0;
    for (int i = 0; i < MAX_STA && n < max; i++)
        if (ap.sta[i].state == ST_ASSOC) memcpy(macs[n++], ap.sta[i].mac, 6);
    return n;
}
