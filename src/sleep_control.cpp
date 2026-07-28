#include "sleep_control.h"

#include <Arduino.h>
#include <esp_sleep.h>

#include "driver/rtc_io.h"

#include "config.h"

[[noreturn]] void goToSleep(uint32_t seconds) {
    Serial1.printf("[MAIN] Sleeping for %lu s (or until the wake button is pressed)\n",
                    static_cast<unsigned long>(seconds));
    Serial1.flush();
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_WAKE_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
    // Normal GPIO pull-up is off during deep sleep; the RTC (keep-alive)
    // domain needs its own pull-up enabled instead.
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(PIN_WAKE_BUTTON));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(PIN_WAKE_BUTTON));
    esp_deep_sleep_start();
    while (true) delay(1000);  // unreachable; esp_deep_sleep_start() never returns
}
