// SPDX-License-Identifier: GPL-2.0-or-later
/* Engine: settings, logging and the hotspot session.
 * Runs on a worker thread started by app_init(). */
#include "app.h"
#include "rt2570.h"
#include "ap.h"
#include "net.h"

int g_verbose = 0;
config_t g_cfg;
char g_datadir[MAX_PATH];

static char g_ini[MAX_PATH], g_allow[MAX_PATH], g_logpath[MAX_PATH];
static HWND g_notify;
static HANDLE g_thread, g_wake_event;
static volatile LONG g_quit, g_want_run, g_restart;
static CRITICAL_SECTION g_lock;          /* g_status and the command fields below */
static app_status_t g_status;
static int g_status_dirty;

/* commands from the window, applied by the worker */
static struct { u8 mac[6]; int allow; } g_decisions[8];
static int g_ndecisions;
static int g_cfg_changed;

/* ---------------- logging ---------------- */

static FILE *g_logf;
static CRITICAL_SECTION g_loglock;
static int g_loglock_ready;

void log_msg(int level, const char *fmt, ...)
{
    if (!g_loglock_ready) { InitializeCriticalSection(&g_loglock); g_loglock_ready = 1; }
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_loglock);
    if (g_logf) {
        fprintf(g_logf, "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                text[0] == '\n' ? text + 1 : text);
        fflush(g_logf);
    }
    LeaveCriticalSection(&g_loglock);
    if (level <= 1 || g_verbose) {
        const char *t = text;
        while (*t == '\n') t++;
        if (!*t) return;
        if (g_notify) {
            char *line = malloc(strlen(t) + 16);
            sprintf(line, "[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, t);
            if (!PostMessageA(g_notify, WM_APP_LOG, 0, (LPARAM)line)) free(line);
        } else {
            printf("[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, t);
            fflush(stdout);
        }
    }
}

/* ---------------- settings ---------------- */

void ip_to_str(u32 net_order, char *out, size_t n)
{
    if (!net_order) { snprintf(out, n, "none"); return; }
    u32 h = ntohl(net_order);
    snprintf(out, n, "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
}

int str_to_ip(const char *s, u32 *net_order)
{
    unsigned a, b, c, d;
    char extra;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || !_stricmp(s, "none")) { *net_order = 0; return 1; }
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *net_order = htonl((a << 24) | (b << 16) | (c << 8) | d);
    return 1;
}

static void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.ssid, sizeof(g_cfg.ssid), "WFC-Connector");
    g_cfg.channel = 6;
    str_to_ip("178.62.43.212", &g_cfg.dns1);
    g_cfg.net_base = 0xC0A84D00u;              /* 192.168.77.0 */
    g_cfg.tx_power = -1;
    g_cfg.led = 4;          /* LED outputs link + activity, inverted: lights the connector */
}

static void config_save(void)
{
    FILE *f = fopen(g_ini, "w");
    if (!f) { LOG("Could not save %s\n", g_ini); return; }
    char d1[32], d2[32], pw[16];
    ip_to_str(g_cfg.dns1, d1, sizeof(d1));
    ip_to_str(g_cfg.dns2, d2, sizeof(d2));
    if (g_cfg.tx_power < 0) snprintf(pw, sizeof(pw), "auto");
    else snprintf(pw, sizeof(pw), "%d", g_cfg.tx_power);
    char ledstr[16];
    if (g_cfg.led < 0) snprintf(ledstr, sizeof(ledstr), "off");
    else snprintf(ledstr, sizeof(ledstr), "%d", g_cfg.led);
    fprintf(f,
        "; WFC Connector settings. The program window edits these for you.\n"
        "ssid=%s\nchannel=%d\ndns=%s\ndns2=%s\napprove=%s\ntxpower=%s\nled=%s\n",
        g_cfg.ssid, g_cfg.channel, d1, d2, g_cfg.approve_all ? "all" : "ask", pw, ledstr);
    fclose(f);
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

static void config_load(void)
{
    config_defaults();
    FILE *f = fopen(g_ini, "r");
    if (!f) { config_save(); return; }
    char line[256];
    int keys = 0;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p, *v = eq + 1;
        trim(k);
        while (*v == ' ' || *v == '\t') v++;
        keys++;
        if (!_stricmp(k, "ssid")) { if (*v) snprintf(g_cfg.ssid, sizeof(g_cfg.ssid), "%.32s", v); }
        else if (!_stricmp(k, "channel")) { int c = atoi(v); if (c >= 1 && c <= 13) g_cfg.channel = c; }
        else if (!_stricmp(k, "dns") || !_stricmp(k, "dns1")) str_to_ip(v, &g_cfg.dns1);
        else if (!_stricmp(k, "dns2")) str_to_ip(v, &g_cfg.dns2);
        else if (!_stricmp(k, "approve")) g_cfg.approve_all = !_stricmp(v, "all");
        else if (!_stricmp(k, "txpower")) {
            g_cfg.tx_power = (!_stricmp(v, "auto") || !*v) ? -1 : atoi(v);
            if (g_cfg.tx_power > 31) g_cfg.tx_power = 31;
        } else if (!_stricmp(k, "led")) {
            /* "off" keeps the light dark; 0 (written by 1.1.0) means the default. */
            if (!_stricmp(v, "off")) g_cfg.led = -1;
            else if (atoi(v) > 0) g_cfg.led = atoi(v);
        }
    }
    fclose(f);
    if (!keys) config_save();          /* new or empty file: write the defaults */
}

/* Settings next to the exe if that folder is writable, else in LocalAppData. */
static void pick_datadir(void)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    char probe[MAX_PATH];
    snprintf(probe, sizeof(probe), "%s\\connector.ini", exe);
    FILE *f = fopen(probe, "a");
    if (f) {
        fclose(f);
        snprintf(g_datadir, sizeof(g_datadir), "%s", exe);
    } else {
        char la[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof(la));
        if (!n || n >= sizeof(la)) snprintf(la, sizeof(la), "%s", exe);
        snprintf(g_datadir, sizeof(g_datadir), "%s\\WFC Connector", la);
        CreateDirectoryA(g_datadir, NULL);
    }
    snprintf(g_ini, sizeof(g_ini), "%s\\connector.ini", g_datadir);
    snprintf(g_allow, sizeof(g_allow), "%s\\allowed_consoles.txt", g_datadir);
    snprintf(g_logpath, sizeof(g_logpath), "%s\\connector-log.txt", g_datadir);
}

static void open_log(void)
{
    char prev[MAX_PATH];
    snprintf(prev, sizeof(prev), "%s\\connector-log-previous.txt", g_datadir);
    MoveFileExA(g_logpath, prev, MOVEFILE_REPLACE_EXISTING);
    g_logf = fopen(g_logpath, "w");
}

/* ---------------- status ---------------- */

static void set_state(app_state_t s, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    if (g_status.state != s || strcmp(g_status.message, msg)) {
        g_status.state = s;
        snprintf(g_status.message, sizeof(g_status.message), "%s", msg);
        g_status_dirty = 1;
    }
    LeaveCriticalSection(&g_lock);
}

static void refresh_consoles(void)
{
    u8 macs[MAX_LIST][6];
    int n = ap_list(macs, MAX_LIST);
    u8 pm[6];
    int pend = ap_pending(pm);
    EnterCriticalSection(&g_lock);
    int changed = n != g_status.nconsoles || pend != g_status.pending ||
                  (pend && memcmp(pm, g_status.pending_mac, 6));
    for (int i = 0; i < n; i++) {
        u32 ip = 0;
        net_lease_for(macs[i], &ip);
        if (memcmp(g_status.mac[i], macs[i], 6) || g_status.ip[i] != ip) changed = 1;
        memcpy(g_status.mac[i], macs[i], 6);
        g_status.ip[i] = ip;
    }
    g_status.nconsoles = n;
    g_status.pending = pend;
    if (pend) memcpy(g_status.pending_mac, pm, 6);
    if (changed) g_status_dirty = 1;
    LeaveCriticalSection(&g_lock);
}

