#include <unity.h>

#include <cstdlib>

#include "time_scheduler.h"

namespace {

struct tm makeTm(int year, int mon, int day, int hour, int min, int sec) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return t;
}

}  // namespace

void setUp() {
    // Fixed, DST-free timezone so results don't depend on the host machine's
    // local timezone or on real-world DST transition dates.
    setenv("TZ", "UTC0", 1);
    tzset();
}

void tearDown() {}

void test_mid_hour_rounds_up_to_next_hour() {
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 14, 37, 52));
    TEST_ASSERT_EQUAL_INT(26, d.target.tm_mday);
    TEST_ASSERT_EQUAL_INT(15, d.target.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, d.target.tm_min);
    TEST_ASSERT_EQUAL_INT64(22 * 60 + 8, d.sleepSeconds);  // 14:37:52 -> 15:00:00
}

void test_exactly_on_the_hour_still_advances_one_hour() {
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 9, 0, 0));
    TEST_ASSERT_EQUAL_INT(10, d.target.tm_hour);
    TEST_ASSERT_EQUAL_INT64(3600, d.sleepSeconds);
}

void test_midnight_skips_straight_to_six() {
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT(6, d.target.tm_hour);
    TEST_ASSERT_EQUAL_INT64(6 * 3600, d.sleepSeconds);
}

void test_late_night_arbitrary_boot_time_still_skips_to_six() {
    // Not exactly on the hour: next hour would be 01:00, which is inside
    // the skip window, so it should still jump to 06:00.
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 0, 58, 10));
    TEST_ASSERT_EQUAL_INT(6, d.target.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, d.target.tm_min);
}

void test_every_skip_window_hour_jumps_to_six() {
    for (int h = 0; h <= 4; h++) {
        WakeDecision d = computeNextWake(makeTm(2026, 7, 26, h, 0, 0));
        TEST_ASSERT_EQUAL_INT(6, d.target.tm_hour);
    }
}

void test_five_am_wake_also_jumps_to_six() {
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 5, 0, 0));
    TEST_ASSERT_EQUAL_INT(6, d.target.tm_hour);
}

void test_six_am_advances_normally_to_seven() {
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 6, 0, 0));
    TEST_ASSERT_EQUAL_INT(7, d.target.tm_hour);
    TEST_ASSERT_EQUAL_INT64(3600, d.sleepSeconds);
}

void test_eleven_pm_advances_to_midnight_without_skip() {
    // 23:00 -> next hour is 00:00 the next day, which is NOT itself inside
    // the skip window (only 01:00-05:00 is) -- the 00:00 refresh still
    // happens, it's the *following* wake (from 00:00) that jumps to 06:00.
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 23, 0, 0));
    TEST_ASSERT_EQUAL_INT(27, d.target.tm_mday);
    TEST_ASSERT_EQUAL_INT(0, d.target.tm_hour);
    TEST_ASSERT_EQUAL_INT64(3600, d.sleepSeconds);
}

void test_end_of_day_late_boot_rolls_to_next_day_and_skips() {
    // 23:30 -> next hour 00:00 (27th) -- not itself in the skip window.
    WakeDecision d = computeNextWake(makeTm(2026, 7, 26, 23, 30, 0));
    TEST_ASSERT_EQUAL_INT(27, d.target.tm_mday);
    TEST_ASSERT_EQUAL_INT(0, d.target.tm_hour);
}

void test_sleep_seconds_always_positive() {
    for (int h = 0; h < 24; h++) {
        for (int m = 0; m < 60; m += 15) {
            WakeDecision d = computeNextWake(makeTm(2026, 7, 26, h, m, 0));
            TEST_ASSERT_GREATER_THAN_INT64(0, d.sleepSeconds);
        }
    }
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_mid_hour_rounds_up_to_next_hour);
    RUN_TEST(test_exactly_on_the_hour_still_advances_one_hour);
    RUN_TEST(test_midnight_skips_straight_to_six);
    RUN_TEST(test_late_night_arbitrary_boot_time_still_skips_to_six);
    RUN_TEST(test_every_skip_window_hour_jumps_to_six);
    RUN_TEST(test_five_am_wake_also_jumps_to_six);
    RUN_TEST(test_six_am_advances_normally_to_seven);
    RUN_TEST(test_eleven_pm_advances_to_midnight_without_skip);
    RUN_TEST(test_end_of_day_late_boot_rolls_to_next_day_and_skips);
    RUN_TEST(test_sleep_seconds_always_positive);
    return UNITY_END();
}
