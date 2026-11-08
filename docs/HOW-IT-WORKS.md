# How WFC Connector works

## Why the original can't run

The connector is a Buffalo WLI-U2-KG54-based USB stick (USB `0411:008B`) with a Ralink RT2570 chip and an
RF2525E radio. Nintendo's software (versions 1.01–1.07) installs `rt25usbap.sys`, a 32-bit NDIS
soft-access-point driver. Its machine type is `0x14c` and its INF has no `NTamd64` section. 64-bit Windows can't
load 32-bit kernel drivers, so the original can't run on Windows 11.

## The replacement

Everything runs in user space in one exe.

| Layer | File | Role |
|---|---|---|
| Driver binding | `driver.c` | Binds Windows' in-box `winusb.inf` ("WinUsb Device", class USBDevice) to `USB\VID_0411&PID_008B` with SetupAPI and `DiInstallDevice`. It also writes `DeviceInterfaceGUIDs` so WinUSB publishes a device interface. It runs in an elevated copy of the exe (`--install-driver`). |
| USB | `wfc_usb.c` | WinUSB control transfers (vendor requests) and bulk endpoints `0x01`/`0x81`. A reader thread keeps six bulk-IN reads queued. |
| Chip | `rt2570.c` | Port of Linux `rt2500usb`: register access (vendor requests 6/7, `wIndex` = register), EEPROM, BBP and RF initialisation, channel tables, the 20-byte TX descriptor, and RX descriptor parsing. |
| Access point | `ap.c` | Open-system 802.11b AP: beacons, probe, auth, assoc and reassoc, deauth, a power-save queue with TIM and PS-Poll, intra-BSS relay, and join approval. |
| Router | `net.c` | ARP; DHCP (`192.168.77.100–199`, gateway and DNS `.1`); a DNS forwarder to the configured server, read on every query, so changes apply live; UDP NAT through one Windows socket per console port, keeping the console's port where possible for peer-to-peer; and a user-space TCP endpoint for each console connection, proxied with an ordinary `connect()`. |
| Engine / UI | `app.c`, `gui.c` | Worker thread, settings and log; Win32 window. |

## Hardware quirks found on a real unit

- **The hardware beacon engine never transmits.** `STA_CSR5`, the beacon counter, stays at 0 in every upload
  order tried, including the Linux "guardian byte" sequence. Consoles could find the network through probe
  responses but failed to join (DS error 51300). Beacons are now sent from software every 102.4 ms through
  the normal TX path with the descriptor's TIMESTAMP bit set, so the chip still inserts the TSF.
- **Unanswered unicast frames block the radio.** With the Linux defaults (retry 7, CWmax 10) each frame a
  console doesn't acknowledge holds the radio for about 420 ms. Beacons stall and USB writes time out. With
  CWmax 6 it's about 125 ms. All transmits go through a queue thread, so the router never waits on USB.
- The RX filter keeps control frames so that PS-Poll reaches the AP.
- The status light is wired to one of the MAC_CSR20 LED lines or GPIO pins. The in-app light finder tests
  them and saves the one that works.

## Diagnostics

`connector-log.txt` records every management frame, every DNS lookup and answer, every TCP connection's
result and byte counts, the UDP peers, and anything the router drops. It also records the first 700 bytes of
plain-text HTTP and peerchat conversations. That's how the Wi-Fi Plaza failure was traced to the service's
`checkProfile.asp` answering `400 Bad Request`.

`build/selftest.exe` exercises the AP and router end to end against the real internet without the connector.
