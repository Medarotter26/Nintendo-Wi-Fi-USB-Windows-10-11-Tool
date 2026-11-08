// SPDX-License-Identifier: GPL-2.0-or-later
/* RT2570 (Nintendo Wi-Fi USB Connector) hardware layer.
 * Register access, initialisation, channel tables, TX descriptors and the
 * beacon upload sequence follow the Linux rt2500usb driver
 * (drivers/net/wireless/ralink/rt2x00/rt2500usb.c, GPL-2.0-or-later). */
#include "rt2570.h"

/* USB vendor requests */
#define USB_DEVICE_MODE   1
#define USB_SINGLE_WRITE  2
#define USB_MULTI_WRITE   6
#define USB_MULTI_READ    7
#define USB_EEPROM_READ   9
#define USB_RX_CONTROL    12
#define USB_MODE_TEST     4

/* Registers */
#define MAC_CSR0   0x0400
#define MAC_CSR1   0x0402
#define MAC_CSR2   0x0404
#define MAC_CSR5   0x040a
#define MAC_CSR8   0x0410
#define MAC_CSR9   0x0412
#define MAC_CSR10  0x0414
#define MAC_CSR11  0x0416
#define MAC_CSR12  0x0418
#define MAC_CSR13  0x041a
#define MAC_CSR14  0x041c
#define MAC_CSR15  0x041e
#define MAC_CSR16  0x0420
#define MAC_CSR17  0x0422
#define MAC_CSR18  0x0424
#define MAC_CSR19  0x0426
#define MAC_CSR20  0x0428
#define MAC_CSR22  0x042c
#define TXRX_CSR0  0x0440
#define TXRX_CSR1  0x0442
#define TXRX_CSR2  0x0444
#define TXRX_CSR5  0x044a
#define TXRX_CSR6  0x044c
#define TXRX_CSR7  0x044e
#define TXRX_CSR8  0x0450
#define TXRX_CSR10 0x0454
#define TXRX_CSR11 0x0456
#define TXRX_CSR18 0x0464
#define TXRX_CSR19 0x0466
#define TXRX_CSR20 0x0468
#define TXRX_CSR21 0x046a
#define PHY_CSR2   0x04c4
#define PHY_CSR4   0x04c8
#define PHY_CSR5   0x04ca
#define PHY_CSR6   0x04cc
#define PHY_CSR7   0x04ce
#define PHY_CSR8   0x04d0
#define PHY_CSR9   0x04d2
#define PHY_CSR10  0x04d4

/* EEPROM word offsets */
#define EE_MAC        0x0002
#define EE_ANTENNA    0x000b
#define EE_NIC        0x000c
#define EE_BBP_START  0x000e
#define EE_TXPOWER    0x001e
#define EE_BBPTUNE    0x0030
#define EE_R24        0x0031
#define EE_R25        0x0032
#define EE_R61        0x0033
#define EE_VGC        0x0034
#define EE_R17        0x0035
#define EE_CALIBRATE  0x0036

enum { RF2522 = 0, RF2523 = 1, RF2524 = 2, RF2525 = 3, RF2525E = 5, RF5222 = 0x10 };
enum { ANT_SW_DIV = 0, ANT_A = 1, ANT_B = 2, ANT_HW_DIV = 3 };
#define BUSY_COUNT 100

typedef struct { u32 rf1, rf2, rf3, rf4; } rfch_t;

/* CWmax 6 (63 slots) instead of 10: an unanswered frame then blocks the radio
 * ~125 ms instead of ~420 ms (measured), so beacons keep flowing. */
int g_tx_retry = 7, g_tx_cwmax = 6;
static void txq_start(rt_t *rt);
static void txq_end(void);

static const rfch_t rf_2522[14] = {
    {0x00002050,0x000c1fda,0x00000101,0},{0x00002050,0x000c1fee,0x00000101,0},
    {0x00002050,0x000c2002,0x00000101,0},{0x00002050,0x000c2016,0x00000101,0},
    {0x00002050,0x000c202a,0x00000101,0},{0x00002050,0x000c203e,0x00000101,0},
    {0x00002050,0x000c2052,0x00000101,0},{0x00002050,0x000c2066,0x00000101,0},
    {0x00002050,0x000c207a,0x00000101,0},{0x00002050,0x000c208e,0x00000101,0},
    {0x00002050,0x000c20a2,0x00000101,0},{0x00002050,0x000c20b6,0x00000101,0},
    {0x00002050,0x000c20ca,0x00000101,0},{0x00002050,0x000c20fa,0x00000101,0}};
static const rfch_t rf_2523[14] = {
    {0x00022010,0x00000c9e,0x000e0111,0x00000a1b},{0x00022010,0x00000ca2,0x000e0111,0x00000a1b},
    {0x00022010,0x00000ca6,0x000e0111,0x00000a1b},{0x00022010,0x00000caa,0x000e0111,0x00000a1b},
    {0x00022010,0x00000cae,0x000e0111,0x00000a1b},{0x00022010,0x00000cb2,0x000e0111,0x00000a1b},
    {0x00022010,0x00000cb6,0x000e0111,0x00000a1b},{0x00022010,0x00000cba,0x000e0111,0x00000a1b},
    {0x00022010,0x00000cbe,0x000e0111,0x00000a1b},{0x00022010,0x00000d02,0x000e0111,0x00000a1b},
    {0x00022010,0x00000d06,0x000e0111,0x00000a1b},{0x00022010,0x00000d0a,0x000e0111,0x00000a1b},
    {0x00022010,0x00000d0e,0x000e0111,0x00000a1b},{0x00022010,0x00000d1a,0x000e0111,0x00000a03}};
