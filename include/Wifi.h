/**
 * @file Wifi.h
 * @brief WiFi management and captive portal interface for ESP32
 * 
 * This header provides the public API for WiFi connection management,
 * captive portal functionality, and HTTP handler registration.
 */

#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"

// Authentication mode constants
#define WIFI_AUTHMODE_OPEN         0           ///< Open network (no authentication)
#define WIFI_AUTHMODE_WPA_PSK      1           ///< WPA/WPA2-Personal (password-based)
#define WIFI_AUTHMODE_ENTERPRISE   2           ///< WPA2/WPA3-Enterprise
#define WIFI_AUTHMODE_INVALID      ((uint8_t)-1) ///< Invalid/unknown authentication mode

/**
 * @brief Configuration structure for captive portal and WiFi settings.
 * 
 * This structure holds all WiFi and network configuration settings, including
 * credentials, IP configuration, mDNS settings, and AP configuration.
 */
typedef struct {
    char ssid[32];              ///< SSID of the WiFi network to connect to (STA mode)
    uint8_t authmode;           ///< Authentication mode: WIFI_AUTHMODE_OPEN, WIFI_AUTHMODE_WPA_PSK, or WIFI_AUTHMODE_ENTERPRISE
    char username[64];          ///< Username for WPA2-Enterprise authentication (currently unused)
    char password[64];          ///< Password for the WiFi network
    bool use_static_ip;         ///< Use static IP if true, DHCP otherwise
    esp_ip4_addr_t static_ip;   ///< Static IP address (only used if use_static_ip is true)
    bool use_mDNS;              ///< Enable mDNS service discovery if true
    char mDNS_hostname[32];     ///< mDNS hostname (e.g., "esp32" becomes "esp32.local")
    char service_name[64];      ///< mDNS service name for service advertisement (e.g., "ESP32 Web Server")
    char ap_ssid[32];           ///< SSID of the access point when in AP mode
    char ap_password[64];       ///< Password for the access point (empty string for open AP)
    wifi_mode_t wifi_mode;      ///< WiFi mode: WIFI_MODE_STA (client), WIFI_MODE_AP (access point), or WIFI_MODE_APSTA (both)
} captive_portal_config;

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
 * @brief Decode a URL-encoded string in place.
 * 
 * Converts URL-encoded characters (like %20 for space, + for space)
 * to their normal ASCII representation. The string is modified in place.
 * 
 * @param str Pointer to null-terminated string to decode (modified in place)
 * 
 * @note Useful for processing form data from HTTP POST requests.
 */
void url_decode(char *str);

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