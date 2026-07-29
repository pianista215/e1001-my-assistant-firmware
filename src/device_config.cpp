#include "device_config.h"

#include <Preferences.h>

#include "config.h"

namespace {
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPass = "pass";
constexpr const char* kKeyUrl = "url";
constexpr const char* kKeyToken = "token";
constexpr const char* kKeyFingerprint = "fp";
constexpr const char* kKeyLang = "lang";
constexpr const char* kKeyDone = "done";
}  // namespace

namespace device_config {

bool isConfigured() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) return false;
    const bool done = prefs.getBool(kKeyDone, false);
    prefs.end();
    return done;
}

bool load(DeviceConfig& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) return false;
    if (!prefs.getBool(kKeyDone, false)) {
        prefs.end();
        return false;
    }
    out.wifiSsid = prefs.getString(kKeySsid, "");
    out.wifiPassword = prefs.getString(kKeyPass, "");
    out.apiBaseUrl = prefs.getString(kKeyUrl, "");
    out.apiAuthToken = prefs.getString(kKeyToken, "");
    out.tlsFingerprint = prefs.getString(kKeyFingerprint, "");
    out.language = prefs.getString(kKeyLang, "en");
    prefs.end();
    return true;
}

void save(const DeviceConfig& cfg) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putString(kKeySsid, cfg.wifiSsid);
    prefs.putString(kKeyPass, cfg.wifiPassword);
    prefs.putString(kKeyUrl, cfg.apiBaseUrl);
    prefs.putString(kKeyToken, cfg.apiAuthToken);
    prefs.putString(kKeyFingerprint, cfg.tlsFingerprint);
    prefs.putString(kKeyLang, cfg.language);
    prefs.putBool(kKeyDone, true);  // last: marks the config as complete/valid
    prefs.end();
}

void clear() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }
}

}  // namespace device_config