static const rfch_t rf_2524[14] = {
    {0x00032020,0x00000c9e,0x00000101,0x00000a1b},{0x00032020,0x00000ca2,0x00000101,0x00000a1b},
    {0x00032020,0x00000ca6,0x00000101,0x00000a1b},{0x00032020,0x00000caa,0x00000101,0x00000a1b},
    {0x00032020,0x00000cae,0x00000101,0x00000a1b},{0x00032020,0x00000cb2,0x00000101,0x00000a1b},
    {0x00032020,0x00000cb6,0x00000101,0x00000a1b},{0x00032020,0x00000cba,0x00000101,0x00000a1b},
    {0x00032020,0x00000cbe,0x00000101,0x00000a1b},{0x00032020,0x00000d02,0x00000101,0x00000a1b},
    {0x00032020,0x00000d06,0x00000101,0x00000a1b},{0x00032020,0x00000d0a,0x00000101,0x00000a1b},
    {0x00032020,0x00000d0e,0x00000101,0x00000a1b},{0x00032020,0x00000d1a,0x00000101,0x00000a03}};
static const rfch_t rf_2525[14] = {
    {0x00022020,0x00080c9e,0x00060111,0x00000a1b},{0x00022020,0x00080ca2,0x00060111,0x00000a1b},
    {0x00022020,0x00080ca6,0x00060111,0x00000a1b},{0x00022020,0x00080caa,0x00060111,0x00000a1b},
    {0x00022020,0x00080cae,0x00060111,0x00000a1b},{0x00022020,0x00080cb2,0x00060111,0x00000a1b},
    {0x00022020,0x00080cb6,0x00060111,0x00000a1b},{0x00022020,0x00080cba,0x00060111,0x00000a1b},
    {0x00022020,0x00080cbe,0x00060111,0x00000a1b},{0x00022020,0x00080d02,0x00060111,0x00000a1b},
    {0x00022020,0x00080d06,0x00060111,0x00000a1b},{0x00022020,0x00080d0a,0x00060111,0x00000a1b},
    {0x00022020,0x00080d0e,0x00060111,0x00000a1b},{0x00022020,0x00080d1a,0x00060111,0x00000a03}};
static const rfch_t rf_2525e[14] = {
    {0x00022010,0x0000089a,0x00060111,0x00000e1b},{0x00022010,0x0000089e,0x00060111,0x00000e07},
    {0x00022010,0x0000089e,0x00060111,0x00000e1b},{0x00022010,0x000008a2,0x00060111,0x00000e07},
    {0x00022010,0x000008a2,0x00060111,0x00000e1b},{0x00022010,0x000008a6,0x00060111,0x00000e07},
    {0x00022010,0x000008a6,0x00060111,0x00000e1b},{0x00022010,0x000008aa,0x00060111,0x00000e07},
    {0x00022010,0x000008aa,0x00060111,0x00000e1b},{0x00022010,0x000008ae,0x00060111,0x00000e07},
    {0x00022010,0x000008ae,0x00060111,0x00000e1b},{0x00022010,0x000008b2,0x00060111,0x00000e07},
    {0x00022010,0x000008b2,0x00060111,0x00000e1b},{0x00022010,0x000008b6,0x00060111,0x00000e23}};
static const rfch_t rf_5222[14] = {
    {0x00022020,0x00001136,0x00000101,0x00000a0b},{0x00022020,0x0000113a,0x00000101,0x00000a0b},
    {0x00022020,0x0000113e,0x00000101,0x00000a0b},{0x00022020,0x00001182,0x00000101,0x00000a0b},
    {0x00022020,0x00001186,0x00000101,0x00000a0b},{0x00022020,0x0000118a,0x00000101,0x00000a0b},
    {0x00022020,0x0000118e,0x00000101,0x00000a0b},{0x00022020,0x00001192,0x00000101,0x00000a0b},
    {0x00022020,0x00001196,0x00000101,0x00000a0b},{0x00022020,0x0000119a,0x00000101,0x00000a0b},
    {0x00022020,0x0000119e,0x00000101,0x00000a0b},{0x00022020,0x000011a2,0x00000101,0x00000a0b},
    {0x00022020,0x000011a6,0x00000101,0x00000a0b},{0x00022020,0x000011ae,0x00000101,0x00000a1b}};

const char *rt_rf_name(int rf)
{
    switch (rf) {
    case RF2522: return "RF2522";
    case RF2523: return "RF2523";
    case RF2524: return "RF2524";
    case RF2525: return "RF2525";
    case RF2525E: return "RF2525E";
    case RF5222: return "RF5222";
    }
    return "unknown";
}

/* ---- register helpers ---- */

static u16 reg_rd(rt_t *rt, u16 off)
{
    u8 b[2] = {0xff, 0xff};
    usb_vendor(&rt->usb, 1, USB_MULTI_READ, 0, off, b, 2);
    return rd16le(b);
}

static void reg_wr(rt_t *rt, u16 off, u16 v)
{
    u8 b[2];
    wr16le(b, v);
    usb_vendor(&rt->usb, 0, USB_MULTI_WRITE, 0, off, b, 2);
}

static void reg_multi_wr(rt_t *rt, u16 off, const u8 *data, u16 len)
{
    u8 b[64];
    memcpy(b, data, len);
    usb_vendor(&rt->usb, 0, USB_MULTI_WRITE, 0, off, b, len);
}

