#include "rtc_pcf8563.h"

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>

#include "config.h"

namespace {

constexpr uint8_t REG_CTRL1 = 0x00;
constexpr uint8_t REG_CTRL2 = 0x01;
constexpr uint8_t REG_SECONDS = 0x02;  // bit7 = VL flag; bits6:0 = seconds (BCD)
constexpr uint8_t REG_CLKOUT = 0x0D;

inline uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10U) + (bcd & 0x0FU); }
inline uint8_t decToBcd(uint8_t dec) { return ((dec / 10U) << 4) | (dec % 10U); }

bool readRegs(uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;  // repeated START
    if (Wire.requestFrom(static_cast<uint8_t>(PCF8563_ADDR), static_cast<uint8_t>(len)) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>(Wire.read());
    return true;
}

bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool probe() {
    Wire.beginTransmission(PCF8563_ADDR);
    return Wire.endTransmission() == 0;
}

}  // namespace

namespace rtc {

bool begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000UL);
    if (!probe()) return false;
    if (!writeReg(REG_CTRL1, 0x00)) return false;  // STOP=0 -> run
    if (!writeReg(REG_CTRL2, 0x00)) return false;  // clear alarm/timer flags
    if (!writeReg(REG_CLKOUT, 0x00)) return false;  // FE=0 -> disable CLKOUT
    return true;
}

bool syncSystemClockFromRtc() {
    uint8_t raw[7] = {};
    if (!readRegs(REG_SECONDS, raw, 7)) return false;

    const bool voltageOK = (raw[0] & 0x80U) == 0U;
    if (!voltageOK) return false;  // stored time unreliable, don't touch system clock

    struct tm t = {};
    t.tm_sec = bcdToDec(raw[0] & 0x7FU);
    t.tm_min = bcdToDec(raw[1] & 0x7FU);
    t.tm_hour = bcdToDec(raw[2] & 0x3FU);
    t.tm_mday = bcdToDec(raw[3] & 0x3FU);
    const int month = bcdToDec(raw[5] & 0x1FU);
    const int yr = bcdToDec(raw[6]);
    const int year = ((raw[5] & 0x80U) != 0U) ? (1900 + yr) : (2000 + yr);
    t.tm_mon = month - 1;
    t.tm_year = year - 1900;
    t.tm_isdst = -1;

    const time_t epoch = mktime(&t);
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
    return true;
}

bool writeTime(const struct tm& tIn) {
    struct tm t = tIn;
    mktime(&t);  // normalize fields and fill in tm_wday

    const int year = t.tm_year + 1900;
    if (year < 2000 || year > 2099) return false;

    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(REG_SECONDS);
    Wire.write(decToBcd(static_cast<uint8_t>(t.tm_sec)));
    Wire.write(decToBcd(static_cast<uint8_t>(t.tm_min)));
    Wire.write(decToBcd(static_cast<uint8_t>(t.tm_hour)));
    Wire.write(decToBcd(static_cast<uint8_t>(t.tm_mday)));
    Wire.write(static_cast<uint8_t>(t.tm_wday));  // weekday is not BCD
    Wire.write(decToBcd(static_cast<uint8_t>(t.tm_mon + 1)));
    Wire.write(decToBcd(static_cast<uint8_t>(year % 100)));
    return Wire.endTransmission() == 0;
}

}  // namespace rtc
