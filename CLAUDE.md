# CLAUDE.md

Architecture context for working in this repo. See also `README.md` for the
flashing guide, and the original plan in
`~/.claude/plans/la-idea-es-hacer-optimized-gem.md` if you need the full
reasoning behind the design decisions.

## What this is

ESP32 firmware (Seeed reTerminal E1001) that, every hour: connects to WiFi,
reads its battery, requests an already-rendered image from the `my-assistant`
backend (`../my-assistant` in this same checkout), paints it on the 800×480
4-gray e-ink panel, and goes back to sleep. No real `loop()`: `setup()` does
the entire cycle and always ends in `esp_deep_sleep_start()` — with one
exception: if there's no saved WiFi/endpoint config yet (or the user has
wiped it with a physical reset gesture), `setup()` enters a provisioning
portal (SoftAP + web) instead of the normal cycle — see "Design decision:
initial provisioning" below.

## API contract (my-assistant)

`GET {API_BASE_URL}/api/v1/display?battery=<1-100>`, header
`Authorization: Bearer <API_AUTH_TOKEN>`. Source of truth:
`../my-assistant/internal/display/codec.go`.

Response `application/octet-stream`, all big-endian:

```
offset  size  field
0       4     magic "EINK"
4       1     format version (1)
5       2     width  (uint16 BE) -> 800
7       2     height (uint16 BE) -> 480
9       1     bits per pixel (2)
10      ...   packed pixels, 2 bits/pixel, 4 pixels/byte, MSB-first,
              row-major, no row padding. 0=black, 1=dark gray,
              2=light gray, 3=white.
```

800×480 → exactly 96000 bytes of payload (96010 total), verified by
spinning up the real server (`go run ./cmd/server` in `../my-assistant`) and
doing a real `curl` during this firmware's development — not just reading
the code.

