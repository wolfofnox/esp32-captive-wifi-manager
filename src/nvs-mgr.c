#include "nvs-mgr.h"
#include "Wifi.h"

#include "sdkconfig.h"
#include "Captive.h"

#include "helpers.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_check.h"


/** @brief NVS namespace used for storing WiFi credentials and settings */
static const char *NVS_NAMESPACE_WIFI = "wifi_settings";

static const char *TAG = "Wifi: NVS Manager";

/**
 * @brief Read WiFi configuration from NVS flash storage.
 * 
 * Opens the WiFi settings namespace and reads all saved configuration values
 * into the provided structure. If values don't exist, they remain unchanged.
 * 
 * @param cfg Pointer to captive_portal_config structure to populate
 */
esp_err_t get_nvs_wifi_settings(captive_portal_config *cfg) {
    ESP_LOGD(TAG, "Reading WiFi settings...");
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Invalid configuration pointer (== NULL)");
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(cfg->ssid);
        nvs_get_str(nvs_handle, "ssid", cfg->ssid, &len);
        len = sizeof(cfg->password);
        nvs_get_str(nvs_handle, "password", cfg->password, &len);
        nvs_get_u8(nvs_handle, "authmode", (uint8_t*)&cfg->authmode);
        len = sizeof(cfg->ap_ssid);
        nvs_get_str(nvs_handle, "ap_ssid", cfg->ap_ssid, &len);
        len = sizeof(cfg->ap_password);
        nvs_get_str(nvs_handle, "ap_password", cfg->ap_password, &len);
        nvs_get_u8(nvs_handle, "use_static_ip", (uint8_t*)&cfg->use_static_ip);
        nvs_get_u8(nvs_handle, "use_mDNS", (uint8_t*)&cfg->use_mDNS);
        nvs_get_u32(nvs_handle, "static_ip", &cfg->static_ip.addr);
        len = sizeof(cfg->mDNS_hostname);
        nvs_get_str(nvs_handle, "mDNS_hostname", cfg->mDNS_hostname, &len);
        len = sizeof(cfg->service_name);
        nvs_get_str(nvs_handle, "service_name", cfg->service_name, &len);
        uint8_t mode_u8;
        if (nvs_get_u8(nvs_handle, "wifi_mode", &mode_u8) == ESP_OK) {
            cfg->wifi_mode = (wifi_mode_t)mode_u8;
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open namespace: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/**
 * @brief Write WiFi configuration to NVS flash storage.
 * 
 * Compares the provided configuration with currently saved values and only
 * writes changed settings to minimize flash wear. Commits changes atomically.
 * 
 * @param cfg Pointer to captive_portal_config structure with values to save
 */
esp_err_t set_nvs_wifi_settings(captive_portal_config *cfg) {
    ESP_LOGD(TAG, "Writing WiFi settings...");
    int8_t n = 0;
    nvs_handle_t nvs_handle;
    captive_portal_config saved_cfg = {0};
    fill_captive_portal_config_struct(&saved_cfg);
    get_nvs_wifi_settings(&saved_cfg);
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        if (strcmp(cfg->ssid, saved_cfg.ssid) != 0) {
            nvs_set_str(nvs_handle, "ssid", cfg->ssid);
            n++;
        }
        if (strcmp(cfg->password, saved_cfg.password) != 0) {
            nvs_set_str(nvs_handle, "password", cfg->password);
            n++;
        }
        if (cfg->authmode != saved_cfg.authmode) {
            nvs_set_u8(nvs_handle, "authmode", (uint8_t)cfg->authmode);
            n++;
        }
        if (strcmp(cfg->ap_ssid, saved_cfg.ap_ssid) != 0) {
            nvs_set_str(nvs_handle, "ap_ssid", cfg->ap_ssid);
            n++;
        }
        if (strcmp(cfg->ap_password, saved_cfg.ap_password) != 0) {
            nvs_set_str(nvs_handle, "ap_password", cfg->ap_password);
            n++;
        }
        if (cfg->use_static_ip != saved_cfg.use_static_ip) {
            nvs_set_u8(nvs_handle, "use_static_ip", (uint8_t)cfg->use_static_ip);
            n++;
        }
        if (cfg->use_mDNS != saved_cfg.use_mDNS) {
            nvs_set_u8(nvs_handle, "use_mDNS", (uint8_t)cfg->use_mDNS);
            n++;
        }
        if (cfg->static_ip.addr != saved_cfg.static_ip.addr) {
            nvs_set_u32(nvs_handle, "static_ip", cfg->static_ip.addr);
            n++;
        }
        if (strcmp(cfg->mDNS_hostname, saved_cfg.mDNS_hostname) != 0) {
            nvs_set_str(nvs_handle, "mDNS_hostname", cfg->mDNS_hostname);
            n++;
        }
        if (strcmp(cfg->service_name, saved_cfg.service_name) != 0) {
            nvs_set_str(nvs_handle, "service_name", cfg->service_name);
            n++;
        }
        if (cfg->wifi_mode != saved_cfg.wifi_mode) {
            nvs_set_u8(nvs_handle, "wifi_mode", (uint8_t)cfg->wifi_mode);
            n++;
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGD(TAG, "WiFi settings written, %d changes made", n);
    } else {
        ESP_LOGW(TAG, "Failed to open namespace: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/**
 * @brief Initialize NVS flash storage.
 */
esp_err_t init_nvs() {
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize NVS");
    return ESP_OK;
}