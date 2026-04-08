idf_component_register(
    SRCS "src/Wifi.c" "include/dns_server/dns_server.c" "src/Flags.c" "src/Captive.c" "src/Server-mgr.c" "src/Runtime-handlers.c" "src/SD-mgr.c" "src/AP.c" "src/STA.c"
    INCLUDE_DIRS include include/dns_server/include
    REQUIRES esp_wifi esp_event nvs_flash esp_http_server lwip mdns led_indicator fatfs
    EMBED_FILES src/captive.html
)
