// main/WifiManagerCustom.c
// Wi-Fi manager with setup portal + robust double-reset via NVS latch
// - First boot (no creds): SoftAP "GW-Setup-XXXX" + web portal at http://192.168.4.1
// - After submit SSID/PASS: saves to NVS, switches to STA, connects
// - Double-tap RESET (two reboots within 5s): clears saved Wi-Fi (NVS) and reboots into setup
// - Optional: hold BOOT (GPIO0) ~3s at power-up to clear Wi-Fi

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_mac.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_http_server.h"
#include "lwip/inet.h"
#include "driver/gpio.h"     // for optional hold-BOOT reset

static const char *TAG = "WiFiMgr";

#define NVS_NS     "gwcfg"
#define KEY_SSID   "ssid"
#define KEY_PASS   "pass"

// Double-reset latch keys (in NVS)
#define DR_KEY_FLAG   "drf"   // u8 {0,1}
#define DR_KEY_TIME   "drt"   // u64 (us)
#define DOUBLE_RESET_WINDOW_US (5ULL * 1000000ULL)  // 5 seconds

static EventGroupHandle_t s_evt;
#define WIFI_CONNECTED_BIT BIT0
static bool s_connected = false;
static httpd_handle_t s_server = NULL;

// Keep handles to avoid duplicate netifs
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;

// ====== tiny helpers ======
static int from_hex(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return 0;
}
static void url_decode(char *s){
    char *o=s, *p=s;
    while(*p){
        if(*p=='%' && p[1] && p[2]){ *o=(char)(from_hex(p[1])*16+from_hex(p[2])); p+=3; o++; }
        else if(*p=='+'){ *o=' '; p++; o++; }
        else { *o++=*p++; }
    }
    *o=0;
}

// ====== NVS creds ======
static bool load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz){
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READONLY, &h)!=ESP_OK) return false;
    size_t ssz=ssid_sz, psz=pass_sz;
    esp_err_t es = nvs_get_str(h, KEY_SSID, ssid, &ssz);
    esp_err_t ep = nvs_get_str(h, KEY_PASS, pass, &psz);
    nvs_close(h);
    return (es==ESP_OK && ep==ESP_OK && ssid[0]!=0);
}
static esp_err_t save_creds(const char *ssid, const char *pass){
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_PASS, pass));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}
static void erase_saved_wifi(void){
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_SSID);
        nvs_erase_key(h, KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "Wi-Fi credentials erased from NVS.");
    }
}

// ====== Double-reset via NVS latch (works with EN/RESET) ======
static void double_reset_check_and_handle(void){
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t flag = 0;
    uint64_t last = 0;
    esp_err_t eflag = nvs_get_u8(h, DR_KEY_FLAG, &flag);
    esp_err_t etime = nvs_get_u64(h, DR_KEY_TIME, &last);
    uint64_t now = esp_timer_get_time();

    bool armed = (eflag == ESP_OK && flag == 1 && etime == ESP_OK);
    if (armed && (now - last) < DOUBLE_RESET_WINDOW_US) {
        // Double reset detected
        ESP_LOGW(TAG, "Double RESET detected -> clearing Wi-Fi creds and rebooting");
        nvs_set_u8(h, DR_KEY_FLAG, 0);
        nvs_commit(h);
        nvs_close(h);
        erase_saved_wifi();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        // (Re)arm latch for the next boot
        nvs_set_u8(h, DR_KEY_FLAG, 1);
        nvs_set_u64(h, DR_KEY_TIME, now);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ====== Wi-Fi events ======
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data){
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START){
        esp_wifi_connect(); // kick off STA connect
    }else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED){
        s_connected=false;
        esp_wifi_connect(); // retry forever
    }
}
static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data){
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        const ip_event_got_ip_t *e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected=true;
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
        // If we were APSTA during setup, drop AP now:
        wifi_mode_t m; esp_wifi_get_mode(&m);
        if (m == WIFI_MODE_APSTA) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            ESP_LOGI(TAG, "Switched to STA only");
        }
    }
}

// ====== HTTP setup portal ======
static const char *HTML_FORM =
"<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>ESP32 Gateway Setup</title>"
"<style>body{font-family:sans-serif;margin:24px;max-width:420px}input{width:100%;padding:8px;margin:6px 0;}button{padding:10px 16px;}</style>"
"</head><body>"
"<h2>Wi-Fi Setup</h2>"
"<form method='POST' action='/save'>"
"SSID:<br><input name='ssid' maxlength='32' autofocus><br>"
"Password:<br><input name='pass' type='password' maxlength='63'><br>"
"<button type='submit'>Save & Connect</button></form>"
"<p>After saving, the device will connect to your Wi-Fi.</p>"
"</body></html>";

static esp_err_t root_get(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_FORM, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req){
    int total = req->content_len;
    if(total<=0 || total>1024) total=1024;
    char *buf = malloc(total+1);
    if(!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    int rcv = httpd_req_recv(req, buf, total);
    if(rcv<=0){ free(buf); return ESP_FAIL; }
    buf[rcv]=0;

    // Expect application/x-www-form-urlencoded: ssid=...&pass=...
    char ssid[33]={0}, pass[65]={0};
    char *p = buf;
    while(p && *p){
        char *key = p;
        char *eq = strchr(p, '=');
        if(!eq) break;
        *eq=0;
        char *val = eq+1;
        char *amp = strchr(val, '&');
        if(amp){ *amp=0; p = amp+1; } else { p = NULL; }
        url_decode(val);
        if(strcmp(key,"ssid")==0){ strlcpy(ssid, val, sizeof(ssid)); }
        else if(strcmp(key,"pass")==0){ strlcpy(pass, val, sizeof(pass)); }
    }
    free(buf);

    if(ssid[0]==0){
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    }

    if(save_creds(ssid, pass)==ESP_OK){
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<html><body><h3>Saved! Connecting…</h3>"
            "<p>You can close this page.</p></body></html>");

        // Ensure STA netif exists (we started in AP mode)
        if (s_netif_sta == NULL) s_netif_sta = esp_netif_create_default_wifi_sta();

        // Switch AP -> APSTA and connect
        wifi_config_t cfg = {0};
        strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
        strlcpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        // Either STA_START will trigger our handler, or this kicks it off.
        esp_err_t e = esp_wifi_connect();
        if (e != ESP_OK && e != ESP_ERR_WIFI_STATE && e != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
        }
        return ESP_OK;
    }else{
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }
}

static void start_http_server(void){
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg)==ESP_OK){
        httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get};
        httpd_uri_t save = {.uri="/save", .method=HTTP_POST, .handler=save_post};
        httpd_register_uri_handler(s_server, &root);
        httpd_register_uri_handler(s_server, &save);
    }else{
        ESP_LOGE(TAG, "HTTP server start failed");
    }
}

// ====== SoftAP (setup) ======
static void start_portal(void){
    if (s_netif_ap == NULL) s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {0};
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "GW-Setup-%02X%02X", mac[4], mac[5]);

    strlcpy((char*)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen((char*)ap.ap.ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN; // open portal

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start()); // start AFTER mode+config

    ESP_LOGI(TAG, "Setup AP started: SSID='%s', IP http://192.168.4.1", ap_ssid);
    start_http_server();
}

// ====== Public API ======
void wifi_manager_start(void){
    s_evt = xEventGroupCreate();

    // Handle double-reset via NVS latch (works with EN/RESET)
    double_reset_check_and_handle();

    // Optional: hold BOOT (GPIO0) ~3s at boot to clear Wi-Fi
#if 1
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 0, // GPIO0
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true, .pull_down_en = false, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    int held_ms = 0;
    while (gpio_get_level(0) == 0 && held_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        held_ms += 10;
    }
    if (held_ms >= 3000) {
        ESP_LOGW(TAG, "BOOT held %dms -> clearing Wi-Fi creds and rebooting", held_ms);
        erase_saved_wifi();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
#endif

    // Base init (idempotent)
    esp_err_t err;
    err = esp_netif_init(); if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    err = esp_event_loop_create_default(); if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    // Make driver NOT persist its own copy; we manage creds in NVS ourselves
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL, NULL));

    // If saved creds exist → STA. Else → setup portal.
    char ssid[33]={0}, pass[65]={0};
    if(load_creds(ssid, sizeof(ssid), pass, sizeof(pass))){
        ESP_LOGI(TAG, "Using saved Wi-Fi: '%s'", ssid);

        if (s_netif_sta == NULL) s_netif_sta = esp_netif_create_default_wifi_sta();

        wifi_config_t cfg = {0};
        strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
        strlcpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        ESP_ERROR_CHECK(esp_wifi_start());     // start AFTER config
        // Do NOT call esp_wifi_connect() here; on_wifi handles it on STA_START
    }else{
        start_portal();
    }
}

bool wifi_manager_is_connected(void){
    return s_connected;
}
