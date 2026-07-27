#include "setup_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cctype>

#include "config.h"
#include "device_config.h"
#include "display_client.h"
#include "eink_driver.h"
#include "sleep_control.h"
#include "wifi_manager.h"

namespace {

WebServer server(80);
DNSServer dnsServer;
unsigned long g_lastActivity = 0;

void touchActivity() { g_lastActivity = millis(); }

// Deterministic per-device suffix so the AP SSID/password (and therefore
// the QR payload) don't change between provisioning retries.
String chipIdHex(uint8_t shiftBytes) {
    char buf[7];
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(buf, sizeof(buf), "%06llX",
             static_cast<unsigned long long>((mac >> (shiftBytes * 8)) & 0xFFFFFFULL));
    return String(buf);
}

String deriveApSsid() { return String(AP_SSID_PREFIX) + chipIdHex(0); }

// WPA2 requires >=8 chars; this is 12.
String deriveApPassword() { return String("e1001-") + chipIdHex(3); }

// Escapes '\', ';', ',', ':' per the informal "WIFI:" QR payload format
// that iOS/Android camera apps recognize for auto-join.
String escapeQrField(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':') out += '\\';
        out += c;
    }
    return out;
}

String buildWifiQrPayload(const String& ssid, const String& password) {
    return "WIFI:T:WPA;S:" + escapeQrField(ssid) + ";P:" + escapeQrField(password) + ";;";
}

