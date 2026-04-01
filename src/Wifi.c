/**
 * @file Wifi.c
 * @brief WiFi and captive portal management for ESP32.
 * 
 * This file implements WiFi initialization, captive portal, event handlers,
 * and web server endpoints for configuration and control.
 */

#include "Wifi.h"

#include "sdkconfig.h"

#include "Flags.h"
#include "Captive.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_mac.h"      // for MAC2STR macro
#include "lwip/inet.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_indicator.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include <dirent.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "time.h"
#include "esp_sntp.h"
#include <sys/stat.h>

#pragma region Variables & Config

/** @brief Mount point path for the SD card filesystem */
static const char *SD_CARD_MOUNT_POINT = "/sdcard";

/** @brief Log tag for general WiFi module messages */
static const char *TAG = "Wifi";

/** @brief Log tag for SD card related messages */
static const char *TAG_SD = "Wifi-SD_Card";

/** @brief Registry array for storing custom HTTP handlers registered by the application */
static httpd_uri_t custom_handlers[CONFIG_WIFI_MAX_CUSTOM_HTTP_HANDLERS];

/** @brief Count of currently registered custom HTTP handlers */
static size_t custom_handler_count = 0;

/** @brief Maximum number of client IPs to track for captive portal redirect */
#define MAX_REDIRECTED_IPS 10

/** @brief Array tracking IPs that have already been redirected to prevent redirect loops */
static uint32_t redirected_ips[MAX_REDIRECTED_IPS];

/** @brief Number of IPs currently tracked in redirected_ips array */
static int redirected_count = 0;

/** @brief HTTP server handle, NULL when server is not running */
httpd_handle_t server = NULL;

/** @brief Counter for consecutive STA connection failures */
static int sta_fails_count = 0;

/** @brief Flag indicating whether SD card is mounted and available */
bool SD_card_present = false;

/** @brief HTTP server configuration structure */
httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();

/** @brief Network interface handles for AP and STA modes */
esp_netif_t *ap_netif, *sta_netif;

/**
 * @brief LED blink pattern enumeration.
 * 
 * Defines different LED patterns for various device states.
 */
enum {
    BLINK_OFF = 0,              ///< LED off
    BLINK_LOADING,              ///< System loading/initializing
    BLINK_LOADED,               ///< System loaded successfully
    BLINK_WIFI_CONNECTING,      ///< Attempting WiFi connection
    BLINK_WIFI_CONNECTED,       ///< WiFi connected successfully
    BLINK_WIFI_DISCONNECTED,    ///< WiFi disconnected/failed
    BLINK_WIFI_AP_STARTING,     ///< AP mode starting
    BLINK_WIFI_AP_STARTED,      ///< AP mode active
    BLINK_MAX                   ///< Total number of blink patterns
};

/** @brief LED pattern for off state - solid off */
static const blink_step_t off[] = {
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0}
};

/** @brief LED pattern for loading state - white breathing animation */
static const blink_step_t loading[] = {
    {LED_BLINK_HSV, SET_HSV(0, 0, 0), 0},
    {LED_BLINK_BREATHE, LED_STATE_75_PERCENT, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0}
};

