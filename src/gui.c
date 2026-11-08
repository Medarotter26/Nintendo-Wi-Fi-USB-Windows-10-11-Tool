// SPDX-License-Identifier: GPL-2.0-or-later
/* WFC Connector - the window. */
#include "app.h"
#include <commctrl.h>
#include <shellapi.h>

enum {
    ID_STATUS = 100, ID_DETAIL, ID_STARTSTOP, ID_DRIVER, ID_LIGHT, ID_FOLDER,
    ID_SSID, ID_CHANNEL, ID_DNS, ID_APPROVE, ID_SAVE, ID_CONSOLES, ID_LOG,
    ID_SYS_REMOVE = 0x1100, ID_SYS_ABOUT = 0x1110
};

static HWND g_wnd, h_status, h_detail, h_startstop, h_driver, h_light, h_folder;
static HWND h_ssid, h_channel, h_dns, h_approve, h_save, h_consoles, h_log;
static HFONT f_normal, f_bold;
static int g_dpi = 96;
static app_status_t g_last;
static int g_prompt_open;
static u8 g_prompted_mac[6];
static int g_driver_busy;
static HBRUSH g_bg;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex)
{
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)f_normal, TRUE);
    return c;
}

static void log_append(const char *line)
{
    int len = GetWindowTextLengthA(h_log);
    if (len > 120000) {                    /* keep the box from growing forever */
        SendMessageA(h_log, EM_SETSEL, 0, 40000);
        SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)"");
        len = GetWindowTextLengthA(h_log);
    }
    /* Convert \n to \r\n for the edit control. */
    char buf[2600];
    int o = 0;
    for (const char *p = line; *p && o < (int)sizeof(buf) - 3; p++) {
        if (*p == '\n') buf[o++] = '\r';
        buf[o++] = *p;
    }
    buf[o] = 0;
    SendMessageA(h_log, EM_SETSEL, len, len);
    SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)buf);
}

static void load_settings_into_controls(void)
{
    SetWindowTextA(h_ssid, g_cfg.ssid);
    SendMessageA(h_channel, CB_SETCURSEL, (WPARAM)(g_cfg.channel - 1), 0);
    char d[32];
    ip_to_str(g_cfg.dns1, d, sizeof(d));
    SetWindowTextA(h_dns, d);
    SendMessageA(h_approve, BM_SETCHECK, g_cfg.approve_all ? BST_UNCHECKED : BST_CHECKED, 0);
}

