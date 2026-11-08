// SPDX-License-Identifier: GPL-2.0-or-later
/* WinUSB access to the Nintendo Wi-Fi USB Connector (USB 0411:008B).
 * The device has no vendor driver on 64-bit Windows; the companion
 * installer binds Microsoft's in-box WinUSB driver to it instead. */
#include "wfc_usb.h"
#include <setupapi.h>
#include <initguid.h>

/* GUID_DEVINTERFACE_USB_DEVICE: every USB device exposes it. */
DEFINE_GUID(GUID_USBDEV_IF, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);
/* Our own interface GUID, written into DeviceInterfaceGUIDs by the installer. */
DEFINE_GUID(GUID_WFC_IF, 0x6e1f5a43, 0x2b8c, 0x4d0a, 0x9b, 0x57, 0x1c, 0x0a, 0x0b, 0x8b, 0x04, 0x11);

volatile LONG g_usb_dead = 0;

static int try_open_path(usbdev_t *u, const char *path)
{
    u->dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (u->dev == INVALID_HANDLE_VALUE)
        return -1;
    if (!WinUsb_Initialize(u->dev, &u->wu)) {
        CloseHandle(u->dev);
        u->dev = INVALID_HANDLE_VALUE;
        return -2;
    }
    return 0;
}

static int find_and_open(usbdev_t *u, const GUID *g, int *saw_device)
{
    HDEVINFO di = SetupDiGetClassDevsA(g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE)
        return -1;
    int rc = -1;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        if (!need)
            continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = malloc(need);
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL)) {
            char low[512];
            size_t n = strlen(det->DevicePath);
            if (n >= sizeof(low)) n = sizeof(low) - 1;
            for (size_t k = 0; k < n; k++) low[k] = (char)tolower((unsigned char)det->DevicePath[k]);
            low[n] = 0;
            if (strstr(low, "vid_0411&pid_008b")) {
                *saw_device = 1;
                if (try_open_path(u, det->DevicePath) == 0) {
                    rc = 0;
                    free(det);
                    break;
                }
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(di);
    return rc;
}

int usb_open(usbdev_t *u, char *err, size_t errlen)
{
    memset(u, 0, sizeof(*u));
    u->dev = INVALID_HANDLE_VALUE;
    int saw = 0;
    if (find_and_open(u, &GUID_WFC_IF, &saw) != 0 &&
        find_and_open(u, &GUID_USBDEV_IF, &saw) != 0) {
        if (!saw)
            snprintf(err, errlen, "The Nintendo Wi-Fi USB Connector is not plugged in.");
        else
            snprintf(err, errlen, "The connector is plugged in but its WinUSB driver is not installed.\n"
                     "Run \"Install Driver.bat\" once (it asks for administrator permission).");
        return -1;
    }

    USB_INTERFACE_DESCRIPTOR ifd;
    if (!WinUsb_QueryInterfaceSettings(u->wu, 0, &ifd)) {
        snprintf(err, errlen, "WinUsb_QueryInterfaceSettings failed (%lu).", GetLastError());
        usb_close(u);
        return -1;
    }
    for (UCHAR i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi;
        if (!WinUsb_QueryPipe(u->wu, 0, i, &pi))
            continue;
        if (pi.PipeType != UsbdPipeTypeBulk)
            continue;
        if (USB_ENDPOINT_DIRECTION_IN(pi.PipeId)) {
            if (!u->ep_in) { u->ep_in = pi.PipeId; u->mps_in = pi.MaximumPacketSize; }
        } else {
            if (!u->ep_out) { u->ep_out = pi.PipeId; u->mps_out = pi.MaximumPacketSize; }
        }
    }
    if (!u->ep_in || !u->ep_out) {
        snprintf(err, errlen, "The connector did not report its bulk endpoints.");
        usb_close(u);
        return -1;
    }
    UCHAR off = FALSE;
    WinUsb_SetPowerPolicy(u->wu, AUTO_SUSPEND, sizeof(off), &off);
    ULONG to = 1000;
    WinUsb_SetPipePolicy(u->wu, u->ep_out, PIPE_TRANSFER_TIMEOUT, sizeof(to), &to);
    WinUsb_FlushPipe(u->wu, u->ep_in);
    u->open = 1;
    DBG("USB: in=0x%02x (mps %u) out=0x%02x (mps %u)\n", u->ep_in, u->mps_in, u->ep_out, u->mps_out);
    return 0;
}

void usb_close(usbdev_t *u)
{
    if (u->wu) { WinUsb_Free(u->wu); u->wu = NULL; }
    if (u->dev && u->dev != INVALID_HANDLE_VALUE) { CloseHandle(u->dev); u->dev = INVALID_HANDLE_VALUE; }
    u->open = 0;
}

static int wait_ov(usbdev_t *u, OVERLAPPED *ov, BOOL ok, ULONG *xfer, DWORD timeout)
{
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        return -1;
    if (WaitForSingleObject(ov->hEvent, timeout) != WAIT_OBJECT_0) {
        CancelIoEx(u->dev, ov);
        WinUsb_GetOverlappedResult(u->wu, ov, xfer, TRUE);
        return -1;
    }
    if (!WinUsb_GetOverlappedResult(u->wu, ov, xfer, FALSE))
        return -1;
    return 0;
}

int usb_vendor(usbdev_t *u, int dir_in, UCHAR req, USHORT value, USHORT index,
               void *buf, USHORT len)
{
    if (!u->open || g_usb_dead)
        return -1;
    WINUSB_SETUP_PACKET sp;
    sp.RequestType = dir_in ? 0xC0 : 0x40;  /* vendor, device recipient */
    sp.Request = req;
    sp.Value = value;
    sp.Index = index;
    sp.Length = len;
    for (int attempt = 0; attempt < 3; attempt++) {
        OVERLAPPED ov = {0};
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        ULONG xfer = 0;
        BOOL ok = WinUsb_ControlTransfer(u->wu, sp, buf, len, &xfer, &ov);
        int rc = wait_ov(u, &ov, ok, &xfer, 1000);
        DWORD e = GetLastError();
        CloseHandle(ov.hEvent);
        if (rc == 0)
            return (int)xfer;
        if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND || e == ERROR_FILE_NOT_FOUND) {
            InterlockedExchange(&g_usb_dead, 1);
            return -1;
        }
    }
    DBG("USB vendor req 0x%02x idx 0x%04x failed (%lu)\n", req, index, GetLastError());
    return -1;
}