/** @brief LED pattern for loaded state - 2 quick white blinks */
static const blink_step_t loaded[] = {
    {LED_BLINK_HSV, SET_HSV(0, 0, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

/** @brief LED pattern for WiFi connecting - yellow/orange breathing */
static const blink_step_t wifi_connecting[] = {
    {LED_BLINK_HSV, SET_HSV(40, MAX_SATURATION, 0), 0},
    {LED_BLINK_BREATHE, LED_STATE_75_PERCENT, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0}
};

/** @brief LED pattern for WiFi connected - 2 quick yellow/orange blinks */
static const blink_step_t wifi_connected[] = {
    {LED_BLINK_HSV, SET_HSV(40, MAX_SATURATION, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

/** @brief LED pattern for WiFi disconnected - 3 quick red blinks */
static const blink_step_t wifi_disconnected[] = {
    {LED_BLINK_HSV, SET_HSV(0, MAX_SATURATION, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

/** @brief LED pattern for AP starting - blue breathing */
static const blink_step_t wifi_ap_starting[] = {
    {LED_BLINK_HSV, SET_HSV(210, MAX_SATURATION, 0), 0},
    {LED_BLINK_BREATHE, LED_STATE_75_PERCENT, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0}
};

/** @brief LED pattern for AP started - 2 quick blue blinks */
static const blink_step_t wifi_ap_started[] = {
    {LED_BLINK_HSV, SET_HSV(210, MAX_SATURATION, 0), 0},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_STOP, 0, 0}
};

/** @brief Handle for the LED indicator component */
led_indicator_handle_t led_handle;

/** @brief Array of LED blink patterns indexed by blink type enumeration */
const blink_step_t *led_blink_list[BLINK_MAX] = {
    [BLINK_OFF] = off,
    [BLINK_LOADING] = loading,
    [BLINK_LOADED] = loaded,
    [BLINK_WIFI_CONNECTING] = wifi_connecting,
    [BLINK_WIFI_CONNECTED] = wifi_connected,
    [BLINK_WIFI_DISCONNECTED] = wifi_disconnected,
    [BLINK_WIFI_AP_STARTING] = wifi_ap_starting,
    [BLINK_WIFI_AP_STARTED] = wifi_ap_started
};

#pragma endregion

#pragma region Functions

// WiFi initialization functions

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
 * @brief Initialize WiFi in station (client) mode.
 * 
 * Connects to a configured WiFi network and starts the HTTP server.
 */
void wifi_init_sta();

/**
 * @brief Initialize WiFi in access point mode.
 * 
 * Starts the device as an access point with the configured SSID and password.
 */
void wifi_init_ap();

/**
 * @brief Synchronize system time with SNTP server.
 * 
 * @param wait_for_sync If true, waits for time synchronization to complete
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sync_time(bool wait_for_sync);

// FreeRTOS task functions

/**
 * @brief FreeRTOS task that listens for WiFi mode switch events.
 * 
 * Monitors event group bits and performs WiFi mode transitions,
 * reconnections, and mDNS updates as requested.
 * 
 * @param pvParameter Unused task parameter
 */
void wifi_flags_listener_task(void *pvParameter);

// HTTP handler registration helpers

/**
 * @brief Register all custom HTTP handlers with the server.
 * 
 * Called when transitioning to STA or AP mode to activate custom handlers.
 */
void register_custom_http_handlers(void);

/**
 * @brief HTTP 404 error handler.
 * 
 * @param req HTTP request handle
 * @param error Error code
 * @return ESP_FAIL to indicate error
 */
esp_err_t not_found_handler(httpd_req_t* req, httpd_err_code_t error);

/**
 * @brief HTTP GET handler for index.html (redirects to root).
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t index_html_get_handler(httpd_req_t* req);

/**
 * @brief HTTP GET handler for WiFi status JSON.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t wifi_status_json_handler(httpd_req_t* req);

/**
 * @brief HTTP GET handler for serving files from SD card.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sd_file_handler(httpd_req_t* req);

/**
 * @brief Helper function to serve a file from SD card with appropriate caching headers.
 * 
 * @param req HTTP request handle
 * @param filepath Path to the file on SD card to serve
 * @param serve_mode Mode for serving the file (revalidate, no-cache, immutable)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t send_sd_file(httpd_req_t* req, const char* filepath);

/**
 * @brief HTTP GET handler for /restart endpoint (reboots device).
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t restart_handler(httpd_req_t *req);

/**
 * @brief HTTP GET handler when SD card is not present.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t no_sd_card_handler(httpd_req_t *req);

// WiFi event handler

/**
 * @brief Main WiFi and IP event handler callback.
 * 
 * Processes WiFi events (connect, disconnect, AP start, etc.) and
 * IP events (got IP, lost IP) to manage system state.
 * 
 * @param arg User argument (unused)
 * @param event_base Event base (WIFI_EVENT or IP_EVENT)
 * @param event_id Specific event ID
 * @param event_data Event-specific data
 */
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

#pragma endregion

#pragma region Initialization

/**
 * @brief Initialize WiFi and start the mode switch task.
 * 
 * Sets up event groups, network interfaces, HTTP server config,
 * and launches the WiFi mode switch FreeRTOS task.
 */
esp_err_t wifi_init() {
    esp_log_level_set("dns_redirect_server", CONFIG_LOG_LEVEL_WIFI < ESP_LOG_WARN ? CONFIG_LOG_LEVEL_WIFI : ESP_LOG_WARN); // Set log level for this module
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);
    esp_log_level_set(TAG_SD, CONFIG_LOG_LEVEL_WIFI);

    ESP_LOGI(TAG, "Initializing WiFi...");

    // Configure LED indicator
    led_indicator_strips_config_t led_indicator_strips_cfg = {
        .led_strip_cfg = {
            .strip_gpio_num = CONFIG_PIN_WIFI_STATUS_LED,  ///< GPIO number for the LED strip
            .max_leds = 1,               ///< Maximum number of LEDs in the strip
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,  ///< Pixel format
            .led_model = LED_MODEL_SK6812,  ///< LED driver model
            .flags.invert_out = 0,       ///< Invert output signal
        },
        .led_strip_driver = LED_STRIP_SPI,
        .led_strip_spi_cfg = {
            .clk_src = SPI_CLK_SRC_DEFAULT,  ///< SPI clock source
            .spi_bus = SPI3_HOST,  ///< SPI bus host
        },
    };
    led_indicator_config_t led_cfg = {
        .mode = LED_STRIPS_MODE,  ///< LED mode (e.g., LED_STRIP_MODE)
        .led_indicator_strips_config = &led_indicator_strips_cfg,
        .blink_lists = led_blink_list,
        .blink_list_num = BLINK_MAX, 
    };

    led_handle = led_indicator_create(&led_cfg);
    if (led_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create LED indicator");
    }
    
    led_indicator_start(led_handle, BLINK_LOADING); // Start LED indicator with loading animation

    if (mount_sd_card() == ESP_OK) {
        ESP_LOGI(TAG_SD, "SD card mounted successfully");
        SD_card_present = true;
    } else {
        ESP_LOGW(TAG_SD, "Falling back to basic server, running without SD card support");
        SD_card_present = false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register event handlers for WiFi and IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // Configure HTTP server
    httpd_config.lru_purge_enable = true;
    httpd_config.max_uri_handlers = CONFIG_WIFI_MAX_CUSTOM_HTTP_HANDLERS + 8;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_config.stack_size = 6144;  // Increase from default 4096 to handle captive portal detection bursts
    
    // Set up default HTTP server configuration
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    
    // Init WiFi with RAM-only storage
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Configure WiFi to store settings in RAM only
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_init_captive();

    // Decide startup mode based on saved wifi_mode and STA config
    if (get_wifi_mode() == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Configured for AP mode, switching to AP...");
        wifi_flags_set_bits(SWITCH_TO_AP_BIT);
    } else if (strlen((char *)get_sta_wifi_config().sta.ssid) == 0) {
        ESP_LOGI(TAG, "No STA SSID configured, launching captive portal AP mode...");
        wifi_flags_set_bits(SWITCH_TO_CAPTIVE_AP_BIT);
    } else {
        ESP_LOGI(TAG, "STA SSID configured, switching to STA mode...");
        wifi_flags_set_bits(SWITCH_TO_STA_BIT);
    }

    // Start WiFi mode switch task
    xTaskCreate(wifi_flags_listener_task, "wifi_flags_listener_task", 4096, NULL, 4, NULL);

    return ESP_OK;
}

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
    ESP_LOGI(TAG_SD, "Mounting SD card...");

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
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

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
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "Failed to mount SD card file system: %s", esp_err_to_name(ret));
        return ret;
    }

    SD_card_present = true;

    DIR *dir = opendir(SD_CARD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG_SD, "Failed to open SD card directory");
    } else {
        struct dirent *entry;
        ESP_LOGV(TAG_SD, "Files on SD card:");
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGV(TAG_SD, "  %s", entry->d_name);
        }
    }
    closedir(dir);

    return ESP_OK;
}

/**
 * @brief Synchronize system time with SNTP server.
 * 
 * Initializes the SNTP client to synchronize time with "pool.ntp.org". If
 * wait_for_sync is true, the function will block until the time is synchronized
 * or a timeout occurs.
 * 
 * @param wait_for_sync Whether to wait for time synchronization
 * @return ESP_OK on successful synchronization
 * @return ESP_ERR_TIMEOUT if synchronization fails after waiting
 */
esp_err_t sync_time(bool wait_for_sync) {
    ESP_LOGI(TAG, "Syncing time with NTP server...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    if (wait_for_sync) {
        // Wait for time to be set
        time_t now = 0;
        struct tm timeinfo = {0};
        int retry = 0;
        const int retry_count = 10;
        while (timeinfo.tm_year < (2020 - 1900) && retry < retry_count) {
            ESP_LOGD(TAG, "Waiting for SNTP time synchronization... (%d/%d)", retry + 1, retry_count);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            time(&now);
            localtime_r(&now, &timeinfo);
            retry++;
        }
        if (retry == retry_count) {
            ESP_LOGW(TAG, "SNTP time synchronization failed after %d attempts", retry_count);
            return ESP_ERR_TIMEOUT;
        } else {
            ESP_LOGI(TAG, "SNTP time synchronized successfully");
            return ESP_OK;
        }
    }
    return ESP_OK;
}

/**
 * @brief Initialize WiFi in station (client) mode.
 * 
 * Connects to the configured WiFi network, starts the HTTP server,
 * registers handlers, and optionally starts mDNS.
 */
void wifi_init_sta() {
    ESP_LOGI(TAG, "Starting WiFi in station mode...");
    
    wifi_config_t wifi_cfg = get_sta_wifi_config();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Set static IP if requested
    
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dhcpc_stop(sta_netif);
    bool use_static_ip;
    esp_ip4_addr_t ip_addr;
    get_static_ip_config(&use_static_ip, &ip_addr);
    if (use_static_ip) {
        uint32_t new_ip = ntohl(ip_addr.addr);
        ip_info.ip.addr = ip_addr.addr;
        ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
        esp_netif_set_ip_info(sta_netif, &ip_info);
    } else {
        esp_netif_dhcpc_start(sta_netif);
    }
    
    // Log IP address
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip_addr_str[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr_str, 16);
    ESP_LOGD(TAG, "Set up STA with IP: %s", ip_addr_str);

    ESP_LOGD(TAG, "Starting web server on port: %d", httpd_config.server_port);
    ESP_ERROR_CHECK(httpd_start(&server, &httpd_config));

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);

    // Register captive portal HTTP handlers (on /captive_portal for STA mode)
    register_captive_portal_handlers();

    httpd_uri_t index_html_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = index_html_get_handler
    };
    httpd_register_uri_handler(server, &index_html_uri);

    httpd_uri_t wifi_status_json_uri = {
        .uri = "/wifi-status.json",
        .method = HTTP_GET,
        .handler = wifi_status_json_handler,
    };
    httpd_register_uri_handler(server, &wifi_status_json_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler
    };
    httpd_register_uri_handler(server, &restart_uri);

    if (SD_card_present) {
        // Register custom handlers
        register_custom_http_handlers();

        httpd_uri_t sd_file_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = sd_file_handler
        };
        httpd_register_uri_handler(server, &sd_file_uri);

        #if CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
        httpd_uri_t spa_sd_file_uri = {
            .uri = "/*",
            .method = HTTP_HEAD,
            .handler = sd_file_handler
        };
        httpd_register_uri_handler(server, &spa_sd_file_uri);
        #endif

    } else {
        httpd_uri_t no_sd_card_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = no_sd_card_handler
        };
        httpd_register_uri_handler(server, &no_sd_card_uri);
    }

    bool use_mDNS;
    char mDNS_hostname[32];
    char service_name[32];
    get_mdns_config(&use_mDNS, mDNS_hostname, sizeof(mDNS_hostname), service_name, sizeof(service_name));
    // Start mDNS if enabled
    if (use_mDNS) {
        ESP_ERROR_CHECK(mdns_init());
        ESP_ERROR_CHECK(mdns_hostname_set(mDNS_hostname));
        ESP_ERROR_CHECK(mdns_instance_name_set(service_name));
        ESP_LOGI(TAG, "mDNS started: http://%s.local", mDNS_hostname);
        ESP_LOGI(TAG, "mDNS service started: %s", service_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

/**
 * @brief Initialize WiFi in AP mode.
 *
 * This is a stub and will be implemented later.
 */
void wifi_init_ap() {
    ESP_LOGI(TAG, "Starting WiFi in access point mode...");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t wifi_cfg = get_ap_wifi_config();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    int8_t max_tx_power;
    ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&max_tx_power));
    ESP_LOGI(TAG, "Max TX power is %d, setting to 44 (11dBm) for AP mode", max_tx_power);
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(44)); // 44 = 11dBm (~5-10m range)

    // Configure AP IP address
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dhcps_stop(ap_netif);  // Stop DHCP SERVER
    bool use_static_ip;
    esp_ip4_addr_t ip_addr;
    get_static_ip_config(&use_static_ip, &ip_addr);
    if (use_static_ip) {
        uint32_t new_ip = ntohl(ip_addr.addr);
        ip_info.ip.addr = ip_addr.addr;
        ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
    } else {
        // Default AP IP: 192.168.4.1
        ip_info.ip.addr = htonl((192 << 24) | (168 << 16) | (4 << 8) | 1);
        ip_info.gw.addr = ip_info.ip.addr;
        ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);
    }
    
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));  // Start DHCP SERVER
    
    // Log IP address
    char ip_addr_str[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr_str, 16);
    ESP_LOGI(TAG, "Set up AP with IP: %s", ip_addr_str);

    ESP_LOGD(TAG, "Starting web server on port: %d", httpd_config.server_port);
    ESP_ERROR_CHECK(httpd_start(&server, &httpd_config));

    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler));

    // Register captive portal HTTP handlers (on /captive_portal for STA mode)
    register_captive_portal_handlers();

    httpd_uri_t index_html_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = index_html_get_handler
    };
    httpd_register_uri_handler(server, &index_html_uri);

    httpd_uri_t wifi_status_json_uri = {
        .uri = "/wifi-status.json",
        .method = HTTP_GET,
        .handler = wifi_status_json_handler,
    };
    httpd_register_uri_handler(server, &wifi_status_json_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler
    };
    httpd_register_uri_handler(server, &restart_uri);

    if (SD_card_present) {
        // Register custom handlers
        register_custom_http_handlers();

        httpd_uri_t sd_file_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = sd_file_handler
        };
        httpd_register_uri_handler(server, &sd_file_uri);
        
        #if CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
        httpd_uri_t spa_sd_file_uri = {
            .uri = "/*",
            .method = HTTP_HEAD,
            .handler = sd_file_handler
        };
        httpd_register_uri_handler(server, &spa_sd_file_uri);
        #endif

    } else {
        // need to run wildcard handler even if no SD card to have captive redirect in AP mode
        httpd_uri_t no_sd_card_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = no_sd_card_handler
        };
        httpd_register_uri_handler(server, &no_sd_card_uri);
    }

    bool use_mDNS;
    char mDNS_hostname[32];
    char service_name[32];
    get_mdns_config(&use_mDNS, mDNS_hostname, sizeof(mDNS_hostname), service_name, sizeof(service_name));
    // Start mDNS if enabled
    if (use_mDNS) {
        ESP_ERROR_CHECK(mdns_init());
        ESP_ERROR_CHECK(mdns_hostname_set(mDNS_hostname));
        ESP_ERROR_CHECK(mdns_instance_name_set(service_name));
        ESP_LOGI(TAG, "mDNS started: http://%s.local", mDNS_hostname);
        ESP_LOGI(TAG, "mDNS service started: %s", service_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

/**
 * @brief Register a custom HTTP handler for use in STA/AP modes.
 * 
 * Stores the handler in a registry and registers it immediately if the server
 * is running in STA or AP mode (not captive portal mode). Handlers are
 * re-registered automatically when switching modes.
 * 
 * @param uri Pointer to httpd_uri_t structure defining the handler
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if uri or handler is NULL
 * @return ESP_ERR_NO_MEM if maximum handlers exceeded
 * @return Error code from httpd_register_uri_handler on registration failure
 */
esp_err_t wifi_register_http_handler(httpd_uri_t *uri) {
    if (uri == NULL || uri->uri == NULL || uri->handler == NULL) {
        ESP_LOGE(TAG, "Cannot register handler: uri or handler is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (custom_handler_count >= CONFIG_WIFI_MAX_CUSTOM_HTTP_HANDLERS) {
        ESP_LOGE(TAG, "Custom handler registry full");
        return ESP_ERR_NO_MEM;
    }
    custom_handlers[custom_handler_count] = *uri;
    custom_handler_count++;

    // Register immediately if server is running and in STA mode
    if (server) {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        
        bool is_captive_mode = false;
        if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
            wifi_config_t ap_config;
            esp_wifi_get_config(WIFI_IF_AP, &ap_config);
            is_captive_mode = (strcmp((char*)ap_config.ap.ssid, "ESP32_Captive_Portal") == 0);
        }
        
        if (!is_captive_mode) {
            esp_err_t err = httpd_register_uri_handler(server, uri);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register custom handler for %s: %s", uri->uri, esp_err_to_name(err));
            }
            return err;
        } else {
            ESP_LOGD(TAG, "Custom handler %s stored, will register when switching to STA/AP mode", uri->uri);
        }
    }
    return ESP_OK;
}

/**
 * @brief Register all stored custom HTTP handlers with the server.
 * 
 * Iterates through the custom handler registry and registers each handler
 * with the HTTP server. Logs errors but continues on failure.
 * 
 * @note Only registers if server handle is not NULL
 */
void register_custom_http_handlers(void) {
    if (server == NULL) return;
    for (size_t i = 0; i < custom_handler_count; ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &custom_handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register custom handler for %s: %s", custom_handlers[i].uri, esp_err_to_name(err));
        }
    }
}

