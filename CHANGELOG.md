# Changelog

## 1.1.1

- The connector's light now turns on by itself while the hotspot runs. The "Find light setting" button is gone.
  To keep the light off, put `led=off` in `connector.ini`.

## 1.1.0

- One program with a window. It includes the driver installer, the settings, console approval, the light
  finder and a live activity log, and replaces the earlier console program and batch files.
- Beacons are sent from software, because the chip's own beacon engine doesn't transmit. This fixed DS error
  51300.
- Transmits go through a queue thread, and the contention window is shorter, so consoles that miss a frame no
  longer stall beacons.
- DNS changes apply immediately to consoles that use automatic DNS.
- The detailed log covers DNS, TCP, UDP and dropped packets, and keeps the previous run's log.
- Closing the window switches the connector's radio off.

## 1.0.0

- First version, a console program: WinUSB driver binding, RT2570 user-space driver, 802.11b access point,
  DHCP, DNS and NAT.
