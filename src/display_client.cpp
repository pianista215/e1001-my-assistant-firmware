#include "display_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Stream.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "config.h"

namespace {
constexpr uint8_t kMagic[4] = {'E', 'I', 'N', 'K'};
constexpr size_t kHeaderLen = 10;
constexpr uint8_t kFormatVersion = 1;
constexpr uint8_t kBitsPerPixel = 2;

// my-assistant's server sends the image body with `Transfer-Encoding:
// chunked` (Go's net/http drops Content-Length once a handler writes more
// than its small internal buffer, which our ~96KB body always does) --
// confirmed against the real server, not assumed. HTTPClient::getSize()
// returns -1 with no Content-Length, so we can't pre-size a buffer from
// it. Instead we hand HTTPClient a fixed-capacity sink and let
// writeToStream() do the de-chunking for us, which works the same way
// regardless of which transfer encoding the server used.
//
// writeToStream() takes a Stream*, not a Print* (Stream : public Print,
// adding the read side) even though it only ever writes into it -- the
// read-side methods below are unused stubs to satisfy that interface.
class MemoryStream : public Stream {
   public:
    MemoryStream(uint8_t* buf, size_t capacity) : _buf(buf), _capacity(capacity) {}

    size_t write(uint8_t b) override {
        if (_written >= _capacity) {
            _overflowed = true;
            return 0;
        }
        _buf[_written++] = b;
        return 1;
    }

    size_t write(const uint8_t* data, size_t len) override {
        size_t toCopy = len;
        if (_written + toCopy > _capacity) {
            toCopy = _capacity - _written;
            _overflowed = true;
        }
        memcpy(_buf + _written, data, toCopy);
        _written += toCopy;
        return toCopy;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

    size_t written() const { return _written; }
    bool overflowed() const { return _overflowed; }

   private:
    uint8_t* _buf;
    size_t _capacity;
    size_t _written = 0;
    bool _overflowed = false;
};

}  // namespace

const char* toString(DisplayFetchError err) {
    switch (err) {
        case DisplayFetchError::None: return "OK";
        case DisplayFetchError::HttpConnectFailed: return "HTTP_CONN";
        case DisplayFetchError::HttpTimeout: return "HTTP_TIMEOUT";
        case DisplayFetchError::HttpStatus: return "HTTP_STATUS";
        case DisplayFetchError::ResponseTooLarge: return "TOO_LARGE";
        case DisplayFetchError::TooShort: return "TOO_SHORT";
        case DisplayFetchError::BadMagic: return "BAD_MAGIC";
        case DisplayFetchError::UnsupportedVersion: return "BAD_VERSION";
        case DisplayFetchError::UnsupportedBpp: return "BAD_BPP";
        case DisplayFetchError::UnexpectedDimensions: return "BAD_DIMS";
        case DisplayFetchError::PayloadTooShort: return "SHORT_PAYLOAD";
        case DisplayFetchError::OutOfMemory: return "OOM";
    }
    return "UNKNOWN";
}

void DisplayFetchResult::free() {
    if (rawBuffer) {
        heap_caps_free(rawBuffer);
        rawBuffer = nullptr;
        pixels = nullptr;
    }
}

DisplayFetchResult fetchDisplayBuffer(int batteryPercent) {
    DisplayFetchResult result;

    const String url =
        String(API_BASE_URL) + "/api/v1/display?battery=" + String(batteryPercent);

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
        result.error = DisplayFetchError::HttpConnectFailed;
        return result;
    }
    http.addHeader("Authorization", String("Bearer ") + API_AUTH_TOKEN);

    const int status = http.GET();
    result.httpStatus = status;
    if (status <= 0) {
        http.end();
        result.error = DisplayFetchError::HttpTimeout;
        return result;
    }
    if (status != HTTP_CODE_OK) {
        http.end();
        result.error = DisplayFetchError::HttpStatus;
        return result;
    }

    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(HTTP_MAX_RESPONSE_BYTES, MALLOC_CAP_SPIRAM));
    if (!buf) {
        http.end();
        result.error = DisplayFetchError::OutOfMemory;
        return result;
    }

    MemoryStream sink(buf, HTTP_MAX_RESPONSE_BYTES);
    const int written = http.writeToStream(&sink);
    http.end();

    if (written < 0) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::HttpTimeout;
        return result;
    }
    if (sink.overflowed()) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::ResponseTooLarge;
        return result;
    }
    const size_t received = sink.written();

    // Validate before trusting anything, mirroring internal/display/codec.go.
    if (received < kHeaderLen) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::TooShort;
        return result;
    }
    if (memcmp(buf, kMagic, 4) != 0) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::BadMagic;
        return result;
    }
    if (buf[4] != kFormatVersion) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::UnsupportedVersion;
        return result;
    }
    if (buf[9] != kBitsPerPixel) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::UnsupportedBpp;
        return result;
    }

    const uint16_t width = (static_cast<uint16_t>(buf[5]) << 8) | buf[6];
    const uint16_t height = (static_cast<uint16_t>(buf[7]) << 8) | buf[8];
    // The panel's init sequence hardcodes its native resolution (see
    // eink_driver.cpp) -- a mismatched size can never be displayed
    // correctly on this specific physical panel, so reject it outright
    // rather than trying to scale/crop.
    if (width != EPD_EXPECTED_WIDTH || height != EPD_EXPECTED_HEIGHT) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::UnexpectedDimensions;
        return result;
    }

    const size_t expectedPayload = (static_cast<size_t>(width) * height * kBitsPerPixel + 7) / 8;
    if (received - kHeaderLen < expectedPayload) {
        heap_caps_free(buf);
        result.error = DisplayFetchError::PayloadTooShort;
        return result;
    }

    result.error = DisplayFetchError::None;
    result.rawBuffer = buf;
    result.pixels = buf + kHeaderLen;
    result.width = width;
    result.height = height;
    return result;
}