#pragma endregion


#pragma region WiFi Config Helpers

void wifi_get_status(bool *out_connected_to_ap, bool *out_in_ap_mode, char **out_ip_str, char **out_ssid, char **out_ap_ssid) {
    EventBits_t bits = wifi_flags_get_bits();
    esp_netif_ip_info_t ip_info = {0};
    if (sta_netif) {
        esp_netif_get_ip_info(sta_netif, &ip_info);
    }

    bool connected = (bits & CONNECTED_BIT) != 0;
    bool in_ap = (bits & AP_MODE_BIT) != 0;

    if (out_connected_to_ap) {
        *out_connected_to_ap = connected;
    }
    if (out_in_ap_mode) {
        *out_in_ap_mode = in_ap;
    }

    if (out_ssid) {
        if (connected && strlen((const char*)get_sta_wifi_config().sta.ssid) > 0) {
            size_t len = strlen((const char*)get_sta_wifi_config().sta.ssid) + 1;
            *out_ssid = malloc(len);
            if (*out_ssid) {
                memcpy(*out_ssid, get_sta_wifi_config().sta.ssid, len);
            }
        } else {
            *out_ssid = NULL;
        }
    }

    if (out_ap_ssid) {
        if (in_ap && strlen((const char*)get_ap_wifi_config().ap.ssid) > 0) {
            size_t len = strlen((const char*)get_ap_wifi_config().ap.ssid) + 1;
            *out_ap_ssid = malloc(len);
            if (*out_ap_ssid) {
                memcpy(*out_ap_ssid, get_ap_wifi_config().ap.ssid, len);
            }
        } else {
            *out_ap_ssid = NULL;
        }
    }

    if (out_ip_str) {
        if (connected && ip_info.ip.addr != 0) {
            *out_ip_str = malloc(IP4ADDR_STRLEN_MAX);
            if (*out_ip_str) {
                esp_ip4addr_ntoa(&ip_info.ip, *out_ip_str, IP4ADDR_STRLEN_MAX);
            }
        } else {
            *out_ip_str = NULL;
        }
    }
}

