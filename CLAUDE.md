# CLAUDE.md

Contexto de arquitectura para trabajar en este repo. Ver también `README.md`
para la guía de flasheo, y el plan original en
`~/.claude/plans/la-idea-es-hacer-optimized-gem.md` si necesitas el
razonamiento completo detrás de las decisiones de diseño.

## Qué es esto

Firmware ESP32 (Seeed reTerminal E1001) que cada hora: conecta a WiFi, lee
su batería, pide una imagen ya renderizada al backend `my-assistant`
(`../my-assistant` en este mismo checkout), la pinta en el panel e-ink de
800×480 4 grises, y se vuelve a dormir. Sin `loop()` real: `setup()` hace
todo el ciclo y termina siempre en `esp_deep_sleep_start()` — con una
excepción: si no hay configuración WiFi/endpoint guardada todavía (o el
usuario la ha borrado con un gesto físico de reset), `setup()` entra en un
portal de aprovisionamiento (SoftAP + web) en vez del ciclo normal — ver
"Decisión de diseño: provisioning inicial" más abajo.

## Contrato de la API (my-assistant)

`GET {API_BASE_URL}/api/v1/display?battery=<1-100>`, header
`Authorization: Bearer <API_AUTH_TOKEN>`. Fuente de verdad:
`../my-assistant/internal/display/codec.go`.

Respuesta `application/octet-stream`, todo big-endian:

```
offset  size  campo
0       4     magic "EINK"
4       1     versión de formato (1)
5       2     ancho  (uint16 BE) -> 800
7       2     alto   (uint16 BE) -> 480
9       1     bits por píxel (2)
10      ...   píxeles empaquetados, 2 bits/píxel, 4 píxeles/byte, MSB-first,
              row-major, sin padding por fila. 0=negro, 1=gris oscuro,
              2=gris claro, 3=blanco.
```

800×480 → exactamente 96000 bytes de payload (96010 total), verificado
levantando el servidor real (`go run ./cmd/server` en `../my-assistant`) y
haciendo un `curl` real durante el desarrollo de este firmware — no es solo
lectura de código.

**La respuesta va sin `Content-Length`, con `Transfer-Encoding: chunked`**
(confirmado con `curl -D -` contra el servidor real): el `net/http` de Go
deja de poder precomputar el `Content-Length` en cuanto el handler escribe
más del buffer interno pequeño que usa para decidirlo, y un cuerpo de ~96KB
siempre lo supera. Esto se descubrió con el primer flasheo real (fallaba
con `TOO_LARGE` porque `HTTPClient::getSize()` devuelve `-1` sin
`Content-Length`). `display_client.cpp` por eso no usa `getSize()` en
absoluto: reserva un buffer de tamaño fijo (`HTTP_MAX_RESPONSE_BYTES`) en
PSRAM y usa `HTTPClient::writeToStream()` con un `Stream` propio
(`MemoryStream`) que vuelca ahí — `writeToStream()` exige un `Stream*`, no
un `Print*`, aunque solo escriba en él (de ahí los métodos de lectura vacíos
de `MemoryStream`). Decodifica chunked transparentemente, así que funciona
igual si el backend cambiara algún día
a `Content-Length` fijo.

`display_client.cpp` valida esto byte a byte antes de tocar el display
(magic, versión, bpp, dimensiones exactas, longitud de payload). Rechaza
cualquier ancho/alto distinto de 800×480: el panel físico no puede mostrar
otra resolución, así que no tiene sentido intentar soportarla.

## Decisión de diseño: driver "raw" en vez de GxEPD2/Seeed_GFX

