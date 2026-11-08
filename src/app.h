// SPDX-License-Identifier: GPL-2.0-or-later
/* Engine <-> window interface. The engine (hotspot, router, connector
 * driver) runs on its own thread; the window only talks to it through
 * these calls and the WM_APP_* messages it posts back. */
#ifndef APP_H
#define APP_H
#include "common.h"

#define APP_NAME    "WFC Connector"
#define APP_VERSION "1.1.0"

enum {
    WM_APP_LOG = WM_APP + 1,      /* lParam: malloc'd char* line (window frees it) */
    WM_APP_STATUS,                /* status snapshot changed */
    WM_APP_LIGHT_CONFIRM,         /* wParam: light setting to confirm with the user */
    WM_APP_LIGHT_DONE,            /* wParam: saved setting or 0 */
    WM_APP_DRIVER_DONE            /* wParam: installer exit code */
};

typedef enum {
    AS_STOPPED = 0,
    AS_WAIT_DEVICE,     /* connector not plugged in */
    AS_NEED_DRIVER,     /* plugged in, WinUSB not installed */
    AS_STARTING,
    AS_RUNNING,
    AS_ERROR,
    AS_LIGHT_TEST
} app_state_t;

#define MAX_LIST 8
typedef struct {
    app_state_t state;
    char message[256];
    int nconsoles;
    u8 mac[MAX_LIST][6];
    u32 ip[MAX_LIST];          /* host order, 0 = none yet */
    int pending;               /* a console is waiting for approval */
    u8 pending_mac[6];
    int light_step, light_total;
    char light_desc[64];
} app_status_t;

/* Paths: settings and logs live next to the exe when that folder is
 * writable, otherwise in %LOCALAPPDATA%\WFC Connector. */
extern char g_datadir[MAX_PATH];

void app_init(HWND notify);
void app_shutdown(void);
void app_start(void);
void app_stop(void);
void app_get_status(app_status_t *out);
void app_decide(const u8 mac[6], int allow);
/* Settings changes are saved at once; a name/channel change restarts the hotspot. */
void app_apply_settings(const char *ssid, int channel, u32 dns_net_order, int approve_all);
void app_find_light(void);
void app_light_seen(void);                 /* "the light is on now" */
void app_light_confirm(int step, int yes); /* answer to WM_APP_LIGHT_CONFIRM */
void app_light_cancel(void);
int app_is_running(void);

/* Settings helpers shared with the window. */
void ip_to_str(u32 net_order, char *out, size_t n);
int str_to_ip(const char *s, u32 *net_order);

/* Command-line tools (no window): returns -1 if argv has none of them. */
int app_cli(int argc, char **argv);

/* driver.c */
typedef enum { DRVST_NOT_PLUGGED = 0, DRVST_NEEDS_INSTALL, DRVST_OK } drv_state_t;
drv_state_t driver_state(void);
int driver_elevated_main(int remove);     /* runs in the elevated copy */
/* Starts the elevated copy; returns 0 if it was launched, waits on a thread
 * and posts WM_APP_DRIVER_DONE to hwnd when it finishes. */
int driver_run_elevated(HWND hwnd, int remove);

#endif
