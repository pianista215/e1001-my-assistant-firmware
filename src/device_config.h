#pragma once

#include <Arduino.h>

// Runtime-provisioned per-device config: WiFi credentials, API endpoint,
// auth token, and (for https:// endpoints) the pinned TLS certificate
// fingerprint. Replaces what used to be compile-time WIFI_SSID/
// WIFI_PASSWORD/API_BASE_URL/API_AUTH_TOKEN secrets -- entered once through
// the first-boot SoftAP setup portal (see setup_portal.h) and persisted in
// NVS so it survives a real power-on/EN reset (unlike RTC_DATA_ATTR
// state, see sleep_state.h).
struct DeviceConfig {
    String wifiSsid;
    String wifiPassword;
    String apiBaseUrl;      // "http://host[:port]" or "https://host[:port]", no trailing slash
    String apiAuthToken;
    String tlsFingerprint;  // 64 uppercase hex chars, no ":"; empty when apiBaseUrl is http://
    String language;        // "en" or "es" -- see i18n.h. Empty/unrecognized -> Lang::EN.
};

namespace device_config {

// Cheap check (reads a single bool) for whether a full config has ever
// been successfully saved -- doesn't load the actual strings.
bool isConfigured();

// Loads the saved config. Returns false (and leaves `out` unchanged) if
// nothing has been saved yet.
bool load(DeviceConfig& out);

// Persists `cfg`. Writes every field before marking the config as done,
// so a power loss mid-save leaves isConfigured() false (re-enters
// provisioning) instead of booting with partially-blank fields.
void save(const DeviceConfig& cfg);

// Wipes the saved config -- used by the physical reset gesture (holding
// the wake button) to force re-entry into the setup portal.
void clear();

}  // namespace device_config