**The response comes with no `Content-Length`, using `Transfer-Encoding:
chunked`** (confirmed with `curl -D -` against the real server): Go's
`net/http` stops being able to precompute `Content-Length` as soon as the
handler writes more than the small internal buffer it uses to decide that,
and a ~96KB body always exceeds it. This was discovered on the first real
flash (it failed with `TOO_LARGE` because `HTTPClient::getSize()` returns
`-1` with no `Content-Length`). That's why `display_client.cpp` doesn't use
`getSize()` at all: it reserves a fixed-size buffer (`HTTP_MAX_RESPONSE_BYTES`)
in PSRAM and uses `HTTPClient::writeToStream()` with its own `Stream`
(`MemoryStream`) that dumps into it — `writeToStream()` requires a
`Stream*`, not a `Print*`, even though it only ever writes into it (hence
`MemoryStream`'s empty read-side methods). It decodes chunked encoding
transparently, so it would work the same if the backend ever switched to a
fixed `Content-Length`.

`display_client.cpp` validates this byte by byte before ever touching the
display (magic, version, bpp, exact dimensions, payload length). It rejects
any width/height other than 800×480: the physical panel can't show any
other resolution, so there's no point trying to support one.

## Design decision: "raw" driver instead of GxEPD2/Seeed_GFX

`eink_driver.cpp` is a direct port (not a wrapper) of Seeed's official
example
[`examples/base/GxEPD2_reTerminal_E1001_Gray4`](https://github.com/Seeed-Projects/OSHW-reTerminal-Series-E-D/blob/main/examples/base/GxEPD2_reTerminal_E1001_Gray4/GxEPD2_reTerminal_E1001_Gray4.ino)
in `Seeed-Projects/OSHW-reTerminal-Series-E-D`. Despite the example's name,
**it doesn't use GxEPD2 or Seeed_GFX** — it's a hand-rolled UC8179 driver
over plain SPI.

**Why**: that example packs its framebuffer (`Gray4Canvas`) exactly the same
way `codec.go` does — 2bpp, 4px/byte, MSB-first, row-major, no padding, same
0=black..3=white convention. That means **the HTTP response body can be
passed straight through** to the panel's bit-plane upload routine
(`eink::drawFrame`), with no decoding into an intermediate framebuffer and
no dragging in a large library (GxEPD2/TFT_eSPI) just for "one full-screen
refresh per hour." The grayscale LUT tables and UC8179 command sequence
(`0x01` power, `0x30` PLL, `0x82` VCOM, `0x06` booster, `0x04` power-on,
`0x00` panel setting, `0x61` resolution, `0x20-0x24` LUTs, `0x10`/`0x13`
DTM1/DTM2, `0x12` refresh, `0x02`/`0x07` sleep) are copied verbatim from
Seeed's example — they're opaque calibration data, not something to derive
by hand.

**Difference from the vendor version**: the original example has no
timeout on waiting for the BUSY pin (`checkBusy()` can hang forever). Here,
`waitBusy()` has a bounded timeout (`EINK_BUSY_TIMEOUT_MS`, 15s) — a stuck
panel can't leave the device hung, draining the battery indefinitely.

`BOARD_SCREEN_COMBO = 520` (UC8179, 800×480) is the constant Seeed uses
across all its official examples to identify this specific panel.

## Design decision: time — RTC (PCF8563) + SNTP correction

The E1001 has a hardware **PCF8563** RTC (I2C 0x51, SCL=GPIO20, SDA=GPIO19)
with its own coin-cell battery, independent of the ESP32's internal RTC
(which only survives *deep sleep*, not a full reset/EN or a total loss of
power).

Approach (`main.cpp` + `rtc_pcf8563.cpp`):
1. On boot: read the PCF8563 → `settimeofday()` immediately (fast, no
   network needed). If the VL (voltage-low) flag is set, the time isn't
   trustworthy and is ignored.
2. After connecting WiFi: try SNTP (`configTzTime` + `getLocalTime`,
   timeout `SNTP_SYNC_TIMEOUT_MS`). If it works, it's authoritative — it
   gets written back to the PCF8563 so drift doesn't accumulate cycle over
   cycle.
3. If SNTP fails but the PCF8563 gave a valid time this cycle, or if a sync
   has already happened at least once since the last power-on (the ESP32's
   own system clock keeps ticking on its own between deep-sleep cycles),
   `time(nullptr)` is used.
4. If there's never been a trustworthy time (true first boot, RTC coin
   cell just installed): don't try to compute an hourly schedule off an
   unknown clock — sleep a short fixed interval (`FIRST_BOOT_RETRY_SLEEP_SEC`)
   until an SNTP sync succeeds.

**Why not just SNTP**: the PCF8563 gives a reasonable time even without a
network (or before WiFi connects), and it survives a total loss of power,
which neither the ESP32's internal RTC nor any RAM-based state can do.

**Why not just the PCF8563**: it's simpler to trust SNTP to correct drift
whenever there's a network, instead of implementing "how long since the
last sync" logic to decide when to resync.

**Real finding on the device**: `SNTP_SYNC_TIMEOUT_MS` started at 3000ms
and turned out to be too tight in practice — on a network with confirmed
real internet access, a cycle's first SNTP sync (DNS resolution of
`pool.ntp.org` plus the NTP packet round trip) can take longer than that,
so the device fell into the "no trustworthy time yet" branch
(`FIRST_BOOT_RETRY_SLEEP_SEC`, a 5-minute wait) on cycles where SNTP would
have succeeded with a bit more headroom — confirmed by forcing a second
cycle with the button, which did sync. Raised to 8000ms: it doesn't
penalize the normal case (a successful sync doesn't take longer just
because it has more budget available) and avoids spending an entire retry
cycle (with its own WiFi reconnect) on a sync that only needed a few more
seconds.

## Design decision: physical manual-refresh button

`main.cpp` enables `esp_sleep_enable_ext1_wakeup()` on `PIN_WAKE_BUTTON`
(GPIO3, the "KEY0" button that Seeed's own `LowPower_DeepSleep.ino` example
uses) **in addition to** the timer, inside `goToSleep()` — that is, on
*every* sleep path (normal cycle, error backoff, first boot), not just on
success. The ESP32 allows several wakeup sources to be active at once;
whichever fires first wins. This lets you force an immediate refresh (or
retry after fixing WiFi/config) without waiting for the timer. The cycle
that runs on waking is identical no matter which source woke it — only the
`wakeupCauseString()` log line at the start changes.

