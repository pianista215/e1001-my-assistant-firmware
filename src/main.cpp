#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_sleep.h>

#include "driver/rtc_io.h"

#include <ctime>
#include <sys/time.h>

#include "battery.h"
#include "config.h"
#include "display_client.h"
#include "eink_driver.h"
#include "rtc_pcf8563.h"
#include "sleep_state.h"
#include "time_scheduler.h"
#include "wifi_manager.h"

// Survives deep sleep (reset only on a true power-on/EN reset -- see
// sleep_state.h). This is the ONLY thing that makes the fast WiFi
// reconnect and failure backoff work across cycles.
RTC_DATA_ATTR static PersistentState g_state;

namespace {

const char* wakeupCauseString(esp_sleep_wakeup_cause_t cause) {
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER: return "TIMER (hourly schedule)";
        case ESP_SLEEP_WAKEUP_EXT1: return "BUTTON (manual refresh)";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "POWER-ON/RESET";
        default: return "OTHER";
    }
}

// Every sleep path (success, backoff, first-boot retry) goes through here,
// so the wake button always works regardless of why the device is
// sleeping -- e.g. forcing an immediate retry after fixing WiFi/API config
// instead of waiting out a backoff.
void goToSleep(uint32_t seconds) {
    Serial1.printf("[MAIN] Sleeping for %lu s (or until the wake button is pressed)\n",
                    static_cast<unsigned long>(seconds));
    Serial1.flush();
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_WAKE_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
    // Normal GPIO pull-up is off during deep sleep; the RTC (keep-alive)
    // domain needs its own pull-up enabled instead.
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(PIN_WAKE_BUTTON));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(PIN_WAKE_BUTTON));
    esp_deep_sleep_start();
}

uint32_t backoffSeconds(uint8_t failures) {
    const uint8_t shift = failures > 4 ? 4 : failures;  // cap to avoid overflow
    uint32_t seconds = BACKOFF_BASE_SEC << shift;
    if (seconds > BACKOFF_MAX_SEC) seconds = BACKOFF_MAX_SEC;
    return seconds;
}

// Never busy-retries with the radio on: always falls back to sleeping and
// retrying next cycle, with capped backoff. Only draws an error screen
// after several consecutive failures, so a transient blip doesn't cost an
// e-paper refresh.
[[noreturn]] void handleFailure(const char* code) {
    if (g_state.consecutiveFailures < 255) g_state.consecutiveFailures++;
    Serial1.printf("[MAIN] Failure: %s (consecutive=%u)\n", code, g_state.consecutiveFailures);

    if (g_state.consecutiveFailures >= ERROR_SCREEN_AFTER_N_FAILURES) {
        eink::init();
        eink::drawErrorScreen(code, g_state.consecutiveFailures);
        eink::sleep();
    }

    wifiDisconnect();
    goToSleep(backoffSeconds(g_state.consecutiveFailures));
    while (true) delay(1000);  // unreachable; esp_deep_sleep_start() never returns
}

}  // namespace