static void post_status_if_dirty(void)
{
    EnterCriticalSection(&g_lock);
    int d = g_status_dirty;
    g_status_dirty = 0;
    LeaveCriticalSection(&g_lock);
    if (d && g_notify) PostMessageA(g_notify, WM_APP_STATUS, 0, 0);
}

void app_get_status(app_status_t *out)
{
    EnterCriticalSection(&g_lock);
    *out = g_status;
    LeaveCriticalSection(&g_lock);
}

int app_is_running(void)
{
    app_status_t s;
    app_get_status(&s);
    return s.state == AS_RUNNING;
}

/* ---------------- RX queue from the USB thread ---------------- */

#define RXQ 256
typedef struct { u8 data[2400]; int len; int rssi; } rxent_t;
static rxent_t *rxq;
static volatile LONG rx_head, rx_tail;
static CRITICAL_SECTION rx_lock;
static SOCKET wake_rx = INVALID_SOCKET, wake_tx = INVALID_SOCKET;
static struct sockaddr_in wake_addr;
static volatile LONG g_rx_frames, g_rx_bad;

static void wake_worker(void)
{
    char b = 1;
    if (wake_tx != INVALID_SOCKET)
        sendto(wake_tx, &b, 1, 0, (struct sockaddr *)&wake_addr, sizeof(wake_addr));
    if (g_wake_event) SetEvent(g_wake_event);
}

static void on_usb_rx(void *ctx, const u8 *buf, ULONG got)
{
    (void)ctx;
    const u8 *frame;
    int rssi;
    int len = rt_parse_rx(buf, (int)got, &frame, &rssi);
    if (len <= 0 || len > 2400) { InterlockedIncrement(&g_rx_bad); return; }
    InterlockedIncrement(&g_rx_frames);
    EnterCriticalSection(&rx_lock);
    LONG next = (rx_head + 1) % RXQ;
    if (next != rx_tail) {
        memcpy(rxq[rx_head].data, frame, len);
        rxq[rx_head].len = len;
        rxq[rx_head].rssi = rssi;
        rx_head = next;
    }
    LeaveCriticalSection(&rx_lock);
    char b = 1;
    sendto(wake_tx, &b, 1, 0, (struct sockaddr *)&wake_addr, sizeof(wake_addr));
}

static void drain_rx(void)
{
    static rxent_t e;
    for (;;) {
        EnterCriticalSection(&rx_lock);
        if (rx_tail == rx_head) { LeaveCriticalSection(&rx_lock); break; }
        e = rxq[rx_tail];
        rx_tail = (rx_tail + 1) % RXQ;
        LeaveCriticalSection(&rx_lock);
        ap_rx_frame(e.data, e.len, e.rssi);
    }
}

static int make_wake_sockets(void)
{
    wake_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    wake_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&wake_addr, 0, sizeof(wake_addr));
    wake_addr.sin_family = AF_INET;
    wake_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(wake_rx, (struct sockaddr *)&wake_addr, sizeof(wake_addr)) != 0) return -1;
    int l = sizeof(wake_addr);
    getsockname(wake_rx, (struct sockaddr *)&wake_addr, &l);
    u_long nb = 1;
    ioctlsocket(wake_rx, FIONBIO, &nb);
    return 0;
}

/* ---------------- hardware glue ---------------- */

static rt_t g_rt;
static int hw_tx(void *ctx, const u8 *f, int len, int rate, int ack, int ts) { return rt_tx((rt_t *)ctx, f, len, rate, ack, ts); }
static int hw_beacon(void *ctx, const u8 *f, int len) { return rt_set_beacon((rt_t *)ctx, f, len); }

