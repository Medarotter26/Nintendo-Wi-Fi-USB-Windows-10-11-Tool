// SPDX-License-Identifier: GPL-2.0-or-later
/* Binds Microsoft's in-box, signed WinUSB driver to the Nintendo Wi-Fi
 * USB Connector (USB\VID_0411&PID_008B) - the same as Device Manager >
 * Update driver > "Let me pick" > Universal Serial Bus devices > WinUsb
 * Device. Nothing unsigned is installed and no certificates are added.
 * The window runs this in an elevated copy of the exe. */
#include "app.h"
#include <setupapi.h>
#include <shellapi.h>

#define HWID "USB\\VID_0411&PID_008B"
#define USBDEVICE_CLASS "{88BAE032-5A81-49F0-BC3D-A4FF138216D6}"
#define OUR_IF_GUID "{6E1F5A43-2B8C-4D0A-9B57-1C0A0B8B0411}"

typedef BOOL (WINAPI *DiInstallDevice_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
typedef BOOL (WINAPI *DiUninstallDevice_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);

/* Exit codes of the elevated copy. */
enum { DRV_EXIT_OK = 0, DRV_EXIT_FAIL = 1, DRV_EXIT_NOT_PLUGGED = 2 };

static FILE *dlog;
static void dsay(const char *fmt, ...)
{
    if (!dlog) {
        char p[MAX_PATH];
        GetTempPathA(sizeof(p), p);
        strncat(p, "wfc-connector-driver.log", sizeof(p) - strlen(p) - 1);
        dlog = fopen(p, "a");
    }
    if (!dlog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(dlog, fmt, ap);
    va_end(ap);
    fflush(dlog);
}

static int find_device(HDEVINFO *out_di, SP_DEVINFO_DATA *dd)
{
    HDEVINFO di = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return 0;
    dd->cbSize = sizeof(*dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, dd); i++) {
        char ids[2048];
        if (!SetupDiGetDeviceRegistryPropertyA(di, dd, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof(ids) - 2, NULL))
            continue;
        ids[sizeof(ids) - 2] = ids[sizeof(ids) - 1] = 0;
        for (char *p = ids; *p; p += strlen(p) + 1) {
            size_t n = strlen(HWID);
            if (!_strnicmp(p, HWID, n) && (p[n] == 0 || p[n] == '&')) { *out_di = di; return 1; }
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return 0;
}

static int uses_winusb(HDEVINFO di, SP_DEVINFO_DATA *dd)
{
    char svc[128] = "";
    return SetupDiGetDeviceRegistryPropertyA(di, dd, SPDRP_SERVICE, NULL, (BYTE *)svc, sizeof(svc), NULL) &&
           !_stricmp(svc, "WinUSB");
}

drv_state_t driver_state(void)
{
    HDEVINFO di;
    SP_DEVINFO_DATA dd;
    if (!find_device(&di, &dd)) return DRVST_NOT_PLUGGED;
    int ok = uses_winusb(di, &dd);
    SetupDiDestroyDeviceInfoList(di);
    return ok ? DRVST_OK : DRVST_NEEDS_INSTALL;
}

static int do_remove(void)
{
    HDEVINFO di;
    SP_DEVINFO_DATA dd;
    if (!find_device(&di, &dd)) return DRV_EXIT_NOT_PLUGGED;
    HMODULE nd = LoadLibraryA("newdev.dll");
    DiUninstallDevice_t un = nd ? (DiUninstallDevice_t)(void *)GetProcAddress(nd, "DiUninstallDevice") : NULL;
    BOOL reboot = FALSE;
    if (!un || !un(NULL, di, &dd, 0, &reboot)) {
        dsay("Removing the driver failed (error %lu).\n", GetLastError());
        return DRV_EXIT_FAIL;
    }
    dsay("Driver removed.\n");
    return DRV_EXIT_OK;
}

static int do_install(void)
{
    HDEVINFO di;
    SP_DEVINFO_DATA dd;
    if (!find_device(&di, &dd)) { dsay("Connector not plugged in.\n"); return DRV_EXIT_NOT_PLUGGED; }
    if (uses_winusb(di, &dd)) { dsay("Already uses WinUSB.\n"); return DRV_EXIT_OK; }

    /* Tell WinUSB which interface GUID to publish for this device. */
    HKEY k = SetupDiCreateDevRegKeyA(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, NULL, NULL);
    if (k != INVALID_HANDLE_VALUE) {
        static const char guids[] = OUR_IF_GUID "\0";
        RegSetValueExA(k, "DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE *)guids, sizeof(guids));
        RegCloseKey(k);
    }
    /* "Universal Serial Bus devices" class, then list the drivers in Windows' own winusb.inf.
     * Wide-character calls: DiInstallDevice only takes the Unicode driver record. */
    SetupDiSetDeviceRegistryPropertyA(di, &dd, SPDRP_CLASSGUID, (const BYTE *)USBDEVICE_CLASS, sizeof(USBDEVICE_CLASS));
    SP_DEVINSTALL_PARAMS_W ip;
    ip.cbSize = sizeof(ip);
    SetupDiGetDeviceInstallParamsW(di, &dd, &ip);
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    _snwprintf(ip.DriverPath, MAX_PATH, L"%ls\\INF\\winusb.inf", windir);
    ip.DriverPath[MAX_PATH - 1] = 0;
    ip.Flags |= DI_ENUMSINGLEINF;
    ip.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    SetupDiSetDeviceInstallParamsW(di, &dd, &ip);
    if (!SetupDiBuildDriverInfoList(di, &dd, SPDIT_CLASSDRIVER)) {
        dsay("Could not read winusb.inf (error %lu).\n", GetLastError());
        return DRV_EXIT_FAIL;
    }
    SP_DRVINFO_DATA_W drv, chosen;
    int found = 0;
    drv.cbSize = sizeof(drv);
    for (DWORD i = 0; SetupDiEnumDriverInfoW(di, &dd, SPDIT_CLASSDRIVER, i, &drv); i++) {
        static BYTE buf[8192];
        SP_DRVINFO_DETAIL_DATA_W *det = (SP_DRVINFO_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof(*det);
        DWORD need;
        if ((SetupDiGetDriverInfoDetailW(di, &dd, &drv, det, sizeof(buf), &need) ||
             GetLastError() == ERROR_INSUFFICIENT_BUFFER) && !_wcsicmp(det->SectionName, L"WINUSB")) {
            chosen = drv;
            found = 1;
            break;
        }
    }
    if (!found) { dsay("WinUSB entry not found in winusb.inf.\n"); return DRV_EXIT_FAIL; }
    if (!SetupDiSetSelectedDriverW(di, &dd, &chosen)) {
        dsay("Selecting WinUSB failed (error %lu).\n", GetLastError());
        return DRV_EXIT_FAIL;
    }
    HMODULE nd = LoadLibraryA("newdev.dll");
    DiInstallDevice_t inst = nd ? (DiInstallDevice_t)(void *)GetProcAddress(nd, "DiInstallDevice") : NULL;
    BOOL reboot = FALSE;
    if (!inst || !inst(NULL, di, &dd, &chosen, 0, &reboot)) {
        dsay("DiInstallDevice failed (error 0x%lx).\n", GetLastError());
        return DRV_EXIT_FAIL;
    }
    SetupDiDestroyDeviceInfoList(di);
    if (driver_state() == DRVST_OK) { dsay("Installed.\n"); return DRV_EXIT_OK; }
    dsay("Install reported success but the device is not on WinUSB yet.\n");
    return DRV_EXIT_FAIL;
}

int driver_elevated_main(int remove)
{
    dsay("--- %s\n", remove ? "remove" : "install");
    return remove ? do_remove() : do_install();
}

typedef struct { HWND hwnd; HANDLE proc; } waitctx_t;

static DWORD WINAPI wait_child(LPVOID arg)
{
    waitctx_t *w = arg;
    WaitForSingleObject(w->proc, INFINITE);
    DWORD code = DRV_EXIT_FAIL;
    GetExitCodeProcess(w->proc, &code);
    CloseHandle(w->proc);
    PostMessageA(w->hwnd, WM_APP_DRIVER_DONE, (WPARAM)code, 0);
    free(w);
    return 0;
}

int driver_run_elevated(HWND hwnd, int remove)
{
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd;
    sei.lpVerb = "runas";
    sei.lpFile = self;
    sei.lpParameters = remove ? "--remove-driver" : "--install-driver";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei) || !sei.hProcess)
        return -1;                       /* the user said No to the permission prompt */
    waitctx_t *w = malloc(sizeof(*w));
    w->hwnd = hwnd;
    w->proc = sei.hProcess;
    HANDLE t = CreateThread(NULL, 0, wait_child, w, 0, NULL);
    if (t) CloseHandle(t);
    return 0;
}
