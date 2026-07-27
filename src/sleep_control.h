#pragma once

#include <cstdint>

// Every sleep path (success, backoff, first-boot retry, provisioning
// portal inactivity timeout) goes through here, so the wake button always
// works regardless of why the device is sleeping -- e.g. forcing an
// immediate retry after fixing WiFi/API config instead of waiting out a
// backoff. Never returns.
[[noreturn]] void goToSleep(uint32_t seconds);
