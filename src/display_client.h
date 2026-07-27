#pragma once
#include <Arduino.h>

#include <cstddef>
#include <cstdint>

// Mirrors exactly what my-assistant's internal/display/codec.go validates
// server-side, so a malformed/unexpected response is rejected before ever
// touching the panel.
enum class DisplayFetchError {
    None,
    HttpConnectFailed,
    HttpTimeout,
    HttpStatus,        // non-200/401 status; see DisplayFetchResult::httpStatus
    HttpUnauthorized,  // 401 specifically -- almost always a wrong auth token
    TlsConnectFailed,       // https:// endpoint: TCP/TLS handshake itself failed
    TlsFingerprintMismatch,  // https:// endpoint: handshake OK, cert doesn't match
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

    // Only set when error == TlsFingerprintMismatch: the SHA-256 (64
    // uppercase hex chars, no ":") actually presented by the server,
    // straight from the failed handshake -- lets a caller log/display
    // "expected X, got Y" instead of a bare mismatch, since a fingerprint
    // that "looks right" when pasted can still differ by one copy/paste
    // slip, or the server's cert may have been regenerated since.
    String actualFingerprintHex;

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

// Endpoint config, previously the compile-time API_BASE_URL/API_AUTH_TOKEN
// macros -- now entered through the setup portal and persisted via
// device_config. `baseUrl`'s scheme picks the transport: "https://" uses
// WiFiClientSecure with fingerprint pinning (`fingerprintHex` required --
// 64 uppercase hex chars, no ":"); "http://" uses a plain WiFiClient and
// ignores `fingerprintHex`. Shared verbatim between the normal hourly
// cycle and the setup portal's live validation step, so there's exactly
// one HTTP client code path.
struct DisplayEndpointConfig {
    String baseUrl;  // "http://host[:port]" or "https://host[:port]", no trailing slash
    String authToken;
    String fingerprintHex;
};

// Fetches and validates the display buffer for the given battery percentage
// (1-100). Requires WiFi to already be connected. Blocks until the request
// completes or HTTP_TIMEOUT_MS elapses -- never hangs indefinitely, since
// this runs unattended on battery.
DisplayFetchResult fetchDisplayBuffer(const DisplayEndpointConfig& cfg, int batteryPercent);