// Accepts whatever punctuation/case the user pasted from the backend's
// /api/v1/tls-cert page (colon-separated, upper or lower case) and
// normalizes to bare uppercase hex, which is what gets stored and what
// WiFiClientSecure::verify() expects.
String normalizeFingerprint(const String& raw) {
    String out;
    out.reserve(64);
    for (size_t i = 0; i < raw.length(); i++) {
        const char c = raw[i];
        if (c == ':' || isspace(static_cast<unsigned char>(c))) continue;
        out += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

bool isValidFingerprint(const String& normalized) {
    if (normalized.length() != 64) return false;
    for (size_t i = 0; i < normalized.length(); i++) {
        if (!isxdigit(static_cast<unsigned char>(normalized[i]))) return false;
    }
    return true;
}

// Translates a fetchDisplayBuffer() failure into a message a non-technical
// user can act on, so the setup form points at the specific field to fix
// instead of a raw error code.
String explainFetchError(const DisplayFetchResult& fetch) {
    switch (fetch.error) {
        case DisplayFetchError::None:
            return "OK";
        case DisplayFetchError::HttpConnectFailed:
            return "No se pudo contactar con el endpoint (revisa la URL y el puerto).";
        case DisplayFetchError::HttpTimeout:
            return "El endpoint no respondió a tiempo.";
        case DisplayFetchError::HttpUnauthorized:
            return "El servidor rechazó el token (401). Revisa el token.";
        case DisplayFetchError::TlsConnectFailed:
            return "No se pudo establecer una conexión TLS con el servidor.";
        case DisplayFetchError::TlsFingerprintMismatch: {
            String msg = "El fingerprint no coincide con el certificado real del servidor.";
            if (fetch.actualFingerprintHex.length() > 0) {
                msg += " El servidor presentó: " + fetch.actualFingerprintHex +
                       ". Cópialo de nuevo desde /api/v1/tls-cert.";
            }
            return msg;
        }
        case DisplayFetchError::HttpStatus: {
            char buf[72];
            snprintf(buf, sizeof(buf), "El servidor respondió con un error inesperado (HTTP %d).",
                      fetch.httpStatus);
            return String(buf);
        }
        case DisplayFetchError::BadMagic:
        case DisplayFetchError::UnsupportedVersion:
        case DisplayFetchError::UnsupportedBpp:
        case DisplayFetchError::UnexpectedDimensions:
        case DisplayFetchError::TooShort:
        case DisplayFetchError::PayloadTooShort:
            return "El servidor respondió, pero con un formato inesperado; revisa que la URL "
                   "apunte a este mismo backend.";
        case DisplayFetchError::ResponseTooLarge:
        case DisplayFetchError::OutOfMemory:
            return "Fallo interno del dispositivo, vuelve a intentarlo.";
    }
    return "Error desconocido.";
}

String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        switch (in[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += in[i];
        }
    }
    return out;
}

String renderFormPage(const DeviceConfig& values, const String& message, bool isError) {
    String html;
    html.reserve(2200);
    html +=
        "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Configura el dispositivo</title><style>"
        "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;}"
        "label{display:block;margin-top:14px;font-weight:600;}"
        "input{width:100%;padding:8px;font-size:16px;box-sizing:border-box;}"
        ".msg{padding:10px;border-radius:6px;margin-bottom:12px;}"
        ".err{background:#fdecea;color:#611a15;}"
        ".ok{background:#e6f4ea;color:#1e4620;}"
        ".hint{color:#666;font-size:13px;margin-top:4px;}"
        "button{margin-top:20px;padding:10px 16px;font-size:16px;}"
        ".secondary{margin-top:24px;padding-top:12px;border-top:1px solid #ddd;font-size:13px;"
        "color:#666;}"
        ".secondary label{display:inline;font-weight:normal;}"
        ".secondary input{width:auto;}"
        "</style></head><body><h2>Configura el dispositivo</h2>";

    if (message.length() > 0) {
        html += "<div class=\"msg ";
        html += isError ? "err" : "ok";
        html += "\">" + htmlEscape(message) + "</div>";
    }

    html += "<form method=\"POST\" action=\"/save\">";
    html += "<label>Red WiFi (SSID)</label><input name=\"ssid\" value=\"" +
            htmlEscape(values.wifiSsid) + "\" required>";
    html += "<label>Contraseña WiFi</label><input type=\"password\" name=\"pass\" value=\"" +
            htmlEscape(values.wifiPassword) + "\">";
    html += "<label>URL del endpoint</label><input name=\"url\" value=\"" +
            htmlEscape(values.apiBaseUrl) +
            "\" placeholder=\"https://mi-servidor:8443\" required>";
    html += "<label>Token</label><input name=\"token\" value=\"" +
            htmlEscape(values.apiAuthToken) + "\" required>";
    html += "<label>Fingerprint SHA-256 del certificado (si es https)</label>"
            "<input name=\"fp\" value=\"" +
            htmlEscape(values.tlsFingerprint) + "\" placeholder=\"AA:BB:CC:...\">";
    html += "<div class=\"hint\">Cópialo desde /api/v1/tls-cert en tu backend.</div>";
    html += "<button type=\"submit\">Guardar y verificar</button>";
    html += "<div class=\"secondary\"><label><input type=\"checkbox\" name=\"save_anyway\" "
            "value=\"1\"> Guardar de todas formas aunque falle la verificación</label></div>";
    html += "</form></body></html>";
    return html;
}

String renderSavedPage() {
    return "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Guardado</title></head><body style=\"font-family:sans-serif;"
           "max-width:480px;margin:40px auto;padding:0 16px;text-align:center;\">"
           "<h2>Configuración guardada</h2><p>El dispositivo se está reiniciando...</p>"
           "</body></html>";
}

void handleRoot() {
    touchActivity();
    server.send(200, "text/html", renderFormPage(DeviceConfig{}, "", false));
}

void handleSave() {
    touchActivity();

    DeviceConfig candidate;
    candidate.wifiSsid = server.arg("ssid");
    candidate.wifiSsid.trim();
    candidate.wifiPassword = server.arg("pass");
    candidate.apiBaseUrl = server.arg("url");
    candidate.apiBaseUrl.trim();
    while (candidate.apiBaseUrl.endsWith("/")) {
        candidate.apiBaseUrl.remove(candidate.apiBaseUrl.length() - 1);
    }
    candidate.apiAuthToken = server.arg("token");
    candidate.apiAuthToken.trim();
    candidate.tlsFingerprint = normalizeFingerprint(server.arg("fp"));
    const bool saveAnyway = server.hasArg("save_anyway");

    const bool isHttps = candidate.apiBaseUrl.startsWith("https://");
    const bool isHttp = candidate.apiBaseUrl.startsWith("http://");

    // Local format checks first -- no reason to touch the radio for a
    // typo'd URL or missing field.
    if (candidate.wifiSsid.length() == 0) {
        server.send(200, "text/html", renderFormPage(candidate, "Falta el SSID de la wifi.", true));
        return;
    }
    if (!isHttps && !isHttp) {
        server.send(200, "text/html",
                     renderFormPage(candidate, "La URL debe empezar por http:// o https://.", true));
        return;
    }
    if (candidate.apiAuthToken.length() == 0) {
        server.send(200, "text/html", renderFormPage(candidate, "Falta el token.", true));
        return;
    }
    if (isHttps && !isValidFingerprint(candidate.tlsFingerprint)) {
        server.send(200, "text/html",
                     renderFormPage(candidate,
                                     "El fingerprint debe tener 64 caracteres hexadecimales "
                                     "(con o sin ':').",
                                     true));
        return;
    }

    if (!saveAnyway) {
        // The SoftAP is already up in WIFI_AP_STA -- this only drives the
        // STA side, the phone connected to our AP stays connected.
        WifiFastConnect scratch;
        wifiBeginConnect(candidate.wifiSsid.c_str(), candidate.wifiPassword.c_str(), scratch);
        const bool wifiOk =
            wifiWaitConnected(scratch, PORTAL_VALIDATE_WIFI_TIMEOUT_MS, PORTAL_VALIDATE_WIFI_TIMEOUT_MS);
        if (!wifiOk) {
            WiFi.disconnect(false);  // drop the STA attempt only, keep the AP alive
            server.send(200, "text/html",
                         renderFormPage(candidate,
                                         "No se pudo conectar a esa wifi (SSID/contraseña "
                                         "incorrectos o fuera de alcance).",
                                         true));
            return;
        }

        // Same fetchDisplayBuffer() the normal hourly cycle uses -- one
        // HTTP client code path, exercised for real here.
        const DisplayEndpointConfig ep{candidate.apiBaseUrl, candidate.apiAuthToken,
                                        candidate.tlsFingerprint};
        DisplayFetchResult fetch = fetchDisplayBuffer(ep, /*batteryPercent=*/50);
        const String errorMessage = fetch.ok() ? String() : explainFetchError(fetch);
        fetch.free();
        WiFi.disconnect(false);

        if (!errorMessage.isEmpty()) {
            server.send(200, "text/html", renderFormPage(candidate, errorMessage, true));
            return;
        }
    }

    device_config::save(candidate);
    server.send(200, "text/html", renderSavedPage());
    server.client().flush();
    delay(1500);  // give the response time to actually reach the phone
    WiFi.softAPdisconnect(true);
    wifiDisconnect();
    ESP.restart();
    while (true) delay(1000);  // unreachable
}

void redirectToPortalRoot() {
    touchActivity();
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}

// Standard captive-portal probe URLs for Apple/Android/Windows/Firefox --
// answering anything other than their expected "everything is fine" body
// makes the OS pop up the "sign in to network" prompt automatically.
// Combined with the DNSServer catch-all below, this covers probes by both
// IP and hostname.
void registerCaptivePortalRoutes() {
    server.on("/hotspot-detect.html", redirectToPortalRoot);
    server.on("/library/test/success.html", redirectToPortalRoot);
    server.on("/generate_204", redirectToPortalRoot);
    server.on("/gen_204", redirectToPortalRoot);
    server.on("/connecttest.txt", redirectToPortalRoot);
    server.on("/ncsi.txt", redirectToPortalRoot);
    server.on("/redirect", redirectToPortalRoot);
    server.on("/success.txt", redirectToPortalRoot);
    server.onNotFound(redirectToPortalRoot);
}

}  // namespace