static void vendor_sw(rt_t *rt, u8 req, u16 offset, u16 value)
{
    usb_vendor(&rt->usb, 0, req, value, offset, NULL, 0);
}

static int wait_not_busy(rt_t *rt, u16 off, u16 mask, u16 *out)
{
    for (int i = 0; i < BUSY_COUNT; i++) {
        u16 r = reg_rd(rt, off);
        if (!(r & mask)) {
            if (out) *out = r;
            return 1;
        }
        Sleep(1);
    }
    DBG("RT: register 0x%04x stayed busy\n", off);
    return 0;
}

static void bbp_wr(rt_t *rt, u8 id, u8 v)
{
    if (wait_not_busy(rt, PHY_CSR8, 0x0001, NULL))
        reg_wr(rt, PHY_CSR7, (u16)(v | ((id & 0x7f) << 8)));
}

static u8 bbp_rd(rt_t *rt, u8 id)
{
    u16 r = 0xffff;
    if (wait_not_busy(rt, PHY_CSR8, 0x0001, NULL)) {
        reg_wr(rt, PHY_CSR7, (u16)(((id & 0x7f) << 8) | 0x8000));
        if (wait_not_busy(rt, PHY_CSR8, 0x0001, NULL))
            r = reg_rd(rt, PHY_CSR7);
    }
    return (u8)(r & 0xff);
}

static void rf_wr(rt_t *rt, int word, u32 v)
{
    if (wait_not_busy(rt, PHY_CSR10, 0x8000, NULL)) {
        reg_wr(rt, PHY_CSR9, (u16)(v & 0xffff));
        reg_wr(rt, PHY_CSR10, (u16)(((v >> 16) & 0xff) | (20 << 8) | 0x8000));
        rt->rf_cache[word] = v;
    }
}

static u16 ee(rt_t *rt, int word) { return rt->eeprom[word]; }

/* ---- bring-up ---- */

static int set_awake(rt_t *rt)
{
    const int state = 3; /* STATE_AWAKE */
    u16 reg = (u16)((state << 1) | (state << 3));
    reg_wr(rt, MAC_CSR17, reg);
    reg |= 1;
    reg_wr(rt, MAC_CSR17, reg);
    for (int i = 0; i < 40; i++) {
        u16 r2 = reg_rd(rt, MAC_CSR17);
        if (((r2 >> 5) & 3) == state && ((r2 >> 7) & 3) == state)
            return 0;
        reg_wr(rt, MAC_CSR17, reg);
        Sleep(30);
    }
    return -1;
}

static void read_eeprom(rt_t *rt)
{
    u8 raw[0x6e];
    memset(raw, 0xff, sizeof(raw));
    usb_vendor(&rt->usb, 1, USB_EEPROM_READ, 0, 0, raw, sizeof(raw));
    for (int i = 0; i < 0x37; i++)
        rt->eeprom[i] = rd16le(raw + 2 * i);
    memcpy(rt->mac, raw + 2 * EE_MAC, 6);
    memcpy(rt->txpower, raw + 2 * EE_TXPOWER, 14);

    /* Defaults for unprogrammed words, as rt2500usb_validate_eeprom does. */
    if (rt->eeprom[EE_ANTENNA] == 0xffff)
        rt->eeprom[EE_ANTENNA] = (u16)(2 | (ANT_SW_DIV << 2) | (ANT_SW_DIV << 4) | (RF2522 << 11));
    if (rt->eeprom[EE_NIC] == 0xffff) rt->eeprom[EE_NIC] = 0;
    if (rt->eeprom[EE_CALIBRATE] == 0xffff) rt->eeprom[EE_CALIBRATE] = 120;
    if (rt->eeprom[EE_BBPTUNE] == 0xffff) rt->eeprom[EE_BBPTUNE] = 45;
    if (rt->eeprom[EE_VGC] == 0xffff) rt->eeprom[EE_VGC] = 0x40;
    if (rt->eeprom[EE_R17] == 0xffff) rt->eeprom[EE_R17] = 0x4148;
    if (rt->eeprom[EE_R24] == 0xffff) rt->eeprom[EE_R24] = 0x8040;
    if (rt->eeprom[EE_R25] == 0xffff) rt->eeprom[EE_R25] = 0x5040;
    if (rt->eeprom[EE_R61] == 0xffff) rt->eeprom[EE_R61] = 0x6d60;
}

int rt_init(rt_t *rt, char *err, size_t errlen)
{
    memset(rt, 0, sizeof(*rt));
    InitializeCriticalSection(&rt->txlock);
    InitializeCriticalSection(&rt->bcnlock);
    if (usb_open(&rt->usb, err, errlen) != 0)
        return -1;

    read_eeprom(rt);
    u16 ant = ee(rt, EE_ANTENNA);
    rt->rf_type = (ant >> 11) & 0x1f;
    rt->ant_tx = (ant >> 2) & 3;
    rt->ant_rx = (ant >> 4) & 3;
    if (rt->ant_tx == ANT_SW_DIV) rt->ant_tx = ANT_HW_DIV;
    if (rt->ant_rx == ANT_SW_DIV) rt->ant_rx = ANT_HW_DIV;

    u16 csr0 = reg_rd(rt, MAC_CSR0);
    rt->rev = csr0 & 0x000f;
    if ((csr0 & 0xfff0) != 0 || (csr0 & 0x000f) == 0) {
        snprintf(err, errlen, "The connector answered, but its chip ID (0x%04x) is not an RT2570.", csr0);
        usb_close(&rt->usb);
        return -1;
    }
    switch (rt->rf_type) {
    case RF2522: case RF2523: case RF2524: case RF2525: case RF2525E: case RF5222: break;
    default:
        snprintf(err, errlen, "Unsupported radio type 0x%x in the connector's EEPROM.", rt->rf_type);
        usb_close(&rt->usb);
        return -1;
    }
    /* GPIO0 as input (hardware radio switch), as the Linux driver does. */
    reg_wr(rt, MAC_CSR19, reg_rd(rt, MAC_CSR19) & (u16)~0x0100);
    return 0;
}

