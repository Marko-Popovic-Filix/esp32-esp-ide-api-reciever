// main/main.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "driver/gpio.h"

#include "cJSON.h"
#include "WifiManagerCustom.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

// ======== Kconfig (menuconfig) ========
#define GW_API_KEY     CONFIG_GW_API_KEY
#define GW_DEVICE_ID   CONFIG_GW_DEVICE_ID
#define GW_URL_LATEST  CONFIG_GW_URL_LATEST
// No status POST URL on purpose (we're disabling POSTs)

// ======== Logs ========
static const char *TAG = "GW";

// ======== GPIO0 long-press to forget Wi-Fi ========
#define WIFI_RESET_BTN_GPIO   0      // IO0 (BOOT)
#define BTN_SAMPLE_MS         20
#define BTN_LONG_MS           3000   // hold ≥ 3s to clear Wi-Fi

static void clear_wifi_credentials_and_reboot(void)
{
    ESP_LOGW(TAG, "Clearing Wi-Fi creds via esp_wifi_restore() + wiping 'wifi_manager' namespace");
    // Stop WiFi if running (ignore errors)
    esp_wifi_stop();
    // Erase Wi-Fi config that the WiFi stack saved
    esp_wifi_restore();

    // If your custom manager uses its own NVS namespace, wipe it as well:
    nvs_handle_t h;
    if (nvs_open("wifi_manager", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "Erased NVS namespace 'wifi_manager'");
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void wifi_clear_button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WIFI_RESET_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // IO0 is pulled up, press to GND
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "GPIO0 long-press enabled (hold ≥ %d ms to forget Wi-Fi).", BTN_LONG_MS);

    bool prev = true;
    int64_t t_down_ms = 0;

    while (1) {
        bool level = gpio_get_level(WIFI_RESET_BTN_GPIO); // 1 = released (pulled up), 0 = pressed
        if (!level) {
            if (prev) {
                // went down now
                t_down_ms = esp_timer_get_time() / 1000;
                prev = false;
            } else {
                // still down — check duration
                int64_t held_ms = (esp_timer_get_time() / 1000) - t_down_ms;
                if (held_ms >= BTN_LONG_MS) {
                    ESP_LOGW(TAG, "GPIO0 long-press detected -> erase Wi-Fi + reboot");
                    clear_wifi_credentials_and_reboot();
                }
            }
        } else {
            // released
            prev = true;
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_SAMPLE_MS));
    }
}

// ======== Tiny helpers ========
static uint32_t fnv1a32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void hex32(uint32_t v, char out[9]) {
    static const char *d = "0123456789abcdef";
    for (int i=7; i>=0; --i) { out[i] = d[v & 0xF]; v >>= 4; }
    out[8] = 0;
}

// ======== HTTP GET only (NO POST) ========
static esp_err_t http_get(const char *url, const char *api_key, char **out)
{
    *out = NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    if (api_key && api_key[0]) {
        esp_http_client_set_header(c, "x-api-key", api_key);
    }

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }

    int64_t cl = esp_http_client_fetch_headers(c); // may be -1 (chunked)

    int cap = (cl > 0 && cl < 32768) ? (int)cl + 1 : 4096;
    char *buf = malloc(cap);
    if (!buf) { esp_http_client_close(c); esp_http_client_cleanup(c); return ESP_ERR_NO_MEM; }

    int total = 0;
    while (1) {
        if (total >= cap - 1) {
            int newcap = cap * 2;
            if (newcap > 65536) break; // safety
            char *nb = realloc(buf, newcap);
            if (!nb) break;
            buf = nb; cap = newcap;
        }
        int n = esp_http_client_read(c, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
        if (cl > 0 && total >= cl) break;
    }
    buf[total] = 0;

    int status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (status != 200) {
        ESP_LOGW(TAG, "GET status %d, body: %.*s", status, total, buf);
        free(buf);
        return ESP_FAIL;
    }

    *out = buf;
    return ESP_OK;
}

// ======== Command parse & dispatch ========
typedef struct {
    bool   valid;
    bool   on;
    uint8_t r, g, b;
    uint8_t brightness;    // 0..255
    char   id[64];         // commandId or hash
    char   target[64];     // deviceId/targetId/nodeId
} gw_cmd_t;

static bool parse_hex2(const char *s, uint8_t *out) {
    int v = 0;
    for (int i=0;i<2;i++){
        char c = s[i];
        int d = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
        if (d<0) return false;
        v = (v<<4)|d;
    }
    *out = (uint8_t)v; return true;
}

