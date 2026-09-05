#pragma once

#include "esp_err.h"
#include "stdbool.h"

/**
 * @brief Registers common common handlers to esp-idf wifi component
 * 
 * @return ESP_OK on success
 */
esp_err_t register_common_handlers(void);