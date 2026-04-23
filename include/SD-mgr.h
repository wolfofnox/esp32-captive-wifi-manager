#pragma once

#include "esp_err.h"
#include "stdbool.h"

/**
 * @brief Mount the SD card using SPI interface.
 * 
 * Initializes the SPI bus and mounts the FAT filesystem from the SD card
 * at the configured mount point. Lists files on the card at debug log level.
 * 
 * @return ESP_OK on success
 * @return Error code if SPI initialization or mounting fails
 */
esp_err_t mount_sd_card();

/**
 * @brief Check if SD card is present and mounted.
 * 
 * @return true if SD card is present and mounted
 * @return false otherwise
 */
bool is_sd_card_present();

/**
 * @brief Get the mount point path for the SD card filesystem.
 * 
 * @return const char* Mount point path (e.g. "/sdcard")
 */

const char* get_sd_card_mount_point();