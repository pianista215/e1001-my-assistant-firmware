#pragma once

// First-boot (or post-reset) WiFi/API provisioning. Opens a SoftAP with a
// per-device WPA2 password, shows a QR + fallback instructions on the
// e-ink panel, and serves a small captive-portal web form (WiFi SSID/
// password, API endpoint URL, auth token, TLS fingerprint). Validates
// everything live against the real network before saving (unless the
// user opts into "save anyway"), then persists it via device_config and
// restarts into the normal cycle.
//
// Called from main.cpp's setup() whenever !device_config::isConfigured().
// Never returns: either ESP.restart()s after a successful save, or
// (if nobody finishes the form) sleeps and lets the next wake retry.
namespace setup_portal {

[[noreturn]] void run();

}  // namespace setup_portal
