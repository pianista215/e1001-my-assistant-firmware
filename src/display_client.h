#pragma once
#include <cstddef>
#include <cstdint>

// Mirrors exactly what my-assistant's internal/display/codec.go validates
// server-side, so a malformed/unexpected response is rejected before ever
// touching the panel.
enum class DisplayFetchError {
    None,
    HttpConnectFailed,
    HttpTimeout,
    HttpStatus,  // non-200 status; see DisplayFetchResult::httpStatus
    ResponseTooLarge,
    TooShort,
    BadMagic,
    UnsupportedVersion,
    UnsupportedBpp,
    UnexpectedDimensions,
    PayloadTooShort,
    OutOfMemory,
};

const char* toString(DisplayFetchError err);

struct DisplayFetchResult {
    DisplayFetchError error = DisplayFetchError::OutOfMemory;
    int httpStatus = 0;

    // Owns the full HTTP response body (allocated in PSRAM). `pixels`
    // points *into* this same buffer at body+10 (no extra copy) -- only
    // valid when error == None. Caller must call free() when done.
    uint8_t* rawBuffer = nullptr;
    const uint8_t* pixels = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;

    bool ok() const { return error == DisplayFetchError::None; }
    void free();
};

// Fetches and validates the display buffer for the given battery percentage
// (1-100). Requires WiFi to already be connected. Blocks until the request
// completes or HTTP_TIMEOUT_MS elapses -- never hangs indefinitely, since
// this runs unattended on battery.
DisplayFetchResult fetchDisplayBuffer(int batteryPercent);
