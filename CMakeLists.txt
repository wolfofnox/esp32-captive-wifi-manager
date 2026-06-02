set(srcs
    "src/Wifi.c"
    "src/Flags.c"
    "src/Server-mgr.c"
    "src/Runtime-handlers.c"
    "src/nvs-mgr.c"
)

set(includes
    "include"
)


if(CONFIG_WIFI_ENABLE_WS_RAP)
list(APPEND srcs "src/ws_server.c")
endif()

if(CONFIG_WIFI_ENABLE_CAPTIVE_PORTAL)
    list(APPEND srcs "src/Captive.c")
    list(APPEND srcs "include/dns_server/dns_server.c")
    list(APPEND includes "include/dns_server/include")
endif()

if(CONFIG_WIFI_ENABLE_AP_MODE)
    list(APPEND srcs "src/AP.c")
endif()

if(CONFIG_WIFI_ENABLE_STA_MODE)
    list(APPEND srcs "src/STA.c")
endif()

set(reqs "esp_wifi" "esp_event" "nvs_flash" "esp_http_server" "lwip" "mdns" "cjson")
if(CONFIG_WIFI_USE_SK6812_STATUS_LED)
    list(APPEND reqs "led_indicator")
endif()

if(CONFIG_WIFI_ENABLE_SD)
    list(APPEND srcs "src/SD-mgr.c")
    list(APPEND reqs "fatfs")
endif()

if (CONFIG_WIFI_ENABLE_CAPTIVE_PORTAL)
set(EMBED_FILES "src/captive.html")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes}
    REQUIRES ${reqs}
    PRIV_REQUIRES fatfs
    EMBED_FILES ${EMBED_FILES}
)

