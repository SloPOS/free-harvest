idf_component_register(
    SRCS "main.c" "hr_usb.c" "hr_wifi.c" "hr_http.c" "hr_mqtt.c" "hr_log.c" "hr_capture.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    REQUIRES hr_protocol nvs_flash esp_wifi esp_http_server esp_netif lwip esp_timer app_update esp_hw_support spiffs
)