#pragma endregion

#pragma region FreeRTOS Tasks

/**
 * @brief FreeRTOS task to handle WiFi mode switching and related events.
 * 
 * Waits for event bits to be set and performs actions such as switching
 * between STA/AP modes, reconnecting, or updating mDNS.
 * 
 * @param pvParameter Unused.
 */
void wifi_flags_listener_task(void *pvParameter) {
    while (1) {
        // Wait for any relevant event bit
        EventBits_t eventBits = wifi_flags_wait_for_bits(
            SWITCH_TO_STA_BIT | SWITCH_TO_AP_BIT | SWITCH_TO_CAPTIVE_AP_BIT | RECONECT_BIT | mDNS_CHANGE_BIT | SYNC_TIME_BIT,
            portMAX_DELAY);
        ESP_LOGV(TAG, "Received event bits: %c%c%c%c%c%c%c%c%c%c",
            eventBits & BIT9 ? '1' : '0',
            eventBits & BIT8 ? '1' : '0',
            eventBits & BIT7 ? '1' : '0',
            eventBits & BIT6 ? '1' : '0',
            eventBits & BIT5 ? '1' : '0',
            eventBits & BIT4 ? '1' : '0',
            eventBits & BIT3 ? '1' : '0',
            eventBits & BIT2 ? '1' : '0',
            eventBits & BIT1 ? '1' : '0',
            eventBits & BIT0 ? '1' : '0');
        vTaskDelay(100 / portTICK_PERIOD_MS);

        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
            mode = WIFI_MODE_NULL;
        }

        // Switch to STA mode
        if (eventBits & SWITCH_TO_STA_BIT) {
            ESP_LOGI(TAG, "Switching to STA mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_start(led_handle, BLINK_WIFI_CONNECTING);
            if (eventBits & CONNECTED_BIT) {
                ESP_LOGW(TAG, "Already connected to AP, no need to switch.");
                wifi_flags_clear_bits(SWITCH_TO_STA_BIT);
                continue;
            }
            if (server) {
                httpd_stop(server);
                server = NULL;
            }
            esp_wifi_stop();
            mdns_free(); // Free mDNS if exists
            wifi_flags_clear_bits(SWITCH_TO_STA_BIT);
            wifi_init_sta();
        }

        // Switch to AP mode (no captive hijack)
        if (eventBits & SWITCH_TO_AP_BIT) {
            ESP_LOGI(TAG, "Switching to AP mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_start(led_handle, BLINK_WIFI_AP_STARTING);
            if (server) {
                httpd_stop(server);
                server = NULL;
            }
            esp_wifi_disconnect();
            esp_wifi_stop();
            mdns_free(); // Free mDNS if exists
            wifi_init_ap();
            wifi_flags_clear_bits(SWITCH_TO_AP_BIT);
        }

        // Switch to captive AP mode
        if (eventBits & SWITCH_TO_CAPTIVE_AP_BIT) {
            ESP_LOGI(TAG, "Switching to AP captive portal mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_start(led_handle, BLINK_WIFI_AP_STARTING);
            if (server) {
                httpd_stop(server);
                server = NULL;
            }
            esp_wifi_disconnect();
            esp_wifi_stop();
            mdns_free(); // Free mDNS if exists
            wifi_start_captive();

            wifi_flags_clear_bits(SWITCH_TO_CAPTIVE_AP_BIT);
        }

        // Reconnect in STA mode
        if (eventBits & RECONECT_BIT && mode == WIFI_MODE_STA) {
            ESP_LOGD(TAG, "Reconnecting to AP...");
            esp_wifi_disconnect();
            ESP_LOGD(TAG, "Waiting for disconnect...");
            while (wifi_flags_get_bits() & CONNECTED_BIT) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            led_indicator_start(led_handle, BLINK_WIFI_CONNECTING);

            wifi_flags_clear_bits(RECONECT_BIT);

            wifi_config_t wifi_cfg = get_sta_wifi_config();
            esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

            // Set static or dynamic IP
            esp_netif_dhcpc_stop(sta_netif);
            bool use_static_ip; 
            esp_ip4_addr_t ip_addr;
            get_static_ip_config(&use_static_ip, &ip_addr);
            if (use_static_ip) {
                uint32_t new_ip = ntohl(ip_addr.addr);
                esp_netif_ip_info_t ip_info;
                ip_info.ip.addr = ip_addr.addr;
                ip_info.gw.addr = htonl((new_ip & 0xFFFFFF00)|0x01);    // x.x.x.1
                ip_info.netmask.addr = htonl((255 << 24) | (255 << 16) | (255 << 8) | 0);   // 255.255.255.0
                esp_netif_set_ip_info(sta_netif, &ip_info);
            } else {
                esp_netif_ip_info_t ip_info = {0};
                esp_netif_set_ip_info(sta_netif, &ip_info);
                esp_netif_dhcpc_start(sta_netif);
            }
            esp_wifi_connect();
        }

        // Update mDNS settings
        if (eventBits & mDNS_CHANGE_BIT && mode == WIFI_MODE_STA) {
            bool use_mDNS;
            char mDNS_hostname[32];
            char service_name[32];
            get_mdns_config(&use_mDNS, mDNS_hostname, sizeof(mDNS_hostname), service_name, sizeof(service_name));
            if (use_mDNS) {
                mdns_init(); // Initialize mDNS if not already done
                ESP_ERROR_CHECK(mdns_hostname_set(mDNS_hostname));
                ESP_ERROR_CHECK(mdns_instance_name_set(service_name));
                ESP_LOGD(TAG, "mDNS hostname updated: %s", mDNS_hostname);
                ESP_LOGD(TAG, "mDNS service name updated: %s", service_name);
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0); // Add mDNS service if not already done
            } else {
                mdns_free(); // Free mDNS if exists
                ESP_LOGD(TAG, "mDNS removed");
            }
            wifi_flags_clear_bits(mDNS_CHANGE_BIT);
        }

        // Sync time with NTP server
        if (eventBits & SYNC_TIME_BIT && eventBits & CONNECTED_BIT) {
            sync_time(true);
            wifi_flags_clear_bits(SYNC_TIME_BIT);
        }
    }
} 

#pragma endregion

#pragma region STA handlers

/**
 * @brief Check if a client IP has already been redirected to the captive portal.
 * 
 * @param ip Client IP address to check
 * @return true if IP has been redirected previously, false otherwise
 */
static bool is_ip_redirected(uint32_t ip) {
    for (int i = 0; i < redirected_count; i++) {
        if (redirected_ips[i] == ip) return true;
    }
    return false;
}

/**
 * @brief Mark a client IP as having been redirected to the captive portal.
 * 
 * Adds the IP to the redirected IPs list to prevent redirect loops.
 * 
 * @param ip Client IP address to mark
 */
static void mark_ip_redirected(uint32_t ip) {
    if (redirected_count < MAX_REDIRECTED_IPS) {
        redirected_ips[redirected_count++] = ip;
    }
}

/**
 * @brief HTTP handler for when SD card is not present.
 * 
 * Returns a 503 Service Unavailable status with a message prompting
 * the user to insert an SD card and restart.
 * 
 * @param req HTTP request handle
 * @return ESP_OK always
 */
esp_err_t no_sd_card_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>SD card not detected</h2>\n<p>Please insert an SD card and <a href=\"/restart\">restart</a> the device</p>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief HTTP handler for /index.html endpoint.
 * 
 * Redirects requests to the root path (/) for consistency.
 * 
 * @param req HTTP request handle
 * @return ESP_OK always
 */
esp_err_t index_html_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    ESP_LOGD(TAG, "Redirecting to /");
    return ESP_OK;
}

