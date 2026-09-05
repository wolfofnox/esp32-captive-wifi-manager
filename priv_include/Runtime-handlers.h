#pragma once

#include "esp_err.h"
#include "stdbool.h"

/**
 * @brief Registers common runtime handlers to esp-idf wifi component
 * 
 * @param sd_card_present flag if the SD card is present and detected
 * @return ESP_OK on success
 */
esp_err_t register_runtime_handlers(bool sd_card_present);