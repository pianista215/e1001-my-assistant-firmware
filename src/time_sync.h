#pragma once
#include <cstdint>

// Real SNTP synchronization detection.
//
// Why this module exists instead of just calling getLocalTime(): the
// Arduino core's getLocalTime() only checks that the system clock's year
// is > 2016 (see cores/esp32/esp32-hal-time.c) -- it does NOT wait for an
// NTP packet. Since main.cpp seeds the system clock from the PCF8563
// before touching the network, that check passes instantly and every
// cycle "succeeded" without ever correcting anything. See CLAUDE.md
// ("Real finding on the device: getLocalTime() is not a sync check").
//
// Here the criterion is the SNTP client's own notification callback,
// which only fires when a received NTP packet has actually set the clock.
namespace timesync {

// Registers the sync-notification callback and starts the SNTP client
// (configTzTime with TZ_STRING/SNTP_SERVER_*). Returns immediately: the
// NTP round trip runs in the background, so the caller can get on with
// the cycle (fetch + panel refresh) while it completes.
void begin();

// True once an NTP packet has set the system clock in this boot.
bool synced();

// Waits until synced() or until `budgetMsFromBegin` has elapsed *since
// begin()* -- not since this call. So spending the budget elsewhere
// (fetching the image, refreshing the panel) costs nothing here: if it's
// already exhausted, this returns right away with whatever state we have.
bool waitSynced(unsigned long budgetMsFromBegin);

// Milliseconds between begin() and the sync landing; only meaningful when
// synced() is true. For logging.
unsigned long syncElapsedMs();

}  // namespace timesync
