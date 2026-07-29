#pragma once

#include <Arduino.h>

#include <cstdint>

#include "display_client.h"

// Two-language selector for every human-facing string this firmware prints,
// on both the e-ink panel and the setup portal's web UI. Persisted per
// device as DeviceConfig::language ("en"/"es", see device_config.h).
//
// English is the fixed default wherever no language has been chosen yet
// (drawProvisioningScreen(), always called before any config exists; the
// portal's very first page load before ?lang= or a saved config is
// available) -- see setup_portal.cpp/eink_driver.cpp for the concrete call
// sites.
enum class Lang : uint8_t { EN, ES };

namespace i18n {

// "en" / "es" -- the exact value stored in NVS, used for <html lang="...">
// and the portal's ?lang= query parameter.
const char* langCode(Lang lang);

// Anything unrecognized (including an empty string, e.g. a config saved
// before this field existed) maps to Lang::EN.
Lang langFromCode(const String& code);

// Strings drawn on the physical e-ink panel. Spanish here is deliberately
// accent-free: eink_driver.cpp's font draws one glyph per raw byte with no
// UTF-8 decoding, so a literal accented character would render as two wrong
// glyphs (see eink_driver.cpp for the full explanation).
struct PanelStrings {
    const char* errorFailuresFmt;  // snprintf format, one %u -- drawErrorScreen()

    // drawProvisioningScreen() -- always rendered with Lang::EN today (see
    // above), but kept in this same table rather than as separate inline
    // literals so all panel prose lives in one place.
    const char* provisioningTitleLine1;
    const char* provisioningTitleLine2;
    const char* provisioningStep1Line1;
    const char* provisioningStep1Line2;
    const char* provisioningFallbackIntro;
    const char* provisioningNetworkLabel;
    const char* provisioningPasswordLabel;
    const char* provisioningStep2Line1;
    const char* provisioningStep2Line2;
};

const PanelStrings& panel(Lang lang);

// Strings served by the setup portal's web UI. Browsers render UTF-8 fine,
// so Spanish keeps its normal accents here.
struct PortalStrings {
    const char* deviceTitle;  // form page <title>/<h2>
    const char* labelSsid;
    const char* labelPass;
    const char* labelUrl;
    const char* labelToken;
    const char* labelLanguage;
    const char* httpsHint;
    const char* saveButton;
    const char* errMissingSsid;
    const char* errBadUrl;
    const char* errMissingToken;
    const char* validatingTitle;
    const char* validatingHeading;
    const char* connectingWifiMsg;
    const char* confirmButton;
    const char* rejectButton;
    const char* slowHint;
    const char* retryLink;
    const char* wifiFailedMsg;
    const char* fetchingCertMsg;
    const char* certProbeFailedPrefix;
    const char* certProbeConnectFailedSuffix;
    const char* certProbeNoPeerCertSuffix;
    const char* certProbeNotHttpsSuffix;
    const char* awaitingFingerprintMsg;
    const char* fingerprintTimeoutMsg;
    const char* fingerprintRejectedMsg;
    const char* testingEndpointMsg;
    const char* successMsg;
    const char* taskStartFailedMsg;
};

const PortalStrings& portal(Lang lang);

// Translates a fetchDisplayBuffer() failure into prose for the portal's
// error banner, keyed directly off the existing DisplayFetchError enum
// (display_client.h) instead of a parallel string-ID enum.
String explainFetchError(Lang lang, const DisplayFetchResult& fetch);

}  // namespace i18n
