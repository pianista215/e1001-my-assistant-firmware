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
void wifiBeginConnect(const WifiFastConnect& cache);

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
void wifiDisconnect();
