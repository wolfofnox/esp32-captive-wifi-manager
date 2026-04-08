#pragma once

#include "esp_err.h"

/**
 * @brief Initialize WiFi in station (client) mode.
 * 
 * Connects to a configured WiFi network and starts the HTTP server.
 */
esp_err_t wifi_init_sta();