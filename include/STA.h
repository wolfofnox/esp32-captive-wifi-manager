#pragma once

#include "esp_err.h"

#if CONFIG_WIFI_ENABLE_STA_MODE

/**
 * @brief Initialize WiFi in station (client) mode.
 *
 * Connects to a configured WiFi network and starts the HTTP server.
 */
esp_err_t wifi_init_sta(void);

#else

static inline esp_err_t wifi_init_sta(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif