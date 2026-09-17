set(srcs "main.c" "hr_usb.c" "hr_wifi.c" "hr_http.c" "hr_mqtt.c" "hr_log.c" "hr_logring.c" "hr_capture.c" "hr_batchstore.c" "hr_compat.c" "hr_quiesce.c" "hr_reboot.c" "hr_netwatch.c")
# The dryer's own batch files (FDFILES / FILEREAD client), see hr_dryerfiles.h.
if(CONFIG_HR_BATCH_HISTORY)
    list(APPEND srcs "hr_dryerfiles.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    EMBED_FILES "www/img/idle.png" "www/img/freeze.png"
                "www/img/dry.png" "www/img/heat.png"
    REQUIRES hr_protocol nvs_flash esp_wifi esp_http_server esp_netif lwip esp_timer app_update esp_hw_support spiffs
)
