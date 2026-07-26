#pragma once
#include <ctime>

// Driver for the onboard PCF8563 RTC (I2C, battery-backed by its own coin
// cell, independent of the ESP32's own RTC timer -- see CLAUDE.md). Ported
// from Seeed's own RTC_PCF8563.ino example.
namespace rtc {

// Initializes the I2C bus and the PCF8563 (clears STOP bit, disables
// CLKOUT). Returns false if the chip doesn't answer on the bus at all
// (wiring/hardware fault) -- distinct from "time is unreliable" (see
// syncSystemClockFromRtc()).
bool begin();

// Reads the RTC and, if its VL (voltage-low) flag is clear, calls
// settimeofday() so time()/localtime() reflect it. Returns false (and
// leaves the system clock untouched) if the VL flag is set -- i.e. the
// backup coin cell was drained at some point and the stored time can't be
// trusted -- or if the I2C read itself failed.
bool syncSystemClockFromRtc();

// Writes `t` to the RTC. Used after a successful SNTP sync so the RTC
// doesn't drift across cycles that never reach the network. `t` is
// expected to already represent local time in the firmware's configured
// timezone (as produced by localtime_r()/getLocalTime()).
bool writeTime(const struct tm& t);

}  // namespace rtc
