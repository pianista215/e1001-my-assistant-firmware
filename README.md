# e1001-my-assistant-firmware

Firmware for the [Seeed reTerminal E1001](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) (800×480, 4-level gray e-ink panel) that every hour:

1. Connects to WiFi as fast as possible.
2. Reads its own battery level.
3. Asks the [`my-assistant`](../my-assistant) backend for the image to paint (`GET /api/v1/display?battery=<pct>`).
4. Paints it on the e-ink panel.
5. Goes back to sleep and repeats — normally every hour, jumping straight from 00:00 to 06:00 (refreshes between 1am and 5am add no value, nobody's looking at the display then).

The very first time it boots (or after a manual reset gesture), it doesn't have any WiFi/endpoint configuration yet — instead it opens its own WiFi hotspot and shows a QR code + instructions on the e-ink panel so you can configure it from your phone. See [Section 3](#3-first-boot-setup-wifi--endpoint-portal) below.

See [`CLAUDE.md`](CLAUDE.md) (in Spanish) for the internal architecture, design decisions, and what's still pending verification on real hardware.

## What you need

- The reTerminal E1001 device, with its **USB-C** cable.
- A Linux laptop (this guide assumes that).
- [PlatformIO Core (CLI)](https://platformio.org/install/cli) — no need for the Arduino IDE or VSCode, though you can use the VSCode extension if you prefer a GUI.
- The `my-assistant` backend running somewhere reachable from the network the E1001 will join (same LAN, or a VPN like Tailscale). It can be plain HTTP or HTTPS with a self-signed certificate — see [Section 3](#3-first-boot-setup-wifi--endpoint-portal).
- A phone to complete the first-boot setup from (any modern browser + QR-capable camera app works).

## 1. Install PlatformIO

```bash
python3 -m venv ~/.platformio-venv
source ~/.platformio-venv/bin/activate
pip install platformio
pio --version
```

(You can use `pipx install platformio` instead if you prefer. Either way works; what matters is having the `pio` command available.)

The first time you build, PlatformIO downloads the Espressif toolchain (compiler, SDK, etc.) — a few hundred MB, takes a few minutes, only happens once.

## 2. Set up the serial port (one-time)

The E1001's USB-C goes through a **CH340** chip (USB↔serial bridge), not the ESP32-S3's native USB. On Linux the driver is already in the kernel, but your user needs permission to use the port:

```bash
sudo usermod -aG dialout $USER
```

Log out and back in (or reboot) for the group change to take effect. Without this, flashing fails with a permission error.

## 3. First-boot setup (WiFi + endpoint portal)

Device-specific configuration (WiFi credentials, the `my-assistant` endpoint URL, its auth token, and — for HTTPS endpoints — the server's TLS certificate fingerprint) is **no longer compiled into the firmware**. It's entered once, over the air, through a setup portal the device itself serves:

1. On first boot (or after the reset gesture below), the device opens its own WiFi hotspot (SSID like `E1001-Setup-XXXXXX`, WPA2-protected with a password unique to that device) and shows a QR code plus fallback text on the e-ink panel.
2. Scan the QR with your phone's camera app to join that hotspot directly (no typing required); if your phone doesn't auto-prompt to open a page, the panel also shows a fallback URL (`http://192.168.4.1/`) and the AP SSID/password as plain text.
3. Fill in the form: your real WiFi SSID/password, the `my-assistant` base URL (`http://...` or `https://...`, no trailing slash), and the auth token. There's no fingerprint field — for HTTPS endpoints the device fetches the certificate itself in the next step.
4. On submit, the device connects to the WiFi you entered. For an `https://` endpoint, it then connects to the server and shows you the certificate's SHA-256 fingerprint on the phone screen, asking you to confirm it (trust-on-first-use) before pinning it — confirm if it looks right (it always will unless you've set up a man-in-the-middle on purpose), or reject to go back and fix the URL if it looks wrong. Once a fingerprint is confirmed (or immediately, for `http://`), it makes a real request to the endpoint with the token — if anything's wrong (bad WiFi password, wrong URL, wrong token), the form tells you exactly what failed and lets you fix it without losing the hotspot. There's no "save anyway" bypass: the config is only ever persisted after a real, live validation succeeds.
5. Once validation passes, the config is stored on the device (in flash, survives power loss) and it reboots straight into the normal hourly cycle.

### Resetting to setup mode

If you ever need to change the WiFi network, endpoint, or token (device moved, backend redeployed elsewhere, etc.), hold the physical **KEY0** button for more than 10 seconds. The device waits to see whether it's a short press (normal manual refresh, unchanged behavior) or a genuine hold before doing anything — a quick tap never accidentally wipes your configuration. After 10s of continuous hold, the saved config is cleared and the device reboots straight into the setup portal from step 1 above.

## 4. Testing against a local server first

Before pointing at a production server, verify the format is as expected using the backend locally (see `my-assistant`'s own README):

```bash
cd ../my-assistant
cp .env.example .env   # if not already configured
go run ./cmd/server
```

In another terminal, check it responds as expected:

```bash
curl -H "Authorization: Bearer $AUTH_TOKEN" "http://localhost:8080/api/v1/display?battery=87" -o buffer.bin
go run ./cmd/preview --file buffer.bin --open   # opens a PNG preview of what the panel would show
```

When you get to the setup portal (Section 3), point the endpoint URL at your laptop's LAN IP, not `localhost` — the request comes from the E1001, not your laptop: `http://<YOUR-LAPTOP-IP>:8080`.

## 5. Configure the timezone (still compile-time)

```bash
cp secrets.ini.example secrets.ini
```

The only thing left in `secrets.ini` is:

- `TZ_STRING`: POSIX timezone string. Defaults to `Europe/Madrid` (`CET-1CEST,M3.5.0,M10.5.0/3`), which handles daylight saving automatically.
- An optional, commented-out `DEBUG_SLEEP_OVERRIDE_SEC` for fast test cycling (see [below](#confirming-deep-sleep-actually-works-without-waiting-an-hour)).

`secrets.ini` is gitignored — never committed. `secrets.ini.example` is, as a template. `platformio.ini` always references `secrets.ini`, so you need to create it even though it's now much shorter than before.

## 6. Build and flash

**Before plugging in the cable, flip the physical power switch (on the back) to ON.** USB-C provides power either way, but Seeed explicitly warns that you can't flash while the device is off or asleep. The green LED lights up for ~30s on boot, indicating it's initializing.

Connect the E1001 via USB-C and run:

```bash
pio run -e reterminal_e1001 -t upload
```

PlatformIO auto-detects the port (usually `/dev/ttyUSB0`). If you have more than one serial device connected and auto-detection fails, specify it explicitly:

```bash
pio run -e reterminal_e1001 -t upload --upload-port /dev/ttyUSB0
```

## 7. Watching the logs

Debug output goes through the same CH340 bridge (`Serial1`, not the native USB-CDC):

```bash
pio device monitor -b 115200
```

On a fresh flash (no config saved yet), you should see something like:

```
[MAIN] Wake cause: POWER-ON/RESET
[PORTAL] SoftAP 'E1001-Setup-1A2B3C' up (password 'e1001-4d5e6f'). Visit http://192.168.4.1/
```

...and after you complete setup from your phone, the device restarts and runs the normal cycle:

```
[MAIN] Wake cause: POWER-ON/RESET
[MAIN] Battery: 87%
[MAIN] WiFi connected.
[MAIN] Cycle OK.
[MAIN] Sleeping for 3600 s (or until the wake button is pressed)
```

The physical "KEY0" button forces an immediate cycle without waiting for the timer (useful to force a refresh, or retry right after fixing something) — the next boot shows `Wake cause: BUTTON (manual refresh)` instead of `TIMER (hourly schedule)`. Holding it past 10 seconds instead triggers the reset-to-setup gesture described in [Section 3](#resetting-to-setup-mode).

If you see nothing at all (even on a fresh boot), check the troubleshooting section.

## Confirming deep sleep actually works (without waiting an hour)

You don't need to wait out a full cycle to know it's really sleeping and not draining the battery:

- **Silence in the monitor after "Sleeping for..."**: the chip stops executing entirely, so you won't see any more logs until it wakes up. If you instead see continuous logs or repeated boots, it's not sleeping.
- **`Wake cause: 4` on the next boot**: `4` is `ESP_SLEEP_WAKEUP_TIMER`, confirming that boot came from a real deep-sleep timer wake, not a hang/reset (which would show as `Wake cause: 0`).
- **To watch several full cycles in a couple of minutes** instead of waiting an hour each, uncomment this line in your `secrets.ini` (shown commented-out as an example in `secrets.ini.example`):
  ```ini
  -D DEBUG_SLEEP_OVERRIDE_SEC=30
  ```
  With this, a full successful cycle (WiFi + request + paint) sleeps for only 30s instead of until the next hour on the dot — nothing else about the behavior changes. **Comment it out again and reflash before leaving the device running unattended**, or it will never sleep the real hour.

## Tests without spending panel refreshes

`time_scheduler` (the "when to wake up" logic) is pure logic with no Arduino dependency, and has tests that run on your laptop with no hardware at all:

```bash
pio test -e native
```

(Still needs `secrets.ini` to exist — `platformio.ini` always references it — even though the `native` environment doesn't use any of its values.)

## Troubleshooting

- **No `/dev/ttyUSB*` shows up when you plug in the cable**: try another USB-C cable (some are charge-only, no data lines) and check `dmesg | tail` after plugging in — you should see something mentioning `ch341`.
- **Flashing hangs / the port doesn't respond**: check the physical power switch is ON (having USB connected isn't enough) and that it's not asleep — if it just woke up, try pressing the green button on top before retrying `pio run -t upload`.
- **`pio run -t upload` fails with "Permission denied" on the port**: you're missing the `dialout` group (step 2), or haven't logged back in since being added to it.
- **`pio device monitor` shows nothing**: confirm `platformio.ini` has `-D ARDUINO_USB_CDC_ON_BOOT=0` in `build_flags` (it does by default) — without that flag, the firmware would write to the ESP32-S3's native USB, which on this board isn't wired to any cable.
- **The panel doesn't paint anything / hangs**: check the logs via `pio device monitor` — the e-ink driver has a 15s timeout waiting on the BUSY pin, so a panel fault shows up as an error log, not a silent hang.
- **Can't reach the setup portal / QR doesn't scan**: make sure you're within range of the device's hotspot and that your phone actually joined it (check its WiFi settings) before trying `http://192.168.4.1/`. The AP SSID/password shown on-panel are also printed to the serial log.
- **Setup form says the WiFi connection failed**: double-check the SSID/password — the E1001 only supports 2.4GHz networks (no 5GHz on the ESP32-S3).
- **Never see the fingerprint confirmation screen for an `https://` URL**: the device couldn't even open a TLS connection to the server — double-check the URL/port and that the server is actually up and reachable from the WiFi network you entered.
- **Rejected the fingerprint by mistake, or it timed out**: just resubmit the form — the device re-fetches and shows the certificate again. If you compare it against `<your-server>/api/v1/tls-cert` and it genuinely doesn't match, the URL may be pointing at the wrong server.
- **Setup form says `401`/token incorrect**: make sure the token matches `AUTH_TOKEN` in the server's `.env` exactly.
- **Need to change WiFi/endpoint/token later**: hold the physical KEY0 button for 10+ seconds to wipe the saved config and re-enter the setup portal — see [Resetting to setup mode](#resetting-to-setup-mode).