static int init_registers(rt_t *rt)
{
    vendor_sw(rt, USB_DEVICE_MODE, 0x0001, USB_MODE_TEST);
    vendor_sw(rt, USB_SINGLE_WRITE, 0x0308, 0x00f0);

    reg_wr(rt, TXRX_CSR2, reg_rd(rt, TXRX_CSR2) | 0x0001); /* RX off */
    reg_wr(rt, MAC_CSR13, 0x1111);
    reg_wr(rt, MAC_CSR14, 0x1e11);

    u16 r = reg_rd(rt, MAC_CSR1);
    r = (u16)((r | 0x0003) & ~0x0004);
    reg_wr(rt, MAC_CSR1, r);
    r = reg_rd(rt, MAC_CSR1);
    r &= (u16)~0x0007;
    reg_wr(rt, MAC_CSR1, r);

    reg_wr(rt, TXRX_CSR5, (u16)(13 | 0x80 | (12 << 8) | 0x8000));
    reg_wr(rt, TXRX_CSR6, (u16)(10 | 0x80 | (11 << 8) | 0x8000));
    reg_wr(rt, TXRX_CSR7, (u16)(7 | 0x80 | (6 << 8) | 0x8000));
    reg_wr(rt, TXRX_CSR8, (u16)(5 | 0x80));

    reg_wr(rt, TXRX_CSR19, reg_rd(rt, TXRX_CSR19) & (u16)~0x001f);
    reg_wr(rt, TXRX_CSR21, 0xe78f);
    reg_wr(rt, MAC_CSR9, 0xff1d);

    if (set_awake(rt) != 0)
        return -1;

    r = reg_rd(rt, MAC_CSR1);
    r = (u16)((r & ~0x0003) | 0x0004);
    reg_wr(rt, MAC_CSR1, r);

    if (rt->rev >= 3) {
        r = reg_rd(rt, PHY_CSR2);
        r &= (u16)~0x0002;
    } else {
        r = (u16)(0x0002 | (3 << 12));
    }
    reg_wr(rt, PHY_CSR2, r);

    reg_wr(rt, MAC_CSR11, 0x0002);
    reg_wr(rt, MAC_CSR22, 0x0053);
    reg_wr(rt, MAC_CSR15, 0x01ee);
    reg_wr(rt, MAC_CSR16, 0x0000);

    r = reg_rd(rt, MAC_CSR8);
    r = (u16)((r & ~0x0fff) | 2432);
    reg_wr(rt, MAC_CSR8, r);

    r = reg_rd(rt, TXRX_CSR0);
    r = (u16)((r & ~0x1fff) | (24 << 3)); /* no cipher, IV offset 24, no keys */
    reg_wr(rt, TXRX_CSR0, r);

    r = reg_rd(rt, MAC_CSR18);
    r = (u16)((r & ~0x00ff) | 90);
    reg_wr(rt, MAC_CSR18, r);

    reg_wr(rt, PHY_CSR4, reg_rd(rt, PHY_CSR4) | 0x0001);
    reg_wr(rt, TXRX_CSR1, reg_rd(rt, TXRX_CSR1) | 0x8000);
    return 0;
}

static int init_bbp(rt_t *rt)
{
    int ok = 0;
    for (int i = 0; i < BUSY_COUNT; i++) {
        u8 v = bbp_rd(rt, 0);
        if (v != 0xff && v != 0x00) { ok = 1; break; }
        Sleep(1);
    }
    if (!ok)
        return -1;
    static const u8 init[][2] = {
        {3,0x02},{4,0x19},{14,0x1c},{15,0x30},{16,0xac},{18,0x18},{19,0xff},{20,0x1e},
        {21,0x08},{22,0x08},{23,0x08},{24,0x80},{25,0x50},{26,0x08},{27,0x23},{30,0x10},
        {31,0x2b},{32,0xb9},{34,0x12},{35,0x50},{39,0xc4},{40,0x02},{41,0x60},{53,0x10},
        {54,0x18},{56,0x08},{57,0x10},{58,0x08},{61,0x60},{62,0x10},{75,0xff}};
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++)
        bbp_wr(rt, init[i][0], init[i][1]);
    for (int i = 0; i < 16; i++) {
        u16 w = ee(rt, EE_BBP_START + i);
        if (w != 0xffff && w != 0x0000)
            bbp_wr(rt, (u8)(w >> 8), (u8)(w & 0xff));
    }
    return 0;
}

static void reset_tuner(rt_t *rt)
{
    bbp_wr(rt, 24, (u8)(ee(rt, EE_R24) & 0xff));
    bbp_wr(rt, 25, (u8)(ee(rt, EE_R25) & 0xff));
    bbp_wr(rt, 61, (u8)(ee(rt, EE_R61) & 0xff));
    bbp_wr(rt, 17, (u8)(ee(rt, EE_VGC) & 0xff));
}