There's one exception to this stemming from wireless provisioning (see the
corresponding section further below): if the device is already configured
and wakes up from this button, `main.cpp` waits up to `RESET_HOLD_MS` (10s)
before launching the refresh, in case the press is actually the reset
gesture. A short press still behaves exactly as before; only that
verification delay is added.

**Power impact**: minimal. The ~14µA figure Seeed itself reports for this
board family comes from that same example, which already has button wakeup
enabled — it's not a "timer-only" number that this adds something new on
top of.

## Sleep algorithm (`time_scheduler.cpp`)

Pure logic over `struct tm`, with no Arduino dependencies — testable with
`pio test -e native` with no hardware. Also verified with an ad-hoc harness
compiled directly with `g++` (without PlatformIO installed) during
development, including the year-rollover case.

`computeNextWake(now)`:
1. `target = now` with minutes/seconds zeroed, `+1` hour, normalized with
   `mktime`/`localtime_r` (handles day/month/year overflow and DST).
2. If `target.tm_hour` falls in `[1,5]`, jump straight to `tm_hour = 6`
   (avoids refreshes between 1am and 5am).
3. `sleepSeconds = mktime(target) - mktime(now)`.

It's recomputed from the real "now" every time — there's no accumulating
counter — so it self-corrects even if the previous cycle woke up a few
seconds late/early, or if the first boot doesn't land exactly on the hour.

## Per-device config

Only `TZ_STRING` (and the optional development flag
`DEBUG_SLEEP_OVERRIDE_SEC`) remain compile-time: `platformio.ini` uses
`extra_configs = secrets.ini` (see `secrets.ini.example`) to inject them as
`-D` flags. `secrets.ini` is gitignored; `.example` is not. `config.h` has
an `#error` guard only for `TZ_STRING`.

WiFi (SSID/password), the endpoint URL, the auth token, and (if the
endpoint is HTTPS) the certificate's SHA-256 fingerprint are **no longer**
compile-time secrets — they're entered once through the wireless
provisioning portal (see the next section) and saved in NVS via
`Preferences` (`device_config.{h,cpp}`, namespace `"e1001cfg"`), which
survives a real power-on/EN reset (unlike `sleep_state.h`'s
`RTC_DATA_ATTR PersistentState g_state`, which only survives deep sleep).
The device's display/portal language preference (`en`/`es`, see `i18n.h`)
is stored the same way, as one more `DeviceConfig` field. `main.cpp` loads
that config with `device_config::load()` at the start of every normal
cycle.

The endpoint can be `http://` (local network/VPN, no TLS) or `https://`
(self-signed cert from the `my-assistant` backend, with fingerprint pinning
— see `display_client.cpp`). There's no longer a fixed "HTTP only"
decision: the saved URL's scheme decides the transport on every request.

## Design decision: initial provisioning (SoftAP + QR + TLS fingerprint)

With no config saved (`device_config::isConfigured()` false — true first
boot, or after the reset gesture described further below),
`setup_portal::run()` takes over `setup()` and **never returns**: it's the
only place in the firmware with an active loop with the radio on instead of
immediately ending in deep sleep.

