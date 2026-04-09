#include "SD-mgr.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <dirent.h>

/** @brief Flag indicating whether SD card is mounted and available */
static bool SD_card_present = false;

static const char *TAG = "Wifi: SD_Mgr";

/** @brief Mount point path for the SD card filesystem */
static const char *SD_CARD_MOUNT_POINT = "/sdcard";

/**
 * @brief Mount the SD card using SPI interface.
 * 
 * Initializes the SPI bus, configures the SD card interface, and mounts
 * the FAT filesystem. Lists directory contents at debug log level if successful.
 * 
 * @return ESP_OK on successful mount
 * @return Error code from SPI initialization or filesystem mount failure
 */
esp_err_t mount_sd_card() {
    ESP_LOGI(TAG, "Mounting SD card...");

    sdmmc_card_t *card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_PIN_WIFI_SD_MOSI,
        .miso_io_num = CONFIG_PIN_WIFI_SD_MISO,
        .sclk_io_num = CONFIG_PIN_WIFI_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,  // Default transfer size
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize SPI bus for SD card: %s", esp_err_to_name(ret));

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_PIN_WIFI_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        #ifdef CONFIG_WIFI_FORMAT_SD_ON_FAIL
        .format_if_mount_failed = true,
        #else
        .format_if_mount_failed = false,
        #endif
        .max_files = 5,                   // Maximum number of open files
        .allocation_unit_size = 16 * 1024, // Allocation unit size
    };
    ret = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to mount SD card file system: %s", esp_err_to_name(ret));

    DIR *dir = opendir(SD_CARD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open SD card directory");
    } else {
        struct dirent *entry;
        ESP_LOGV(TAG, "Files on SD card:");
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGV(TAG, "  %s", entry->d_name);
        }
    }
    closedir(dir);

    SD_card_present = true;
    return ESP_OK;
}

bool is_sd_card_present() {
    return SD_card_present;
}

const char* get_sd_card_mount_point() {
    return SD_CARD_MOUNT_POINT;
}