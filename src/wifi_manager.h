#pragma once
#include <cstdint>

// Cached across deep-sleep cycles (see sleep_state.h) to enable a fast
// reconnect: WiFi.begin() with a known BSSID+channel skips a full scan.
struct WifiFastConnect {
    uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
    uint8_t channel = 0;
    bool valid = false;
};

// Starts the connection attempt without blocking: uses the cached
// BSSID/channel if available (skips a full scan), otherwise a normal scan.
// Call wifiWaitConnected() afterwards to block until it completes -- the
// gap between the two calls is free time to do other work (e.g. read the
// battery) while the radio associates.
//
// Does NOT set WiFi.mode() -- the caller decides that. The normal cycle
// wants WIFI_STA; the setup portal's live-validation step needs to keep
// WIFI_AP_STA (calling this here must not tear down the SoftAP a phone is
// currently connected to).
void wifiBeginConnect(const char* ssid, const char* password, const WifiFastConnect& cache);

// Blocks until connected or timed out. If the fast (cached) path was used
// and it times out, falls back to a full scan+connect automatically. On
// success, refreshes `cache` with the BSSID/channel actually used, so a
// stale cache self-heals on the next successful connect (e.g. after a
// router reboot or AP channel change). Returns false if neither path
// connects.
bool wifiWaitConnected(WifiFastConnect& cache,
                        unsigned long fastTimeoutMs,
                        unsigned long fullTimeoutMs);

// Explicitly disconnects and powers down the radio before deep sleep.
// Sets WiFi.mode(WIFI_OFF), which also tears down any active SoftAP --
// never call this from the setup portal while a phone may still be
// connected to it. Use WiFi.disconnect(false) there instead, to drop only
// the STA side.
void wifiDisconnect();