static gw_cmd_t parse_command_json(const char *json)
{
    gw_cmd_t out = {0};
    cJSON *root = cJSON_Parse(json);
    if (!root) return out;

    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "commandId");
    if (cJSON_IsString(jid) && jid->valuestring && jid->valuestring[0]) {
        strlcpy(out.id, jid->valuestring, sizeof(out.id));
    } else {
        char h[9]; hex32(fnv1a32(json, strlen(json)), h);
        strlcpy(out.id, h, sizeof(out.id));
    }

    const char *tkeys[] = {"deviceId","targetId","nodeId"};
    for (size_t i=0;i<sizeof(tkeys)/sizeof(tkeys[0]);++i){
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, tkeys[i]);
        if (cJSON_IsString(jt) && jt->valuestring && jt->valuestring[0]) {
            strlcpy(out.target, jt->valuestring, sizeof(out.target));
            break;
        }
    }
    if (out.target[0] == 0) strlcpy(out.target, "all", sizeof(out.target));

    bool have_cmd = false;
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *cs = (cJSON_IsString(cmd)?cmd->valuestring:NULL);
    const char *as = (cJSON_IsString(act)?act->valuestring:NULL);
    const char *s = cs ? cs : as;
    if (s) {
        if (!strcasecmp(s, "on") || !strcasecmp(s, "led_on")) { out.on = true; have_cmd = true; }
        else if (!strcasecmp(s, "off") || !strcasecmp(s, "led_off")) { out.on = false; have_cmd = true; }
    } else {
        have_cmd = true; out.on = true;
    }

    int b = 255;
    const cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(jb)) {
        int v = (int)jb->valuedouble;
        if (v >= 0 && v <= 100) { b = (v * 255) / 100; }
        else { if (v < 0) v = 0; if (v > 255) v = 255; b = v; }
    }
    out.brightness = (uint8_t)b;

    out.r = out.g = out.b = 0xFF;
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "color");
    if (cJSON_IsString(jc) && jc->valuestring && jc->valuestring[0]) {
        const char *csz = jc->valuestring;
        if (csz[0]=='#' && strlen(csz)>=7) {
            uint8_t r,g,bb;
            if (parse_hex2(csz+1,&r) && parse_hex2(csz+3,&g) && parse_hex2(csz+5,&bb)) {
                out.r=r; out.g=g; out.b=bb;
            }
        }
    }

    out.valid = have_cmd;
    cJSON_Delete(root);
    return out;
}

// ======== Poller (runs only when STA has IP) ========
static TaskHandle_t s_poll_task = NULL;
static char s_last_cmd_id[64] = {0};

static void forward_to_mesh_stub(const gw_cmd_t *c)
{
    ESP_LOGI(TAG, "→ MESH target[%s]: %s R:%u G:%u B:%u BRI:%u ID:%s",
             c->target, c->on ? "ON" : "OFF", c->r, c->g, c->b, c->brightness, c->id);
}

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "[POLL] task running (STA connected).");
    char *body = NULL;

    while (1) {
        body = NULL;
        if (http_get(GW_URL_LATEST, GW_API_KEY, &body) == ESP_OK && body) {
            char *p = body; while (*p && isspace((unsigned char)*p)) ++p;
            if (*p) {
                ESP_LOGI(TAG, "latest-command: %s", p);

                gw_cmd_t c = parse_command_json(p);
                if (c.valid && strcmp(s_last_cmd_id, c.id) != 0) {
                    strlcpy(s_last_cmd_id, c.id, sizeof(s_last_cmd_id));
                    forward_to_mesh_stub(&c);
                }
            } else {
                ESP_LOGI(TAG, "latest-command:");
            }
            free(body);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void start_poll_task_if_needed(void)
{
    if (!s_poll_task) {
        xTaskCreatePinnedToCore(poll_task, "poll", 4096, NULL, 5, &s_poll_task, 0);
        ESP_LOGI(TAG, "[POLL] CREATED");
    }
}
static void stop_poll_task_if_running(void)
{
    if (s_poll_task) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
        ESP_LOGI(TAG, "[POLL] DELETED");
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected -> stop poller");
        stop_poll_task_if_running();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "STA got IP -> start poller");
        start_poll_task_if_needed();
    }
}

// ======== app_main ========
void app_main(void)
{
    ESP_LOGI(TAG, "BUILD MARK: GPIO0 long-press erase | GET-only poller");

    // NVS (must succeed for Wi-Fi creds to persist)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Event loop + Wi-Fi events (for poller gating)
    esp_event_loop_create_default();
    esp_event_handler_instance_t h1, h2;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2);

    // Start button monitor (GPIO0 long-press)
    xTaskCreatePinnedToCore(wifi_clear_button_task, "btn", 2048, NULL, 10, NULL, 0);

    // Start Wi-Fi manager (keeps creds; we’re NOT erasing anywhere at boot)
    ESP_LOGI(TAG, "Gateway starting: Wi-Fi manager init");
    wifi_manager_start();  // NOTE: this returns void in your project

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    ESP_LOGI(TAG, "HTTPS cert bundle enabled");
#endif

    ESP_LOGI("main_task", "Returned from app_main()");
}
