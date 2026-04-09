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
#include "Server-mgr.h"
#include "Runtime-handlers.h"
#include "SD-mgr.h"
#include "AP.h"
#include "STA.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"      // for MAC2STR macro
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_indicator.h"

#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "mdns.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "time.h"
#include "esp_sntp.h"

#pragma region Variables & Config

/** @brief Log tag for general WiFi module messages */
static const char *TAG = "Wifi";

/** @brief Counter for consecutive STA connection failures */
static int sta_fails_count = 0;

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
    
    wifi_flags_init();

    if (mount_sd_card() == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted successfully");
    } else {
        ESP_LOGW(TAG, "Falling back to basic server, running without SD card support");
    }

    ESP_ERROR_CHECK(server_mgr_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register event handlers for WiFi and IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    
    // Set up default HTTP server configuration
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    // Init WiFi with RAM-only storage
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Configure WiFi to store settings in RAM only
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(wifi_init_captive());

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

    ESP_LOGI(TAG, "WiFi initialization complete");
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

esp_netif_t *wifi_get_ap_netif(void) {
    return ap_netif;
}

esp_netif_t *wifi_get_sta_netif(void) {
    return sta_netif;
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
    ESP_LOGI(TAG, "WiFi flags listener task started");
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
            server_mgr_stop();
            esp_wifi_stop();
            wifi_stop_captive(); // Stop DNS server if running
            mdns_free(); // Free mDNS if exists
            wifi_flags_clear_bits(SWITCH_TO_STA_BIT);
            wifi_init_sta();
        }

        // Switch to AP mode (no captive hijack)
        if (eventBits & SWITCH_TO_AP_BIT) {
            ESP_LOGI(TAG, "Switching to AP mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_stop(led_handle, BLINK_WIFI_CONNECTING);
            led_indicator_start(led_handle, BLINK_WIFI_AP_STARTING);
            server_mgr_stop();
            esp_wifi_disconnect();
            esp_wifi_stop();
            wifi_stop_captive(); // Stop DNS server if running
            mdns_free(); // Free mDNS if exists
            wifi_init_ap();
            wifi_flags_clear_bits(SWITCH_TO_AP_BIT);
        }

        // Switch to captive AP mode
        if (eventBits & SWITCH_TO_CAPTIVE_AP_BIT) {
            ESP_LOGI(TAG, "Switching to AP captive portal mode...");
            led_indicator_stop(led_handle, BLINK_LOADING);
            led_indicator_start(led_handle, BLINK_WIFI_AP_STARTING);
            server_mgr_stop();
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
        led_indicator_stop(led_handle, BLINK_WIFI_AP_STARTING);
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