namespace setup_portal {

[[noreturn]] void run() {
    WiFi.mode(WIFI_AP_STA);

    const String apSsid = deriveApSsid();
    const String apPassword = deriveApPassword();
    WiFi.softAP(apSsid.c_str(), apPassword.c_str());
    delay(200);  // let the AP interface settle before DNS/HTTP bind to it

    dnsServer.start(53, "*", WiFi.softAPIP());

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    registerCaptivePortalRoutes();
    server.begin();

    Serial1.printf("[PORTAL] SoftAP '%s' up (password '%s'). Visit http://%s/\n", apSsid.c_str(),
                    apPassword.c_str(), WiFi.softAPIP().toString().c_str());

    const String qrPayload = buildWifiQrPayload(apSsid, apPassword);
    eink::init();
    eink::drawProvisioningScreen(apSsid.c_str(), apPassword.c_str(), qrPayload.c_str(),
                                  "http://192.168.4.1/");
    eink::sleep();

    touchActivity();
    while (true) {
        dnsServer.processNextRequest();
        server.handleClient();
        if (millis() - g_lastActivity > PORTAL_INACTIVITY_TIMEOUT_MS) {
            Serial1.println("[PORTAL] Inactivity timeout -- sleeping, will retry on next wake.");
            server.stop();
            dnsServer.stop();
            WiFi.softAPdisconnect(true);
            wifiDisconnect();
            goToSleep(PORTAL_RETRY_SLEEP_SEC);
        }
        delay(10);
    }
}

}  // namespace setup_portal
