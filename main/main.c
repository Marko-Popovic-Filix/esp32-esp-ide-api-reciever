// main.c ??? ESP32 Gateway (poll-only)
// - Uses WifiManagerCustom for Wi-Fi provisioning / saved creds
// - HTTPS GET to fetch the latest command JSON
// - Optional x-api-key header (leave blank in menuconfig if not used)
// - Status POST is disabled by default (guarded by CONFIG_GW_ENABLE_STATUS)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "sdkconfig.h"
#include "WifiManagerCustom.h"

static const char *TAG = "GW";

// -------- menuconfig bindings --------
#define GW_API_KEY     CONFIG_GW_API_KEY       // may be empty
#define GW_DEVICE_ID   CONFIG_GW_DEVICE_ID
#define GW_URL_LATEST  CONFIG_GW_URL_LATEST

#if CONFIG_GW_ENABLE_STATUS
#define GW_URL_STATUS  CONFIG_GW_URL_STATUS
#endif

// -------- HTTP helpers (GET [+optional x-api-key]) --------
typedef struct {
    char *buf;
    int   len;
    int   cap;
} resp_buf_t;

static esp_err_t http_evt_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        if (rb) { rb->len = 0; }
        break;
    case HTTP_EVENT_ON_DATA:
        if (!rb || !evt->data || evt->data_len <= 0) break;
        if (rb->cap - rb->len < evt->data_len + 1) {
            int new_cap = rb->cap ? rb->cap : 1024;
            while (new_cap - rb->len < evt->data_len + 1) new_cap *= 2;
            char *nb = realloc(rb->buf, new_cap);
            if (!nb) return ESP_FAIL;
            rb->buf = nb; rb->cap = new_cap;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->buf[rb->len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url, const char *api_key, char **out_body, int *out_status)
{
    *out_body = NULL;
    if (out_status) *out_status = -1;

    resp_buf_t rb = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_evt_handler,
        .user_data = &rb,
        .crt_bundle_attach = esp_crt_bundle_attach, // use IDF root bundle
        .timeout_ms = 7000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    if (api_key && api_key[0] != '\0') {
        esp_http_client_set_header(client, "x-api-key", api_key);
    }
    esp_http_client_set_header(client, "Cache-Control", "no-cache");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (out_status) *out_status = status;

        if (status == 200 && rb.buf) {
            // Trim any trailing NUL already set in handler
            *out_body = rb.buf;
            rb.buf = NULL; // hand off ownership
        }
    }

    esp_http_client_cleanup(client);
    if (rb.buf) free(rb.buf);
    return err;
}

#if CONFIG_GW_ENABLE_STATUS
// -------- optional status POST (guarded) --------
static esp_err_t http_post_json(const char *url, const char *api_key, const char *json_body, int *out_status)
{
    if (out_status) *out_status = -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 7000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    if (api_key && api_key[0] != '\0') {
        esp_http_client_set_header(client, "x-api-key", api_key);
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");

    esp_http_client_set_post_field(client, json_body, strlen(json_body));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        if (out_status) *out_status = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}
#endif // CONFIG_GW_ENABLE_STATUS

// -------- polling task --------
#define POLL_MS 1500

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "Gateway polling started (deviceId='%s')", GW_DEVICE_ID);

    while (1) {
        char url[512];
        // Append deviceId as query (Lambda/S3 may ignore it, safe to send)
        snprintf(url, sizeof(url), "%s?deviceId=%s", GW_URL_LATEST, GW_DEVICE_ID);

        char *body = NULL;
        int http_status = -1;
        esp_err_t err = http_get(url, GW_API_KEY, &body, &http_status);

        if (err == ESP_OK && http_status == 200) {
            if (body && body[0] != '\0') {
                ESP_LOGI(TAG, "latest-command: %s", body);
                // TODO: parse JSON and forward to BLE Mesh here
            } else {
                // empty body is fine ??? nothing queued
                // ESP_LOGD(TAG, "No command at the moment");
            }
        } else {
            ESP_LOGW(TAG, "GET status %d (err=%d)", http_status, (int)err);
        }

        if (body) free(body);

        // ---------- OPTIONAL STATUS (disabled unless enabled in menuconfig) ----------
#if CONFIG_GW_ENABLE_STATUS
        // Build a tiny heartbeat/ack as needed; example payload:
        // {"gatewayId":"%s","status":"online"}
        char status_body[160];
        snprintf(status_body, sizeof(status_body),
                 "{\"gatewayId\":\"%s\",\"status\":\"online\"}", GW_DEVICE_ID);

        int post_status = -1;
        err = http_post_json(GW_URL_STATUS, GW_API_KEY, status_body, &post_status);
        if (err == ESP_OK) {
            if (post_status != 200) {
                ESP_LOGW(TAG, "POST status %d", post_status);
            }
        } else {
            ESP_LOGW(TAG, "POST failed (err=%d)", (int)err);
        }
#endif
        // ---------------------------------------------------------------------------

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// -------- app_main --------
void app_main(void)
{
    // NVS & netif init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Start Wi-Fi manager (provisions if no creds; uses saved creds otherwise)
    ESP_LOGI(TAG, "Gateway starting: Wi-Fi manager init");
    wifi_manager_start();

    ESP_LOGI(TAG, "HTTPS cert bundle enabled"); // using esp_crt_bundle_attach

    // Start the polling task
    xTaskCreate(poll_task, "poll_task", 6 * 1024, NULL, 5, NULL);
}