static void update_ui(void)
{
    app_status_t s;
    app_get_status(&s);
    static const char *title[] = {"Hotspot off", "Waiting for the connector", "Driver needed", "Starting...",
                                  "Hotspot on", "Problem", "Finding the light setting"};
    SetWindowTextA(h_status, title[s.state]);
    SetWindowTextA(h_detail, s.message);

    int light = s.state == AS_LIGHT_TEST;
    int on = s.state == AS_RUNNING || s.state == AS_STARTING || s.state == AS_WAIT_DEVICE || s.state == AS_NEED_DRIVER;
    SetWindowTextA(h_startstop, light ? "Cancel" : on ? "Stop hotspot" : "Start hotspot");
    SetWindowTextA(h_light, light ? "The light is on!" : "Find light setting...");
    EnableWindow(h_driver, s.state == AS_NEED_DRIVER && !g_driver_busy);
    EnableWindow(h_light, !g_driver_busy);
    EnableWindow(h_save, !light);

    /* Console list */
    SendMessageA(h_consoles, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s.nconsoles; i++) {
        char line[96];
        u32 ip = s.ip[i];
        if (ip)
            snprintf(line, sizeof(line), MACFMT "     %u.%u.%u.%u", MACARG(s.mac[i]), ip >> 24, (ip >> 16) & 255,
                     (ip >> 8) & 255, ip & 255);
        else
            snprintf(line, sizeof(line), MACFMT "     (getting an address)", MACARG(s.mac[i]));
        SendMessageA(h_consoles, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (s.nconsoles == 0)
        SendMessageA(h_consoles, LB_ADDSTRING, 0,
                     (LPARAM)(s.state == AS_RUNNING ? "No consoles connected. On the DS: Nintendo WFC Settings > "
                                                      "Search for an Access Point > WFC-Connector."
                                                    : "No consoles connected."));
    InvalidateRect(h_status, NULL, TRUE);
    g_last = s;

    /* A console waiting for approval: ask once per console. */
    if (s.pending && !g_prompt_open && memcmp(g_prompted_mac, s.pending_mac, 6)) {
        g_prompt_open = 1;
        memcpy(g_prompted_mac, s.pending_mac, 6);
        char q[256];
        snprintf(q, sizeof(q),
                 "A console wants to connect to your hotspot:\n\n    " MACFMT "\n\n"
                 "Allow it? It will be remembered, so this is only asked once.",
                 MACARG(s.pending_mac));
        int r = MessageBoxA(g_wnd, q, APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        app_decide(s.pending_mac, r == IDYES);
        g_prompt_open = 0;
        if (r != IDYES) memset(g_prompted_mac, 0, 6);
    }
}

static void save_settings(void)
{
    char ssid[64], dns[64];
    GetWindowTextA(h_ssid, ssid, sizeof(ssid));
    GetWindowTextA(h_dns, dns, sizeof(dns));
    int ch = (int)SendMessageA(h_channel, CB_GETCURSEL, 0, 0) + 1;
    int ask = SendMessageA(h_approve, BM_GETCHECK, 0, 0) == BST_CHECKED;
    char *p = ssid;
    while (*p == ' ') p++;
    if (!*p || strlen(p) > 32) {
        MessageBoxA(g_wnd, "The network name must be 1 to 32 characters.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    u32 ip;
    if (!str_to_ip(dns, &ip) || !ip) {
        MessageBoxA(g_wnd, "The DNS server must be an address like 178.62.43.212.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    if (ch < 1 || ch > 11) ch = 6;
    app_apply_settings(p, ch, ip, !ask);
    log_append("Settings saved.\n");
}

static void on_command(int id)
{
    app_status_t s;
    app_get_status(&s);
    switch (id) {
    case ID_STARTSTOP:
        if (s.state == AS_LIGHT_TEST) app_light_cancel();
        else if (s.state == AS_STOPPED || s.state == AS_ERROR) app_start();
        else app_stop();
        break;
    case ID_DRIVER:
        if (driver_run_elevated(g_wnd, 0) == 0) {
            g_driver_busy = 1;
            log_append("Installing the driver (Windows asks for permission)...\n");
        } else {
            log_append("The driver was not installed: administrator permission was not given.\n");
        }
        update_ui();
        break;
    case ID_LIGHT:
        if (s.state == AS_LIGHT_TEST) { app_light_seen(); break; }
        if (MessageBoxA(g_wnd,
                        "This switches the hotspot off and tries each way of lighting the connector's light, "
                        "one every 3 seconds.\n\nWatch the connector and click \"The light is on!\" as soon as "
                        "it lights up.\n\nStart now?",
                        APP_NAME, MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
            app_find_light();
        break;
    case ID_FOLDER:
        ShellExecuteA(g_wnd, "open", g_datadir, NULL, NULL, SW_SHOWNORMAL);
        break;
    case ID_SAVE:
        save_settings();
        break;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp));
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == ID_SYS_REMOVE) {
            if (MessageBoxA(h, "Remove the WinUSB driver from the connector?\n\nThe hotspot stops, and you need "
                               "\"Install driver\" again before the next use.", APP_NAME,
                            MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                app_stop();
                Sleep(800);
                if (driver_run_elevated(h, 1) == 0) { g_driver_busy = 1; log_append("Removing the driver...\n"); }
            }
            return 0;
        }
        if ((wp & 0xFFF0) == ID_SYS_ABOUT) {
            MessageBoxA(h, APP_NAME " " APP_VERSION "\n\nRuns the Nintendo Wi-Fi USB Connector as a hotspot on "
                           "64-bit Windows, using Microsoft's built-in WinUSB driver.\n\n"
                           "Free software under the GNU GPL v2 or later. Chip setup is based on the Linux "
                           "rt2500usb driver. Not affiliated with Nintendo.", APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;
    case WM_APP_LOG: {
        char *line = (char *)lp;
        log_append(line);
        free(line);
        return 0;
    }
    case WM_APP_STATUS:
        update_ui();
        return 0;
    case WM_APP_LIGHT_CONFIRM: {
        int r = MessageBoxA(h, "Is the connector's light on right now?", APP_NAME,
                            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        app_light_confirm((int)wp, r == IDYES);
        return 0;
    }
    case WM_APP_LIGHT_DONE:
        MessageBoxA(h, wp ? "Light setting saved. The light now comes on while the hotspot runs."
                          : "No light setting was saved. You can try again any time.",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        app_start();
        return 0;
    case WM_APP_DRIVER_DONE:
        g_driver_busy = 0;
        if (wp == 0) {
            log_append("Driver step finished.\n");
            app_start();
        } else if (wp == 2) {
            MessageBoxA(h, "The connector is not plugged in. Plug it in and try again.", APP_NAME, MB_OK | MB_ICONWARNING);
        } else {
            char m[512], p[MAX_PATH];
            GetTempPathA(sizeof(p), p);
            snprintf(m, sizeof(m), "The driver step did not finish (code %u).\n\nDetails are in %swfc-connector-driver.log\n\n"
                     "You can also do it by hand: Device Manager > right-click \"Nintendo Wi-Fi USB Connector\" > "
                     "Update driver > Browse my computer > Let me pick > Universal Serial Bus devices > WinUsb Device.",
                     (unsigned)wp, p);
            MessageBoxA(h, m, APP_NAME, MB_OK | MB_ICONWARNING);
        }
        update_ui();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == h_status) {
            COLORREF c = RGB(60, 60, 60);
            switch (g_last.state) {
            case AS_RUNNING: c = RGB(16, 124, 16); break;
            case AS_WAIT_DEVICE: case AS_NEED_DRIVER: case AS_STARTING: case AS_LIGHT_TEST: c = RGB(176, 96, 0); break;
            case AS_ERROR: c = RGB(196, 43, 28); break;
            default: break;
            }
            SetTextColor(dc, c);
        }
        return (LRESULT)g_bg;
    }
    case WM_CLOSE:
        if (g_last.nconsoles > 0 &&
            MessageBoxA(h, "A console is still connected. Close the hotspot anyway?", APP_NAME,
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        SetWindowTextA(h_status, "Closing...");
        app_shutdown();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void build_ui(void)
{
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    NONCLIENTMETRICSA ncm;
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    f_normal = CreateFontIndirectA(&ncm.lfMessageFont);
    LOGFONTA lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(14, g_dpi, 72);
    lf.lfWeight = FW_SEMIBOLD;
    f_bold = CreateFontIndirectA(&lf);

    h_status = mk("STATIC", "", SS_LEFT, 16, 12, 600, 28, ID_STATUS, 0);
    SendMessageA(h_status, WM_SETFONT, (WPARAM)f_bold, TRUE);
    h_detail = mk("STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 16, 42, 600, 20, ID_DETAIL, 0);

    h_startstop = mk("BUTTON", "Start hotspot", BS_PUSHBUTTON | WS_TABSTOP, 16, 70, 130, 30, ID_STARTSTOP, 0);
    h_driver = mk("BUTTON", "Install driver", BS_PUSHBUTTON | WS_TABSTOP, 154, 70, 130, 30, ID_DRIVER, 0);
    h_light = mk("BUTTON", "Find light setting...", BS_PUSHBUTTON | WS_TABSTOP, 292, 70, 160, 30, ID_LIGHT, 0);
    h_folder = mk("BUTTON", "Open data folder", BS_PUSHBUTTON | WS_TABSTOP, 460, 70, 156, 30, ID_FOLDER, 0);

    mk("BUTTON", "Settings", BS_GROUPBOX, 12, 110, 608, 132, 0, 0);
    mk("STATIC", "Network name:", SS_LEFT, 28, 136, 100, 20, 0, 0);
    h_ssid = mk("EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 130, 133, 200, 24, ID_SSID, 0);
    mk("STATIC", "Channel:", SS_LEFT, 350, 136, 60, 20, 0, 0);
    h_channel = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 412, 132, 70, 300, ID_CHANNEL, 0);
    for (int i = 1; i <= 11; i++) {
        char b[8];
        snprintf(b, sizeof(b), "%d", i);
        SendMessageA(h_channel, CB_ADDSTRING, 0, (LPARAM)b);
    }
    mk("STATIC", "DNS server:", SS_LEFT, 28, 170, 100, 20, 0, 0);
    h_dns = mk("EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 130, 167, 200, 24, ID_DNS, 0);
    mk("STATIC", "Decides which online service the DS reaches. Default: 178.62.43.212", SS_LEFT, 340, 162, 270, 34, 0, 0);
    h_approve = mk("BUTTON", "Ask me before a new console can join", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 204, 300, 24,
                   ID_APPROVE, 0);
    h_save = mk("BUTTON", "Save settings", BS_PUSHBUTTON | WS_TABSTOP, 486, 202, 120, 28, ID_SAVE, 0);

    mk("BUTTON", "Connected consoles", BS_GROUPBOX, 12, 250, 608, 96, 0, 0);
    h_consoles = mk("LISTBOX", "", LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_VSCROLL, 24, 272, 584, 64, ID_CONSOLES,
                    WS_EX_CLIENTEDGE);

    mk("STATIC", "Activity", SS_LEFT, 16, 354, 200, 20, 0, 0);
    h_log = mk("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 12, 374, 608, 190, ID_LOG,
               WS_EX_CLIENTEDGE);
    SendMessageA(h_log, EM_SETLIMITTEXT, 0, 0);

    HMENU sys = GetSystemMenu(g_wnd, FALSE);
    AppendMenuA(sys, MF_SEPARATOR, 0, NULL);
    AppendMenuA(sys, MF_STRING, ID_SYS_REMOVE, "Remove connector driver...");
    AppendMenuA(sys, MF_STRING, ID_SYS_ABOUT, "About " APP_NAME);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    int cli = app_cli(__argc, __argv);
    if (cli >= 0) return cli;

    HANDLE mtx = CreateMutexA(NULL, TRUE, "Local\\WFCConnectorWin11");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowA("WFCConnectorWnd", NULL);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    (void)mtx;

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_bg = GetSysColorBrush(COLOR_BTNFACE);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = g_bg;
    wc.lpszClassName = "WFCConnectorWnd";
    RegisterClassExA(&wc);

    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    RECT r = {0, 0, MulDiv(632, dpi, 96), MulDiv(576, dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExA(0, "WFCConnectorWnd", APP_NAME " - Nintendo Wi-Fi USB Connector for Windows 11", style,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    build_ui();
    app_init(g_wnd);
    load_settings_into_controls();
    update_ui();
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    app_start();

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    return 0;
}