`eink_driver.cpp` es un port directo (no un wrapper) del ejemplo oficial de
Seeed
[`examples/base/GxEPD2_reTerminal_E1001_Gray4`](https://github.com/Seeed-Projects/OSHW-reTerminal-Series-E-D/blob/main/examples/base/GxEPD2_reTerminal_E1001_Gray4/GxEPD2_reTerminal_E1001_Gray4.ino)
en `Seeed-Projects/OSHW-reTerminal-Series-E-D`. A pesar del nombre del
ejemplo, **no usa GxEPD2 ni Seeed_GFX** — es un driver UC8179 hecho a mano
sobre SPI puro.

**Por qué**: ese ejemplo empaqueta su framebuffer (`Gray4Canvas`)
exactamente igual que `codec.go` — 2bpp, 4px/byte, MSB-first, row-major, sin
padding, mismo convenio 0=negro..3=blanco. Eso significa que **el cuerpo de
la respuesta HTTP se puede pasar directamente** a la rutina de subida de
bitplanes del panel (`eink::drawFrame`), sin decodificar a un framebuffer
intermedio ni arrastrar una librería grande (GxEPD2/TFT_eSPI) solo para
"un refresco de pantalla completa por hora". Las tablas LUT de escala de
grises y la secuencia de comandos UC8179 (`0x01` power, `0x30` PLL, `0x82`
VCOM, `0x06` booster, `0x04` power-on, `0x00` panel setting, `0x61`
resolución, `0x20-0x24` LUTs, `0x10`/`0x13` DTM1/DTM2, `0x12` refresh,
`0x02`/`0x07` sleep) están copiadas verbatim del ejemplo de Seeed — son
datos de calibración opacos, no algo para derivar a mano.

**Diferencia respecto al vendor**: el ejemplo original no tiene timeout en
la espera del pin BUSY (`checkBusy()` puede colgarse para siempre). Aquí
`waitBusy()` tiene un timeout acotado (`EINK_BUSY_TIMEOUT_MS`, 15s) — un
panel atascado no puede dejar el dispositivo colgado consumiendo batería
indefinidamente.

`BOARD_SCREEN_COMBO = 520` (UC8179, 800×480) es la constante que Seeed usa
en todos sus ejemplos oficiales para identificar este panel concreto.

## Decisión de diseño: hora — RTC (PCF8563) + corrección SNTP

El E1001 trae un RTC hardware **PCF8563** (I2C 0x51, SCL=GPIO20,
SDA=GPIO19) con pila botón propia, independiente del RTC interno del ESP32
(que solo sobrevive *deep sleep*, no un reset/EN completo o pérdida total de
alimentación).

Enfoque (`main.cpp` + `rtc_pcf8563.cpp`):
1. Al arrancar: leer PCF8563 → `settimeofday()` inmediatamente (rápido, sin
   red). Si el flag VL (voltage-low) está activo, la hora no es fiable y se
   ignora.
2. Tras conectar WiFi: intentar SNTP (`configTzTime` + `getLocalTime`,
   timeout corto). Si funciona, es autoritativo — se re-escribe al PCF8563
   para que el drift no se acumule ciclo a ciclo.
3. Si SNTP falla pero el PCF8563 dio hora válida este ciclo, o si ya se
   sincronizó alguna vez desde el último power-on (el reloj del sistema del
   ESP32 sigue avanzando solo entre ciclos de deep sleep), se usa
   `time(nullptr)`.
4. Si nunca hay una hora fiable (primer arranque real, pila del RTC recién
   puesta): no se intenta calcular una franja horaria con un reloj
   desconocido — se duerme un intervalo corto fijo (`FIRST_BOOT_RETRY_SLEEP_SEC`)
   hasta que un SNTP tenga éxito.

**Por qué no solo SNTP**: el PCF8563 permite tener una hora razonable
incluso sin red (o antes de que WiFi conecte), y sobrevive a una pérdida
total de alimentación, cosa que ni el RTC interno del ESP32 ni cualquier
estado en RAM pueden hacer.

**Por qué no solo el PCF8563**: es más simple confiar en que SNTP corrige
el drift cada vez que hay red, en vez de implementar lógica de "cuánto ha
pasado desde la última sincronización" para decidir cuándo re-sincronizar.

## Decisión de diseño: botón físico de refresco manual

`main.cpp` habilita `esp_sleep_enable_ext1_wakeup()` sobre `PIN_WAKE_BUTTON`
(GPIO3, el botón "KEY0" que usa el propio ejemplo `LowPower_DeepSleep.ino`
de Seeed) **además** del timer, dentro de `goToSleep()` — es decir, en
*todos* los caminos de sueño (ciclo normal, backoff de errores, primer
arranque), no solo el éxito. El ESP32 permite varias fuentes de wakeup
activas a la vez; la que ocurra primero gana. Esto permite forzar un
refresco inmediato (o reintentar tras arreglar WiFi/config) sin esperar al
temporizador. El ciclo que se ejecuta al despertar es idéntico venga de
donde venga — solo cambia el log de `wakeupCauseString()` al inicio.

Desde el aprovisionamiento inalámbrico (ver la sección correspondiente más
abajo), esto tiene una excepción: si el dispositivo ya está configurado y
despierta por este botón, `main.cpp` espera hasta `RESET_HOLD_MS` (10s)
antes de lanzar el refresco, por si la pulsación es en realidad el gesto
de reset. Una pulsación corta sigue comportándose exactamente igual que
antes; solo se añade ese retraso de comprobación.

**Impacto en consumo**: mínimo. El propio dato de ~14µA que reporta Seeed
para esta familia de placas viene de ese mismo ejemplo, que ya tiene el
wakeup por botón activado — no es una cifra "solo timer" a la que esto le
suma algo nuevo.

## Algoritmo de sueño (`time_scheduler.cpp`)

Lógica pura sobre `struct tm`, sin dependencias de Arduino — testeable con
`pio test -e native` sin hardware. Verificado además con un arnés ad-hoc
compilado con `g++` directo (sin PlatformIO instalado) durante el
desarrollo, incluyendo el caso de cruce de año.

`computeNextWake(now)`:
1. `target = now` con minutos/segundos a 0, `+1` hora, normalizar con
   `mktime`/`localtime_r` (maneja overflow de día/mes/año y DST).
2. Si `target.tm_hour` cae en `[1,5]`, saltar a `tm_hour = 6` directamente
   (evita refrescos entre la 1 y las 5 de la madrugada).
3. `sleepSeconds = mktime(target) - mktime(now)`.

Se recalcula desde el "ahora" real cada vez — no hay contador acumulativo —
así que se autocorrige aunque el ciclo anterior se despertara unos segundos
tarde/pronto, o aunque el primer arranque no caiga justo en hora en punto.

## Config por dispositivo

Solo `TZ_STRING` (y el flag opcional de desarrollo
`DEBUG_SLEEP_OVERRIDE_SEC`) siguen siendo compile-time: `platformio.ini`
usa `extra_configs = secrets.ini` (ver `secrets.ini.example`) para
inyectarlos como flags `-D`. `secrets.ini` está gitignored; `.example` no.
`config.h` tiene un `#error` guard solo para `TZ_STRING`.

WiFi (SSID/contraseña), la URL del endpoint, el token de autenticación y
(si el endpoint es HTTPS) el fingerprint SHA-256 del certificado ya **no**
son secretos de compilación — se introducen una vez a través del portal de
aprovisionamiento inalámbrico (ver la siguiente sección) y se guardan en
NVS vía `Preferences` (`device_config.{h,cpp}`, namespace `"e1001cfg"`),
que sobrevive a un power-on/EN reset real (a diferencia del
`RTC_DATA_ATTR PersistentState g_state` de `sleep_state.h`, que solo
sobrevive deep sleep). `main.cpp` carga esa config con
`device_config::load()` al principio de cada ciclo normal.

El endpoint puede ser `http://` (red local/VPN, sin TLS) o `https://` (cert
autofirmado del backend `my-assistant`, con fingerprint pinning — ver
`display_client.cpp`). Ya no hay una decisión fija de "solo HTTP": el
scheme de la URL guardada decide el transporte en cada petición.

## Decisión de diseño: provisioning inicial (SoftAP + QR + fingerprint TLS)

Sin config guardada (`device_config::isConfigured()` false — primer
arranque real, o tras el gesto de reset descrito más abajo),
`setup_portal::run()` toma el control de `setup()` y **no vuelve**: es el
único sitio del firmware donde hay un bucle activo con el radio encendido
en vez de terminar en deep sleep inmediatamente.

Flujo: `WiFi.mode(WIFI_AP_STA)` (nunca `WIFI_AP` a secas — hace falta STA
simultáneamente para la validación en vivo, ver abajo) → `WiFi.softAP()`
con SSID `E1001-Setup-<hex chip id>` y contraseña WPA2 de 12 caracteres,
ambos derivados deterministamente de `ESP.getEfuseMac()` (mismo QR en
cada reintento, no cambia entre ciclos) → `DNSServer` en modo catch-all
(`dns.start(53, "*", WiFi.softAPIP())`) + `WebServer` respondiendo 302 a
`/` en las rutas de sondeo de captive portal de Apple/Android/Windows/
Firefox (`/hotspot-detect.html`, `/generate_204`, `/ncsi.txt`, etc.) para
que el móvil muestre el popup de "unirse a la red" automáticamente → se
pinta **una vez** `eink::drawProvisioningScreen()` (QR `WIFI:T:WPA;S:...;
P:...;;` para autoconexión desde la cámara, más SSID/contraseña/URL como
fallback en texto) → bucle `dnsServer.processNextRequest()` +
`server.handleClient()` hasta que el usuario complete el formulario o
pasen `PORTAL_INACTIVITY_TIMEOUT_MS` (10 min) sin actividad, en cuyo caso
se apaga todo y `goToSleep(PORTAL_RETRY_SLEEP_SEC)` — al despertar,
`isConfigured()` sigue en `false` así que se reentra en el portal solo,
sin lógica extra en `main.cpp`.

**Validación en vivo antes de guardar** (`setup_portal.cpp`,
`handleSave()`): con el AP ya activo, conecta la STA a la wifi candidata
(`wifiBeginConnect`/`wifiWaitConnected` con caché vacía) y, si conecta,
llama a la **misma** `fetchDisplayBuffer()` de producción con
`battery=50` de prueba — cero lógica HTTP duplicada entre el ciclo normal
y la validación del portal. Cada tipo de fallo (wifi, TLS, fingerprint no
coincide, 401, otro HTTP status, formato de respuesta inesperado) se
traduce a un mensaje específico en la web, con el formulario prerellenado
para corregir sin perder el hotspot. Un checkbox "guardar de todas formas"
permite saltarse la validación como escape hatch secundario. Importante:
durante esta validación nunca se llama a `wifiDisconnect()` (que hace
`WiFi.mode(WIFI_OFF)` y tiraría también el AP) — se usa
`WiFi.disconnect(false)` para soltar solo el lado STA.

**Hallazgo real con el dispositivo (validación bloqueaba el portal)**: en
`WIFI_AP_STA`, el ESP32 solo puede tener el AP y la STA en el mismo canal
radio — en cuanto la STA se asocia a la wifi candidata, si está en un
canal distinto al que el AP usaba con el móvil, el chip fuerza al AP a
saltar a ese canal, y el móvil sufre una microdesconexión/reasociación en
ese instante. La primera versión de `handleSave()` era totalmente
síncrona (hasta ~23s bloqueando el único hilo de `WebServer`: 15s de
`wifiWaitConnected` + 8s de `fetchDisplayBuffer`), así que si ese salto de
canal coincidía con la única petición `POST /save` en curso, esa petición
se perdía sin más reintento posible y la página se quedaba con el
spinner sin reaccionar — confirmado por el usuario en pruebas reales
("se ha quedado pillado el portal sin reaccionar a los botones"; el
dispositivo en sí no se colgaba, solo esa respuesta HTTP concreta nunca
llegaba). Arreglado moviendo la validación a una tarea FreeRTOS en segundo
plano (`validationTask()`, `xTaskCreate` con 16384 bytes de stack —
el doble del stack de 8192 bytes con el que `loopTask` ya ejecuta este
mismo `fetchDisplayBuffer()` con su handshake TLS sin problema — y
prioridad 1, igual que `loopTask`), dejando `run()` libre para seguir
atendiendo `dnsServer.processNextRequest()`/`server.handleClient()` sin
bloqueos largos. Un estado compartido (`ValidationStage` +
`ValidationState`, protegido con un spinlock `portMUX_TYPE`, ya que se
lee/escribe desde la tarea de fondo y desde los handlers HTTP del loop
principal) expone `Idle → ConnectingWifi → TestingEndpoint →
Success`/`Failed`; `handleSave()` ahora responde al instante con una
página que hace polling a `GET /validate-status` (JSON) cada ~1s vía
`fetch()` con su propio `AbortController` de 4s (sin esto, un poll que se
quede a medias por el mismo salto de canal reproduciría el mismo síntoma
de "colgado", ahora en el JS en vez de en el servidor) — si un poll falla,
simplemente se reintenta en el siguiente ciclo en vez de mostrarse como
error, que es la mejora de resiliencia real frente al diseño síncrono
anterior. Invariante importante: `validationTask()` nunca toca el objeto
`WebServer` (no es thread-safe), solo el estado protegido por el mutex.
`tryStartValidation()` hace un check-and-set atómico para que un
doble-tap en "Guardar" no lance dos tareas compitiendo por el WiFi a la
vez. `handleRoot()` usa el mismo estado para prerellenar el formulario
tras un fallo, o para devolver la página de progreso si el usuario
navega a `/` con una validación ya en curso.

**Por qué `wifiBeginConnect()` ya no fija `WiFi.mode()` internamente**:
antes ponía `WIFI_STA` incondicionalmente; si el portal la reutilizara tal
cual, cada intento de validación tiraría el SoftAP al que el móvil del
usuario está conectado en ese momento. Ahora el modo lo decide quien
llama: `main.cpp` pone `WIFI_STA` antes de llamar (ciclo normal),
`setup_portal.cpp` ya está en `WIFI_AP_STA` desde `run()` y no lo vuelve a
tocar.

**Fingerprint TLS**: `display_client.cpp` conecta con `WiFiClientSecure`
usando `setInsecure()` (sin cadena de CA — el pinning por fingerprint
reemplaza la verificación de cadena) y `.verify(fingerprintHex, host)`
**antes** de `http.begin(secureClient, url)`; `HTTPClient::connect()`
reutiliza un cliente ya conectado (visto en el `.cpp` del core instalado),
así que no hay un segundo handshake sin verificar. El fingerprint que
pega el usuario se normaliza (`normalizeFingerprint()`: quita `:` y
espacios, pasa a mayúsculas) antes de guardarse y de usarse — el parser de
`verify_ssl_fingerprint()` del core Arduino-ESP32 2.0.17 tolera tanto
`:` como ausencia de separadores, así que no hace falta reconvertir. El
backend `my-assistant` ya expone este mismo fingerprint (formato
`openssl x509 -fingerprint -sha256`) sin autenticación en
`GET /api/v1/tls-cert`, pensado exactamente para copiarlo desde el móvil
durante el setup.

**Hallazgo real con el dispositivo (primer flasheo con endpoint HTTPS)**:
`.verify(fingerprint, host)` de este core encadena dos comprobaciones —
primero el fingerprint, y si coincide, además que `host` aparezca en los
SAN/CN del certificado (`verify_ssl_dn()` en `ssl_client.cpp`). Esa segunda
comprobación trata **todas** las entradas SAN como texto ASCII sin mirar
su tipo ASN.1; para un SAN de tipo `iPAddress` (justo lo que genera el
propio `my-assistant --https` para las IPs locales, ver
`generateSelfSignedCert()` en `cmd/server/tls.go`) el contenido son 4
bytes binarios de la IP, no el string `"192.168.x.x"` — así que la
comprobación de host **siempre falla** para un endpoint por IP, aunque el
fingerprint copiado sea perfecto, y `fetchDisplayBuffer()` lo reportaba
como `TLS_FINGERPRINT` sin distinguir cuál de las dos comprobaciones había
fallado. Confirmado con un log real: `Display fetch failed: TLS_FINGERPRINT
(http=0)` con el fingerprint copiado tal cual desde `/api/v1/tls-cert`.
Arreglado pasando `nullptr` como `domain_name` (`display_client.cpp`) —
el propio código del core contempla ese caso (`if (domain_name) ... else
return true;`) y omite la comprobación de host, que es redundante de
todas formas: el pinning por fingerprint ya autentica el certificado
exacto, una garantía más fuerte que comprobar un nombre/IP. Además,
`DisplayFetchResult::actualFingerprintHex` ahora se rellena (vía
`WiFiClientSecure::getFingerprintSHA256()`) cuando sí hay un mismatch real
de fingerprint, y tanto `main.cpp` como el mensaje de error del portal
(`setup_portal.cpp`) lo muestran junto al esperado, para diagnosticar sin
ambigüedad si alguna vez vuelve a fallar de verdad.

**Gesto de reset**: además del comportamiento ya descrito del botón KEY0
(fuerza un ciclo inmediato), si el dispositivo **ya está configurado** y
despierta por `ESP_SLEEP_WAKEUP_EXT1`, `main.cpp` no lanza el ciclo normal
de inmediato — primero hace polling de `digitalRead(PIN_WAKE_BUTTON)`
durante `RESET_HOLD_MS` (10s). Si se suelta antes, es una pulsación normal
y cae al ciclo de refresco manual de siempre, sin cambios. Si sigue
pulsado los 10s completos, se interpreta como el gesto de reset:
`device_config::clear()` + `ESP.restart()`, que reinicia directamente en
`setup_portal::run()` (config ya vacía). El usuario confirmó que aunque el
chasis tiene otros dos botones físicos, deliberadamente no se usan para
este gesto — es solo KEY0 mantenido.

## Estructura de módulos

| Archivo | Responsabilidad |
|---|---|
| `main.cpp` | Orquestación del ciclo completo; único sitio con `RTC_DATA_ATTR PersistentState g_state`; gesto de reset por hold del botón; maneja errores/backoff |
| `config.h` | Constantes no-secretas: pines, timeouts, umbrales, constantes de aprovisionamiento; valida que `TZ_STRING` exista |
| `sleep_state.h` | `struct PersistentState` (caché WiFi, contador de fallos, flag de hora sincronizada) — RTC memory, se borra en power-on/EN reset |
| `sleep_control.{h,cpp}` | `goToSleep()` (timer + EXT1 wakeup, pull-up RTC) — compartido por `main.cpp` y `setup_portal.cpp` |
| `device_config.{h,cpp}` | `struct DeviceConfig` (wifi, endpoint, token, fingerprint) sobre `Preferences`/NVS — sobrevive power-on/EN reset, a diferencia de `sleep_state.h` |
| `setup_portal.{h,cpp}` | SoftAP + captive portal (DNSServer + WebServer) + validación en vivo + orquestación del modo aprovisionamiento; único sitio con un bucle activo (no termina en deep sleep de inmediato) |
| `wifi_manager.{h,cpp}` | Reconexión rápida con caché BSSID/canal en RTC memory, fallback a scan completo; ya no fija `WiFi.mode()` internamente |
| `battery.{h,cpp}` | Lectura ADC GPIO1/GPIO21 (promedio de 8 muestras) → porcentaje 1-100 |
| `rtc_pcf8563.{h,cpp}` | Driver I2C del RTC hardware |
| `display_client.{h,cpp}` | Cliente HTTP(S) + fingerprint pinning + validación estricta del formato EINK; reutilizado tal cual por la validación en vivo del portal |
| `eink_driver.{h,cpp}` | Driver UC8179 raw (init, LUTs, subida de bitplanes, refresh, sleep, pantalla de error, pantalla de aprovisionamiento con QR) |
| `time_scheduler.{h,cpp}` | Lógica pura de cálculo de próxima hora de despertar |

## Decisión de diseño: la batería se lee antes de tocar el WiFi

`main.cpp` llama a `readBatteryPercent()` **antes** de `wifiBeginConnect()`,
no en paralelo con la negociación WiFi como en una versión anterior. La
ráfaga de corriente que consume la radio al asociarse hunde momentáneamente
el raíl de la batería (caída por resistencia interna), y muestrear el ADC
justo en ese momento da una lectura de voltaje más baja de lo real —
suficiente para que el porcentaje reportado salte varios puntos entre
ciclos sin que la batería haya perdido esa energía de verdad. `battery.cpp`
además promedia 8 muestras del ADC para suavizar el ruido de una sola
lectura. El coste en tiempo despierto de leer secuencial en vez de en
paralelo es de pocos milisegundos — irrelevante frente a la precisión que
gana el reporte de batería.

## Manejo de errores

Nunca se reintenta en bucle activo con el radio encendido. Fallos de WiFi,
HTTP o respuesta corrupta incrementan `g_state.consecutiveFailures` (en RTC
memory) y aplican backoff exponencial acotado
(`BACKOFF_BASE_SEC << failures`, tope `BACKOFF_MAX_SEC`). Solo se dibuja una
pantalla de error (`eink::drawErrorScreen`) tras
`ERROR_SCREEN_AFTER_N_FAILURES` fallos consecutivos, para no gastar
refrescos de e-ink en blips transitorios — mientras tanto el panel
simplemente mantiene la última imagen buena (el e-ink no consume energía
por quedarse quieto, así que es el mejor fallback posible). El contador se
resetea en cualquier ciclo exitoso.

## Qué está verificado vs. qué falta por comprobar con el dispositivo físico

**Verificado durante el desarrollo** (sin tener el dispositivo en mano):
- El formato binario exacto, levantando `go run ./cmd/server` real y
  haciendo `curl` real contra `/api/v1/display` (800×480, 96010 bytes,
  magic/versión/bpp correctos, payload exacto).
- Los códigos de error HTTP reales del backend (401 sin token, 400 sin
  `battery`).
- La lógica de `time_scheduler` compilada y ejecutada de verdad (casos de
  cada hora 0-23, salto 01-05→06, arranque a hora arbitraria, cruce de año).
- Los pines, secuencia de comandos UC8179, patrón de batería y registros
  del PCF8563 vienen de ejemplos de Seeed confirmados como funcionales
  (`examples/base/GxEPD2_reTerminal_E1001_Gray4`, `Battery_Monitor.ino`,
  `RTC_PCF8563.ino`, `LowPower_DeepSleep.ino` en
  `Seeed-Projects/OSHW-reTerminal-Series-E-D`), leídos directamente del
  repositorio, no de memoria.
- El aprovisionamiento inalámbrico (SoftAP + QR + fingerprint TLS)
  compila limpio para `reterminal_e1001` con la librería `ricmoo/QRCode`
  añadida, y los tests de `time_scheduler` en `pio test -e native` siguen
  pasando (10/10; hizo falta añadir `test_build_src = true` al `[env:native]`
  de `platformio.ini`, una brecha de configuración preexistente sin
  relación con esta feature — sin ese flag, `pio test` no enlazaba
  `time_scheduler.cpp` con el binario de test). La firma real de
  `WiFiClientSecure::verify()`/`verify_ssl_fingerprint()` del core
  Arduino-ESP32 2.0.17 instalado se confirmó leyendo
  `ssl_client.cpp`/`.h` directamente: acepta el fingerprint con o sin
  `:` y espacios, así que `normalizeFingerprint()` no necesita
  reintroducir separadores. También se confirmó leyendo
  `HTTPClient.cpp` que `HTTPClient::connect()` reutiliza un `Client`
  que ya está conectado en vez de reconectar, que es lo que permite
  conectar+verificar el fingerprint manualmente antes de
  `http.begin(secureClient, url)` sin un segundo handshake sin
  verificar por medio.

**Pendiente de verificar con el hardware real** (no se puede comprobar sin
el dispositivo):
- Que el driver UC8179 portado realmente pinta bien en esta unidad física
  concreta — probar primero el patrón de demo del propio ejemplo de Seeed
  (no incluido aquí, está en su repo) antes de confiar en contenido real de
  la API.
- Polaridad/timing real del pin BUSY y duración real de un refresco
  completo (para ajustar `EINK_BUSY_TIMEOUT_MS` si hiciera falta).
- Tamaño de flash real (`esptool.py flash_id`) — la wiki de Seeed dice
  32MB pero el board definition de PlatformIO para `seeed_xiao_esp32s3`
  asume 8MB; con `default_8MB.csv` funciona igual, solo desaprovecha
  espacio si el real es mayor.
- Tiempos reales de reconexión WiFi con caché de BSSID/canal (¿realmente
  sub-segundo como reporta la comunidad para ESP32 Arduino?).
- Que `HTTPClient::setConnectTimeout()` existe tal cual en la versión del
  core ESP32-Arduino que resuelva PlatformIO al compilar (método presente
  en versiones recientes; si la build falla aquí, es la primera señal a
  mirar).
- Consumo real en deep sleep (~14µA reportado por Seeed para esta familia
  de placas, no medido aquí).
- Que el botón físico (GPIO3, "KEY0") despierte realmente el dispositivo en
  esta unidad concreta y sea el botón que uno espera al mirar la carcasa —
  la polaridad (`ANY_LOW`) y el pull-up del dominio RTC vienen del ejemplo
  de Seeed, no verificados en este hardware todavía.
- Que el móvil dispare de verdad el popup de "unirse a la red" con las
  rutas de captive portal implementadas en `setup_portal.cpp` — el
  comportamiento exacto varía por versión de iOS/Android/Windows y no es
  100% predecible desde el código; como fallback siempre está la URL
  `http://192.168.4.1/` mostrada en el panel.
- Legibilidad real del QR de aprovisionamiento en el panel de 4 grises
  (contraste, tamaño de módulo) — `QR_VERSION`/el tamaño de caja en
  `eink_driver.cpp` están dimensionados con margen sobre el payload
  esperado, pero no se ha escaneado un QR real desde este panel.
- Que el polling de `/validate-status` (con el `AbortController` de 4s y
  reintento cada ~1s) efectivamente absorba el salto de canal del AP en
  `WIFI_AP_STA` sin que el usuario tenga que recargar manualmente la
  página — el salto de canal en sí ya está confirmado que ocurre en
  hardware real (ver "Hallazgo real" arriba); lo que falta por confirmar
  es que la mitigación async lo haga imperceptible en la práctica y no
  solo en el diseño.
- Consumo real de batería con el SoftAP + `DNSServer` + `WebServer`
  activos durante varios minutos, para calibrar
  `PORTAL_INACTIVITY_TIMEOUT_MS`/`PORTAL_RETRY_SLEEP_SEC` con datos reales
  en vez de una estimación.
- Fiabilidad del gesto de reset de 10s con el pull-up normal
  (`INPUT_PULLUP`) que se reconfigura nada más despertar, frente al
  pull-up del dominio RTC usado durante el propio sueño.
- Que `WiFiClientSecure::verify()` rechace de verdad un fingerprint
  incorrecto (no solo que acepte uno correcto, ya confirmado en hardware
  real tras el fix del `domain_name` descrito arriba) sin colgarse en el
  handshake TLS.
- Que `ESP.restart()` (usado tanto al guardar la config como al aplicar el
  gesto de reset) preserve `RTC_DATA_ATTR g_state` en esta unidad concreta
  — documentado así en ESP-IDF (solo un power-on/EN real lo resetea), no
  confirmado empíricamente en este hardware todavía.