int usb_bulk_out(usbdev_t *u, const void *buf, ULONG len)
{
    if (!u->open || g_usb_dead)
        return -1;
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ULONG xfer = 0;
    BOOL ok = WinUsb_WritePipe(u->wu, u->ep_out, (PUCHAR)buf, len, &xfer, &ov);
    int rc = wait_ov(u, &ov, ok, &xfer, 2000);
    DWORD e = GetLastError();
    CloseHandle(ov.hEvent);
    if (rc != 0) {
        if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND || e == ERROR_FILE_NOT_FOUND)
            InterlockedExchange(&g_usb_dead, 1);
        DBG("USB bulk out failed (%lu)\n", e);
        return -1;
    }
    return 0;
}

/* ---- reader thread: keeps several bulk IN reads queued ---- */

#define NREADS 6
#define READ_SIZE 2560   /* 2432-byte frame buffer + 16-byte RX descriptor, rounded */

static struct {
    usbdev_t *u;
    usb_rx_cb cb;
    void *ctx;
    HANDLE thread;
    volatile LONG stop;
} rd;

static DWORD WINAPI reader_main(LPVOID arg)
{
    (void)arg;
    usbdev_t *u = rd.u;
    OVERLAPPED ov[NREADS];
    u8 *bufs[NREADS];
    for (int i = 0; i < NREADS; i++) {
        memset(&ov[i], 0, sizeof(ov[i]));
        ov[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        bufs[i] = malloc(READ_SIZE);
        ResetEvent(ov[i].hEvent);
        if (!WinUsb_ReadPipe(u->wu, u->ep_in, bufs[i], READ_SIZE, NULL, &ov[i]) &&
            GetLastError() != ERROR_IO_PENDING) {
            InterlockedExchange(&g_usb_dead, 1);
        }
    }
    int idx = 0;
    int errors = 0;
    while (!rd.stop && !g_usb_dead) {
        DWORD w = WaitForSingleObject(ov[idx].hEvent, 200);
        if (w == WAIT_TIMEOUT)
            continue;
        ULONG got = 0;
        if (WinUsb_GetOverlappedResult(u->wu, &ov[idx], &got, FALSE)) {
            errors = 0;
            if (got > 0)
                rd.cb(rd.ctx, bufs[idx], got);
        } else {
            DWORD e = GetLastError();
            if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND ||
                e == ERROR_FILE_NOT_FOUND || ++errors > 50) {
                InterlockedExchange(&g_usb_dead, 1);
                break;
            }
            if (e == ERROR_GEN_FAILURE)  /* stalled pipe */
                WinUsb_ResetPipe(u->wu, u->ep_in);
        }
        ResetEvent(ov[idx].hEvent);
        if (!WinUsb_ReadPipe(u->wu, u->ep_in, bufs[idx], READ_SIZE, NULL, &ov[idx]) &&
            GetLastError() != ERROR_IO_PENDING) {
            DWORD e = GetLastError();
            if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND || e == ERROR_FILE_NOT_FOUND) {
                InterlockedExchange(&g_usb_dead, 1);
                break;
            }
        }
        idx = (idx + 1) % NREADS;
    }
    WinUsb_AbortPipe(u->wu, u->ep_in);
    for (int i = 0; i < NREADS; i++) {
        ULONG got;
        WinUsb_GetOverlappedResult(u->wu, &ov[i], &got, TRUE);
        CloseHandle(ov[i].hEvent);
        free(bufs[i]);
    }
    return 0;
}

int usb_start_reader(usbdev_t *u, usb_rx_cb cb, void *ctx)
{
    rd.u = u;
    rd.cb = cb;
    rd.ctx = ctx;
    rd.stop = 0;
    rd.thread = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
    return rd.thread ? 0 : -1;
}

void usb_stop_reader(usbdev_t *u)
{
    (void)u;
    if (!rd.thread)
        return;
    InterlockedExchange(&rd.stop, 1);
    WaitForSingleObject(rd.thread, 3000);
    CloseHandle(rd.thread);
    rd.thread = NULL;
}
