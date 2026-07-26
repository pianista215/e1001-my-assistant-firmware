#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>

#include "config.h"

namespace {

bool waitForConnection(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) return false;
        delay(50);
    }
    return true;
}

void refreshCache(WifiFastConnect& cache) {
    const uint8_t* bssid = WiFi.BSSID();
    if (bssid) memcpy(cache.bssid, bssid, 6);
    cache.channel = static_cast<uint8_t>(WiFi.channel());
    cache.valid = true;
}

}  // namespace

void wifiBeginConnect(const WifiFastConnect& cache) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);  // avoid wearing out flash NVS every cycle

    if (cache.valid) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, cache.channel, cache.bssid, true);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
}

bool wifiWaitConnected(WifiFastConnect& cache,
                        unsigned long fastTimeoutMs,
                        unsigned long fullTimeoutMs) {
    const bool usedCache = cache.valid;
    const unsigned long firstTimeout = usedCache ? fastTimeoutMs : fullTimeoutMs;

    if (waitForConnection(firstTimeout)) {
        refreshCache(cache);
        return true;
    }

    if (usedCache) {
        // Cached BSSID/channel is stale (router rebooted, roamed, etc.) --
        // fall back to a full scan+connect instead of giving up.
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        if (waitForConnection(fullTimeoutMs)) {
            refreshCache(cache);
            return true;
        }
    }

    cache.valid = false;
    return false;
}

void wifiDisconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}