static void config_antenna(rt_t *rt)
{
    u8 r2 = bbp_rd(rt, 2);
    u8 r14 = bbp_rd(rt, 14);
    u16 csr5 = reg_rd(rt, PHY_CSR5);
    u16 csr6 = reg_rd(rt, PHY_CSR6);
    int t, rx;
    switch (rt->ant_tx) {
    case ANT_HW_DIV: t = 1; break;
    case ANT_A: t = 0; break;
    default: t = 2; break;
    }
    switch (rt->ant_rx) {
    case ANT_HW_DIV: rx = 1; break;
    case ANT_A: rx = 0; break;
    default: rx = 2; break;
    }
    r2 = (u8)((r2 & ~0x03) | t);
    csr5 = (u16)((csr5 & ~0x0003) | t);
    csr6 = (u16)((csr6 & ~0x0003) | t);
    r14 = (u8)((r14 & ~0x03) | rx);
    if (rt->rf_type == RF2525E || rt->rf_type == RF5222) {
        r2 |= 0x04;
        csr5 |= 0x0004;
        csr6 |= 0x0004;
        if (rt->rf_type == RF2525E)
            r14 &= (u8)~0x04;
    } else {
        csr5 &= (u16)~0x0004;
        csr6 &= (u16)~0x0004;
    }
    bbp_wr(rt, 2, r2);
    bbp_wr(rt, 14, r14);
    reg_wr(rt, PHY_CSR5, csr5);
    reg_wr(rt, PHY_CSR6, csr6);
}

static void config_channel(rt_t *rt, int ch, int txpower)
{
    const rfch_t *tab;
    switch (rt->rf_type) {
    case RF2522: tab = rf_2522; break;
    case RF2523: tab = rf_2523; break;
    case RF2524: tab = rf_2524; break;
    case RF2525: tab = rf_2525; break;
    case RF2525E: tab = rf_2525e; break;
    default: tab = rf_5222; break;
    }
    rfch_t rf = tab[ch - 1];
    if (txpower < 0) txpower = 0;
    if (txpower > 31) txpower = 31;
    rf.rf3 = (rf.rf3 & ~0x00003e00u) | ((u32)txpower << 9);
    if (rt->rf_type == RF2525E) {
        static const u32 vals[] = {0x8aa,0x8ae,0x8ae,0x8b2,0x8b2,0x8b6,0x8b6,
                                   0x8ba,0x8ba,0x8be,0x8b7,0x902,0x902,0x906};
        rf_wr(rt, 2, vals[ch - 1]);
        if (rf.rf4) rf_wr(rt, 4, rf.rf4);
    }
    rf_wr(rt, 1, rf.rf1);
    rf_wr(rt, 2, rf.rf2);
    rf_wr(rt, 3, rf.rf3);
    if (rf.rf4) rf_wr(rt, 4, rf.rf4);
    rt->channel = ch;
}

int rt_start_ap(rt_t *rt, int channel, int txpower_override)
{
    if (channel < 1 || channel > 14)
        channel = 6;
    if (init_registers(rt) != 0 || init_bbp(rt) != 0)
        return -1;
    reset_tuner(rt);
    config_antenna(rt);

    int pw = rt->txpower[channel - 1];
    if (pw > 31) pw = 24;
    if (txpower_override >= 0) pw = txpower_override;
    config_channel(rt, channel, pw);
    INFO("Radio: %s, channel %d, TX power %d/31\n", rt_rf_name(rt->rf_type), channel, pw);

    /* Interface: access point, TSF runs freely and we send beacons. */
    u16 r = reg_rd(rt, TXRX_CSR20);
    r = (u16)((r & ~0xffff) | (240 >> 6) | (2 << 13));
    reg_wr(rt, TXRX_CSR20, r);
    reg_wr(rt, TXRX_CSR18, reg_rd(rt, TXRX_CSR18) & (u16)~0x000f);
    r = reg_rd(rt, TXRX_CSR19);
    r = (u16)((r & ~0x0006) | (3 << 1)); /* TSF_SYNC_AP_NONE */
    reg_wr(rt, TXRX_CSR19, r);
    reg_multi_wr(rt, MAC_CSR2, rt->mac, 6);
    reg_multi_wr(rt, MAC_CSR5, rt->mac, 6);   /* BSSID = our MAC */
    /* Read the address registers back: if the chip does not hold our MAC it
     * will ignore every frame a console sends to us. */
    for (int pass = 0; pass < 2; pass++) {
        u8 got[12];
        for (int i = 0; i < 6; i++)
            wr16le(got + 2 * i, reg_rd(rt, (u16)(MAC_CSR2 + 2 * i)));
        int ok = !memcmp(got, rt->mac, 6) && !memcmp(got + 6, rt->mac, 6);
        DBG("RT: MAC regs " MACFMT " BSSID regs " MACFMT " (%s)\n", MACARG(got), MACARG(got + 6),
            ok ? "ok" : "MISMATCH");
        if (ok) break;
        for (int i = 0; i < 3; i++) {
            reg_wr(rt, (u16)(MAC_CSR2 + 2 * i), rd16le(rt->mac + 2 * i));
            reg_wr(rt, (u16)(MAC_CSR5 + 2 * i), rd16le(rt->mac + 2 * i));
        }
    }

    /* ERP: long preamble, basic rates 1+2 Mbit, 100 TU beacons, 11b timing. */
    reg_wr(rt, TXRX_CSR10, reg_rd(rt, TXRX_CSR10) & (u16)~0x0004);
    reg_wr(rt, TXRX_CSR11, 0x0003);
    r = reg_rd(rt, TXRX_CSR18);
    r = (u16)((r & 0x000f) | ((100 * 4) << 4));
    reg_wr(rt, TXRX_CSR18, r);
    reg_wr(rt, MAC_CSR10, 20);
    reg_wr(rt, MAC_CSR11, 10);
    reg_wr(rt, MAC_CSR12, 364);

    /* RX filter: drop bad CRC/PLCP, frames for other stations and bad
     * versions; keep control frames (PS-Poll), to-DS frames, multicast. */
    r = reg_rd(rt, TXRX_CSR2);
    r &= (u16)~(0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0200 | 0x0400);
    r |= 0x0002 | 0x0004 | 0x0010 | 0x0040;
    reg_wr(rt, TXRX_CSR2, r);

    /* Start receiving and light the link LED. */
    reg_wr(rt, TXRX_CSR2, reg_rd(rt, TXRX_CSR2) & (u16)~0x0001);
    reg_wr(rt, MAC_CSR20, reg_rd(rt, MAC_CSR20) | 0x0002);
    DBG("RT: TXRX_CSR2=%04x CSR18=%04x CSR19=%04x CSR20=%04x MAC_CSR19=%04x MAC_CSR20=%04x EEPROM ant=%04x nic=%04x\n",
        reg_rd(rt, TXRX_CSR2), reg_rd(rt, TXRX_CSR18), reg_rd(rt, TXRX_CSR19), reg_rd(rt, TXRX_CSR20),
        reg_rd(rt, MAC_CSR19), reg_rd(rt, MAC_CSR20), ee(rt, EE_ANTENNA), ee(rt, EE_NIC));
    rt->led_csr19 = reg_rd(rt, MAC_CSR19);
    rt->led_csr20 = (u16)(reg_rd(rt, MAC_CSR20) & ~0x0007);
    txq_start(rt);
    return 0;
}

