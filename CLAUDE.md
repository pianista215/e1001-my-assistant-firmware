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
todo el ciclo y termina siempre en `esp_deep_sleep_start()`.

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

`platformio.ini` usa `extra_configs = secrets.ini` (ver
`secrets.ini.example`) para inyectar `WIFI_SSID`, `WIFI_PASSWORD`,
`API_BASE_URL`, `API_AUTH_TOKEN`, `TZ_STRING` como flags `-D` — quedan
disponibles como macros en cualquier `.cpp` sin incluir ningún header
generado. `secrets.ini` está gitignored; `.example` no. `config.h` tiene
`#error` guards si alguno falta.

Decisión ya tomada con el usuario: **HTTP plano** (red local/VPN), no
HTTPS — el firmware no implementa `WiFiClientSecure`. Si en algún momento
el backend pasa a tener TLS con dominio público, `display_client.cpp` es el
único sitio que necesitaría cambiar (usar `WiFiClientSecure` + verificación
de certificado en vez de `WiFiClient` plano vía `HTTPClient`).

## Estructura de módulos

| Archivo | Responsabilidad |
|---|---|
| `main.cpp` | Orquestación del ciclo completo; único sitio con `RTC_DATA_ATTR PersistentState g_state`; maneja errores/backoff |
| `config.h` | Constantes no-secretas: pines, timeouts, umbrales; valida que las secrets existan |
| `sleep_state.h` | `struct PersistentState` (caché WiFi, contador de fallos, flag de hora sincronizada) |
| `wifi_manager.{h,cpp}` | Reconexión rápida con caché BSSID/canal en RTC memory, fallback a scan completo |
| `battery.{h,cpp}` | Lectura ADC GPIO1/GPIO21 → porcentaje 1-100 |
| `rtc_pcf8563.{h,cpp}` | Driver I2C del RTC hardware |
| `display_client.{h,cpp}` | Cliente HTTP + validación estricta del formato EINK |
| `eink_driver.{h,cpp}` | Driver UC8179 raw (init, LUTs, subida de bitplanes, refresh, sleep, pantalla de error) |
| `time_scheduler.{h,cpp}` | Lógica pura de cálculo de próxima hora de despertar |

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