/* Opens the connector; keeps trying while it is unplugged or lacks the driver. */
static int open_connector(void)
{
    char err[512];
    app_state_t last = AS_STOPPED;
    while (!g_quit && g_want_run && !g_restart) {
        g_usb_dead = 0;
        if (rt_init(&g_rt, err, sizeof(err)) == 0)
            return 0;
        app_state_t s = strstr(err, "not plugged in") ? AS_WAIT_DEVICE
                      : strstr(err, "driver is not installed") ? AS_NEED_DRIVER : AS_ERROR;
        if (s != last) {
            if (s == AS_WAIT_DEVICE) {
                set_state(s, "Plug the Nintendo Wi-Fi USB Connector into this PC.");
                LOG("Waiting for the connector to be plugged in...\n");
            } else if (s == AS_NEED_DRIVER) {
                set_state(s, "The connector needs its driver. Click \"Install driver\".");
                LOG("The connector is plugged in but has no driver yet.\n");
            } else {
                set_state(s, "%s", err);
                LOG("%s\n", err);
            }
            post_status_if_dirty();
            last = s;
        }
        WaitForSingleObject(g_wake_event, 1500);
    }
    return -1;
}

static void apply_pending_commands(void)
{
    EnterCriticalSection(&g_lock);
    int nd = g_ndecisions;
    struct { u8 mac[6]; int allow; } d[8];
    memcpy(d, g_decisions, sizeof(d));
    g_ndecisions = 0;
    int cfg = g_cfg_changed;
    g_cfg_changed = 0;
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < nd; i++) ap_decide(d[i].mac, d[i].allow);
    if (cfg) ap_set_approve_all(g_cfg.approve_all);
}

static void run_session(void)
{
    set_state(AS_STARTING, "Starting...");
    post_status_if_dirty();
    if (open_connector() != 0)
        return;
    LOG("Connector found: MAC " MACFMT ", radio %s.\n", MACARG(g_rt.mac), rt_rf_name(g_rt.rf_type));
    if (rt_start_ap(&g_rt, g_cfg.channel, g_cfg.tx_power) != 0) {
        LOG("The connector did not respond during start-up. Unplug it and plug it back in.\n");
        set_state(AS_ERROR, "The connector did not respond. Unplug it and plug it back in.");
        post_status_if_dirty();
        rt_stop(&g_rt);
        WaitForSingleObject(g_wake_event, 3000);
        return;
    }
    rt_led_apply(&g_rt, g_cfg.led);
    ap_hw_t hw = { &g_rt, hw_tx, hw_beacon };
    ap_init(&hw, g_rt.mac, g_cfg.ssid, g_cfg.channel, g_allow);
    ap_set_approve_all(g_cfg.approve_all);
    net_init(g_rt.mac);
    rx_head = rx_tail = 0;
    usb_start_reader(&g_rt.usb, on_usb_rx, NULL);
    char d1[32];
    ip_to_str(g_cfg.dns1, d1, sizeof(d1));
    LOG("Hotspot \"%s\" is on (channel %d, DNS %s).\n", g_cfg.ssid, g_cfg.channel, d1);
    set_state(AS_RUNNING, "Hotspot \"%s\" is on, channel %d.", g_cfg.ssid, g_cfg.channel);

    u64 last_tick = 0;
    while (!g_quit && g_want_run && !g_restart && !g_usb_dead) {
        fd_set r, w, e;
        FD_ZERO(&r); FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(wake_rx, &r);
        net_fill_fdsets(&r, &w, &e);
        struct timeval tv = {0, 50000};
        int n = select(0, &r, &w, &e, &tv);
        if (n < 0) Sleep(10);
        if (n > 0 && FD_ISSET(wake_rx, &r)) {
            char b[256];
            while (recv(wake_rx, b, sizeof(b), 0) > 0) {}
        }
        drain_rx();
        if (n > 0) net_handle_fdsets(&r, &w, &e);
        u64 t = now_ms();
        if (t - last_tick >= 100) {
            last_tick = t;
            apply_pending_commands();
            net_tick(t);
            ap_tick(t);
            refresh_consoles();
            post_status_if_dirty();
        }
    }
    int unplugged = g_usb_dead && !g_quit;
    net_shutdown();
    rt_stop(&g_rt);
    EnterCriticalSection(&g_lock);
    g_status.nconsoles = 0;
    g_status.pending = 0;
    LeaveCriticalSection(&g_lock);
    if (unplugged) {
        LOG("The connector was unplugged.\n");
    } else {
        LOG("Hotspot stopped; the connector's radio is off.\n");
    }
}

