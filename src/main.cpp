#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_sleep.h>

#include <ctime>
#include <sys/time.h>

#include "battery.h"
#include "config.h"
#include "device_config.h"
#include "display_client.h"
#include "eink_driver.h"
#include "i18n.h"
#include "rtc_pcf8563.h"
#include "setup_portal.h"
#include "sleep_control.h"
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

// Polls the wake button for up to `holdMs`. Returns true if it's still
// held down the whole time (reset gesture), false if released early
// (ordinary manual-refresh press). Deliberately blocks before doing
// anything else on an EXT1 wake -- the button must not trigger a refresh
// the instant it's pressed, or a long hold could never be told apart from
// a quick one.
bool buttonHeldFor(unsigned long holdMs) {
    const unsigned long start = millis();
    while (millis() - start < holdMs) {
        if (digitalRead(PIN_WAKE_BUTTON) == HIGH) return false;  // released early
        delay(RESET_HOLD_POLL_MS);
    }
    return true;
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
[[noreturn]] void handleFailure(const char* code, Lang lang) {
    if (g_state.consecutiveFailures < 255) g_state.consecutiveFailures++;
    Serial1.printf("[MAIN] Failure: %s (consecutive=%u)\n", code, g_state.consecutiveFailures);

    if (g_state.consecutiveFailures >= ERROR_SCREEN_AFTER_N_FAILURES) {
        eink::init();
        eink::drawErrorScreen(code, g_state.consecutiveFailures, lang);
        eink::sleep();
    }

    wifiDisconnect();
    goToSleep(backoffSeconds(g_state.consecutiveFailures));
    while (true) delay(1000);  // unreachable; goToSleep() never returns
}

}  // namespace

void setup() {
    Serial1.begin(SERIAL_BAUD, SERIAL_8N1, PIN_SERIAL_RX, PIN_SERIAL_TX);
    delay(100);
    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    Serial1.printf("[MAIN] Wake cause: %s\n", wakeupCauseString(wakeCause));

    // Normal GPIO pull-up is what was active during deep sleep (see
    // sleep_control.cpp); re-establish it as a plain digital input now
    // that we're awake, so digitalRead() below reads reliably.
    pinMode(PIN_WAKE_BUTTON, INPUT_PULLUP);

    // Reset-to-setup gesture: holding the wake button for RESET_HOLD_MS
    // wipes the saved config and re-enters the setup portal. Only checked
    // when already configured -- an unconfigured device is heading into
    // the portal anyway, no need to disambiguate the press. A short press
    // falls through to the ordinary manual-refresh cycle below, unchanged.
    if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 && device_config::isConfigured()) {
        if (buttonHeldFor(RESET_HOLD_MS)) {
            Serial1.println("[MAIN] Wake button held -- clearing config, entering setup portal.");
            device_config::clear();
            ESP.restart();
        }
        Serial1.println("[MAIN] Wake button released early -- manual refresh cycle.");
    }

    if (!device_config::isConfigured()) {
        setup_portal::run();
    }

    DeviceConfig cfg;
    device_config::load(cfg);

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

    WiFi.mode(WIFI_STA);
    wifiBeginConnect(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str(), g_state.wifi);
    const bool wifiOk =
        wifiWaitConnected(g_state.wifi, WIFI_FAST_RECONNECT_TIMEOUT_MS, WIFI_FULL_CONNECT_TIMEOUT_MS);
    if (!wifiOk) {
        handleFailure("WIFI", i18n::langFromCode(cfg.language));
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

    const DisplayEndpointConfig endpoint{cfg.apiBaseUrl, cfg.apiAuthToken, cfg.tlsFingerprint};
    DisplayFetchResult fetch = fetchDisplayBuffer(endpoint, batteryPct);
    if (!fetch.ok()) {
        Serial1.printf("[MAIN] Display fetch failed: %s (http=%d)\n", toString(fetch.error),
                        fetch.httpStatus);
        if (fetch.error == DisplayFetchError::TlsFingerprintMismatch) {
            Serial1.printf("[MAIN] Expected fingerprint: %s\n", cfg.tlsFingerprint.c_str());
            Serial1.printf("[MAIN] Server presented:     %s\n",
                            fetch.actualFingerprintHex.length() > 0
                                ? fetch.actualFingerprintHex.c_str()
                                : "(couldn't read peer cert)");
        }
        handleFailure(toString(fetch.error), i18n::langFromCode(cfg.language));
    }

    eink::init();
    const bool drawOk = eink::drawFrame(fetch.pixels, fetch.width, fetch.height);
    eink::sleep();
    fetch.free();

    if (!drawOk) {
        handleFailure("PANEL", i18n::langFromCode(cfg.language));
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
