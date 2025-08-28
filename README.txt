otvori ESP-IDF 5.5 PowerShell
i pokreni sa
idf.py build
idf-py -p COM8 flash monitor

ukoliko je program već uploadan 
idf-py -p COM8 monitor

27.08.


program započinje sa wifi ap kako bi se esp prijavio na internet. nakon toga gasi ap i kreće sa primanjem poruka svakih 3000 ms

28-08

update: 
wifi ap se aktivira kako bi se esp prijavio na internet. nakon toga gasi ap i kreće sa primanjem poruka svakih 3000 ms (MOŽE SE MIJENJATI)
nakon gašenja wifi credentials ostaju spremljeni, reset wifi credentialsa za ponovno povezivanje odradi se sa spajanjem GPIO-0 sa GND > 3 sec

Project overview

Your ESP32 “gateway” firmware does three big things:

Wi-Fi onboarding & persistence (in WifiManagerCustom.c)

If no saved Wi-Fi, it boots a captive-style setup AP (open network) and serves a tiny web form where you enter SSID/password.

When you save, it writes creds to NVS, switches from AP→AP+STA→STA, and connects.

Creds are kept across power cycles. You can wipe them in two ways:

Double-reset latch: two resets within 5 seconds → erase SSID/PASS in NVS → reboot into setup AP.

GPIO0 (BOOT) hold: hold the BOOT button low for ~3 seconds at power-up → erase SSID/PASS → reboot.

Polling your cloud endpoint (in main.c)

When connected to Wi-Fi, it HTTPS GETs your “latest command” URL (API Gateway/Lambda/S3) every ~3s.

It parses the JSON command, de-duplicates repeated commands, and forwards it to the mesh layer (currently a stub/log).

Optional: it can POST status to another URL if you configure it.

Safety & TLS

Uses the ESP-IDF certificate bundle for TLS (no custom cert files).

All HTTP(S) operations handle Content-Length or chunked responses; buffers grow dynamically.

File-by-file
main/main.c
Includes & config

Standard FreeRTOS / ESP-IDF headers.

esp_http_client.h for HTTPS.

cJSON for JSON parsing.

"WifiManagerCustom.h" to start Wi-Fi manager and check connectivity.

Optional esp_crt_bundle.h if MBEDTLS Certificate Bundle is enabled.

Kconfig bindings (set via menuconfig):

CONFIG_GW_API_KEY → GW_API_KEY (adds x-api-key header to your API calls)

CONFIG_GW_DEVICE_ID → GW_DEVICE_ID (gateway self-ID used in status JSON)

CONFIG_GW_URL_LATEST → GW_URL_LATEST (polling GET URL)

CONFIG_GW_URL_STATUS → GW_URL_STATUS (optional POST URL; empty string disables POSTs)

Utility: de-duplication

FNV-1a 32-bit hash + hex32() provides a stable ID if the incoming JSON doesn’t have a commandId.

The last processed command ID is kept in s_last_cmd_id; if the next one matches, it’s ignored.

HTTP helpers

http_get(url, api_key, &out):

Configures HTTPS with .crt_bundle_attach = esp_crt_bundle_attach (if enabled).

Adds x-api-key when present.

Opens, reads chunked or sized responses into a malloc’d buffer, returns it NUL-terminated.

Returns error if status != 200; logs code and short body preview.

http_post_json(url, api_key, json, &status):

Skips entirely if url is empty (so you can “disable” reporting via Kconfig).

Adds Content-Type: application/json and optional x-api-key.

Logs non-2xx status.

Command parsing

parse_command_json(const char *json) → gw_cmd_t:

Command ID: uses commandId if present; otherwise hashes the whole payload (dedup safe).

Target: accepts any of deviceId, targetId, nodeId. Defaults to "all" if missing.

Command: recognizes "on" / "off" (or led_on / led_off). If omitted but color/brightness supplied → assume on.

Brightness: accepts 0..255 or 0..100 (percent mapped to 0..255).

Color: accepts "#RRGGBB" and fills r/g/b. Defaults to white (255/255/255) if absent.

Sets .valid when it has a usable command.

Forwarding stub & (optional) reporting

forward_to_mesh_stub() currently just logs: target, ON/OFF, R/G/B, brightness, commandId. Replace this with your BLE Mesh publish/send.

send_status() builds a JSON like:

{"deviceId":"<GW>","targetId":"<device>","commandId":"<id>","status":"received|executed|error","ts":<ms>,"error":"...optional..."}


and POSTs it to GW_URL_STATUS (if configured).

Polling task

poll_task() runs forever:

Only does work if wifi_manager_is_connected() is true.

GETs GW_URL_LATEST, logs latest-command: <payload> on success.

Parses, de-dups, then:

send_status(...,"received")

forward_to_mesh_stub(...)

send_status(...,"executed")

Sleeps ~3s (vTaskDelay(pdMS_TO_TICKS(3000))).

app_main()

Initializes NVS (with “erase-and-retry” on version mismatch).

Logs start, calls wifi_manager_start() to bring up Wi-Fi logic.

Logs HTTPS bundle enabled (if compiled that way).

Creates poll_task pinned to core 0, stack 4096, prio 5.

Returns.

main/WifiManagerCustom.c
Purpose