// Restart from a background task so the HTTP server can finish sending
// the response and close the connection gracefully before reboot.
static void restart_delayed_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    vTaskDelete(NULL);
}

/**
 * @brief HTTP handler for /restart endpoint (restarts ESP32).
 */
esp_err_t restart_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, "Restarting...", HTTPD_RESP_USE_STRLEN);

    BaseType_t r = xTaskCreate(restart_delayed_task, "restart_delayed", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) {
        // If task creation failed, fall back to delaying in-place (best-effort)
        esp_restart();
    }

    return ESP_OK;
}

/**
 * @brief HTTP error handler for 404.
 */
esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error) {
    #if CONFIG_WIFI_SHOW_SPA_404_HINT
    char text[300];
    #else
    char text[128];
    #endif
    size_t len = 0;
    len += snprintf(text + len, sizeof(text) - len, "404 Not Found\n\n");
    len += snprintf(text + len, sizeof(text) - len, "URI: %s\n", req->uri);
    len += snprintf(text + len, sizeof(text) - len, "Method: %s\n", (req->method == HTTP_GET) ? "GET" : "POST");
    len += snprintf(text + len, sizeof(text) - len, "Arguments:\n");
    #if CONFIG_WIFI_SHOW_SPA_404_HINT
    len += snprintf(text + len, sizeof(text) - len, "\nHint: If using an SPA framework (React, Vue, Angular, etc.) and see this error when routing directly, try setting the menuconfig option WIFI_SD_FILE_SERVING_MODE to SPA mode.");
    #endif
    char query[128];
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        httpd_req_get_url_query_str(req, query, query_len);
        len += snprintf(text + len, sizeof(text) - len, "%s\n", query);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    ESP_LOGW(TAG, "%s", text);
    return ESP_FAIL;
}

