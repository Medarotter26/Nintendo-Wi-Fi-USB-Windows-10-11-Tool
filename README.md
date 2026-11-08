# WFC Connector

**Use the Nintendo Wi-Fi USB Connector on Windows 10 and 11 (64-bit).**

The Nintendo Wi-Fi USB Connector (2005–2008) let a Nintendo DS or Wii get online through a PC. Nintendo's
software only works on 32-bit Windows XP and Vista: its driver is 32-bit, and 64-bit Windows can never load
it. WFC Connector replaces the whole thing with one small program. There's nothing to install except
Microsoft's own WinUSB driver.

![WFC Connector window](docs/screenshot.png)

## What it does

- Runs the connector as a Wi-Fi hotspot your DS, DS Lite, DSi or Wii can join.
- Gives consoles an address and carries their traffic through your PC's normal internet connection, wired or
  Wi-Fi. No Internet Connection Sharing or network settings are touched.
- Hands consoles a DNS server you choose, so they reach a replacement for Nintendo's shut-down online
  service. Change it any time; connected consoles switch immediately.
- Asks you the first time each new console tries to join, like Nintendo's original registration tool.
- Uses only Microsoft's signed, in-box WinUSB driver. Nothing unsigned is installed, and no test mode or
  certificates are needed.

## Download and use

1. Download `WFCConnector-v1.1.0-win64.zip` from the [Releases](../../releases) page and unzip it anywhere.
2. Plug the connector straight into the PC, not into a USB hub.
3. Run `WFCConnector.exe`. If it says **Driver needed**, click **Install driver** and allow the Windows
   permission prompt. This only needs doing once per PC.
4. When it says **Hotspot on**, go to the DS: open a game's **Nintendo WFC Settings** > **Nintendo WFC
   Connection Settings**, pick an empty connection, choose **Search for an Access Point**, and select
   **WFC-Connector**. Leave the IP address and DNS settings on automatic.
5. Click **Yes** when the program asks whether to allow the console.

> Use **Search for an Access Point** on the DS. The DS menu option called "Nintendo Wi-Fi USB Connector" only
> works with Nintendo's original registration software.

The first time a game goes online, Windows Firewall may ask about `WFCConnector.exe`. Allow it, so that
peer-to-peer games (trading, battles) can reach the DS.

### Settings

| Setting | What it does |
|---|---|
| Network name | The name the console sees when it searches. Default `WFC-Connector`. |
| Channel | Wi-Fi channel 1–11. Try 1 or 11 if the connection is poor. |
| DNS server | Which online service consoles reach. Default `178.62.43.212`. |
| Ask me before a new console can join | Off = any console in range can join. |

Settings are saved in `connector.ini` next to the exe, or in `%LOCALAPPDATA%\WFC Connector` if that folder
isn't writable. The same folder holds `connector-log.txt`, a detailed log that's useful when reporting
problems. **Open data folder** takes you there.

### The light

Nobody documented how the connector's light is wired. Click **Find light setting...** once, watch the
connector, and click **The light is on!** when it lights up. The setting is saved.

### Removing the driver

Open the window menu (click the icon at the top left) > **Remove connector driver...**.

## Which games work

The hotspot passes all of a console's traffic. Whether a feature works depends on the online service your
DNS points to. For example, with the default DNS, Pokémon Platinum's Wi-Fi Club works, but the Wi-Fi Plaza
fails with error 10626. The log shows that service's stats server answering the Plaza's lobby check with
`400 Bad Request`. Try another service's DNS if a feature you need isn't supported.

## Building

You need MinGW-w64 (`x86_64-w64-mingw32-gcc`), for example from WSL or MSYS2.

```
make            # build/WFCConnector.exe
make test       # build/selftest.exe
```

`selftest.exe` runs on Windows without the connector. A simulated DS joins the access point using raw 802.11
frames, gets an address over DHCP, and loads web pages through the router to the real internet. It also
covers DNS changes, power saving and a 1 MB download with packet loss.

Command-line extras: `WFCConnector.exe --set-dns 1.2.3.4` and `WFCConnector.exe --seconds 30 -v` (a hotspot run
without the window, logging to the console).

See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) for the technical details.

## Credits and license

The RT2570 chip set-up (registers, radio tables, TX descriptors) is ported from the Linux `rt2500usb` driver by
Ivo van Doorn and the rt2x00 project. That's why this project is licensed under the **GNU GPL v2 or later**;
see [LICENSE](LICENSE).

Not affiliated with or endorsed by Nintendo, Buffalo or Ralink. "Nintendo", "Nintendo DS", "Wii" and "Nintendo
Wi-Fi Connection" are trademarks of Nintendo. No Nintendo software is included.
