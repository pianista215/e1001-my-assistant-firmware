#include "time_sync.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include "config.h"

// The legacy sntp_* names are what arduino-esp32 2.x (IDF 4.x) exposes;
// IDF 5.x renamed them esp_sntp_*. platformio.ini doesn't pin a platform
// version, so support both rather than break on a silent core bump.
#if ESP_IDF_VERSION_MAJOR >= 5
#define SNTP_SET_SYNC_CB esp_sntp_set_time_sync_notification_cb
#else
#define SNTP_SET_SYNC_CB sntp_set_time_sync_notification_cb
#endif

namespace {

// Written from the lwIP task, read from the main loop -- a single bool
// flag, no other state, so volatile is enough (no torn reads possible).
volatile bool g_synced = false;
bool g_began = false;
unsigned long g_beganMs = 0;
volatile unsigned long g_syncedMs = 0;

void onTimeSync(struct timeval* /*tv*/) {
    g_syncedMs = millis();
    g_synced = true;
}

constexpr unsigned long POLL_INTERVAL_MS = 25;

}  // namespace

namespace timesync {

void begin() {
    g_synced = false;
    g_syncedMs = 0;
    g_beganMs = millis();
    g_began = true;
    // Must be registered before the client starts: configTzTime() does
    // sntp_stop() + sntp_init() internally, and the callback is state of
    // the SNTP module itself, so it survives that restart.
    SNTP_SET_SYNC_CB(onTimeSync);
    configTzTime(TZ_STRING, SNTP_SERVER_1, SNTP_SERVER_2);
}

bool synced() { return g_synced; }

bool waitSynced(unsigned long budgetMsFromBegin) {
    // Never started (WiFi failed before begin()): nothing to wait for.
    if (!g_began) return false;
    while (!g_synced && (millis() - g_beganMs) < budgetMsFromBegin) {
        delay(POLL_INTERVAL_MS);
    }
    return g_synced;
}

unsigned long syncElapsedMs() { return g_synced ? (g_syncedMs - g_beganMs) : 0; }

}  // namespace timesync