/* ---- status light ----
 * How the connector wires its light is not documented, so these are the
 * candidate ways to drive it: the chip's LED outputs (MAC_CSR20) and its
 * eight general-purpose pins (MAC_CSR19). "Find Light Setting.bat" steps
 * through them and saves the one that lights up. */
#define LED_N (5 + 8 * 3)

int rt_led_count(void) { return LED_N; }

void rt_led_desc(int idx, char *out, size_t n)
{
    static const char *mac[5] = {"LED output: activity", "LED output: link", "LED outputs: link + activity",
                                 "LED outputs: link + activity, inverted", "LED output: inverted only"};
    if (idx <= 0) snprintf(out, n, "off");
    else if (idx <= 5) snprintf(out, n, "%s", mac[idx - 1]);
    else if (idx <= LED_N) {
        int k = (idx - 6) / 3, m = (idx - 6) % 3;
        static const char *mode[3] = {"high", "high (direction flipped)", "low (direction flipped)"};
        snprintf(out, n, "pin %d %s", k, mode[m]);
    } else snprintf(out, n, "unknown");
}

void rt_led_apply(rt_t *rt, int idx)
{
    u16 c19 = rt->led_csr19, c20 = rt->led_csr20;
    if (idx >= 1 && idx <= 5) {
        static const u16 v[5] = {0x0001, 0x0002, 0x0003, 0x0007, 0x0004};
        c20 = (u16)(c20 | v[idx - 1]);
    } else if (idx > 5 && idx <= LED_N) {
        int k = (idx - 6) / 3, m = (idx - 6) % 3;
        u16 dir = (u16)(0x0100 << k), val = (u16)(1 << k);
        if (m == 0) c19 = (u16)(c19 | dir | val);
        else if (m == 1) c19 = (u16)((c19 & ~dir) | val);
        else c19 = (u16)(c19 & ~dir & ~val);
    }
    reg_wr(rt, MAC_CSR19, c19);
    reg_wr(rt, MAC_CSR20, c20);
}

void rt_stop(rt_t *rt)
{
    if (!rt->usb.open)
        return;
    rt_stop_beacon(rt);
    txq_end();
    if (!g_usb_dead) {
        rt_led_apply(rt, 0);
        reg_wr(rt, MAC_CSR20, reg_rd(rt, MAC_CSR20) & (u16)~0x0003);
        reg_wr(rt, MAC_CSR13, 0x2121);
        reg_wr(rt, MAC_CSR14, 0x2121);
        reg_wr(rt, TXRX_CSR19, 0);
        vendor_sw(rt, USB_RX_CONTROL, 0, 0);
    }
    usb_stop_reader(&rt->usb);
    usb_close(&rt->usb);
}

/* ---- TX path ---- */

static int plcp_code(int rate)
{
    switch (rate) {
    case RATE_1M: return 0x00;
    case RATE_2M: return 0x01;
    case RATE_5M5: return 0x02;
    default: return 0x03;
    }
}

