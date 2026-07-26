# e1001-my-assistant-firmware

Firmware para el [Seeed reTerminal E1001](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) (panel e-ink de 800×480, 4 niveles de gris) que cada hora:

1. Se conecta a WiFi lo más rápido posible.
2. Lee su propia batería.
3. Pide la imagen a pintar al backend [`my-assistant`](../my-assistant) (`GET /api/v1/display?battery=<pct>`).
4. La pinta en el e-ink.
5. Se duerme y repite — 1 hora normalmente, saltando directamente de las 00:00 a las 06:00 (los refrescos de la 1 a las 5 de la madrugada no aportan nada).

Ver [`CLAUDE.md`](CLAUDE.md) para la arquitectura interna, decisiones de diseño y qué queda por verificar con el hardware físico.

## Qué necesitas

- El dispositivo reTerminal E1001, con su cable **USB-C**.
- Un portátil Linux (esta guía asume eso).
- [PlatformIO Core (CLI)](https://platformio.org/install/cli) — no hace falta la IDE de Arduino ni VSCode, aunque puedes usar la extensión de VSCode si prefieres GUI.
- El backend `my-assistant` corriendo en algún sitio accesible por HTTP desde la red donde estará el E1001 (misma LAN o VPN tipo Tailscale — este firmware asume HTTP simple, no HTTPS).

## 1. Instalar PlatformIO

```bash
python3 -m venv ~/.platformio-venv
source ~/.platformio-venv/bin/activate
pip install platformio
pio --version
```

(Puedes usar `pipx install platformio` si lo prefieres. Cualquiera de los dos métodos funciona; lo importante es tener el comando `pio` disponible.)

La primera vez que compiles, PlatformIO descargará el toolchain de Espressif (compilador, SDK, etc.) — son varios cientos de MB, tarda unos minutos y solo pasa una vez.

## 2. Preparar el puerto serie (una sola vez)

El USB-C del E1001 va por un chip **CH340** (puente USB↔serie), no por el USB nativo del ESP32-S3. En Linux el driver ya viene en el kernel, pero tu usuario necesita permiso para usar el puerto:

```bash
sudo usermod -aG dialout $USER
```

Cierra sesión y vuelve a entrar (o reinicia) para que el cambio de grupo tenga efecto. Sin esto, verás un error de permisos al flashear.

## 3. Configurar tu dispositivo

```bash
cp secrets.ini.example secrets.ini
```

Edita `secrets.ini` con:

- `WIFI_SSID` / `WIFI_PASSWORD`: tu red WiFi (2.4GHz — el ESP32-S3 no tiene 5GHz).
- `API_BASE_URL`: URL base de tu servidor `my-assistant`, **sin barra final** (p. ej. `http://192.168.1.50:8080`).
- `API_AUTH_TOKEN`: el mismo `AUTH_TOKEN` que tiene configurado el servidor (`my-assistant/.env`).
- `TZ_STRING`: zona horaria en formato POSIX. Por defecto trae `Europe/Madrid` (`CET-1CEST,M3.5.0,M10.5.0/3`), que maneja el cambio de horario de verano solo.

`secrets.ini` está en `.gitignore` — nunca se sube al repositorio. `secrets.ini.example` sí, como plantilla.

## 4. Probar contra el servidor local antes de flashear

Antes de apuntar al servidor de producción, comprueba que todo el formato es el esperado usando el propio backend en local (ver el README de `my-assistant`):

```bash
cd ../my-assistant
cp .env.example .env   # si no lo tienes ya configurado
go run ./cmd/server
```

En otra terminal, comprueba que responde como se espera:

```bash
curl -H "Authorization: Bearer $AUTH_TOKEN" "http://localhost:8080/api/v1/display?battery=87" -o buffer.bin
go run ./cmd/preview --file buffer.bin --open   # abre un PNG con lo que vería el panel
```

Si quieres apuntar el firmware a este servidor local durante las primeras pruebas, pon en `secrets.ini` la IP de tu portátil en la red local (no `localhost`, porque quien hace la petición es el E1001, no tu portátil): `API_BASE_URL='"http://<IP-DE-TU-PORTATIL>:8080"'`.

## 5. Compilar y flashear

Conecta el E1001 por USB-C y ejecuta:

```bash
pio run -e reterminal_e1001 -t upload
```

PlatformIO detecta el puerto automáticamente (normalmente `/dev/ttyUSB0`). Si tienes más de un dispositivo serie conectado y falla la autodetección, indícalo explícitamente:

```bash
pio run -e reterminal_e1001 -t upload --upload-port /dev/ttyUSB0
```

## 6. Ver los logs

La consola de depuración va por el mismo puente CH340 (`Serial1`, no el USB-CDC nativo):

```bash
pio device monitor -b 115200
```

Deberías ver algo como:

```
[MAIN] Wake cause: 0
[MAIN] Battery: 87%
[MAIN] WiFi connected.
[MAIN] Cycle OK.
[MAIN] Sleeping for 3600 s
```

Si no ves nada en absoluto (ni al reiniciar la placa), revisa la sección de troubleshooting.

## Pruebas sin gastar refrescos del panel

`time_scheduler` (la lógica de "cuándo despertar") es lógica pura, sin dependencias de Arduino, y tiene tests que corren en tu portátil sin ningún hardware:

```bash
pio test -e native
```

(Necesita `secrets.ini` creado igualmente — `platformio.ini` lo referencia siempre, aunque el entorno `native` no use ninguno de esos valores.)

## Troubleshooting

- **No aparece ningún `/dev/ttyUSB*` al conectar el cable**: prueba otro cable USB-C (algunos son solo de carga, sin líneas de datos) y comprueba `dmesg | tail` tras conectar — deberías ver algo mencionando `ch341`.
- **`pio run -t upload` falla con "Permission denied" en el puerto**: te falta el grupo `dialout` (paso 2) o no has vuelto a iniciar sesión tras añadirte.
- **`pio device monitor` no muestra nada**: confirma que `platformio.ini` tiene `-D ARDUINO_USB_CDC_ON_BOOT=0` en `build_flags` (ya viene así) — sin ese flag, el firmware escribiría por el USB nativo del ESP32-S3, que en esta placa no está conectado a ningún cable.
- **El panel no pinta nada / se queda colgado**: mira los logs por `pio device monitor` — el driver del e-ink tiene un timeout de 15s en la espera del pin BUSY, así que un fallo de panel se ve como un log de error, no como un cuelgue silencioso.
- **`400`/`401` en los logs al pedir la imagen**: revisa que `API_AUTH_TOKEN` en `secrets.ini` coincide exactamente con `AUTH_TOKEN` en el `.env` del servidor.
