#pragma once
#include "esp_err.h"
#include "esp_wifi.h"

/**
 * @brief Initialize WiFi captive portal mode.
 */
esp_err_t wifi_init_captive();

/**
 * @brief Start WiFi in captive portal AP mode.
 * 
 * Configures the device as an access point with DNS hijacking for captive portal.
 */
esp_err_t wifi_start_captive();

/**
 * @brief Register captive portal HTTP handlers with the server.
 * 
 * Registers handlers for /captive, /captive.json, and /scan.json endpoints.
 */
esp_err_t register_captive_portal_handlers(void);

/**
 * @brief Create WiFi configuration for AP mode from captive portal config.
 * 
 * @param cfg Pointer to captive portal configuration
 * @return WiFi configuration structure for AP mode
 */
wifi_config_t get_ap_wifi_config();

/**
 * @brief Create WiFi configuration for STA mode from captive portal config.
 * 
 * @param cfg Pointer to captive portal configuration
 * @return WiFi configuration structure for STA mode
 */
wifi_config_t get_sta_wifi_config();

/**
 * @brief Create WiFi configuration for captive portal AP mode.
 * 
 * Uses hardcoded SSID "ESP32_Captive_Portal" with no password.
 * 
 * @param cfg Pointer to captive portal configuration (unused, for signature compatibility)
 * @return WiFi configuration structure for captive AP mode
 */
wifi_config_t get_captive_ap_wifi_config();

esp_err_t get_mdns_config(bool *use_mDNS, char *hostname, size_t hostname_len, char *service_name, size_t service_name_len);
esp_err_t get_static_ip_config(bool *use_static_ip, esp_ip4_addr_t *static_ip);
wifi_mode_t get_wifi_mode();