/**
 * @deprecated
 * @brief HTTP handler for returning current WiFi connection status as JSON.
 * 
 * Provides information on whether connected, current IP address, SSID, and AP mode status.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t wifi_status_json_handler(httpd_req_t *req) {
    char json[256];
    EventBits_t bits = wifi_flags_get_bits();
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    bool connected = (bits & CONNECTED_BIT) != 0;
    char ip_str[IP4ADDR_STRLEN_MAX];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, IP4ADDR_STRLEN_MAX);
    snprintf(json, sizeof(json), "{\"connected\": %s, \"ip\": \"%s\", \"ssid\": \"%s\", \"in_ap_mode\": %s, \"ap_ssid\": \"%s\"}",
             connected ? "true" : "false",
             ip_str,
             connected ? (const char*)get_sta_wifi_config().sta.ssid : "",
             (bits & AP_MODE_BIT) ? "true" : "false",
             (bits & AP_MODE_BIT) ? (const char*)get_ap_wifi_config().ap.ssid : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGV(TAG, "WiFi status JSON sent: %s", json);
    return ESP_OK;
}


/* Small MIME mapping - extend as needed */
static const char *mime_from_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++; // skip '.'
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "map") == 0) return "application/octet-stream";
    if (strcasecmp(ext, "woff") == 0) return "font/woff";
    if (strcasecmp(ext, "woff2") == 0) return "font/woff2";
    if (strcasecmp(ext, "ttf") == 0) return "font/ttf";
    if (strcasecmp(ext, "mp4") == 0) return "video/mp4";
    return "application/octet-stream";
}

/**
 * @brief HTTP handler for serving files from the SD card.
 * 
 * This handler serves files from the SD card mounted at /sdcard. It performs:
 * - Captive portal detection URL handling (redirects first request, returns 204 for subsequent)
 * - Directory index handling (serves index.html for directories)
 * - File extension-based content type detection
 * - Automatic .html extension appending for extensionless paths
 * 
 * Supported file types include HTML, CSS, JS, JSON, images, fonts, video, and more.
 * 
 * @param req HTTP request handle
 * @return ESP_OK on successful file serving
 * @return ESP_FAIL if file not found or cannot be opened
 */
esp_err_t sd_file_handler(httpd_req_t *req) {
    // Handle captive portal detection URLs from various operating systems
    // Android: /generate_204, /gen_204
    // iOS: /hotspot-detect.html
    // Windows: /ncsi.txt, /connecttest.txt
    // Generic: /success.txt, /redirect, /204
    if (!strcmp(req->uri, "/generate_204") ||
        !strcmp(req->uri, "/gen_204") ||
        !strcmp(req->uri, "/ncsi.txt") ||
        !strcmp(req->uri, "/connecttest.txt") ||
        !strcmp(req->uri, "/hotspot-detect.html") ||
        !strcmp(req->uri, "/success.txt") ||
        !strcmp(req->uri, "/redirect") ||
        !strcmp(req->uri, "/204") ||
        !strcmp(req->uri, "/ipv6check")) {
        
        ESP_LOGV(TAG, "Captive portal detection request: %s", req->uri);
        
        // Extract client IP address from socket for redirect tracking
        uint32_t client_ip = 0;
        int sockfd = httpd_req_to_sockfd(req);
        
        if (sockfd >= 0) {
            struct sockaddr_storage addr;
            socklen_t addr_len = sizeof(addr);
            
            if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
                if (addr.ss_family == AF_INET) {
                    // IPv4
                    struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
                    client_ip = addr_in->sin_addr.s_addr;
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, INET_ADDRSTRLEN);
                    ESP_LOGV(TAG, "Client IPv4 obtained: %s (0x%08X)", ip_str, (unsigned int)client_ip);
                } else if (addr.ss_family == AF_INET6) {
                    // IPv6 - use hash of last 4 bytes as identifier
                    struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&addr;
                    memcpy(&client_ip, &addr_in6->sin6_addr.s6_addr[12], 4);
                    char ip_str[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, &(addr_in6->sin6_addr), ip_str, INET6_ADDRSTRLEN);
                    ESP_LOGV(TAG, "Client IPv6 obtained: %s (hash: 0x%08X)", ip_str, (unsigned int)client_ip);
                } else {
                    ESP_LOGW(TAG, "Unknown address family: %d", addr.ss_family);
                }
            } else {
                ESP_LOGW(TAG, "getpeername failed: errno=%d (%s)", errno, strerror(errno));
            }
        } else {
            ESP_LOGW(TAG, "Invalid socket fd: %d", sockfd);
        }
        
        // Determine response based on request type and redirect tracking
        // For Microsoft NCSI (Windows) - return the exact expected content
        if (!strcmp(req->uri, "/ncsi.txt") || !strcmp(req->uri, "/connecttest.txt")) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        
        // First request from this IP: redirect to portal to open popup/browser
        // Subsequent requests from same IP: return 204 to indicate internet connectivity
        if ((client_ip != 0 && !is_ip_redirected(client_ip))||!strcmp(req->uri, "/redirect")) {
            mark_ip_redirected(client_ip);
            
            char location[64];
            bool use_mDNS;
            char mDNS_hostname[32];
            char service_name[32];
            get_mdns_config(&use_mDNS, mDNS_hostname, sizeof(mDNS_hostname), service_name, sizeof(service_name));            
            if (use_mDNS) {
                snprintf(location, sizeof(location), "http://%s.local/", mDNS_hostname);
            } else {
                esp_netif_ip_info_t ip_info;
                esp_netif_get_ip_info(ap_netif, &ip_info);
                char ip_addr[16];
                inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
                snprintf(location, sizeof(location), "http://%s/", ip_addr);
            }
            
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", location);
            httpd_resp_send(req, NULL, 0);
            ESP_LOGI(TAG, "First captive detection, redirecting to %s", location);
            return ESP_OK;
        }
        
        // Subsequent requests: return 204
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    #if CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
    bool is_navigation_request = true;
    const char *last_slash = strrchr(req->uri, '/');
    const char *last_dot = strrchr(req->uri, '.');
    if (last_dot && (!last_slash || last_dot > last_slash)) {
        ESP_LOGD(TAG, "Found an asset extension in URI: %s", req->uri);
        is_navigation_request = false;
    }
    ssize_t accept_len = httpd_req_get_hdr_value_len(req, "Accept");
    if (accept_len > 0) {
        char *accept = malloc(accept_len + 1);
        if (httpd_req_get_hdr_value_str(req, "Accept", accept, accept_len + 1) == ESP_OK) {
            // If Accept explicitly excludes HTML (no "text/html" and no "*/*"), not a navigation request
            if (strstr(accept, "text/html") == NULL && strstr(accept, "*/*") == NULL) {
                ESP_LOGD(TAG, "Accept header does not include text/html: %s", accept);
                is_navigation_request = false;
            }
        }
        free(accept);
    }
    if (is_navigation_request) {
        ESP_LOGD(TAG, "Handling as navigation request, serving index.html for URI: %s", req->uri);
        
        esp_err_t r = send_sd_file(req, "/index.html");
        if (r != ESP_OK) {
            ESP_LOGD(TAG, "Served index.html for navigation request");
            return not_found_handler(req, HTTPD_404_NOT_FOUND);
        }
        return ESP_OK;
    }
    #endif


    #if CONFIG_WIFI_SD_FILE_SERVING_MODE_STATIC
    // Construct full filesystem path from URI
    char filepath[530];
    snprintf(filepath, sizeof(filepath), "%s%s", SD_CARD_MOUNT_POINT, req->uri);

    // Handle directory requests by appending index.html
    struct stat st;
    size_t len = strlen(filepath);
    if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (len > 0 && filepath[len - 1] == '/') {
            snprintf(filepath + len, sizeof(filepath) - len, "index.html");
        } else {
            snprintf(filepath + len, sizeof(filepath) - len, "/index.html");
        }
    } else if (!strchr(req->uri, '.') && stat(filepath, &st) != 0) {
        // If URI has no extension and file doesn't exist, try adding .html
        snprintf(filepath + len, sizeof(filepath) - len, ".html");
    }
    #endif

    esp_err_t r = send_sd_file(req, req->uri);
    if (r == ESP_OK) {
        ESP_LOGD(TAG, "Served file: %s", req->uri);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to serve file: %s, error: %d", req->uri, r);
        return not_found_handler(req, HTTPD_404_NOT_FOUND);
    }
}

