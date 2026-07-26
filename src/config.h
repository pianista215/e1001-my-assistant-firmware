#pragma once

// Non-secret compile-time constants: pins, timeouts, thresholds. Per-device
// secrets (WIFI_SSID, WIFI_PASSWORD, API_BASE_URL, API_AUTH_TOKEN, TZ_STRING)
// are injected as -D build flags from secrets.ini (see secrets.ini.example)
// and are already visible as macros everywhere without an include -- these
// #ifndef guards just fail the build early with a clear message if that
// file was never copied.

#ifndef WIFI_SSID
#error "WIFI_SSID not defined -- did you copy secrets.ini.example to secrets.ini?"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD not defined -- did you copy secrets.ini.example to secrets.ini?"
#endif
#ifndef API_BASE_URL
#error "API_BASE_URL not defined -- did you copy secrets.ini.example to secrets.ini?"
#endif
#ifndef API_AUTH_TOKEN
#error "API_AUTH_TOKEN not defined -- did you copy secrets.ini.example to secrets.ini?"
#endif
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
