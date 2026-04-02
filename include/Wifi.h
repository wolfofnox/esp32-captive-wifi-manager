/**
 * @file Wifi.h
 * @brief WiFi management and captive portal interface for ESP32
 * 
 * This header provides the public API for WiFi connection management,
 * captive portal functionality, and HTTP handler registration.
 */

#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include "helpers.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"

/**
 * @brief Initialize the WiFi manager and start network services.
 * 
 * This function performs complete WiFi initialization including:
 * - NVS initialization for credential storage
 * - WiFi stack initialization
 * - HTTP server setup
 * - LED indicator initialization
 * - SD card mounting (if available)
 * - Mode selection based on saved configuration
 * 
 * @return ESP_OK on success
 * @return ESP_FAIL or other error code on failure
 */
esp_err_t wifi_init();

/**
 * @brief Type definition for HTTP handler functions.
 * 
 * Custom HTTP handlers must match this signature.
 */
typedef esp_err_t (*wifi_http_handler_t)(httpd_req_t *r);

/**
 * @brief Register a custom HTTP handler with the web server.
 * 
 * Allows applications to add custom HTTP endpoints that will be served
 * alongside the WiFi management interface. Maximum of 8 custom handlers
 * can be registered (CONFIG_WIFI_MAX_CUSTOM_HTTP_HANDLERS).
 * 
 * @param uri Pointer to httpd_uri_t structure defining the endpoint
 *            (uri path, method, handler function, etc.)
 * 
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if uri or handler is NULL
 * @return ESP_ERR_NO_MEM if maximum handlers exceeded
 * @return ESP_FAIL if registration with HTTP server failed
 */
esp_err_t wifi_register_http_handler(httpd_uri_t *uri);

/**
 * @brief Manually set the status LED color and brightness.
 * 
 * Overrides the automatic LED status indication to display a custom
 * color. Useful for application-specific status indication.
 * 
 * @param irgb LED color in RGB format (0x00RRGGBB)
 * @param brightness Brightness level (0-255)
 * 
 * @note This is a deprecated feature and the LED interface may be improved in future versions.
 */
void wifi_set_led_rgb(uint32_t irgb, uint8_t brightness);

/**
 * @brief Get the current WiFi connection status and configuration.
 * 
 * Retrieves the current WiFi connection status, including whether connected
 * to an AP, whether in AP mode, current IP address, and SSIDs. The caller is responsible for providing valid pointers for output parameters.
 * 
 * @param out_connected_to_ap Pointer to bool that will be set to true if connected to an AP, false otherwise
 * @param out_in_ap_mode Pointer to bool that will be set to true if in AP mode, false otherwise
 * @param out_ip_str Pointer to char* that will be set to a newly allocated string containing the current IP address (caller must free), or NULL if not connected
 * @param out_ssid Pointer to char* that will be set to a newly allocated string containing the connected SSID (caller must free), or NULL if not connected
 * @param out_ap_ssid Pointer to char* that will be set to a newly allocated string containing the AP SSID if in AP mode (caller must free), or NULL if not in AP mode
 */
void wifi_get_status(bool *out_connected_to_ap, bool *out_in_ap_mode, char **out_ip_str, char **out_ssid, char **out_ap_ssid); 

#endif