#pragma once
#include <cstdint>

// Raw UC8179 driver for the E1001's 7.5" 800x480 4-level-gray panel, ported
// from Seeed's own confirmed-working example (see CLAUDE.md for the exact
// source). Deliberately does NOT depend on GxEPD2 or Seeed_GFX: the
// my-assistant backend already emits pixels packed in this exact panel-
// native bit-plane-friendly layout (2bpp, MSB-first, row-major, no row
// padding, 0=black..3=white), so the HTTP response body can be handed
// straight to drawFrame() with zero extra framebuffer copies.
namespace eink {

// One-time SPI/GPIO pin setup. Call once per wake cycle before drawFrame()
// or drawErrorScreen().
void init();

// Runs the panel's gray-mode init sequence, uploads `packed2bpp` as two
// UC8179 bit planes, and triggers a full-screen refresh. `width`/`height`
// must match the panel's native resolution (display_client.cpp already
// rejects anything else before this is called). Returns false if the BUSY
// line never released within the bounded timeout -- treat as a hardware
// fault, not something to retry immediately.
bool drawFrame(const uint8_t* packed2bpp, uint16_t width, uint16_t height);

// Puts the panel into its own low-power deep sleep. Call every cycle right
// before the ESP32 itself sleeps -- independent of the MCU's own sleep
// state, the panel has to be told separately.
void sleep();

// Renders a short error code plus the consecutive-failure count using a
// minimal built-in font, for the rare case of repeated fetch/decode
// failures. Costs one extra panel refresh, so callers should only invoke
// this after several consecutive failures, not on the first blip.
void drawErrorScreen(const char* code, uint8_t consecutiveFailures);

// Renders the first-boot / reset provisioning screen: a scannable
// "WIFI:T:WPA;S:...;P:...;;" QR code (see setup_portal.h) that lets a
// phone camera join the setup hotspot directly, plus the AP SSID/password
// and a fallback URL as plain text for phones that don't auto-join from a
// QR. Called once per portal session (see setup_portal.cpp) -- not meant
// to be redrawn on every retry.
void drawProvisioningScreen(const char* apSsid, const char* apPassword,
                             const char* qrPayload, const char* fallbackUrl);

}  // namespace eink
