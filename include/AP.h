#pragma once

#include "esp_err.h"

/**
 * @brief Initialize WiFi in access point mode.
 * 
 * Starts the device as an access point with the configured SSID and password.
 */
esp_err_t wifi_init_ap();