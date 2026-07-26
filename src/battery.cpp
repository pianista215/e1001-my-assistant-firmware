#include "battery.h"

#include <Arduino.h>

#include "config.h"

namespace {
bool g_adcConfigured = false;
}

int readBatteryPercent() {
    if (!g_adcConfigured) {
        analogReadResolution(12);
        analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
        g_adcConfigured = true;
    }

    pinMode(PIN_BATTERY_ENABLE, OUTPUT);
    digitalWrite(PIN_BATTERY_ENABLE, HIGH);
    delay(5);  // let the divider circuit settle

    int mv = analogReadMilliVolts(PIN_BATTERY_ADC);

    digitalWrite(PIN_BATTERY_ENABLE, LOW);  // restore low-power state

    const float volts = (mv / 1000.0f) * 2.0f;  // x2: onboard voltage divider
    float pct = (volts - BATTERY_VOLTAGE_EMPTY) /
                (BATTERY_VOLTAGE_FULL - BATTERY_VOLTAGE_EMPTY) * 100.0f;

    int rounded = static_cast<int>(pct + 0.5f);
    if (rounded < 1) rounded = 1;
    if (rounded > 100) rounded = 100;
    return rounded;
}
