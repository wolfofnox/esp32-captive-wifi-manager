#pragma once

#include "esp_err.h"

#if CONFIG_WIFI_ENABLE_AP_MODE

/**
 * @brief Initialize WiFi in access point mode.
 * 
 * Starts the device as an access point with the configured SSID and password.
 */
esp_err_t wifi_init_ap(void);

#else

static inline esp_err_t wifi_init_ap(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif