idf_component_register(
  SRCS "main.c" "WifiManagerCustom.c"
  REQUIRES esp_http_client esp_event nvs_flash json esp_netif esp_wifi esp_http_server driver
  PRIV_REQUIRES mbedtls esp_timer
)