/* Heuristic: treat path as immutable if it is under /static/ or filename contains a long hex token before ext */
static bool is_immutable_path(const char *path)
{
    if (!path) return false;
    // path-based heuristic
    if (strstr(path, "/static/") == path || strstr(path, "/assets/") == path) return true;

    // filename hash heuristic: filename like main.1a2b3c4d.js
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    // look backwards for a '.' or '-' separating token before extension
    const char *hash = NULL;
    for (const char *p = name; p < dot; ++p) {
        if (*p == '.' || *p == '-') hash = p;
    }
    if (!hash) return false;
    size_t token_len = (size_t)(dot - hash - 1);
    if (token_len >= 8 && token_len <= 64) {
        // check if token is hex-like
        size_t hex_count = 0;
        for (const char *p = hash + 1; p < dot; ++p) {
            if (isxdigit((unsigned char)*p)) hex_count++;
        }
        if (hex_count == token_len) return true;
    }
    return false;
}

/* days_from_civil algorithm:
   Returns number of days since 1970-01-01 (Unix epoch) for given y,m,d.
   y: full year (e.g., 2026)
   m: 1..12
   d: 1..31
*/
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468; // 719468 = days from civil(0,3,1) to 1970-01-01
}

/* portable_timegm:
   Convert struct tm in UTC to time_t (seconds since 1970-01-01 UTC).
   tm->tm_year is years since 1900, tm->tm_mon is 0..11, tm->tm_mday is 1..31.
*/
time_t portable_timegm(struct tm *tm)
{
    if (!tm) return (time_t)-1;

    int64_t year = (int64_t)tm->tm_year + 1900;
    unsigned month = (unsigned)tm->tm_mon + 1; // make 1..12
    unsigned day = (unsigned)tm->tm_mday;

    int64_t days = days_from_civil(year, month, day);
    int64_t secs = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    return (time_t)secs;
}

/* Format RFC1123 date into buf (buf must be >= 32 bytes) */
static void format_rfc1123(time_t t, char *buf, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm); // convert to UTC
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* Try to parse RFC1123 timestamp to time_t. Returns true on success. Best-effort parsing. */
static bool parse_rfc1123(const char *s, time_t *out)
{
    if (!s || !out) return false;
#if defined(__GNUC__) || defined(__GLIBC__) || defined(_POSIX_TIMERS)
    struct tm tm = {0};
    char *res = strptime(s, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!res) {
        // try without weekday
        res = strptime(s, "%d %b %Y %H:%M:%S GMT", &tm);
        if (!res) return false;
    }
    // timegm is a GNU extension; try it first, otherwise fall back to mktime with TZ adjustment (best-effort)
#if defined(HAVE_TIMEGM) || defined(__USE_MISC) || defined(_BSD_SOURCE) || defined(_GNU_SOURCE)
    time_t t = portable_timegm(&tm);
    *out = t;
    return true;
#else
    // fallback: assume server local timezone is UTC (may be wrong on some systems)
    tm.tm_isdst = 0;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return false;
    *out = t;
    return true;
#endif
#else
    (void)s; (void)out;
    return false;
#endif
}

/* Build a weak ETag string from mtime and size (buf must be big enough) */
static void build_weak_etag(char *buf, size_t len, time_t mtime, uint64_t size)
{
    // W/"<mtime>-<size>"
    snprintf(buf, len, "W/\"%lx-%lx\"", (unsigned long)mtime, (unsigned long)size);
}

