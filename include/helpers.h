#pragma once
#include "esp_err.h"
#include "esp_wifi.h"

typedef enum {
    WIFI_AUTHMODE_OPEN = 0,
    WIFI_AUTHMODE_WPA_PSK = 1,
    WIFI_AUTHMODE_ENTERPRISE = 2,
    WIFI_AUTHMODE_INVALID = 255
} wifi_captive_auth_mode_t;

/**
 * @brief Configuration structure for captive portal and WiFi settings.
 * 
 * This structure holds all WiFi and network configuration settings, including
 * credentials, IP configuration, mDNS settings, and AP configuration.
 */
typedef struct {
    char ssid[33];              ///< SSID of the WiFi network to connect to (STA mode) — match esp-idf AP record size (33)
    wifi_captive_auth_mode_t authmode;           ///< Authentication mode: WIFI_AUTHMODE_OPEN, WIFI_AUTHMODE_WPA_PSK, or WIFI_AUTHMODE_ENTERPRISE
    char username[64];          ///< Username for WPA2-Enterprise authentication (currently unused)
    char password[64];          ///< Password for the WiFi network
    bool use_static_ip;         ///< Use static IP if true, DHCP otherwise
    esp_ip4_addr_t static_ip;   ///< Static IP address (only used if use_static_ip is true)
    bool use_mDNS;              ///< Enable mDNS service discovery if true
    char mDNS_hostname[32];     ///< mDNS hostname (e.g., "esp32" becomes "esp32.local")
    char service_name[64];      ///< mDNS service name for service advertisement (e.g., "ESP32 Web Server")
    char ap_ssid[33];           ///< SSID of the access point when in AP mode — match esp-idf AP record size (33)
    char ap_password[64];       ///< Password for the access point (empty string for open AP)
    wifi_mode_t wifi_mode;      ///< WiFi mode: WIFI_MODE_STA (client), WIFI_MODE_AP (access point)
} captive_portal_config;

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
static inline void url_decode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            int hi = src[1], lo = src[2];
            hi = (hi >= 'A') ? (hi & ~0x20) - 'A' + 10 : hi - '0';
            lo = (lo >= 'A') ? (lo & ~0x20) - 'A' + 10 : lo - '0';
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Fill the captive portal configuration structure with empty values.
 * 
 * This function initializes the captive portal configuration structure
 * with empty data to ensure it is ready for use.
 * 
 * @param cfg Pointer to the captive portal configuration structure to fill.
 */
static inline void fill_captive_portal_config_struct(captive_portal_config *cfg) {
    cfg->ssid[0] = '\0';
    cfg->password[0] = '\0';
    cfg->use_static_ip = false;
    cfg->static_ip.addr = 0;
    cfg->use_mDNS = false;
    cfg->mDNS_hostname[0] = '\0';
    cfg->service_name[0] = '\0';
    cfg->ap_ssid[0] = '\0';
    cfg->ap_password[0] = '\0';
    cfg->authmode = WIFI_AUTHMODE_OPEN;
    cfg->wifi_mode = WIFI_MODE_STA;  // Default to station mode
}
