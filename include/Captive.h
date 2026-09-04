#pragma once
#include "esp_err.h"
#include "esp_wifi.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

#if CONFIG_WIFI_ENABLE_CAPTIVE_PORTAL

/**
 * @brief Initialize WiFi captive portal mode.
 */
esp_err_t wifi_init_captive(void);

/**
 * @brief Start WiFi in captive portal AP mode.
 *
 * Configures the device as an access point with DNS hijacking for captive portal.
 */
esp_err_t wifi_start_captive(void);

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
esp_err_t get_ap_wifi_config(wifi_config_t *cfg);

/**
 * @brief Create WiFi configuration for STA mode from captive portal config.
 * 
 * @param cfg Pointer to captive portal configuration
 * @return WiFi configuration structure for STA mode
 */
esp_err_t get_sta_wifi_config(wifi_config_t *cfg);

/**
 * @brief Create WiFi configuration for captive portal AP mode.
 *
 * Uses hardcoded SSID "ESP32_Captive_Portal" with no password.
 *
 * @param cfg Pointer to captive portal configuration (unused, for signature compatibility)
 * @return WiFi configuration structure for captive AP mode
 */
esp_err_t get_captive_ap_wifi_config(wifi_config_t *cfg);

esp_err_t get_mdns_config(bool *use_mDNS, char *hostname, size_t hostname_len, char *service_name, size_t service_name_len);
esp_err_t get_static_ip_config(bool *use_static_ip, esp_ip4_addr_t *static_ip);
wifi_mode_t get_wifi_mode(void);

/**
 * @brief Stop the captive portal DNS server if it is running.
 *
 * Call this when switching away from captive portal mode to release
 * the DNS server task and its associated resources.
 */
void wifi_stop_captive(void);

#else

#include <string.h>
#include "nvs-mgr.h"

/* Provide minimal static inline stubs when captive portal is disabled.
 * These return success and populate safe defaults so callers do not need
 * to be wrapped in feature guards. This prevents runtime errors and
 * undefined behavior when features are disabled. */

static inline esp_err_t wifi_init_captive(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t wifi_start_captive(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t register_captive_portal_handlers(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t get_captive_ap_wifi_config(wifi_config_t *cfg) { if (cfg) memset(cfg, 0, sizeof(*cfg)); return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t get_mdns_config(bool *use_mDNS, char *hostname, size_t hostname_len, char *service_name, size_t service_name_len) {
	if (use_mDNS) *use_mDNS = false;
	if (hostname && hostname_len > 0) hostname[0] = '\0';
	if (service_name && service_name_len > 0) service_name[0] = '\0';
	return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t get_static_ip_config(bool *use_static_ip, esp_ip4_addr_t *static_ip) {
	if (use_static_ip) *use_static_ip = false;
	if (static_ip) static_ip->addr = 0;
	return ESP_ERR_NOT_SUPPORTED;
}
static inline void wifi_stop_captive(void) { /* no-op */ }


/**
 * @brief Create WiFi configuration for station (client) mode.
 * 
 * This function fills the WiFi configuration structure with the
 * provided captive portal settings.
 * 
 * @param cfg Pointer to the captive portal configuration structure.
 * 
 * @return WiFi configuration structure for station mode.
 */
static inline esp_err_t get_sta_wifi_config(wifi_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_wifi_get_config(WIFI_IF_STA, cfg);
	captive_portal_config captive_cfg = {0};
	get_nvs_wifi_settings(&captive_cfg);

    strncpy((char *)cfg->sta.ssid, captive_cfg.ssid, sizeof(cfg->sta.ssid) - 1);
    ((char *)cfg->sta.ssid)[sizeof(cfg->sta.ssid) - 1] = '\0';
    if (captive_cfg.authmode == WIFI_AUTHMODE_OPEN) {
        cfg->sta.password[0] = '\0';
        cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        strncpy((char *)cfg->sta.password, captive_cfg.password, sizeof(cfg->sta.password) - 1);
        ((char *)cfg->sta.password)[sizeof(cfg->sta.password) - 1] = '\0';
        cfg->sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    return ESP_OK;
}
    
/**
 * @brief Create WiFi configuration for captive portal AP mode.
 * 
 * This function fills the WiFi configuration structure with the
 * provided captive portal settings.
 * 
 * @param cfg Pointer to the captive portal configuration structure.
 * 
 * @return WiFi configuration structure for AP mode.
 */
static inline esp_err_t get_ap_wifi_config(wifi_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_wifi_get_config(WIFI_IF_AP, cfg);
	captive_portal_config captive_cfg = {0};
	get_nvs_wifi_settings(&captive_cfg);

    /* Copy AP SSID/password into esp-idf structure with bounds checking */
    strncpy((char *)cfg->ap.ssid, captive_cfg.ap_ssid, sizeof(cfg->ap.ssid) - 1);
    ((char *)cfg->ap.ssid)[sizeof(cfg->ap.ssid) - 1] = '\0';
    strncpy((char *)cfg->ap.password, captive_cfg.ap_password, sizeof(cfg->ap.password) - 1);
    ((char *)cfg->ap.password)[sizeof(cfg->ap.password) - 1] = '\0';
    size_t ap_ssid_len = strnlen(captive_cfg.ap_ssid, sizeof(captive_cfg.ap_ssid));
    cfg->ap.ssid_len = (uint8_t)MIN(ap_ssid_len, sizeof(cfg->ap.ssid));
    cfg->ap.max_connection = 4;

    if (captive_cfg.ap_password[0] == 0) {
        cfg->ap.authmode = WIFI_AUTH_OPEN;
    } else {
        cfg->ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    return ESP_OK;
}

static inline wifi_mode_t get_wifi_mode() {
    wifi_mode_t mode;
	captive_portal_config captive_cfg = {0};
	get_nvs_wifi_settings(&captive_cfg);
    mode = captive_cfg.wifi_mode;
    return mode;
}

#endif