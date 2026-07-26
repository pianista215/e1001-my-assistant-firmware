#include "eink_driver.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <SPI.h>

#include <cstdlib>
#include <cstring>

#include "config.h"

// UC8179 init sequence, gray LUT tables, and bit-plane upload logic are
// ported verbatim from Seeed's own example
// (examples/base/GxEPD2_reTerminal_E1001_Gray4 in
// Seeed-Projects/OSHW-reTerminal-Series-E-D) -- see CLAUDE.md for details
// and for what's still worth re-verifying against the physical panel.

namespace {

SPIClass hspi(HSPI);
SPISettings spiSet(2000000, MSBFIRST, SPI_MODE0);

// Each LUT is 7 phases x 6 bytes = 42 bytes. Verbatim from Seeed_GFX's
// UC8179_Defines.h via Seeed's own example -- opaque panel calibration
// data, not something to hand-derive.
const uint8_t LUT_VCOM_GRAY[] = {
    0x00, 0x00, 0x06, 0x08, 0x07, 0x01, 0x00, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x00, 0x03, 0x03, 0x00, 0x00, 0x03, 0x00, 0x05, 0x09, 0x06, 0x06, 0x01,
    0x00, 0x02, 0x02, 0x0A, 0x0A, 0x01, 0x00, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x00, 0x02, 0x01, 0x02, 0x01, 0x01,
};
const uint8_t LUT_WW_GRAY[] = {
    0x15, 0x00, 0x06, 0x08, 0x07, 0x01, 0x54, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03, 0x2A, 0x05, 0x09, 0x06, 0x06, 0x01,
    0xAA, 0x02, 0x02, 0x0A, 0x0A, 0x01, 0x00, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x28, 0x02, 0x01, 0x02, 0x01, 0x01,
};
const uint8_t LUT_KW_GRAY[] = {
    0x2A, 0x00, 0x06, 0x08, 0x07, 0x01, 0x59, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03, 0x5A, 0x05, 0x09, 0x06, 0x06, 0x01,
    0xA8, 0x02, 0x02, 0x0A, 0x0A, 0x01, 0x45, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0xA8, 0x02, 0x01, 0x02, 0x01, 0x01,
};
const uint8_t LUT_WK_GRAY[] = {
    0x16, 0x00, 0x06, 0x08, 0x07, 0x01, 0xA0, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03, 0x99, 0x05, 0x09, 0x06, 0x06, 0x01,
    0xA0, 0x02, 0x02, 0x0A, 0x0A, 0x01, 0x40, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x20, 0x02, 0x01, 0x02, 0x01, 0x01,
};
const uint8_t LUT_KK_GRAY[] = {
    0x26, 0x00, 0x06, 0x08, 0x07, 0x01, 0x6A, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03, 0x65, 0x05, 0x09, 0x06, 0x06, 0x01,
    0x50, 0x02, 0x02, 0x0A, 0x0A, 0x01, 0x10, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x10, 0x02, 0x01, 0x02, 0x01, 0x01,
};
const uint8_t CMD_USER_GRAY[] = {0x17, 0x3F, 0x3F, 0x07, 0x06, 0x12};

bool waitBusy() {
    delay(10);
    const unsigned long start = millis();
    while (!digitalRead(PIN_EPD_BUSY)) {
        if (millis() - start > EINK_BUSY_TIMEOUT_MS) return false;
        delay(10);
    }
    return true;
}

void writeCommand(uint8_t cmd) {
    hspi.beginTransaction(spiSet);
    digitalWrite(PIN_EPD_DC, LOW);
    digitalWrite(PIN_EPD_CS, LOW);
    hspi.transfer(cmd);
    digitalWrite(PIN_EPD_CS, HIGH);
    digitalWrite(PIN_EPD_DC, HIGH);
    hspi.endTransaction();
}

void writeData(uint8_t data) {
    hspi.beginTransaction(spiSet);
    digitalWrite(PIN_EPD_CS, LOW);
    hspi.transfer(data);
    digitalWrite(PIN_EPD_CS, HIGH);
    hspi.endTransaction();
}

void writeLUT(uint8_t cmd, const uint8_t* lut, uint16_t len) {
    writeCommand(cmd);
    for (uint16_t i = 0; i < len; i++) writeData(lut[i]);
}

// Full gray-mode init sequence (power/PLL/VCOM/booster/resolution/LUTs).
bool initGrayMode() {
    digitalWrite(PIN_EPD_RES, LOW);
    delay(10);
    digitalWrite(PIN_EPD_RES, HIGH);
    delay(10);
    if (!waitBusy()) return false;

    writeCommand(0x01);  // Power setting
    writeData(0x07);
    writeData(CMD_USER_GRAY[0]);
    writeData(CMD_USER_GRAY[1]);
    writeData(CMD_USER_GRAY[2]);
    writeData(CMD_USER_GRAY[3]);

    writeCommand(0x30);  // PLL
    writeData(CMD_USER_GRAY[4]);

    writeCommand(0x82);  // VCOM DC
    writeData(CMD_USER_GRAY[5]);

    writeCommand(0x06);  // Booster
    writeData(0x27);
    writeData(0x27);
    writeData(0x28);
    writeData(0x17);

    writeCommand(0x04);  // Power ON
    delay(100);
    if (!waitBusy()) return false;

    writeCommand(0x00);  // Panel setting
    writeData(0x3F);

    writeCommand(0xE3);  // Power saving
    writeData(0x88);

    writeCommand(0x50);  // VCOM and data interval
    writeData(0x10);
    writeData(0x07);

    writeCommand(0x52);  // PLL setting
    writeData(0x00);

    writeCommand(0x61);  // Resolution
    writeData(EPD_EXPECTED_WIDTH >> 8);
    writeData(EPD_EXPECTED_WIDTH & 0xFF);
    writeData(EPD_EXPECTED_HEIGHT >> 8);
    writeData(EPD_EXPECTED_HEIGHT & 0xFF);

    // LUTC/LUTWW/LUTKW must be flushed (BUSY-waited) individually; LUTWK and
    // LUTKK can be written back-to-back -- matches the vendor sequence.
    writeLUT(0x20, LUT_VCOM_GRAY, sizeof(LUT_VCOM_GRAY));
    if (!waitBusy()) return false;
    writeLUT(0x21, LUT_WW_GRAY, sizeof(LUT_WW_GRAY));
    if (!waitBusy()) return false;
    writeLUT(0x22, LUT_KW_GRAY, sizeof(LUT_KW_GRAY));
    if (!waitBusy()) return false;
    writeLUT(0x23, LUT_WK_GRAY, sizeof(LUT_WK_GRAY));
    writeLUT(0x24, LUT_KK_GRAY, sizeof(LUT_KK_GRAY));

    return true;
}

// Uploads one UC8179 bit plane from a packed 2bpp buffer (row-major, 4
// pixels/byte, MSB-first, 0=black..3=white -- the exact wire format
// my-assistant sends). `planeBit` selects which bit of the (inverted) gray
// level goes out: 0 -> DTM1 (cmd 0x10), 1 -> DTM2 (cmd 0x13). The 3-minus
// inversion is the same one Seeed's own confirmed-working demo performs --
// it reconciles our 0=black..3=white convention with how UC8179's two DTM
// bit planes physically encode gray level.
void uploadPlane(uint8_t cmd, uint8_t planeBit, const uint8_t* buf, uint16_t width, uint16_t height) {
    const uint32_t bytesPerRow = width / 4;
    writeCommand(cmd);
    hspi.beginTransaction(spiSet);
    digitalWrite(PIN_EPD_CS, LOW);
    for (uint16_t row = 0; row < height; row++) {
        const uint8_t* rowPtr = buf + static_cast<uint32_t>(row) * bytesPerRow;
        for (uint16_t col8 = 0; col8 < width / 8; col8++) {
            uint8_t out = 0;
            for (uint8_t bit = 0; bit < 8; bit++) {
                const uint16_t px = col8 * 8 + bit;
                const uint32_t idx = px / 4;
                const uint8_t shift = (3 - (px & 3)) * 2;
                const uint8_t gray = 3 - ((rowPtr[idx] >> shift) & 0x03);
                if (gray & (1 << planeBit)) out |= (0x80 >> bit);
            }
            hspi.transfer(out);
        }
    }
    digitalWrite(PIN_EPD_CS, HIGH);
    hspi.endTransaction();
}

// Minimal Adafruit_GFX canvas over the same 2bpp packing, used only by
// drawErrorScreen() -- the one path that needs per-pixel/text drawing
// instead of a pre-packed buffer straight from the network.
class Gray4Buffer : public Adafruit_GFX {
   public:
    Gray4Buffer(uint16_t w, uint16_t h) : Adafruit_GFX(w, h) {}
    uint8_t* buf = nullptr;

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (!buf || x < 0 || x >= _width || y < 0 || y >= _height) return;
        const uint8_t g = color & 0x03;
        const uint32_t idx = static_cast<uint32_t>(y) * (_width / 4) + x / 4;
        const uint8_t shift = (3 - (x & 3)) * 2;
        buf[idx] = (buf[idx] & ~(0x03 << shift)) | (g << shift);
    }
};

}  // namespace

namespace eink {

void init() {
    pinMode(PIN_EPD_CS, OUTPUT);
    digitalWrite(PIN_EPD_CS, HIGH);
    pinMode(PIN_EPD_DC, OUTPUT);
    digitalWrite(PIN_EPD_DC, HIGH);
    pinMode(PIN_EPD_RES, OUTPUT);
    digitalWrite(PIN_EPD_RES, HIGH);
    pinMode(PIN_EPD_BUSY, INPUT);
    hspi.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, -1);
}

bool drawFrame(const uint8_t* packed2bpp, uint16_t width, uint16_t height) {
    if (!initGrayMode()) return false;
    uploadPlane(0x10, 0, packed2bpp, width, height);
    uploadPlane(0x13, 1, packed2bpp, width, height);
    writeCommand(0x12);  // Display refresh
    delay(100);
    return waitBusy();
}

void sleep() {
    writeCommand(0x02);  // Power OFF
    waitBusy();
    writeCommand(0x07);  // Deep sleep
    writeData(0xA5);
}

void drawErrorScreen(const char* code, uint8_t consecutiveFailures) {
    Gray4Buffer canvas(EPD_EXPECTED_WIDTH, EPD_EXPECTED_HEIGHT);
    const uint32_t bufSize = static_cast<uint32_t>(EPD_EXPECTED_WIDTH) * EPD_EXPECTED_HEIGHT / 4;
    canvas.buf = static_cast<uint8_t*>(malloc(bufSize));
    if (!canvas.buf) return;
    memset(canvas.buf, 0xFF, bufSize);  // 0b11 = 3 = white

    canvas.setTextColor(0);  // black
    canvas.setTextSize(4);
    canvas.setCursor(60, 180);
    canvas.print(code);

    char line2[40];
    snprintf(line2, sizeof(line2), "fallos consecutivos: %u", consecutiveFailures);
    canvas.setTextSize(2);
    canvas.setCursor(60, 260);
    canvas.print(line2);

    if (initGrayMode()) {
        uploadPlane(0x10, 0, canvas.buf, EPD_EXPECTED_WIDTH, EPD_EXPECTED_HEIGHT);
        uploadPlane(0x13, 1, canvas.buf, EPD_EXPECTED_WIDTH, EPD_EXPECTED_HEIGHT);
        writeCommand(0x12);
        delay(100);
        waitBusy();
    }

    free(canvas.buf);
}

}  // namespace eink