/* Builds TX descriptor + frame into out. Returns the USB transfer length. */
static int build_tx(rt_t *rt, u8 *out, const u8 *frame, int len, int rate,
                    int need_ack, int timestamp)
{
    u8 *txd = out;
    memset(txd, 0, 20);
    memcpy(out + 20, frame, len);

    /* Software sequence numbers (the chip needs REQUIRE_SW_SEQNO). */
    u8 *f = out + 20;
    if ((f[0] & 0x0c) != 0x04 && len >= 24) { /* not a control frame */
        wr16le(f + 22, (u16)((rt->seq & 0x0fff) << 4));
        rt->seq++;
    }

    int retry = need_ack ? g_tx_retry : 0;
    u32 w0 = ((u32)retry << 4) | ((u32)(need_ack ? 1 : 0) << 9) |
             ((u32)(timestamp ? 1 : 0) << 10) | (1u << 12) /* NEW_SEQ */ |
             (0u << 13) /* IFS backoff */ | ((u32)len << 16);
    u32 w1 = (2u << 6) | (5u << 8) | ((u32)g_tx_cwmax << 12); /* AIFS 2, CWmin 5, CWmax */

    unsigned data_len = (unsigned)len + 4; /* + FCS */
    unsigned dur = data_len * 8 * 10 / (unsigned)rate;
    unsigned res = data_len * 8 * 10 % (unsigned)rate;
    u32 service = 0x04;
    if (res) {
        dur++;
        if (rate == RATE_11M && res <= 30)
            service |= 0x80;
    }
    u32 w2 = (u32)plcp_code(rate) | (service << 8) | ((dur & 0xff) << 16) | (((dur >> 8) & 0xff) << 24);
    wr32le(txd + 0, w0);
    wr32le(txd + 4, w1);
    wr32le(txd + 8, w2);

    int total = 20 + len;
    total = (total + 1) & ~1;
    if (rt->usb.mps_out && total % rt->usb.mps_out == 0)
        total += 2;
    return total;
}

/* Writes one frame to the chip now (blocks until USB accepts it). */
static int tx_now(rt_t *rt, const u8 *frame, int len, int rate, int need_ack, int timestamp)
{
    u8 buf[2500];
    memset(buf, 0, sizeof(buf));
    EnterCriticalSection(&rt->txlock);
    int n = build_tx(rt, buf, frame, len, rate, need_ack, timestamp);
    int rc = usb_bulk_out(&rt->usb, buf, (ULONG)n);
    LeaveCriticalSection(&rt->txlock);
    if (rc) { rt->tx_fail++; DBG("RT: TX of %d-byte frame (type %02x) failed\n", len, frame[0]); }
    else rt->tx_ok++;
    return rc;
}

/* ---- TX queue: the router thread never waits on USB ---- */

#define TXQ 512
typedef struct { u8 f[2400]; int len, rate, ack, ts; } txq_ent_t;
static txq_ent_t *txq;
static int txq_head, txq_tail;
static CRITICAL_SECTION txq_lock;
static HANDLE txq_event, txq_thread;
static volatile LONG txq_stop;
static long txq_dropped;

static DWORD WINAPI txq_main(LPVOID arg)
{
    rt_t *rt = arg;
    static txq_ent_t e;
    while (!txq_stop && !g_usb_dead) {
        EnterCriticalSection(&txq_lock);
        int have = txq_tail != txq_head;
        if (have) {
            e = txq[txq_tail];
            txq_tail = (txq_tail + 1) % TXQ;
        }
        LeaveCriticalSection(&txq_lock);
        if (!have) {
            WaitForSingleObject(txq_event, 50);
            continue;
        }
        tx_now(rt, e.f, e.len, e.rate, e.ack, e.ts);
    }
    return 0;
}

