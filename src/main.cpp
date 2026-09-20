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
#include "time_sync.h"
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

// Snapshot of what the PCF8563 said at boot, kept for the drift log below.
bool g_rtcOk = false;
bool g_rtcTimeOk = false;
time_t g_rtcEpochAtBoot = 0;
unsigned long g_rtcReadMs = 0;

// Collects the SNTP round trip started right after WiFi came up (see
// setup()) and pushes the corrected time back into the PCF8563. Runs at
// the end of the cycle -- success or failure -- because a cycle that
// reached the network still has a good clock to hand to the RTC even if
// the endpoint itself failed. Idempotent: only the first call does work.
void finishTimeSync() {
    static bool done = false;
    if (done) return;
    done = true;

    // Normally already synced: the NTP answer landed while the image was
    // being fetched and painted, so this spends no extra awake time. If
    // timesync::begin() was never reached (WiFi failed), the budget is
    // long expired and this returns immediately.
    if (!timesync::waitSynced(SNTP_SYNC_TIMEOUT_MS)) {
        Serial1.println("[TIME] SNTP not synced this cycle; using RTC/system time.");
        if (g_rtcTimeOk) g_state.timeEverSynced = true;
        return;
    }

    Serial1.printf("[TIME] SNTP synced in %lu ms.\n", timesync::syncElapsedMs());
    if (g_rtcTimeOk) {
        // What the RTC would be reading right now, minus the real time:
        // negative means the RTC is running behind (wake lands late).
        const time_t rtcNow = g_rtcEpochAtBoot + static_cast<time_t>((millis() - g_rtcReadMs) / 1000);
        Serial1.printf("[TIME] RTC drift vs SNTP: %+ld s\n",
                        static_cast<long>(rtcNow - time(nullptr)));
    }
    g_state.timeEverSynced = true;
    if (g_rtcOk && !rtc::writeNow()) {
        Serial1.println("[TIME] Failed to write the corrected time to the RTC.");
    }
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
    finishTimeSync();
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
    g_rtcOk = rtcOk;
    g_rtcTimeOk = rtcTimeOk;
    g_rtcEpochAtBoot = rtcTimeOk ? time(nullptr) : 0;
    g_rtcReadMs = millis();
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

    // Start the NTP round trip and deliberately DON'T wait for it here:
    // it runs in the background while the image is fetched and painted,
    // so a real sync costs the cycle no extra awake time. It's collected
    // in finishTimeSync() just before sleeping.
    timesync::begin();

    if (!rtcTimeOk && !g_state.timeEverSynced) {
        // No trustworthy time source anywhere yet (very first boot, RTC
        // coin cell just installed). This is the one case where we have to
        // block on SNTP: without a clock there's no hourly schedule to
        // compute, not even a sensible retry.
        if (!timesync::waitSynced(SNTP_SYNC_TIMEOUT_MS)) {
            Serial1.println("[TIME] No trustworthy time source yet; short retry sleep.");
            wifiDisconnect();
            goToSleep(FIRST_BOOT_RETRY_SLEEP_SEC);
            return;
        }
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
    finishTimeSync();
    wifiDisconnect();

#ifdef DEBUG_SLEEP_OVERRIDE_SEC
    // Only touches the normal successful-cycle sleep, so a full real cycle
    // (WiFi, fetch, draw) still runs every time -- just more often, to
    // watch several deep-sleep/wake cycles without waiting an hour each.
    // Never define this for real unattended use: see secrets.ini.example.
    Serial1.println("[MAIN] DEBUG_SLEEP_OVERRIDE_SEC active -- not sleeping a full hour.");
    goToSleep(DEBUG_SLEEP_OVERRIDE_SEC);
#else
    // Read the clock HERE, not before the fetch: the HTTP request and the
    // full-panel refresh take tens of seconds, and computing the sleep
    // from a "now" captured before them made every wake land that much
    // past the hour (the sleep started long after the instant it was
    // measured from).
    const time_t nowEpoch = time(nullptr);
    struct tm now = {};
    localtime_r(&nowEpoch, &now);

    const WakeDecision wake = computeNextWake(now);
    Serial1.printf("[TIME] Now %02d:%02d:%02d -> next wake %02d:%02d:%02d\n", now.tm_hour,
                    now.tm_min, now.tm_sec, wake.target.tm_hour, wake.target.tm_min,
                    wake.target.tm_sec);
    goToSleep(static_cast<uint32_t>(wake.sleepSeconds));
#endif
}

void loop() {
    // setup() always ends in deep sleep; loop() should never actually run.
    Serial1.println("[MAIN][ERROR] deep sleep did not start!");
    delay(1000);
}
