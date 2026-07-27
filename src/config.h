#pragma once

// Non-secret compile-time constants: pins, timeouts, thresholds. WiFi
// credentials and API endpoint/token/fingerprint are no longer compile-time
// secrets -- they're entered through the first-boot SoftAP setup portal
// (see device_config.h/setup_portal.h) and persisted in NVS. Only
// TZ_STRING (and the optional DEBUG_SLEEP_OVERRIDE_SEC dev flag) remain as
// -D build flags from secrets.ini (see secrets.ini.example).

#ifndef TZ_STRING
#error "TZ_STRING not defined -- did you copy secrets.ini.example to secrets.ini?"
#endif

#include <cstddef>
#include <cstdint>

// ---- Panel (E1001, UC8179 controller, 800x480, 4-level gray) ----
constexpr int PIN_EPD_SCK = 7;
constexpr int PIN_EPD_MOSI = 9;
constexpr int PIN_EPD_CS = 10;
constexpr int PIN_EPD_DC = 11;
constexpr int PIN_EPD_RES = 12;
constexpr int PIN_EPD_BUSY = 13;

constexpr uint16_t EPD_EXPECTED_WIDTH = 800;
constexpr uint16_t EPD_EXPECTED_HEIGHT = 480;

// Vendor's own demo has no timeout on the BUSY-wait loop; we add one so a
// stuck panel can never hang the device forever on battery.
constexpr unsigned long EINK_BUSY_TIMEOUT_MS = 15000;

// ---- Battery (resistor divider + ADC, no fuel-gauge IC on this board) ----
constexpr int PIN_BATTERY_ADC = 1;
constexpr int PIN_BATTERY_ENABLE = 21;
constexpr float BATTERY_VOLTAGE_EMPTY = 3.27f;  // ~0%
constexpr float BATTERY_VOLTAGE_FULL = 4.15f;   // ~100%

// ---- RTC (PCF8563, battery-backed, independent of the ESP32's own RTC) ----
constexpr int PIN_I2C_SDA = 19;
constexpr int PIN_I2C_SCL = 20;
constexpr uint8_t PCF8563_ADDR = 0x51;

// ---- Serial console: the USB-C port is bridged via CH340 to UART0, wired
// to Serial1 on these pins -- NOT the native USB-CDC `Serial`. ----
constexpr int PIN_SERIAL_RX = 44;
constexpr int PIN_SERIAL_TX = 43;
constexpr unsigned long SERIAL_BAUD = 115200;

// Physical "KEY0" button (Seeed's own naming/pin choice for this board,
// used identically in their LowPower_DeepSleep.ino example) -- lets a
// manual press force an immediate cycle instead of waiting for the next
// scheduled wake. Only GPIO0-21 can be an EXT1 deep-sleep wakeup source on
// the ESP32-S3.
constexpr int PIN_WAKE_BUTTON = 3;

// ---- WiFi ----
constexpr unsigned long WIFI_FAST_RECONNECT_TIMEOUT_MS = 2500;
constexpr unsigned long WIFI_FULL_CONNECT_TIMEOUT_MS = 15000;

// ---- HTTP ----
constexpr unsigned long HTTP_TIMEOUT_MS = 8000;
constexpr size_t HTTP_MAX_RESPONSE_BYTES = 200000;  // defensive cap before allocating

// ---- SNTP ----
constexpr unsigned long SNTP_SYNC_TIMEOUT_MS = 3000;
constexpr const char* SNTP_SERVER_1 = "pool.ntp.org";
constexpr const char* SNTP_SERVER_2 = "time.google.com";

// ---- Sleep scheduling: skip the overnight hours that add no value ----
constexpr int NIGHT_SKIP_FROM_HOUR = 1;   // inclusive
constexpr int NIGHT_SKIP_TO_HOUR = 5;     // inclusive
constexpr int NIGHT_SKIP_TARGET_HOUR = 6;
// Used only when no trustworthy time source exists yet (very first boot,
// RTC coin cell just installed): retry soon rather than guess a schedule.
constexpr uint32_t FIRST_BOOT_RETRY_SLEEP_SEC = 5 * 60;

// ---- Error backoff (never busy-retry with the radio on) ----
constexpr uint8_t ERROR_SCREEN_AFTER_N_FAILURES = 3;
constexpr uint32_t BACKOFF_BASE_SEC = 5 * 60;
constexpr uint32_t BACKOFF_MAX_SEC = 60 * 60;

// ---- Provisioning / reset-to-setup gesture ----
// Holding the wake button this long on an EXT1 wake (while already
// configured) clears the saved config and re-enters the setup portal.
// Below this, it's treated as a normal manual-refresh press.
constexpr unsigned long RESET_HOLD_MS = 10000;
constexpr unsigned long RESET_HOLD_POLL_MS = 100;
// If nobody finishes the setup form, stop burning battery on AP/DNS/HTTP
// and go back to deep sleep for a while -- device_config::isConfigured()
// is still false, so the next wake re-enters the portal automatically.
constexpr uint32_t PORTAL_INACTIVITY_TIMEOUT_MS = 10UL * 60 * 1000;
constexpr uint32_t PORTAL_RETRY_SLEEP_SEC = 5 * 60;
// Timeout for the STA connection attempted live during setup validation
// (no cached BSSID/channel yet, so always the "full" timeout).
constexpr unsigned long PORTAL_VALIDATE_WIFI_TIMEOUT_MS = 15000;

// ---- Provisioning SoftAP ----
constexpr const char* AP_SSID_PREFIX = "E1001-Setup-";

// ---- Provisioning QR code (WiFi-join payload) ----
// QR version 5 (37x37 modules) comfortably covers a
// "WIFI:T:WPA;S:<ssid>;P:<password>;;" payload (SSID up to 32 chars +
// our own fixed-length generated password) at ECC level LOW -- see
// qrcodegen's capacity table if the payload format ever grows.
constexpr uint8_t QR_VERSION = 5;
constexpr uint8_t QR_QUIET_ZONE_MODULES = 4;

// ---- NVS (runtime-persisted device config) ----
constexpr const char* NVS_NAMESPACE = "e1001cfg";
