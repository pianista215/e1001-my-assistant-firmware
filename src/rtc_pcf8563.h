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

// Writes `t` to the RTC. `t` is expected to already represent local time
// in the firmware's configured timezone (as produced by localtime_r()).
// The raw primitive behind writeNow(), which is what the normal cycle
// uses after a confirmed SNTP sync.
bool writeTime(const struct tm& t);

// Writes the *current* system time to the RTC, rounded to the nearest
// second. writeTime() takes a struct tm, whose seconds are already
// truncated, so every write silently lost the sub-second remainder --
// always in the same direction (the RTC ends up behind). Rounding makes
// that error zero-mean instead of cumulative, which matters on cycles
// where the RTC is the only time source available.
bool writeNow();

}  // namespace rtc