Flow: `WiFi.mode(WIFI_AP_STA)` (never plain `WIFI_AP` — STA is needed
simultaneously for live validation, see below) → `WiFi.softAP()` with SSID
`E1001-Setup-<hex chip id>` and a 12-character WPA2 password, both
deterministically derived from `ESP.getEfuseMac()` (same QR on every retry,
doesn't change between cycles) → a catch-all `DNSServer`
(`dns.start(53, "*", WiFi.softAPIP())`) + `WebServer` responding 302 to `/`
on Apple/Android/Windows/Firefox's captive-portal probe routes
(`/hotspot-detect.html`, `/generate_204`, `/ncsi.txt`, etc.) so the phone
shows the "join network" popup automatically → `eink::drawProvisioningScreen()`
is painted **once** (QR `WIFI:T:WPA;S:...;P:...;;` for camera auto-join,
plus SSID/password/URL as text fallback) → a loop of
`dnsServer.processNextRequest()` + `server.handleClient()` until the user
completes the form or `PORTAL_INACTIVITY_TIMEOUT_MS` (10 min) passes with
no activity, in which case everything shuts down and
`goToSleep(PORTAL_RETRY_SLEEP_SEC)` runs — on waking, `isConfigured()` is
still `false` so the portal is re-entered on its own, with no extra logic
in `main.cpp`.

**Live validation before saving** (`setup_portal.cpp`, `handleSave()`):
with the AP already up, it connects the STA to the candidate WiFi
(`wifiBeginConnect`/`wifiWaitConnected` with an empty cache) and, if it
connects, calls the **same** production `fetchDisplayBuffer()` with a test
`battery=50` — zero duplicated HTTP logic between the normal cycle and the
portal's validation. Each type of failure (wifi, TLS, fingerprint
mismatch, 401, other HTTP status, unexpected response format) is
translated into a specific message on the web page, with the form
pre-filled so it can be corrected without losing the hotspot. There's no
"save anyway" escape hatch — the user deliberately decided that config is
only persisted after a successful live validation (including, for HTTPS, a
human confirming the certificate — see below). Important: this validation
never calls `wifiDisconnect()` (which does `WiFi.mode(WIFI_OFF)` and would
also tear down the AP) — it uses `WiFi.disconnect(false)` to drop only the
STA side.

