#pragma once
#include <cstdint>

#include "wifi_manager.h"

// Persisted in RTC memory (see main.cpp's `RTC_DATA_ATTR PersistentState
// g_state`), which survives deep sleep but resets to these initializers on
// a true power-on/EN reset. This relies on documented ESP32 behavior:
// RTC_DATA_ATTR initializers only run once, on a real power-on reset, not on
// wake-from-deep-sleep -- the same behavior Seeed's own LowPower_DeepSleep.ino
// example relies on for its `s_bootCount` counter.
struct PersistentState {
    WifiFastConnect wifi;
    uint8_t consecutiveFailures = 0;
    // Whether a full wall-clock time has ever been established (via SNTP or
    // a trusted RTC read) since the last power-on reset. Used to decide
    // whether it's safe to fall back to the system clock (time(nullptr))
    // when this cycle's own RTC read and SNTP both fail.
    bool timeEverSynced = false;
};
