#pragma once
#include <cstdint>
#include <ctime>

// Pure logic, no Arduino/ESP-IDF dependency -- unit-tested on the host via
// `pio test -e native` (see test/test_time_scheduler).

struct WakeDecision {
    struct tm target;      // normalized local time of the next wake, minute/second = 0
    int64_t sleepSeconds;  // always > 0
};

// Computes when to wake up next, given the current local time `nowLocal`
// (as produced by localtime_r()/getLocalTime(), tm_isdst set appropriately
// by the caller -- this function re-derives it via mktime()).
//
// Always lands on the top of an hour. If that hour would fall in
// [nightSkipFromHour, nightSkipToHour] (inclusive), jumps straight to
// nightSkipTargetHour instead -- those overnight refreshes add no value
// (nobody is looking at the display at 2am), so skipping straight to the
// morning saves battery.
//
// Self-corrects regardless of when "now" actually is: the target is always
// recomputed fresh from `nowLocal`, never from a previously-scheduled
// target, so a boot that doesn't land exactly on the hour (e.g. the very
// first boot) still produces a correct, hour-aligned target.
WakeDecision computeNextWake(struct tm nowLocal,
                              int nightSkipFromHour = 1,
                              int nightSkipToHour = 5,
                              int nightSkipTargetHour = 6);