/* Main API: sends file from SD with conditional GET support and cache headers */
esp_err_t send_sd_file(httpd_req_t *req, const char *filepath)
{
    if (!req || !filepath) return ESP_ERR_INVALID_ARG;

    char fullpath[512];
    // ensure filepath begins with '/'
    const char *rel = filepath;
    if (rel[0] != '/') {
        // temporary buffer to build path
        snprintf(fullpath, sizeof(fullpath), "%s/%s", SD_CARD_MOUNT_POINT, rel);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", SD_CARD_MOUNT_POINT, rel);
    }

    struct stat st;
    if (stat(fullpath, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s: %s", fullpath, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    time_t mtime = st.st_mtime;
    uint64_t fsize = (uint64_t)st.st_size;

    /* Build ETag and Last-Modified */
    char etag[64];
    build_weak_etag(etag, sizeof(etag), mtime, fsize);

    char last_modified[64];
    format_rfc1123(mtime, last_modified, sizeof(last_modified));

    /* Read request conditional headers */
    bool not_modified = false;

    ssize_t inm_len = httpd_req_get_hdr_value_len(req, "If-None-Match");
    if (inm_len > 0) {
        char *inm = malloc(inm_len + 1);
        if (inm) {
            if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, inm_len + 1) == ESP_OK) {
                // simple substring match; handles weak/strong lists crudely
                if (strstr(inm, etag) != NULL) {
                    not_modified = true;
                }
            }
            free(inm);
        }
    }

    if (!not_modified) {
        ssize_t ims_len = httpd_req_get_hdr_value_len(req, "If-Modified-Since");
        if (ims_len > 0) {
            char *ims = malloc(ims_len + 1);
            if (ims) {
                if (httpd_req_get_hdr_value_str(req, "If-Modified-Since", ims, ims_len + 1) == ESP_OK) {
                    time_t when;
                    if (parse_rfc1123(ims, &when)) {
                        if (when >= mtime) {
                            not_modified = true;
                        }
                    }
                }
                free(ims);
            }
        }
    }

    /* Decide Cache-Control based on compile-time mode and immutability heuristic */
    const char *cache_control = NULL;
#ifdef CONFIG_WIFI_SD_FILE_SERVING_MODE_SPA
    bool immutable = is_immutable_path(filepath);
    if (immutable) {
        cache_control = "public, max-age=31536000, immutable";
    } else {
        // index and other HTML (short/no-cache), others short
        const char *ct = mime_from_path(filepath);
        if (ct && strcmp(ct, "text/html") == 0) {
            cache_control = "no-cache, must-revalidate";
        } else {
            cache_control = "public, max-age=60";
        }
    }
#else
    /* STATIC mode: default short cache, but long for immutable heuristics */
    bool immutable = is_immutable_path(filepath);
    if (immutable) {
        cache_control = "public, max-age=31536000, immutable";
    } else {
        cache_control = "public, max-age=60";
    }
#endif

    /* Set headers that should always be present */
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Last-Modified", last_modified);
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);

    /* If not modified, reply 304 */
    if (not_modified) {
        ESP_LOGD(TAG, "Not modified: %s", fullpath);
        httpd_resp_set_status(req, "304 Not Modified");
        // No body for 304
        return httpd_resp_send(req, NULL, 0);
    }

    /* Otherwise, stream the file */
    const char *mime = mime_from_path(filepath);
    if (mime) {
        httpd_resp_set_type(req, mime);
    }

    // HEAD should return headers only
    if (req->method == HTTP_HEAD) {
        return httpd_resp_send(req, NULL, 0);
    }

    FILE *f = fopen(fullpath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s (%s)", fullpath, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    // Stream in small chunks
    const size_t buf_size = 1024;
    char *buf = malloc(buf_size);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "OOM allocating stream buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t r;
    esp_err_t res = ESP_OK;
    while ((r = fread(buf, 1, buf_size, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            ESP_LOGW(TAG, "Client closed during send");
            res = ESP_FAIL;
            break;
        }
    }

    // finish chunked response
    if (res == ESP_OK) {
        if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to finalize chunked response");
            res = ESP_FAIL;
        }
    }

    free(buf);
    fclose(f);
    return res;
}

#pragma endregion

#pragma region Wifi Event Handler

/**
 * @brief WiFi and IP event handler.
 * 
 * Handles AP/STA connect/disconnect, IP acquisition, and triggers mode switches.
 */
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    EventBits_t bits = wifi_flags_get_bits();
    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Wi-Fi AP started."); 
        led_indicator_stop(led_handle, BLINK_WIFI_AP_STARTING);
        led_indicator_start(led_handle, BLINK_WIFI_AP_STARTED);
        wifi_flags_set_bits(AP_MODE_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "Wi-Fi AP stopped.");
        led_indicator_stop(led_handle, BLINK_WIFI_AP_STARTED);
        wifi_flags_clear_bits(AP_MODE_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGD(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGD(TAG, "station " MACSTR " leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && mode == WIFI_MODE_STA) {
        ESP_LOGI(TAG, "Wi-Fi STA started, connecting...");
        wifi_flags_clear_bits(AP_MODE_BIT);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "Connected to AP: %s", event->ssid);
        wifi_flags_set_bits(CONNECTED_BIT);
        sta_fails_count = 0;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        led_indicator_stop(led_handle, BLINK_WIFI_CONNECTING);
        led_indicator_stop(led_handle, BLINK_WIFI_CONNECTED);
        led_indicator_start(led_handle, BLINK_WIFI_DISCONNECTED);
        if ((bits & RECONECT_BIT) == 0 && mode == WIFI_MODE_STA && (bits & SWITCH_TO_CAPTIVE_AP_BIT) == 0) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
            sta_fails_count++;
            if (sta_fails_count >= CONFIG_WIFI_MAX_RECONNECTS) {
                ESP_LOGW(TAG, "Max STA reconect fails reached, switching to AP mode...");
                esp_wifi_disconnect();
                sta_fails_count = 0;
                wifi_flags_set_bits(SWITCH_TO_CAPTIVE_AP_BIT);
                return;
            } else {
                ESP_LOGD(TAG, "Reconnecting...");
                esp_wifi_connect();
                led_indicator_start(led_handle, BLINK_WIFI_CONNECTING);
            }
        } else {
            ESP_LOGI(TAG, "Wi-Fi disconnected.");
        } 
        wifi_flags_clear_bits(CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[IP4ADDR_STRLEN_MAX];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, IP4ADDR_STRLEN_MAX);
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        sta_fails_count = 0;
        led_indicator_stop(led_handle, BLINK_WIFI_CONNECTING);
        led_indicator_start(led_handle, BLINK_WIFI_CONNECTED);
        wifi_flags_set_bits(CONNECTED_BIT);
    } else {
        ESP_LOGW(TAG, "Unhandled event: %s:%ld", event_base, event_id);
    }
}

#pragma endregion

/**
 * @brief Manually set the LED color and brightness using RGB.
 * 
 * @param irgb LED color in 0xRRGGBB format
 * @param brightness LED brightness (0-255)
 */
void wifi_set_led_rgb(uint32_t irgb, uint8_t brightness) {
    if (led_handle) {
        led_indicator_set_rgb(led_handle, irgb);
        led_indicator_set_brightness(led_handle, brightness);
    }
}