/* ---------------- worker thread ---------------- */

static DWORD WINAPI worker_main(LPVOID arg)
{
    (void)arg;
    while (!g_quit) {
        if (g_want_run) {
            InterlockedExchange(&g_restart, 0);
            run_session();
            if (!g_want_run && !g_quit) {
                set_state(AS_STOPPED, "Hotspot is off.");
                post_status_if_dirty();
            }
            continue;
        }
        set_state(AS_STOPPED, "Hotspot is off.");
        post_status_if_dirty();
        WaitForSingleObject(g_wake_event, 1000);
    }
    return 0;
}

void app_init(HWND notify)
{
    g_notify = notify;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&rx_lock);
    pick_datadir();
    open_log();
    config_load();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    rxq = calloc(RXQ, sizeof(rxent_t));
    make_wake_sockets();
    g_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    LOG(APP_NAME " " APP_VERSION " started. Settings: %s\n", g_ini);
    g_thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
}

void app_shutdown(void)
{
    InterlockedExchange(&g_quit, 1);
    wake_worker();
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
}

void app_start(void) { InterlockedExchange(&g_want_run, 1); wake_worker(); }
void app_stop(void) { InterlockedExchange(&g_want_run, 0); wake_worker(); }

void app_decide(const u8 mac[6], int allow)
{
    EnterCriticalSection(&g_lock);
    if (g_ndecisions < 8) {
        memcpy(g_decisions[g_ndecisions].mac, mac, 6);
        g_decisions[g_ndecisions].allow = allow;
        g_ndecisions++;
    }
    LeaveCriticalSection(&g_lock);
    wake_worker();
}

void app_apply_settings(const char *ssid, int channel, u32 dns, int approve_all)
{
    int restart = strcmp(ssid, g_cfg.ssid) != 0 || channel != g_cfg.channel;
    u32 olddns = g_cfg.dns1;
    EnterCriticalSection(&g_lock);
    snprintf(g_cfg.ssid, sizeof(g_cfg.ssid), "%.32s", ssid);
    g_cfg.channel = channel;
    g_cfg.dns1 = dns;            /* net.c reads it on every lookup */
    g_cfg.approve_all = approve_all;
    g_cfg_changed = 1;
    LeaveCriticalSection(&g_lock);
    config_save();
    if (dns != olddns) {
        char d[32];
        ip_to_str(dns, d, sizeof(d));
        LOG("DNS is now %s. Connected consoles use it right away.\n", d);
    }
    if (restart && g_want_run) {
        LOG("Restarting the hotspot with the new name/channel...\n");
        InterlockedExchange(&g_restart, 1);
    }
    wake_worker();
}

/* ---------------- command-line tools ---------------- */

static void attach_console(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

int app_cli(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install-driver")) return driver_elevated_main(0);
        if (!strcmp(argv[i], "--remove-driver")) return driver_elevated_main(1);
        if (!strcmp(argv[i], "--set-dns")) {
            attach_console();
            pick_datadir();
            config_load();
            u32 ip;
            const char *v = i + 1 < argc ? argv[i + 1] : "";
            char old[32], now[32];
            ip_to_str(g_cfg.dns1, old, sizeof(old));
            if (!*v) { printf("The DNS is %s.\n", old); return 0; }
            if (!str_to_ip(v, &ip) || !ip) { printf("\"%s\" is not a valid address.\n", v); return 1; }
            g_cfg.dns1 = ip;
            config_save();
            ip_to_str(ip, now, sizeof(now));
            printf("DNS changed from %s to %s.\n", old, now);
            return 0;
        }
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            /* Diagnostic run without a window: hotspot for N seconds. */
            attach_console();
            int secs = atoi(argv[i + 1]);
            for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "-v")) g_verbose = 1;
            app_init(NULL);
            app_start();
            Sleep((DWORD)secs * 1000);
            LOG("Test run: %ld frames received, %ld rejected.\n", g_rx_frames, g_rx_bad);
            app_shutdown();
            return 0;
        }
    }
    return -1;
}
