// SPDX-License-Identifier: GPL-2.0-or-later
/* Thin WinUSB wrapper for the connector. */
#ifndef USB_H
#define USB_H
#include "common.h"
#include <winusb.h>

#define CONNECTOR_VID 0x0411
#define CONNECTOR_PID 0x008B

typedef struct {
    HANDLE dev;
    WINUSB_INTERFACE_HANDLE wu;
    UCHAR ep_in, ep_out;
    USHORT mps_in, mps_out;
    int open;
} usbdev_t;

/* Opens the first connector found. Returns 0 on success, else fills err. */
int usb_open(usbdev_t *u, char *err, size_t errlen);
void usb_close(usbdev_t *u);
/* Vendor request to the device. dir_in != 0 reads. Returns bytes moved or -1. */
int usb_vendor(usbdev_t *u, int dir_in, UCHAR req, USHORT value, USHORT index,
               void *buf, USHORT len);
/* Synchronous bulk OUT. Returns 0 on success. */
int usb_bulk_out(usbdev_t *u, const void *buf, ULONG len);

/* Asynchronous bulk IN reader running on its own thread. */
typedef void (*usb_rx_cb)(void *ctx, const u8 *buf, ULONG len);
int usb_start_reader(usbdev_t *u, usb_rx_cb cb, void *ctx);
void usb_stop_reader(usbdev_t *u);
extern volatile LONG g_usb_dead;   /* set when the device goes away */

#endif