**Real finding on the device (validation blocked the portal)**: in
`WIFI_AP_STA`, the ESP32 can only have the AP and the STA on the same radio
channel — as soon as the STA associates with the candidate WiFi, if it's on
a different channel than the one the AP was using with the phone, the chip
forces the AP to hop to that channel, and the phone suffers a brief
disconnect/re-association at that instant. The first version of
`handleSave()` was fully synchronous (up to ~23s blocking `WebServer`'s
single thread: 15s of `wifiWaitConnected` + 8s of `fetchDisplayBuffer`), so
if that channel hop coincided with the single in-flight `POST /save`
request, that request was lost with no further retry possible and the page
was left with a spinner that never reacted — confirmed by the user in real
testing ("the portal got stuck not reacting to the buttons"; the device
itself wasn't hung, only that one specific HTTP response never arrived).
Fixed by moving validation to a background FreeRTOS task (`validationTask()`,
`xTaskCreate` with a 16384-byte stack — double the 8192-byte stack
`loopTask` already runs this same `fetchDisplayBuffer()` with, TLS
handshake and all, without issue — and priority 1, same as `loopTask`),
leaving `run()` free to keep servicing
`dnsServer.processNextRequest()`/`server.handleClient()` without long
blocks. A shared state (`ValidationStage` + `ValidationState`, protected by
a `portMUX_TYPE` spinlock, since it's read/written from both the
background task and the main loop's HTTP handlers) exposes `Idle →
ConnectingWifi → [FetchingCertificate → AwaitingFingerprintConfirmation,
https only] → TestingEndpoint → Success`/`Failed`; `handleSave()` now
responds instantly with a page that polls `GET /validate-status` (JSON)
every ~1s via `fetch()` with its own 4s `AbortController` (without this, a
poll that stalls mid-flight from the same channel hop would reproduce the
same "stuck" symptom, just in the JS instead of the server) — if a poll
fails, it's simply retried on the next cycle instead of being shown as an
error, which is the real resilience improvement over the previous
synchronous design. Important invariant: `validationTask()` never touches
the `WebServer` object (not thread-safe), only the mutex-protected state.
`tryStartValidation()` does an atomic check-and-set so a double-tap on
"Save" doesn't launch two tasks racing over the WiFi radio. `handleRoot()`
uses the same state to pre-fill the form after a failure, or to return the
progress page if the user navigates to `/` with a validation already in
flight.

**Why `wifiBeginConnect()` no longer sets `WiFi.mode()` internally**: it
used to unconditionally set `WIFI_STA`; if the portal reused it as-is,
every validation attempt would tear down the SoftAP the user's phone is
currently connected to. Now the caller decides the mode: `main.cpp` sets
`WIFI_STA` before calling it (normal cycle), `setup_portal.cpp` is already
in `WIFI_AP_STA` from `run()` and never touches it again.

**TLS fingerprint**: `display_client.cpp` connects with `WiFiClientSecure`
using `setInsecure()` (no CA chain — fingerprint pinning replaces chain
verification) and `.verify(fingerprintHex, nullptr)` **before**
`http.begin(secureClient, url)`; `HTTPClient::connect()` reuses an
already-connected client (seen in the installed core's `.cpp`), so there's
no second unverified handshake. The `my-assistant` backend computes this
same fingerprint (`openssl x509 -fingerprint -sha256` format) and exposes
it unauthenticated at `GET /api/v1/tls-cert` — originally meant to be
copied by hand from the phone, though since the interactive TOFU
confirmation (see below) that's no longer necessary: the device itself
fetches it and displays it.

**Interactive fingerprint confirmation (TOFU)**: the user decided that
copying the fingerprint by hand was unnecessary friction (the backend
itself is what generates the self-signed certificate), so the form's
manual field was removed entirely — for HTTPS, `handleSave()` always
leaves `tlsFingerprint` empty and `validationTask()` resolves it on its
own: after connecting to WiFi, `probeTlsFingerprintInsecure()`
(`display_client.h`/`.cpp`) connects over TLS with `setInsecure()` and
**zero verification, not even of the fingerprint** — deliberately, this
function exists *only* for this TOFU flow, never for the normal cycle —
and returns the real fingerprint via
`WiFiClientSecure::getFingerprintSHA256()`. That fingerprint is published
in the shared state (stage `AwaitingFingerprintConfirmation`, field
`pendingFingerprint`) and the web page shows it with two buttons
("Confirm"/"Reject"); `validationTask()` waits for the decision with its
own polling (`vTaskDelay(300ms)` on a new `FingerprintDecision decision`
field, same `portMUX_TYPE` as the rest of the state — no new
semaphore/queue needed, since the decision depends on a human pressing a
button, so 300ms of internal latency is irrelevant) until it changes or
`PORTAL_FINGERPRINT_CONFIRM_TIMEOUT_MS` (5 min — this is a real trust
decision, no need to rush it, and the phone's `/validate-status` polling
keeps refreshing `g_lastActivity` via `touchActivity()` in the meantime, so
the portal's overall 10-minute timeout isn't triggered just by waiting on
this confirmation) expires. If confirmed, that fingerprint is locked into
`candidate.tlsFingerprint` and the flow continues exactly as before
(`fetchDisplayBuffer()` now genuinely pinned, also validating the token);
if rejected or the deadline expires, `Failed` with nothing saved. `POST
/validate-confirm` (`handleValidateConfirm()`) is the new endpoint that
receives the phone's decision; `trySetFingerprintDecision()` only applies
it if the state is still `AwaitingFingerprintConfirmation`, so a duplicate
or late confirmation (e.g. a browser retry after the JS's own timeout)
doesn't contaminate an attempt that's already closed or restarted.

**Real finding on the device (`WiFiClientSecure::setTimeout()` is in
seconds, not ms)**: while testing TOFU confirmation on real hardware, the
"Fetching the server's certificate..." step would hang (or take a very
long time to error out) instead of failing within seconds as
`fetchDisplayBuffer()`'s documentation claimed. Cause:
`WiFiClientSecure::setTimeout(uint32_t seconds)` takes **seconds**, not
milliseconds — unlike `HTTPClient::setTimeout()`, which does the
conversion internally (`(timeout + 500) / 1000` before calling the
underlying `Client`, seen in `HTTPClient.cpp`). Both `fetchDisplayBuffer()`
and `probeTlsFingerprintInsecure()` (`display_client.cpp`) were calling
`secureClient.setTimeout(HTTP_TIMEOUT_MS)`, passing `8000` directly, which
the core interpreted as **8000 seconds** (~2h13min) instead of 8s.
Additionally, `sslclient->handshake_timeout` (the TLS handshake phase's own
timeout, separate from the TCP connection timeout) has its own 120000ms
default and was never touched, so even when the TCP `connect()` was fast, a
handshake that stalled (e.g. coinciding with the AP's channel hop in
`WIFI_AP_STA` — see above) could take up to 2 minutes to fail. Fixed by
calling `secureClient.setTimeout(HTTP_TIMEOUT_MS / 1000)` **and**
`secureClient.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000)` (this one also
in seconds) in both places, genuinely bounding the entire HTTPS handshake
to the documented `HTTP_TIMEOUT_MS` budget. While at it, logging over
`Serial1` was added in `validationTask()` (SSID/URL at the start, the TLS
probe's result and duration, the fingerprint obtained, the confirmation
result, `fetchDisplayBuffer()`'s final result) so any future failure during
setup can be diagnosed unambiguously.

**Real finding on the device (first flash with an HTTPS endpoint)**:
this core's `.verify(fingerprint, host)` chains two checks — first the
fingerprint, and if it matches, also that `host` appears in the
certificate's SAN/CN (`verify_ssl_dn()` in `ssl_client.cpp`). That second
check treats **all** SAN entries as ASCII text without looking at their
ASN.1 type; for an `iPAddress`-type SAN (exactly what `my-assistant
--https` generates for local IPs, see `generateSelfSignedCert()` in
`cmd/server/tls.go`) the content is 4 binary octets of the IP, not the
string `"192.168.x.x"` — so the host check **always fails** for an
IP-based endpoint, even with a byte-perfect fingerprint match, and
`fetchDisplayBuffer()` reported it as `TLS_FINGERPRINT` without
distinguishing which of the two checks had failed. Confirmed with a real
log: `Display fetch failed: TLS_FINGERPRINT (http=0)` with the fingerprint
copied exactly from `/api/v1/tls-cert`. Fixed by passing `nullptr` as
`domain_name` (`display_client.cpp`) — the core's own code accounts for
this case (`if (domain_name) ... else return true;`) and skips the host
check, which is redundant anyway: fingerprint pinning already authenticates
the exact certificate, a stronger guarantee than checking a name/IP.
Additionally, `DisplayFetchResult::actualFingerprintHex` is now populated
(via `WiFiClientSecure::getFingerprintSHA256()`) whenever there's a genuine
fingerprint mismatch, and both `main.cpp` and the portal's error message
(`setup_portal.cpp`) show it next to the expected one, so any real future
failure can be diagnosed unambiguously.

**Reset gesture**: in addition to the KEY0 button behavior already
described (forces an immediate cycle), if the device **is already
configured** and wakes up via `ESP_SLEEP_WAKEUP_EXT1`, `main.cpp` doesn't
launch the normal cycle right away — it first polls
`digitalRead(PIN_WAKE_BUTTON)` for `RESET_HOLD_MS` (10s). If it's released
before that, it's a normal press and falls through to the usual
manual-refresh cycle, unchanged. If it stays pressed for the full 10s,
it's interpreted as the reset gesture: `device_config::clear()` +
`ESP.restart()`, which restarts straight into `setup_portal::run()`
(config already empty). The user confirmed that even though the chassis
has two other physical buttons, they're deliberately not used for this
gesture — it's KEY0-held only.

## Module structure

| File | Responsibility |
|---|---|
| `main.cpp` | Orchestrates the full cycle; the only place with `RTC_DATA_ATTR PersistentState g_state`; button-hold reset gesture; handles errors/backoff |
| `config.h` | Non-secret constants: pins, timeouts, thresholds, provisioning constants; validates that `TZ_STRING` exists |
| `sleep_state.h` | `struct PersistentState` (WiFi cache, failure counter, time-synced flag) — RTC memory, wiped on power-on/EN reset |
| `sleep_control.{h,cpp}` | `goToSleep()` (timer + EXT1 wakeup, RTC pull-up) — shared by `main.cpp` and `setup_portal.cpp` |
| `device_config.{h,cpp}` | `struct DeviceConfig` (wifi, endpoint, token, fingerprint, language) over `Preferences`/NVS — survives power-on/EN reset, unlike `sleep_state.h` |
| `setup_portal.{h,cpp}` | SoftAP + captive portal (DNSServer + WebServer) + live validation + provisioning-mode orchestration; the only place with an active loop (doesn't immediately end in deep sleep) |
| `wifi_manager.{h,cpp}` | Fast reconnect with a BSSID/channel cache in RTC memory, falling back to a full scan; no longer sets `WiFi.mode()` internally |
| `battery.{h,cpp}` | ADC read on GPIO1/GPIO21 (8-sample average) → 1-100 percentage |
| `rtc_pcf8563.{h,cpp}` | I2C driver for the hardware RTC |
| `display_client.{h,cpp}` | HTTP(S) client + fingerprint pinning + strict EINK format validation; reused as-is by the portal's live validation |
| `eink_driver.{h,cpp}` | Raw UC8179 driver (init, LUTs, bit-plane upload, refresh, sleep, error screen, provisioning screen with QR) |
| `i18n.{h,cpp}` | English/Spanish string tables for the e-ink panel and the setup portal's web UI, selected by the device's saved language (see "Per-device config" above) |
| `time_scheduler.{h,cpp}` | Pure logic for computing the next wake time |

## Design decision: battery is read before touching WiFi

`main.cpp` calls `readBatteryPercent()` **before** `wifiBeginConnect()`, not
in parallel with the WiFi negotiation like an earlier version did. The
current burst the radio draws while associating momentarily sags the
battery rail (internal-resistance voltage drop), and sampling the ADC right
at that moment reads a lower voltage than the battery's real level —
enough for the reported percentage to jump several points between cycles
without the battery actually having lost that energy. `battery.cpp` also
averages 8 ADC samples to smooth out single-reading noise. The awake-time
cost of reading sequentially instead of in parallel is a handful of
milliseconds — irrelevant next to the accuracy gained in the battery
report.

## Error handling

Never retries in an active loop with the radio on. WiFi, HTTP, or corrupt
response failures increment `g_state.consecutiveFailures` (in RTC memory)
and apply a bounded exponential backoff (`BACKOFF_BASE_SEC << failures`,
capped at `BACKOFF_MAX_SEC`). An error screen (`eink::drawErrorScreen`) is
only drawn after `ERROR_SCREEN_AFTER_N_FAILURES` consecutive failures, so
transient blips don't spend an e-ink refresh — in the meantime the panel
simply keeps the last good image (e-ink doesn't consume power by staying
still, so that's the best possible fallback). The counter resets on any
successful cycle.

## What's verified vs. what's still pending on the physical device

**Verified during development** (without having the device in hand):
- The exact binary format, by spinning up a real `go run ./cmd/server` and
  doing a real `curl` against `/api/v1/display` (800×480, 96010 bytes,
  correct magic/version/bpp, exact payload).
- The backend's real HTTP error codes (401 with no token, 400 with no
  `battery`).
- `time_scheduler`'s logic, actually compiled and run (every hour 0-23,
  the 01-05→06 skip, boot at an arbitrary hour, year rollover).
- The pins, UC8179 command sequence, battery pattern, and PCF8563
  registers come from Seeed examples confirmed to work
  (`examples/base/GxEPD2_reTerminal_E1001_Gray4`, `Battery_Monitor.ino`,
  `RTC_PCF8563.ino`, `LowPower_DeepSleep.ino` in
  `Seeed-Projects/OSHW-reTerminal-Series-E-D`), read directly from the
  repository, not from memory.
- Wireless provisioning (SoftAP + QR + TLS fingerprint) compiles cleanly
  for `reterminal_e1001` with the `ricmoo/QRCode` library added, and
  `time_scheduler`'s tests under `pio test -e native` still pass (10/10;
  needed adding `test_build_src = true` to `[env:native]` in
  `platformio.ini`, a pre-existing configuration gap unrelated to this
  feature — without that flag, `pio test` wasn't linking
  `time_scheduler.cpp` into the test binary). The installed Arduino-ESP32
  2.0.17 core's real `WiFiClientSecure::verify()`/`verify_ssl_fingerprint()`
  signature was confirmed by reading `ssl_client.cpp`/`.h` directly: it
  accepts the fingerprint with or without `:` and spaces, so
  `normalizeFingerprint()` doesn't need to reintroduce separators. Also
  confirmed by reading `HTTPClient.cpp` that `HTTPClient::connect()` reuses
  a `Client` that's already connected instead of reconnecting, which is
  what allows connecting and manually verifying the fingerprint before
  `http.begin(secureClient, url)` with no second unverified handshake in
  between.

**Still pending verification on real hardware** (can't be checked without
the device):
- That the ported UC8179 driver actually paints correctly on this specific
  physical unit — test Seeed's own demo pattern first (not included here,
  it's in their repo) before trusting real content from the API.
- The BUSY pin's real polarity/timing and a full refresh's real duration
  (to adjust `EINK_BUSY_TIMEOUT_MS` if needed).
- Real flash size (`esptool.py flash_id`) — Seeed's wiki says 32MB but
  PlatformIO's board definition for `seeed_xiao_esp32s3` assumes 8MB; it
  works the same with `default_8MB.csv`, it just wastes space if the real
  size is larger.
- Real WiFi reconnect times with the BSSID/channel cache (is it really
  sub-second like the community reports for ESP32 Arduino?).
- That `HTTPClient::setConnectTimeout()` exists as-is in whatever
  ESP32-Arduino core version PlatformIO resolves at build time (the method
  is present in recent versions; if the build fails here, that's the first
  thing to check).
- Real deep-sleep power consumption (~14µA reported by Seeed for this
  board family, not measured here).
- That the physical button (GPIO3, "KEY0") actually wakes the device on
  this specific unit and is the button one would expect when looking at
  the case — the polarity (`ANY_LOW`) and the RTC domain's pull-up come
  from Seeed's example, not yet verified on this hardware.
- That the phone really triggers the "join network" popup with the
  captive-portal routes implemented in `setup_portal.cpp` — the exact
  behavior varies by iOS/Android/Windows version and isn't 100%
  predictable from the code; the `http://192.168.4.1/` URL shown on the
  panel is always there as a fallback.
- Real legibility of the provisioning QR on the 4-gray panel (contrast,
  module size) — `QR_VERSION`/the box size in `eink_driver.cpp` are sized
  with margin over the expected payload, but no real QR has been scanned
  from this panel yet.
- That `/validate-status` polling (with its 4s `AbortController` and ~1s
  retry) actually absorbs the AP's channel hop in `WIFI_AP_STA` without
  the user having to manually reload the page — the channel hop itself is
  already confirmed to happen on real hardware (see "Real finding" above);
  what's left to confirm is that the async mitigation makes it
  imperceptible in practice and not just on paper.
- Real battery consumption with the SoftAP + `DNSServer` + `WebServer`
  active for several minutes, to calibrate
  `PORTAL_INACTIVITY_TIMEOUT_MS`/`PORTAL_RETRY_SLEEP_SEC` with real data
  instead of an estimate.
- Reliability of the 10s reset gesture with the normal pull-up
  (`INPUT_PULLUP`) that gets reconfigured right after waking, versus the
  RTC domain's pull-up used during sleep itself.
- That `WiFiClientSecure::verify()` genuinely rejects an incorrect
  fingerprint (not just that it accepts a correct one, already confirmed
  on real hardware after the `domain_name` fix described above) without
  hanging in the TLS handshake.
- That `ESP.restart()` (used both when saving config and when applying the
  reset gesture) preserves `RTC_DATA_ATTR g_state` on this specific unit —
  documented as such in ESP-IDF (only a real power-on/EN resets it), not
  empirically confirmed on this hardware yet.