void setup() {
    Serial1.begin(SERIAL_BAUD, SERIAL_8N1, PIN_SERIAL_RX, PIN_SERIAL_TX);
    delay(100);
    Serial1.printf("[MAIN] Wake cause: %s\n", wakeupCauseString(esp_sleep_get_wakeup_cause()));

    // Must happen before any mktime()/localtime_r() call (including inside
    // rtc_pcf8563.cpp), since those interpret struct tm as local time in
    // whatever TZ is currently set.
    setenv("TZ", TZ_STRING, 1);
    tzset();

    const bool rtcOk = rtc::begin();
    const bool rtcTimeOk = rtcOk && rtc::syncSystemClockFromRtc();
    if (!rtcOk) {
        Serial1.println("[MAIN] PCF8563 not responding on I2C.");
    } else if (!rtcTimeOk) {
        Serial1.println("[MAIN] PCF8563 time not trusted yet (VL flag set).");
    }

    // Read the battery BEFORE the radio does anything: the WiFi association
    // burst draws enough current to sag the battery rail for a moment, and
    // sampling during that sag reads a lower (wrong) voltage than the
    // battery's real resting level -- worth the handful of milliseconds
    // this costs versus overlapping it with WiFi connect.
    const int batteryPct = readBatteryPercent();
    Serial1.printf("[MAIN] Battery: %d%%\n", batteryPct);

    wifiBeginConnect(g_state.wifi);
    const bool wifiOk =
        wifiWaitConnected(g_state.wifi, WIFI_FAST_RECONNECT_TIMEOUT_MS, WIFI_FULL_CONNECT_TIMEOUT_MS);
    if (!wifiOk) {
        handleFailure("WIFI");
    }
    Serial1.println("[MAIN] WiFi connected.");

    configTzTime(TZ_STRING, SNTP_SERVER_1, SNTP_SERVER_2);
    struct tm now = {};
    const bool sntpOk = getLocalTime(&now, SNTP_SYNC_TIMEOUT_MS);

    if (sntpOk) {
        Serial1.println("[MAIN] SNTP sync OK, correcting RTC.");
        rtc::writeTime(now);
        g_state.timeEverSynced = true;
    } else if (rtcTimeOk) {
        Serial1.println("[MAIN] SNTP failed, using RTC-derived time.");
        const time_t nowEpoch = time(nullptr);
        localtime_r(&nowEpoch, &now);
        g_state.timeEverSynced = true;
    } else if (g_state.timeEverSynced) {
        // Neither RTC nor SNTP worked this cycle, but the ESP32's own system
        // clock has kept ticking since a previous successful sync (it
        // survives deep-sleep-only cycles on its own).
        Serial1.println("[MAIN] SNTP and RTC both unavailable, trusting system clock.");
        const time_t nowEpoch = time(nullptr);
        localtime_r(&nowEpoch, &now);
    } else {
        // No trustworthy time source anywhere yet (very first boot, RTC
        // coin cell just installed). Don't guess an hourly schedule off of
        // an unknown clock -- just retry soon.
        Serial1.println("[MAIN] No trustworthy time source yet; short retry sleep.");
        wifiDisconnect();
        goToSleep(FIRST_BOOT_RETRY_SLEEP_SEC);
        return;
    }

    DisplayFetchResult fetch = fetchDisplayBuffer(batteryPct);
    if (!fetch.ok()) {
        Serial1.printf("[MAIN] Display fetch failed: %s (http=%d)\n", toString(fetch.error),
                        fetch.httpStatus);
        handleFailure(toString(fetch.error));
    }

    eink::init();
    const bool drawOk = eink::drawFrame(fetch.pixels, fetch.width, fetch.height);
    eink::sleep();
    fetch.free();

    if (!drawOk) {
        handleFailure("PANEL");
    }

    Serial1.println("[MAIN] Cycle OK.");
    g_state.consecutiveFailures = 0;
    wifiDisconnect();

#ifdef DEBUG_SLEEP_OVERRIDE_SEC
    // Only touches the normal successful-cycle sleep, so a full real cycle
    // (WiFi, fetch, draw) still runs every time -- just more often, to
    // watch several deep-sleep/wake cycles without waiting an hour each.
    // Never define this for real unattended use: see secrets.ini.example.
    Serial1.println("[MAIN] DEBUG_SLEEP_OVERRIDE_SEC active -- not sleeping a full hour.");
    goToSleep(DEBUG_SLEEP_OVERRIDE_SEC);
#else
    const WakeDecision wake = computeNextWake(now);
    goToSleep(static_cast<uint32_t>(wake.sleepSeconds));
#endif
}

void loop() {
    // setup() always ends in deep sleep; loop() should never actually run.
    Serial1.println("[MAIN][ERROR] deep sleep did not start!");
    delay(1000);
}