Everything about Wi-Fi onboarding, storing credentials, and deciding whether we’re in setup AP or STA mode.

NVS keys & namespaces

Namespace: "gwcfg".

Keys:

"ssid" / "pass": saved Wi-Fi credentials.

"drf" (u8) and "drt" (u64): double-reset latch.

When boot happens, the code checks if a previous boot happened within 5 seconds. If yes → clear Wi-Fi creds and reboot.

Connection state & netifs

Tracks s_connected and an event bit WIFI_CONNECTED_BIT.

Holds STA/AP netif pointers (s_netif_sta, s_netif_ap) to avoid duplicate netif creation when switching modes.

Event handlers

on_wifi():

On WIFI_EVENT_STA_START → esp_wifi_connect().

On WIFI_EVENT_STA_DISCONNECTED → set s_connected=false, retry with esp_wifi_connect().

on_ip():

On IP_EVENT_STA_GOT_IP → mark connected, set the event bit, and if we’re in APSTA (from setup) switch to STA only.

Setup AP + web form

If no SSID in NVS:

Create AP netif, set mode to AP, SSID "GW-Setup-<MAC4><MAC5>", channel 6, open auth, max 4 clients.

Start HTTP server (port 80) with:

GET / → simple HTML form asking ssid and pass.

POST /save (form-urlencoded) → URL-decodes values, saves to NVS, then:

Ensures STA netif exists.

Switches to APSTA, sets wifi_config_t with your SSID/PASS, and calls esp_wifi_connect().

After IP is obtained, on_ip() moves to STA only.

Double-reset latch (works with EN/RESET button)

In wifi_manager_start() it calls double_reset_check_and_handle():

If a previous boot timestamp exists and current boot is within 5s of that → erase creds and reboot.

Otherwise, it arms the latch by writing current time and a flag.

GPIO0 hold erase (optional)

At boot, it samples GPIO0; if held low for ≥3s, it erases Wi-Fi creds and reboots.

This gives you a manual “factory Wi-Fi reset” without needing double-reset timing.

Normal STA boot (creds present)

Creates STA netif, sets mode to STA, sets wifi_config_t from NVS, and starts Wi-Fi.

esp_wifi_set_storage(WIFI_STORAGE_RAM) is used: the driver doesn’t manage its own NVS copy; your code owns NVS.

Public API

void wifi_manager_start(void): sets everything up and either starts AP or STA depending on NVS.

bool wifi_manager_is_connected(void): returns s_connected (set on GOT_IP).

Runtime flow (happy path)

Boot

NVS init

Double-reset latch check (maybe wipes Wi-Fi and restarts)

Optional GPIO0-hold wipe (maybe wipes Wi-Fi and restarts)

2a) No creds → Setup AP

Starts WIFI_MODE_AP with SSID GW-Setup-XXXX

Serves / form. You submit SSID/PASS.

Writes to NVS, switch AP→APSTA, connect, get IP, then STA only.

2b) Creds present → STA

Start WIFI_MODE_STA, connect, GOT_IP.

Polling loop (only when STA connected)

Every ~3s: GET latest command (HTTPS + cert bundle + optional API key)

If new command:

De-dup by commandId or hash of payload

(Optionally) POST “received/executed” status

Forward to mesh (replace stub with your BLE Mesh publish)

Command JSON your gateway accepts

Minimal accepted shape (keys are case-sensitive):

{
  "deviceId": "lamp-02",        // or "targetId" or "nodeId"; default "all" if omitted
  "commandId": "uuid-or-number",// optional; will be auto-hashed if missing
  "command": "on",              // "on" | "off"  (also accepts "led_on"/"led_off")
  "color": "#RRGGBB",           // optional; default white
  "brightness": 100             // 0..255 (absolute) or 0..100 (percent)
}


Anything with the same commandId (or same exact payload → same hash) is ignored as a duplicate.

How TLS/HTTP is configured

When MBEDTLS Certificate Bundle is enabled in menuconfig, the HTTP client uses:

cfg.crt_bundle_attach = esp_crt_bundle_attach;


so you don’t ship any PEM files; it validates public CAs out of the box.

http_get():

Opens connection, fetches headers (may return -1 for chunked).

Reads until either Content-Length is satisfied or EOF for chunked.

Doubles the buffer up to a ceiling if needed.

http_post_json():

Skips if URL is "". Otherwise posts JSON with headers and logs non-2xx.

What you can customize quickly

Erase gestures

Double-reset window: DOUBLE_RESET_WINDOW_US (currently 5 seconds).

GPIO0 hold time: change held_ms < 3000.

Setup AP SSID: GW-Setup-%02X%02X in start_portal().

Polling interval: vTaskDelay(pdMS_TO_TICKS(3000)) in poll_task().

Status POST: set CONFIG_GW_URL_STATUS to your API; leave empty to disable.

Mesh forwarding: swap out forward_to_mesh_stub() with your BLE Mesh calls, using .target, .on, .r/.g/.b, .brightness.

Build/IDF notes

You’re on ESP-IDF v5.5. The client config field is crt_bundle_attach (not cert_bundle_attach).

You added esp_timer to PRIV_REQUIRES in main/CMakeLists.txt (needed by the latch logic).

Compiler treats warnings as errors; you fixed the GCC-12 “address” warning by removing the incorrect field name earlier.
