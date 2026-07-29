#include "setup_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "device_config.h"
#include "display_client.h"
#include "eink_driver.h"
#include "i18n.h"
#include "sleep_control.h"
#include "wifi_manager.h"

namespace {

WebServer server(80);
DNSServer dnsServer;
unsigned long g_lastActivity = 0;

void touchActivity() { g_lastActivity = millis(); }

constexpr uint32_t VALIDATION_TASK_STACK_BYTES = 16384;
constexpr UBaseType_t VALIDATION_TASK_PRIORITY = 1;  // same as loopTask

// Live-validation progress, polled by the phone during handleSave() so a
// single dropped request (e.g. the SoftAP hopping channels when the STA
// associates elsewhere -- see CLAUDE.md) never leaves the page stuck
// waiting on one long-lived response. Written by validationTask() (a
// background FreeRTOS task), read by the HTTP handlers running on the
// main loop task -- protected by g_validationMux since it's genuinely
// cross-task shared state.
enum class ValidationStage : uint8_t {
    Idle,
    ConnectingWifi,
    FetchingCertificate,             // https only: TOFU TLS probe in flight
    AwaitingFingerprintConfirmation,  // https only: waiting on a human decision
    TestingEndpoint,
    Success,
    Failed,
};

// Set by handleValidateConfirm() (main loop task), read by validationTask()
// (background task) while it's parked waiting on AwaitingFingerprintConfirmation.
enum class FingerprintDecision : uint8_t { Pending, Confirmed, Rejected };

struct ValidationState {
    ValidationStage stage = ValidationStage::Idle;
    String message;          // human-readable, also used to prefill the error banner
    DeviceConfig candidate;  // last attempt, so a failed retry doesn't retype everything
    String pendingFingerprint;                              // awaiting confirmation
    FingerprintDecision decision = FingerprintDecision::Pending;
};

ValidationState g_validationState;
portMUX_TYPE g_validationMux = portMUX_INITIALIZER_UNLOCKED;

ValidationState snapshotValidationState() {
    portENTER_CRITICAL(&g_validationMux);
    ValidationState copy = g_validationState;
    portEXIT_CRITICAL(&g_validationMux);
    return copy;
}

void setValidationProgress(ValidationStage stage, const String& message) {
    portENTER_CRITICAL(&g_validationMux);
    g_validationState.stage = stage;
    g_validationState.message = message;
    portEXIT_CRITICAL(&g_validationMux);
}

// Atomic check-and-set: only applies the decision if we're still actually
// waiting on one (guards against a stale/duplicate confirm arriving after
// the attempt already timed out, was rejected, or a new attempt started).
void trySetFingerprintDecision(FingerprintDecision decision) {
    portENTER_CRITICAL(&g_validationMux);
    if (g_validationState.stage == ValidationStage::AwaitingFingerprintConfirmation) {
        g_validationState.decision = decision;
    }
    portEXIT_CRITICAL(&g_validationMux);
}

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

// Runs on a background FreeRTOS task so the WebServer/DNSServer loop in
// setup_portal::run() never blocks on WiFi/HTTPS timeouts (up to ~23s
// combined) -- see CLAUDE.md for why that mattered on real hardware.
// Invariant: this function must NEVER touch `server` (WebServer isn't
// thread-safe) -- only setValidationProgress()/the mutex-protected state.
const char* probeErrorToString(TlsFingerprintProbeError err) {
    switch (err) {
        case TlsFingerprintProbeError::None: return "OK";
        case TlsFingerprintProbeError::UrlNotHttps: return "URL_NOT_HTTPS";
        case TlsFingerprintProbeError::ConnectFailed: return "TLS_CONNECT_FAILED";
        case TlsFingerprintProbeError::FingerprintUnavailable: return "NO_PEER_CERT";
    }
    return "UNKNOWN";
}

void validationTask(void* param) {
    DeviceConfig* candidate = static_cast<DeviceConfig*>(param);
    const Lang lang = i18n::langFromCode(candidate->language);
    const i18n::PortalStrings& ps = i18n::portal(lang);

    Serial1.printf("[PORTAL] Validating: ssid='%s' url='%s'\n", candidate->wifiSsid.c_str(),
                    candidate->apiBaseUrl.c_str());

    WifiFastConnect scratch;
    wifiBeginConnect(candidate->wifiSsid.c_str(), candidate->wifiPassword.c_str(), scratch);
    const bool wifiOk =
        wifiWaitConnected(scratch, PORTAL_VALIDATE_WIFI_TIMEOUT_MS, PORTAL_VALIDATE_WIFI_TIMEOUT_MS);
    if (!wifiOk) {
        Serial1.println("[PORTAL] Validation failed: WiFi did not connect.");
        WiFi.disconnect(false);  // drop the STA attempt only, keep the AP alive
        setValidationProgress(ValidationStage::Failed, ps.wifiFailedMsg);
        delete candidate;
        vTaskDelete(nullptr);
        return;  // unreachable; vTaskDelete(nullptr) never returns
    }
    Serial1.printf("[PORTAL] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());

    const bool isHttps = candidate->apiBaseUrl.startsWith("https://");
    if (isHttps) {
        // Trust-on-first-use: connect with zero verification, show the
        // human what certificate the server actually presented, and only
        // pin it if they confirm. probeTlsFingerprintInsecure() is
        // strictly TOFU-only -- the real pinned check happens below via
        // fetchDisplayBuffer() once a fingerprint has been confirmed.
        setValidationProgress(ValidationStage::FetchingCertificate, ps.fetchingCertMsg);
        Serial1.printf("[PORTAL] Probing TLS certificate at %s ...\n", candidate->apiBaseUrl.c_str());
        const unsigned long probeStart = millis();
        const TlsFingerprintProbeResult probe = probeTlsFingerprintInsecure(candidate->apiBaseUrl);
        Serial1.printf("[PORTAL] TLS probe result: %s (took %lu ms)\n",
                        probeErrorToString(probe.error),
                        static_cast<unsigned long>(millis() - probeStart));
        if (probe.error != TlsFingerprintProbeError::None) {
            WiFi.disconnect(false);
            String msg = ps.certProbeFailedPrefix;
            switch (probe.error) {
                case TlsFingerprintProbeError::ConnectFailed:
                    msg += ps.certProbeConnectFailedSuffix;
                    break;
                case TlsFingerprintProbeError::FingerprintUnavailable:
                    msg += ps.certProbeNoPeerCertSuffix;
                    break;
                default:
                    msg += ps.certProbeNotHttpsSuffix;
                    break;
            }
            setValidationProgress(ValidationStage::Failed, msg);
            delete candidate;
            vTaskDelete(nullptr);
            return;  // unreachable; vTaskDelete(nullptr) never returns
        }
        Serial1.printf("[PORTAL] Certificate fingerprint: %s\n", probe.fingerprintHex.c_str());

        portENTER_CRITICAL(&g_validationMux);
        g_validationState.stage = ValidationStage::AwaitingFingerprintConfirmation;
        g_validationState.pendingFingerprint = probe.fingerprintHex;
        g_validationState.decision = FingerprintDecision::Pending;
        g_validationState.message = ps.awaitingFingerprintMsg;
        portEXIT_CRITICAL(&g_validationMux);

        const unsigned long waitStart = millis();
        FingerprintDecision decision = FingerprintDecision::Pending;
        while (decision == FingerprintDecision::Pending) {
            vTaskDelay(pdMS_TO_TICKS(300));
            decision = snapshotValidationState().decision;
            if (decision == FingerprintDecision::Pending &&
                millis() - waitStart > PORTAL_FINGERPRINT_CONFIRM_TIMEOUT_MS) {
                break;
            }
        }

        if (decision == FingerprintDecision::Pending) {
            Serial1.println("[PORTAL] Fingerprint confirmation timed out.");
            WiFi.disconnect(false);
            setValidationProgress(ValidationStage::Failed, ps.fingerprintTimeoutMsg);
            delete candidate;
            vTaskDelete(nullptr);
            return;  // unreachable; vTaskDelete(nullptr) never returns
        }
        if (decision == FingerprintDecision::Rejected) {
            Serial1.println("[PORTAL] Fingerprint rejected by user.");
            WiFi.disconnect(false);
            setValidationProgress(ValidationStage::Failed, ps.fingerprintRejectedMsg);
            delete candidate;
            vTaskDelete(nullptr);
            return;  // unreachable; vTaskDelete(nullptr) never returns
        }

        Serial1.println("[PORTAL] Fingerprint confirmed by user.");
        candidate->tlsFingerprint = probe.fingerprintHex;  // Confirmed
    }

    setValidationProgress(ValidationStage::TestingEndpoint, ps.testingEndpointMsg);

    // Same fetchDisplayBuffer() the normal hourly cycle uses -- one HTTP
    // client code path, exercised for real here. For https this re-does
    // the TLS handshake, this time actually pinned against the
    // just-confirmed fingerprint, and also exercises the Bearer token.
    const DisplayEndpointConfig ep{candidate->apiBaseUrl, candidate->apiAuthToken,
                                    candidate->tlsFingerprint};
    DisplayFetchResult fetch = fetchDisplayBuffer(ep, /*batteryPercent=*/50);
    const bool fetchOk = fetch.ok();
    Serial1.printf("[PORTAL] Endpoint test result: %s (http=%d)\n", toString(fetch.error),
                    fetch.httpStatus);
    const String errorMessage = fetchOk ? String() : i18n::explainFetchError(lang, fetch);
    fetch.free();
    WiFi.disconnect(false);

    if (!fetchOk) {
        setValidationProgress(ValidationStage::Failed, errorMessage);
        delete candidate;
        vTaskDelete(nullptr);
        return;  // unreachable; vTaskDelete(nullptr) never returns
    }

    Serial1.println("[PORTAL] Validation OK, saving config.");
    setValidationProgress(ValidationStage::Success, ps.successMsg);
    device_config::save(*candidate);
    delete candidate;

    // The client learns about success on its next poll (up to ~1s away),
    // not from a direct HTTP response -- keep this delay so the SoftAP
    // doesn't disappear before that poll can land, same reasoning the old
    // synchronous handler had for delaying before tearing down the AP.
    delay(1500);
    WiFi.softAPdisconnect(true);
    wifiDisconnect();
    ESP.restart();
    while (true) delay(1000);  // unreachable
}

// Atomic check-and-set: refuses to start a second validation if one's
// already in flight (e.g. a double-tap on "Guardar"), which would
// otherwise race two tasks over the same WiFi radio.
void tryStartValidation(const DeviceConfig& candidate) {
    portENTER_CRITICAL(&g_validationMux);
    const bool alreadyRunning = g_validationState.stage == ValidationStage::ConnectingWifi ||
                                 g_validationState.stage == ValidationStage::FetchingCertificate ||
                                 g_validationState.stage == ValidationStage::AwaitingFingerprintConfirmation ||
                                 g_validationState.stage == ValidationStage::TestingEndpoint;
    if (!alreadyRunning) {
        g_validationState = ValidationState{};
        g_validationState.stage = ValidationStage::ConnectingWifi;
        g_validationState.message =
            i18n::portal(i18n::langFromCode(candidate.language)).connectingWifiMsg;
        g_validationState.candidate = candidate;
    }
    portEXIT_CRITICAL(&g_validationMux);
    if (alreadyRunning) return;

    DeviceConfig* taskArg = new DeviceConfig(candidate);
    const BaseType_t created = xTaskCreate(validationTask, "portal_validate",
                                            VALIDATION_TASK_STACK_BYTES, taskArg,
                                            VALIDATION_TASK_PRIORITY, nullptr);
    if (created != pdPASS) {
        delete taskArg;
        setValidationProgress(
            ValidationStage::Failed,
            i18n::portal(i18n::langFromCode(candidate.language)).taskStartFailedMsg);
    }
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
    const Lang lang = i18n::langFromCode(values.language);
    const i18n::PortalStrings& ps = i18n::portal(lang);

    String html;
    html.reserve(2400);
    html +=
        "<!DOCTYPE html><html lang=\"" + String(i18n::langCode(lang)) +
        "\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>" + String(ps.deviceTitle) + "</title><style>"
        "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;}"
        "label{display:block;margin-top:14px;font-weight:600;}"
        "input,select{width:100%;padding:8px;font-size:16px;box-sizing:border-box;}"
        ".msg{padding:10px;border-radius:6px;margin-bottom:12px;}"
        ".err{background:#fdecea;color:#611a15;}"
        ".ok{background:#e6f4ea;color:#1e4620;}"
        ".hint{color:#666;font-size:13px;margin-top:4px;}"
        "button{margin-top:20px;padding:10px 16px;font-size:16px;}"
        "</style></head><body><h2>" + String(ps.deviceTitle) + "</h2>";

    if (message.length() > 0) {
        html += "<div class=\"msg ";
        html += isError ? "err" : "ok";
        html += "\">" + htmlEscape(message) + "</div>";
    }

    html += "<form method=\"POST\" action=\"/save\">";
    html += "<label>" + String(ps.labelLanguage) +
            "</label><select name=\"lang\" onchange=\"location.search='?lang='+this.value\">";
    html += String("<option value=\"en\"") + (lang == Lang::EN ? " selected" : "") +
            ">English</option>";
    html += String("<option value=\"es\"") + (lang == Lang::ES ? " selected" : "") +
            ">Español</option>";
    html += "</select>";
    html += "<label>" + String(ps.labelSsid) + "</label><input name=\"ssid\" value=\"" +
            htmlEscape(values.wifiSsid) + "\" required>";
    html += "<label>" + String(ps.labelPass) +
            "</label><input type=\"password\" name=\"pass\" value=\"" +
            htmlEscape(values.wifiPassword) + "\">";
    html += "<label>" + String(ps.labelUrl) + "</label><input name=\"url\" value=\"" +
            htmlEscape(values.apiBaseUrl) +
            "\" placeholder=\"https://my-server:8443\" required>";
    html += "<label>" + String(ps.labelToken) + "</label><input name=\"token\" value=\"" +
            htmlEscape(values.apiAuthToken) + "\" required>";
    html += "<div class=\"hint\">" + String(ps.httpsHint) + "</div>";
    html += "<button type=\"submit\">" + String(ps.saveButton) + "</button>";
    html += "</form></body></html>";
    return html;
}

const char* stageToJsonToken(ValidationStage stage) {
    switch (stage) {
        case ValidationStage::Idle: return "idle";
        case ValidationStage::ConnectingWifi: return "connecting_wifi";
        case ValidationStage::FetchingCertificate: return "fetching_certificate";
        case ValidationStage::AwaitingFingerprintConfirmation: return "awaiting_fingerprint_confirmation";
        case ValidationStage::TestingEndpoint: return "testing_endpoint";
        case ValidationStage::Success: return "success";
        case ValidationStage::Failed: return "failed";
    }
    return "idle";
}

// Defensive: messages are mostly generated in-house, but
// explainFetchError() folds in fetch.actualFingerprintHex, which
// ultimately comes from whatever certificate the remote server presented
// -- not something to trust blindly inside a JSON string literal.
String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Polls GET /validate-status every ~1s. Deliberately does NOT treat a
// failed fetch() as an error -- the SoftAP briefly hopping channels (see
// CLAUDE.md) can drop one poll, and the fix is to just retry, not to give
// up. Each poll carries its own AbortController timeout: fetch() has no
// timeout by default, and an unresolved (not immediately failed) request
// would otherwise reproduce the exact "stuck with no reaction" symptom
// this whole mechanism exists to avoid.
String renderValidatingPage(Lang lang) {
    const i18n::PortalStrings& ps = i18n::portal(lang);
    return "<!DOCTYPE html><html lang=\"" + String(i18n::langCode(lang)) +
           "\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>" + String(ps.validatingTitle) + "</title><style>"
           "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;}"
           ".msg{padding:12px;border-radius:6px;margin-top:16px;}"
           ".err{background:#fdecea;color:#611a15;}"
           ".ok{background:#e6f4ea;color:#1e4620;}"
           ".hint{color:#666;font-size:13px;margin-top:12px;}"
           "code{display:block;font-family:monospace;word-break:break-all;background:#f4f4f4;"
           "padding:8px;border-radius:6px;margin-top:8px;}"
           "button{margin-top:12px;margin-right:8px;padding:10px 16px;font-size:16px;}"
           "</style></head><body><h2>" + String(ps.validatingHeading) + "</h2>"
           "<div id=\"status\" class=\"msg\">" + String(ps.connectingWifiMsg) + "</div>"
           "<div id=\"fpConfirm\" style=\"display:none\">"
           "<code id=\"fpValue\"></code>"
           "<button id=\"fpConfirmBtn\">" + String(ps.confirmButton) + "</button>"
           "<button id=\"fpRejectBtn\">" + String(ps.rejectButton) + "</button>"
           "</div>"
           "<div id=\"slow\" class=\"hint\" style=\"display:none\">" + String(ps.slowHint) + "</div>"
           "<p id=\"retryLink\" style=\"display:none\"><a href=\"/\">" + String(ps.retryLink) +
           "</a></p>"
           "<script>"
           "var startedAt = Date.now();"
           "var slowWarned = false;"
           "var decisionSent = false;"
           "function setFpButtonsDisabled(v) {"
           "  document.getElementById('fpConfirmBtn').disabled = v;"
           "  document.getElementById('fpRejectBtn').disabled = v;"
           "}"
           "function sendDecision(decision) {"
           "  if (decisionSent) return;"
           "  decisionSent = true;"
           "  setFpButtonsDisabled(true);"
           "  fetch('/validate-confirm', {"
           "    method: 'POST',"
           "    headers: {'Content-Type': 'application/x-www-form-urlencoded'},"
           "    body: 'decision=' + decision"
           "  }).catch(function() {"
           "    decisionSent = false;"
           "    setFpButtonsDisabled(false);"
           "  });"
           "}"
           "document.getElementById('fpConfirmBtn').onclick = function() { sendDecision('confirm'); };"
           "document.getElementById('fpRejectBtn').onclick = function() { sendDecision('reject'); };"
           "function poll() {"
           "  var controller = new AbortController();"
           "  var timeoutId = setTimeout(function() { controller.abort(); }, 4000);"
           "  fetch('/validate-status', {cache: 'no-store', signal: controller.signal})"
           "    .then(function(r) { return r.json(); })"
           "    .then(function(data) {"
           "      clearTimeout(timeoutId);"
           "      var el = document.getElementById('status');"
           "      el.textContent = data.message || '...';"
           "      if (data.stage === 'awaiting_fingerprint_confirmation') {"
           "        if (!decisionSent) {"
           "          document.getElementById('fpValue').textContent = data.fingerprint || '';"
           "          document.getElementById('fpConfirm').style.display = 'block';"
           "        }"
           "        document.getElementById('slow').style.display = 'none';"
           "        startedAt = Date.now();"  // waiting on a human, not stuck -- don't count it
           "        setTimeout(poll, 1000);"
           "        return;"
           "      }"
           "      document.getElementById('fpConfirm').style.display = 'none';"
           "      if (data.stage === 'failed') {"
           "        el.className = 'msg err';"
           "        document.getElementById('retryLink').style.display = 'block';"
           "        return;"
           "      }"
           "      if (data.stage === 'success') {"
           "        el.className = 'msg ok';"
           "        return;"
           "      }"
           "      scheduleNext();"
           "    })"
           "    .catch(function() { clearTimeout(timeoutId); scheduleNext(); });"
           "}"
           "function scheduleNext() {"
           "  if (!slowWarned && Date.now() - startedAt > 60000) {"
           "    slowWarned = true;"
           "    document.getElementById('slow').style.display = 'block';"
           "  }"
           "  setTimeout(poll, 1000);"
           "}"
           "poll();"
           "</script></body></html>";
}

void handleRoot() {
    touchActivity();
    const ValidationState snap = snapshotValidationState();
    if (snap.stage == ValidationStage::Failed) {
        server.send(200, "text/html", renderFormPage(snap.candidate, snap.message, true));
        return;
    }
    if (snap.stage == ValidationStage::ConnectingWifi ||
        snap.stage == ValidationStage::FetchingCertificate ||
        snap.stage == ValidationStage::AwaitingFingerprintConfirmation ||
        snap.stage == ValidationStage::TestingEndpoint) {
        // A validation is already in flight (user navigated back to / or
        // reopened the page) -- keep them on the progress view instead of
        // a blank form that would invite a redundant resubmit.
        server.send(200, "text/html",
                     renderValidatingPage(i18n::langFromCode(snap.candidate.language)));
        return;
    }
    // Nothing saved/attempted yet -- the language <select>'s onchange reload
    // (?lang=...) is what lets a fresh visit switch the rendered language
    // before anything has been submitted; defaults to English per i18n.h.
    DeviceConfig blank;
    blank.language = server.hasArg("lang") ? server.arg("lang") : String("en");
    server.send(200, "text/html", renderFormPage(blank, "", false));
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
    candidate.language = server.arg("lang");
    // tlsFingerprint is never entered manually -- for https it's always
    // obtained and confirmed interactively during validation (TOFU, see
    // validationTask()).

    const Lang lang = i18n::langFromCode(candidate.language);
    const i18n::PortalStrings& ps = i18n::portal(lang);

    const bool isHttps = candidate.apiBaseUrl.startsWith("https://");
    const bool isHttp = candidate.apiBaseUrl.startsWith("http://");

    // Local format checks first -- no reason to touch the radio for a
    // typo'd URL or missing field.
    if (candidate.wifiSsid.length() == 0) {
        server.send(200, "text/html", renderFormPage(candidate, ps.errMissingSsid, true));
        return;
    }
    if (!isHttps && !isHttp) {
        server.send(200, "text/html", renderFormPage(candidate, ps.errBadUrl, true));
        return;
    }
    if (candidate.apiAuthToken.length() == 0) {
        server.send(200, "text/html", renderFormPage(candidate, ps.errMissingToken, true));
        return;
    }

    // No "save anyway" escape hatch: the config is only ever persisted
    // after live validation succeeds (including, for https, an explicit
    // human confirmation of the server's certificate) -- see
    // validationTask().
    //
    // Validation itself (WiFi connect + HTTPS test, up to ~23s combined)
    // runs on a background task -- see validationTask() -- so this
    // handler returns immediately and the phone gets progress via polling
    // instead of one long-lived request that a SoftAP channel hop could
    // drop with no way to recover (see CLAUDE.md for how that showed up
    // on real hardware).
    tryStartValidation(candidate);
    server.send(200, "text/html", renderValidatingPage(lang));
}

void handleValidateStatus() {
    touchActivity();
    const ValidationState snap = snapshotValidationState();
    const String json = String("{\"stage\":\"") + stageToJsonToken(snap.stage) + "\",\"message\":\"" +
                         jsonEscape(snap.message) + "\",\"fingerprint\":\"" +
                         jsonEscape(snap.pendingFingerprint) + "\"}";
    server.send(200, "application/json", json);
}

void handleValidateConfirm() {
    touchActivity();
    const String decision = server.arg("decision");
    if (decision == "confirm") {
        trySetFingerprintDecision(FingerprintDecision::Confirmed);
    } else if (decision == "reject") {
        trySetFingerprintDecision(FingerprintDecision::Rejected);
    }
    server.send(200, "application/json", "{\"ok\":true}");
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
    server.on("/validate-status", HTTP_GET, handleValidateStatus);
    server.on("/validate-confirm", HTTP_POST, handleValidateConfirm);
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
