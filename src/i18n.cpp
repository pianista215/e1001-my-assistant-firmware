#include "i18n.h"

namespace i18n {

const char* langCode(Lang lang) { return lang == Lang::ES ? "es" : "en"; }

Lang langFromCode(const String& code) { return code == "es" ? Lang::ES : Lang::EN; }

namespace {

// Index 0 = EN, index 1 = ES, matching static_cast<uint8_t>(Lang).

constexpr PanelStrings kPanelStrings[2] = {
    // EN
    {
        "consecutive failures: %u",
        "Set up the",
        "device",
        "1. Scan the QR code to",
        "join this WiFi network.",
        "If it doesn't connect:",
        "Name:",
        "Pass:",
        "2. If it doesn't open,",
        "visit:",
    },
    // ES (accent-free -- see i18n.h)
    {
        "fallos consecutivos: %u",
        "Configura el",
        "dispositivo",
        "1. Escanea el QR para",
        "unirte a esta red WiFi.",
        "Si no conecta solo:",
        "Red:",
        "Clave:",
        "2. Si no se abre solo,",
        "visita:",
    },
};

constexpr PortalStrings kPortalStrings[2] = {
    // EN
    {
        "Set up the device",
        "WiFi network (SSID)",
        "WiFi password",
        "Endpoint URL",
        "Token",
        "Language",
        "If the URL is https, the device will fetch the server's certificate "
        "and ask you to confirm its fingerprint during verification.",
        "Save and verify",
        "The WiFi SSID is missing.",
        "The URL must start with http:// or https://.",
        "The token is missing.",
        "Verifying...",
        "Verifying the configuration",
        "Connecting to the WiFi network...",
        "Confirm",
        "Reject",
        "This is taking longer than usual -- check that your phone is still "
        "connected to the device's hotspot.",
        "Go back and try again",
        "Could not connect to that WiFi network (wrong SSID/password or out "
        "of range).",
        "Connected to WiFi. Fetching the server's certificate...",
        "Could not obtain the server's TLS certificate",
        " (the TLS connection didn't respond -- check the URL, the port, "
        "and that it's powered on).",
        " (connected but couldn't read the server's certificate).",
        " (check that the URL starts with https://).",
        "The server's certificate is self-signed. Confirm that the "
        "fingerprint is correct.",
        "The fingerprint wasn't confirmed in time. Try again.",
        "Fingerprint rejected. Check that the URL points to the right "
        "server and try again.",
        "Connected to WiFi. Testing the server...",
        "Configuration verified. Saving and restarting...",
        "Could not start validation (not enough memory). Try again.",
    },
    // ES
    {
        "Configura el dispositivo",
        "Red WiFi (SSID)",
        "Contraseña WiFi",
        "URL del endpoint",
        "Token",
        "Idioma",
        "Si la URL es https, el dispositivo obtendrá el certificado del "
        "servidor y te pedirá que confirmes su fingerprint durante la "
        "verificación.",
        "Guardar y verificar",
        "Falta el SSID de la wifi.",
        "La URL debe empezar por http:// o https://.",
        "Falta el token.",
        "Verificando...",
        "Verificando la configuración",
        "Conectando a la red wifi...",
        "Confirmar",
        "Rechazar",
        "Esto está tardando más de lo normal -- comprueba que tu móvil "
        "sigue conectado al hotspot del dispositivo.",
        "Volver e intentar de nuevo",
        "No se pudo conectar a esa wifi (SSID/contraseña incorrectos o "
        "fuera de alcance).",
        "Conectado a la wifi. Obteniendo el certificado del servidor...",
        "No se pudo obtener el certificado TLS del servidor",
        " (no respondió la conexión TLS -- revisa la URL, el puerto y que "
        "esté encendido).",
        " (conectó pero no se pudo leer el certificado del servidor).",
        " (revisa que la URL empiece por https://).",
        "El certificado del servidor es autofirmado. Confirma que el "
        "fingerprint es correcto.",
        "No se confirmó el fingerprint a tiempo. Vuelve a intentarlo.",
        "Fingerprint rechazado. Revisa que la URL apunte al servidor "
        "correcto e inténtalo de nuevo.",
        "Conectado a la wifi. Probando el servidor...",
        "Configuración verificada. Guardando y reiniciando...",
        "No se pudo iniciar la validación (memoria insuficiente). Vuelve a "
        "intentarlo.",
    },
};

}  // namespace

const PanelStrings& panel(Lang lang) {
    return kPanelStrings[static_cast<uint8_t>(lang)];
}

const PortalStrings& portal(Lang lang) {
    return kPortalStrings[static_cast<uint8_t>(lang)];
}

String explainFetchError(Lang lang, const DisplayFetchResult& fetch) {
    const bool es = lang == Lang::ES;
    switch (fetch.error) {
        case DisplayFetchError::None:
            return "OK";
        case DisplayFetchError::HttpConnectFailed:
            return es ? "No se pudo contactar con el endpoint (revisa la URL y el puerto)."
                       : "Could not reach the endpoint (check the URL and port).";
        case DisplayFetchError::HttpTimeout:
            return es ? "El endpoint no respondió a tiempo."
                       : "The endpoint did not respond in time.";
        case DisplayFetchError::HttpUnauthorized:
            return es ? "El servidor rechazó el token (401). Revisa el token."
                       : "The server rejected the token (401). Check the token.";
        case DisplayFetchError::TlsConnectFailed:
            return es ? "No se pudo establecer una conexión TLS con el servidor."
                       : "Could not establish a TLS connection with the server.";
        case DisplayFetchError::TlsFingerprintMismatch: {
            String msg = es ? "El fingerprint no coincide con el certificado real del servidor."
                              : "The fingerprint doesn't match the server's actual certificate.";
            if (fetch.actualFingerprintHex.length() > 0) {
                msg += es ? (" El servidor presentó: " + fetch.actualFingerprintHex +
                             ". Cópialo de nuevo desde /api/v1/tls-cert.")
                           : (" The server presented: " + fetch.actualFingerprintHex +
                              ". Copy it again from /api/v1/tls-cert.");
            }
            return msg;
        }
        case DisplayFetchError::HttpStatus: {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     es ? "El servidor respondió con un error inesperado (HTTP %d)."
                         : "The server responded with an unexpected error (HTTP %d).",
                     fetch.httpStatus);
            return String(buf);
        }
        case DisplayFetchError::BadMagic:
        case DisplayFetchError::UnsupportedVersion:
        case DisplayFetchError::UnsupportedBpp:
        case DisplayFetchError::UnexpectedDimensions:
        case DisplayFetchError::TooShort:
        case DisplayFetchError::PayloadTooShort:
            return es ? "El servidor respondió, pero con un formato inesperado; revisa que la "
                        "URL apunte a este mismo backend."
                       : "The server responded, but with an unexpected format; check that the "
                        "URL points to this same backend.";
        case DisplayFetchError::ResponseTooLarge:
        case DisplayFetchError::OutOfMemory:
            return es ? "Fallo interno del dispositivo, vuelve a intentarlo."
                       : "Internal device failure, try again.";
    }
    return es ? "Error desconocido." : "Unknown error.";
}

}  // namespace i18n