static void txq_start(rt_t *rt)
{
    if (txq_thread) return;
    if (!txq) {
        txq = calloc(TXQ, sizeof(txq_ent_t));
        InitializeCriticalSection(&txq_lock);
        txq_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    txq_head = txq_tail = 0;
    txq_stop = 0;
    txq_thread = CreateThread(NULL, 0, txq_main, rt, 0, NULL);
    if (txq_thread) SetThreadPriority(txq_thread, THREAD_PRIORITY_ABOVE_NORMAL);
}

static void txq_end(void)
{
    if (!txq_thread) return;
    InterlockedExchange(&txq_stop, 1);
    SetEvent(txq_event);
    WaitForSingleObject(txq_thread, 3000);
    CloseHandle(txq_thread);
    txq_thread = NULL;
}

int rt_tx(rt_t *rt, const u8 *frame, int len, int rate, int need_ack, int timestamp)
{
    if (len < 10 || len > 2400)
        return -1;
    if (!txq_thread)
        return tx_now(rt, frame, len, rate, need_ack, timestamp);
    EnterCriticalSection(&txq_lock);
    int next = (txq_head + 1) % TXQ;
    int rc = 0;
    if (next == txq_tail) {
        txq_dropped++;
        rc = -1;
    } else {
        txq_ent_t *e = &txq[txq_head];
        memcpy(e->f, frame, len);
        e->len = len; e->rate = rate; e->ack = need_ack; e->ts = timestamp;
        txq_head = next;
    }
    LeaveCriticalSection(&txq_lock);
    if (rc) DBG("RT: TX queue full, frame dropped (%ld so far)\n", txq_dropped);
    SetEvent(txq_event);
    return rc;
}

int g_beacon_variant = 0;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Sends the current beacon every 100 TU (102.4 ms) through the normal TX
 * path. The chip fills in the TSF timestamp (TXD TIMESTAMP bit). */
static DWORD WINAPI beacon_main(LPVOID arg)
{
    rt_t *rt = arg;
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tmr) tmr = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER freq, start, nowc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    u64 n = 0;
    u8 f[1024];
    while (!rt->bcn_stop && !g_usb_dead) {
        EnterCriticalSection(&rt->bcnlock);
        int len = rt->bcn_len;
        if (len) memcpy(f, rt->bcn, len);
        LeaveCriticalSection(&rt->bcnlock);
        if (len && tx_now(rt, f, len, RATE_1M, 0, 1) == 0)
            InterlockedIncrement(&rt->bcn_sent);
        n++;
        /* Absolute schedule so the interval does not drift. */
        double due_us = (double)n * 102400.0;
        QueryPerformanceCounter(&nowc);
        double now_us = (double)(nowc.QuadPart - start.QuadPart) * 1e6 / (double)freq.QuadPart;
        double wait_us = due_us - now_us;
        if (wait_us < 0) {               /* fell behind (PC busy): resync */
            QueryPerformanceCounter(&start);
            n = 0;
            continue;
        }
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(wait_us * 10.0);   /* relative, 100 ns units */
        if (tmr && SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(tmr, 1000);
        else
            Sleep((DWORD)(wait_us / 1000.0));
    }
    if (tmr) CloseHandle(tmr);
    return 0;
}

static void start_sw_beacon(rt_t *rt)
{
    if (rt->bcn_thread)
        return;
    rt->bcn_stop = 0;
    rt->bcn_thread = CreateThread(NULL, 0, beacon_main, rt, 0, NULL);
    if (rt->bcn_thread)
        SetThreadPriority(rt->bcn_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

void rt_stop_beacon(rt_t *rt)
{
    if (!rt->bcn_thread)
        return;
    InterlockedExchange(&rt->bcn_stop, 1);
    WaitForSingleObject(rt->bcn_thread, 2000);
    CloseHandle(rt->bcn_thread);
    rt->bcn_thread = NULL;
}

int rt_set_beacon(rt_t *rt, const u8 *frame, int len)
{
    if (len > 900)
        return -1;
    if (g_beacon_variant == 0) {
        /* Software beacon: keep the TSF running for timestamps, but leave the
         * hardware beacon generator off so beacons are never doubled. */
        EnterCriticalSection(&rt->bcnlock);
        memcpy(rt->bcn, frame, len);
        rt->bcn_len = len;
        LeaveCriticalSection(&rt->bcnlock);
        if (!rt->bcn_thread) {
            EnterCriticalSection(&rt->txlock);
            u16 reg = reg_rd(rt, TXRX_CSR19);
            reg = (u16)((reg & ~0x0010) | 0x0001 | 0x0008);
            reg_wr(rt, TXRX_CSR19, reg);
            LeaveCriticalSection(&rt->txlock);
            start_sw_beacon(rt);
        }
        return 0;
    }
    rt_stop_beacon(rt);
    return rt_set_beacon_hw(rt, frame, len);
}

int rt_set_beacon_hw(rt_t *rt, const u8 *frame, int len)
{
    u8 buf[1024];
    if (len > 900)
        return -1;
    memset(buf, 0, sizeof(buf));
    EnterCriticalSection(&rt->txlock);
    u16 reg = reg_rd(rt, TXRX_CSR19);
    reg &= (u16)~0x0010;               /* BEACON_GEN off while reloading */
    reg_wr(rt, TXRX_CSR19, reg);

    int n = build_tx(rt, buf, frame, len, RATE_1M, 0, 1);
    u8 guardian = 0;
    int rc = 0;
    int v = g_beacon_variant - 1;      /* hardware upload order 0..3 */
    if (v != 3)
        rc = usb_bulk_out(&rt->usb, &guardian, 1);
    if (rc == 0 && v != 1)
        rc = usb_bulk_out(&rt->usb, buf, (ULONG)n);

    reg |= 0x0001 | 0x0008;            /* TSF_COUNT, TBCN */
    u16 reg0 = reg;
    reg |= 0x0010;                     /* BEACON_GEN */
    /* Beacon generation fails at first; toggling it a few times helps
     * (same workaround as the Linux driver). */
    reg_wr(rt, TXRX_CSR19, reg);
    reg_wr(rt, TXRX_CSR19, reg0);
    reg_wr(rt, TXRX_CSR19, reg);
    reg_wr(rt, TXRX_CSR19, reg0);
    reg_wr(rt, TXRX_CSR19, reg);
    if (rc == 0 && v == 1)             /* Linux order: beacon data after the toggles */
        rc = usb_bulk_out(&rt->usb, buf, (ULONG)n);
    DBG("RT: beacon loaded (variant %d), TXRX_CSR19=%04x\n", v, reg_rd(rt, TXRX_CSR19));
    LeaveCriticalSection(&rt->txlock);
    return rc;
}

/* Reads (and so clears) STA_CSR5..10: beacons sent plus TX statistics. */
void rt_read_stats(rt_t *rt, u16 out[6])
{
    for (int i = 0; i < 6; i++)
        out[i] = reg_rd(rt, (u16)(0x04ea + 2 * i));
}

/* ---- RX path ---- */

int rt_parse_rx(const u8 *buf, int got, const u8 **frame, int *rssi)
{
    if (got < 16 + 14)
        return -1;
    const u8 *rxd = buf + got - 16;
    u32 w0 = rd32le(rxd);
    u32 w1 = rd32le(rxd + 4);
    if (w0 & (0x20 | 0x80))           /* CRC or PLCP error */
        return -1;
    int size = (int)((w0 >> 16) & 0x0fff);
    if (size < 14 || size > got - 16)
        return -1;
    *frame = buf;
    if (rssi) *rssi = (int)(w1 & 0xff) - 120;
    return size - 4;                  /* strip FCS */
}
