#include "Captive.h"

#include "sdkconfig.h"

#include "Flags.h"
#include "Server-mgr.h"

#include "helpers.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"

#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "dns_server.h"   // for captive portal DNS hijack
#include "lwip/inet.h"
#include "esp_http_server.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

typedef enum {
    WIFI_AUTHMODE_OPEN = 0,
    WIFI_AUTHMODE_WPA_PSK = 1,
    WIFI_AUTHMODE_ENTERPRISE = 2,
    WIFI_AUTHMODE_INVALID = -1
} wifi_captive_auth_mode_t;

/**
 * @brief Configuration structure for captive portal and WiFi settings.
 * 
 * This structure holds all WiFi and network configuration settings, including
 * credentials, IP configuration, mDNS settings, and AP configuration.
 */
typedef struct {
    char ssid[32];              ///< SSID of the WiFi network to connect to (STA mode)
    wifi_captive_auth_mode_t authmode;           ///< Authentication mode: WIFI_AUTHMODE_OPEN, WIFI_AUTHMODE_WPA_PSK, or WIFI_AUTHMODE_ENTERPRISE
    char username[64];          ///< Username for WPA2-Enterprise authentication (currently unused)
    char password[64];          ///< Password for the WiFi network
    bool use_static_ip;         ///< Use static IP if true, DHCP otherwise
    esp_ip4_addr_t static_ip;   ///< Static IP address (only used if use_static_ip is true)
    bool use_mDNS;              ///< Enable mDNS service discovery if true
    char mDNS_hostname[32];     ///< mDNS hostname (e.g., "esp32" becomes "esp32.local")
    char service_name[64];      ///< mDNS service name for service advertisement (e.g., "ESP32 Web Server")
    char ap_ssid[32];           ///< SSID of the access point when in AP mode
    char ap_password[64];       ///< Password for the access point (empty string for open AP)
    wifi_mode_t wifi_mode;      ///< WiFi mode: WIFI_MODE_STA (client), WIFI_MODE_AP (access point)
} captive_portal_config;

/** @brief Current captive portal and WiFi configuration */
captive_portal_config captive_cfg = { 0 };

// HTML page binary symbols (linked at build time, defined in CMakeLists.txt)
/** @brief Start address of embedded captive portal HTML page */
extern const char captive_html_start[] asm("_binary_captive_html_start");

/** @brief End address of embedded captive portal HTML page */
extern const char captive_html_end[] asm("_binary_captive_html_end");

/** @brief NVS namespace used for storing WiFi credentials and settings */
static const char *NVS_NAMESPACE_WIFI = "wifi_settings";

static const char *TAG = "Wifi: Captive";

extern esp_netif_t *ap_netif;

/**
 * @brief Fill the captive portal configuration structure with empty values.
 * 
 * This function initializes the captive portal configuration structure
 * with empty data to ensure it is ready for use.
 * 
 * @param cfg Pointer to the captive portal configuration structure to fill.
 */
static inline void fill_captive_portal_config_struct(captive_portal_config *cfg) {
    strcpy(cfg->ssid, "");
    strcpy(cfg->password, "");
    cfg->use_static_ip = false;
    cfg->static_ip.addr = 0;
    cfg->use_mDNS = false;
    strcpy(cfg->mDNS_hostname, "");
    strcpy(cfg->service_name, "");
    strcpy(cfg->ap_ssid, "");
    strcpy(cfg->ap_password, "");
    cfg->authmode = WIFI_AUTHMODE_OPEN;
    cfg->wifi_mode = WIFI_MODE_STA;  // Default to station mode
}

/**
 * @brief Read WiFi configuration from NVS flash storage.
 * 
 * Opens the WiFi settings namespace and reads all saved configuration values
 * into the provided structure. If values don't exist, they remain unchanged.
 * 
 * @param cfg Pointer to captive_portal_config structure to populate
 */
void get_nvs_wifi_settings(captive_portal_config *cfg) {
    ESP_LOGD(TAG, "Reading NVS WiFi settings...");
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Invalid configuration pointer (== NULL)");
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
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Write WiFi configuration to NVS flash storage.
 * 
 * Compares the provided configuration with currently saved values and only
 * writes changed settings to minimize flash wear. Commits changes atomically.
 * 
 * @param cfg Pointer to captive_portal_config structure with values to save
 */
void set_nvs_wifi_settings(captive_portal_config *cfg) {
    ESP_LOGD(TAG, "Writing NVS WiFi settings...");
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
        ESP_LOGD(TAG, "NVS WiFi settings written, %d changes made", n);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
    }
}

/**
 * @brief HTTP handler for serving the captive portal HTML page.
 */
esp_err_t captive_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const uint32_t captive_html_len = captive_html_end - captive_html_start;
    httpd_resp_send(req, (const char *)captive_html_start, captive_html_len);
    ESP_LOGD(TAG, "Captive portal page served");
    return ESP_OK;
}

/**
 * @brief HTTP error handler for redirecting to the captive portal.
 */
esp_err_t captive_error_redirect(httpd_req_t *req, httpd_err_code_t error) {
    httpd_resp_set_status(req, "302 Temporary Redirect");
    ESP_LOGD(TAG, "Redirecting to captive portal URI: /captive");
    httpd_resp_set_hdr(req, "Location", "/captive");
    httpd_resp_send(req, "Redirected to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief HTTP handler for scanning available WiFi networks and returning JSON results.
 */
esp_err_t scan_json_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Scan request received, starting WiFi scan...");
    char json[700];
    uint16_t ap_count = 0;
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 0
    };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGD(TAG, "Found %d access points", ap_count);
    if (ap_count > CONFIG_WIFI_SCAN_MAX_APS) {
        ap_count = CONFIG_WIFI_SCAN_MAX_APS;
        ESP_LOGD(TAG, "Limiting to %d access points", ap_count);
    }
    wifi_ap_record_t ap_records[CONFIG_WIFI_SCAN_MAX_APS];
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    int len = snprintf(json, sizeof(json), "{\"ap_count\": %d, \"aps\": [", ap_count);
    for (int i = 0; i < ap_count; i++) {
        char ssid[33];
        uint8_t authmode;
        snprintf(ssid, sizeof(ssid), "%s", ap_records[i].ssid);
        if (ap_records[i].authmode == WIFI_AUTH_OPEN || ap_records[i].authmode == WIFI_AUTH_OWE || ap_records[i].authmode == WIFI_AUTH_DPP) {
            authmode = WIFI_AUTHMODE_OPEN;
        } else if (ap_records[i].authmode == WIFI_AUTH_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA2_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA3_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || ap_records[i].authmode == WIFI_AUTH_WPA3_ENT_192 || ap_records[i].authmode == WIFI_AUTH_WPA_ENTERPRISE) {
            authmode = WIFI_AUTHMODE_ENTERPRISE;
        } else {
            authmode = WIFI_AUTHMODE_WPA_PSK;
        }
        len += snprintf(json + len, sizeof(json) - len,
        "%s{\"ssid\": \"%s\", \"rssi\": %d, \"authmode\": %d}",
            (i > 0) ? "," : "",
            ssid,
            ap_records[i].rssi,
            authmode);
        if (len < 0 || len >= sizeof(json)) {
            // Buffer full, truncate
            break;
        }
    }
    if ((len + 2) < sizeof(json)) {
        json[len++] = ']';
        json[len++] = '}';
        json[len] = '\0';
    } else {
        // Buffer full, ensure valid JSON
        strncpy(json + sizeof(json) - 3, "]}", 3);
        json[sizeof(json) - 1] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGD(TAG, "Scan results sent: %d APs; JSON: %s", ap_count, json);
    return ESP_OK;
}

/**
 * @brief HTTP handler for returning saved captive portal configuration as JSON.
 */
esp_err_t captive_json_handler(httpd_req_t *req) {
    char json[512];
    snprintf(json, sizeof(json),
        "{\"ssid\": \"%s\", \"authmode\": %d, \"password\": \"%s\", \"use_static_ip\": %s, \"static_ip\": \"%s\", \"use_mDNS\": %s, \"mDNS_hostname\": \"%s\", \"service_name\": \"%s\", \"wifi_mode\": %d, \"ap_ssid\": \"%s\", \"ap_password\": \"%s\"}",
        captive_cfg.ssid,
        captive_cfg.authmode,
        captive_cfg.password,
        captive_cfg.use_static_ip ? "true" : "false",
        inet_ntoa(captive_cfg.static_ip.addr),
        captive_cfg.use_mDNS ? "true" : "false",
        captive_cfg.mDNS_hostname,
        captive_cfg.service_name,
        captive_cfg.wifi_mode,
        captive_cfg.ap_ssid,
        captive_cfg.ap_password
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGD(TAG, "Captive portal JSON data sent: %s", json);
    return ESP_OK;
}

/**
 * @brief HTTP POST handler for updating captive portal configuration.
 * 
 * Parses POST data, updates config, and triggers reconnect or mDNS update as needed.
 */
esp_err_t captive_post_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    bool need_reconnect = false;
    bool need_mdns_update = false;
    bool ssid_changed = false;
    bool mode_changed = false;
    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    ESP_LOGI(TAG, "Received POST request to update WiFi settings");
    if (len > 0) {
        buf[len] = '\0';
        ESP_LOGV(TAG, "POST data: %s (len = %d)", buf, len);
        char param[32];
        
        // Parse wifi_mode first
        if (httpd_query_key_value(buf, "wifi_mode", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed WiFi Mode: %s", param);
            int mode_val = atoi(param);
            wifi_mode_t new_mode = (mode_val == WIFI_MODE_AP) ? WIFI_MODE_AP : WIFI_MODE_STA;
            if (captive_cfg.wifi_mode != new_mode) {
                mode_changed = true;
                captive_cfg.wifi_mode = new_mode;
            }
        }
        
        // Parse AP settings
        if (httpd_query_key_value(buf, "ap_ssid", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed AP SSID: %s", param);
            if (strcmp(captive_cfg.ap_ssid, param) != 0) {
                strcpy(captive_cfg.ap_ssid, param);
                if (captive_cfg.wifi_mode == WIFI_MODE_AP) {
                    mode_changed = true;
                }
            }
        }
        
        if (httpd_query_key_value(buf, "ap_password", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed AP Password: %s", param);
            // Only update if not empty (empty = unchanged)
            if (strlen(param) > 0 && strcmp(captive_cfg.ap_password, param) != 0) {
                strcpy(captive_cfg.ap_password, param);
                if (captive_cfg.wifi_mode == WIFI_MODE_AP) {
                    mode_changed = true;
                }
            }
        }
        
        if (httpd_query_key_value(buf, "ssid", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed SSID: %s", param);
            if (strcmp((char*)&captive_cfg.ssid, param) != 0) {
                ssid_changed = true;  // Mark SSID as changed
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "SSID changed, reconnecting...");
                }
                strcpy((char*)&captive_cfg.ssid, param);
            }
        }
        if (httpd_query_key_value(buf, "authmode", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "") == 0) {
                ESP_LOGD(TAG, "Authmode empty");
                captive_cfg.authmode = WIFI_AUTHMODE_INVALID;
            } else {
                int new_authmode = atoi(param);
                ESP_LOGD(TAG, "Parsed Authmode: %d", new_authmode);
                if (new_authmode == WIFI_AUTHMODE_ENTERPRISE) {
                    ESP_LOGW(TAG, "Enterprise networks (authmode 2) rejected");
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_send(req, "Enterprise networks not supported", HTTPD_RESP_USE_STRLEN);
                    return ESP_OK;
                }
                if (new_authmode < 0 || new_authmode > 1) {
                    ESP_LOGW(TAG, "Authmode out of range, rejected");
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_send(req, "Authmode out of range", HTTPD_RESP_USE_STRLEN);
                    return ESP_OK;
                }
                if (captive_cfg.authmode != new_authmode) {
                    if (mode == WIFI_MODE_STA) {
                        need_reconnect = true;
                        ESP_LOGD(TAG, "Authmode changed, reconnecting...");
                    }
                }
                captive_cfg.authmode = new_authmode;
            }
        }
        if (httpd_query_key_value(buf, "password", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed Password: %s", param);
            
            // Safety: If SSID changed and password is empty, reject the request
            if (ssid_changed && strlen(param) == 0 && captive_cfg.authmode == WIFI_AUTHMODE_WPA_PSK) {
                ESP_LOGW(TAG, "SSID changed but no password provided for WPA network");
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "Password required for new network", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            
            if ((captive_cfg.authmode != WIFI_AUTHMODE_OPEN && strlen(param) != 0 && strcmp((char*)&captive_cfg.password, param) != 0) || captive_cfg.authmode == WIFI_AUTHMODE_INVALID) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "Password changed, reconnecting...");
                }
                strcpy((char*)&captive_cfg.password, param);
            } else if (captive_cfg.authmode == WIFI_AUTHMODE_OPEN && captive_cfg.authmode != WIFI_AUTHMODE_INVALID) {
                strcpy((char*)&captive_cfg.password, "");
            }
        }
        if (captive_cfg.authmode == WIFI_AUTHMODE_INVALID) {
            if (captive_cfg.password[0] != 0) {
                captive_cfg.authmode = WIFI_AUTHMODE_WPA_PSK; // WPA/WPA2-PSK
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "Invalid authmode corrected to WPA/WPA2-Personal, password is not empty, reconnecting...");
                }
            } else {
                captive_cfg.authmode = WIFI_AUTHMODE_OPEN; // Open
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "Invalid authmode corrected to Open, password is empty, reconnecting...");
                }
            }
        }
        if (httpd_query_key_value(buf, "use_static_ip", param, sizeof(param)) == ESP_OK) {
            bool new_use_static_ip = strcmp(param, "true") == 0;
            ESP_LOGD(TAG, "Parsed Use Static IP: %s", param);
            if (captive_cfg.use_static_ip != new_use_static_ip) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "Static IP usage changed, reconnecting...");
                }
            }
            captive_cfg.use_static_ip = new_use_static_ip;
        } else {
            if (captive_cfg.use_static_ip) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "Static IP usage disabled, reconnecting...");
                }
            }
            captive_cfg.use_static_ip = false;
        }
        if (httpd_query_key_value(buf, "static_ip", param, sizeof(param)) == ESP_OK) {
            uint32_t new_ip = inet_addr(param);
            ESP_LOGD(TAG, "Parsed Static IP: %s", param);
            if (captive_cfg.static_ip.addr != new_ip && captive_cfg.use_static_ip) {
                if (mode == WIFI_MODE_STA) {
                    need_reconnect = true;
                    ESP_LOGD(TAG, "Static IP changed, reconnecting...");
                }
            }
            captive_cfg.static_ip.addr = new_ip;
        }
        if (httpd_query_key_value(buf, "use_mDNS", param, sizeof(param)) == ESP_OK) {
            bool new_use_mdns = strcmp(param, "true") == 0;
            ESP_LOGD(TAG, "Parsed Use mDNS: %s", param);
            if (captive_cfg.use_mDNS != new_use_mdns) {
                if (mode == WIFI_MODE_STA) {
                    need_mdns_update = true;
                    ESP_LOGD(TAG, "mDNS usage changed, updating...");
                }
            }
            captive_cfg.use_mDNS = new_use_mdns;
        } else {
            if (captive_cfg.use_mDNS) {
                if (mode == WIFI_MODE_STA) {
                    need_mdns_update = true;
                    ESP_LOGD(TAG, "mDNS usage disabled, updating...");
                }
            }
            captive_cfg.use_mDNS = false;
        }
        if (httpd_query_key_value(buf, "mDNS_hostname", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed mDNS Hostname: %s", param);
            if ((strcmp(captive_cfg.mDNS_hostname, param) != 0)) {
                if (captive_cfg.use_mDNS) {
                    if (mode == WIFI_MODE_STA) {
                        need_mdns_update = true;
                        ESP_LOGD(TAG, "mDNS hostname changed, updating...");
                    }
                }
                strcpy(captive_cfg.mDNS_hostname, param);
            }
        }
        if (httpd_query_key_value(buf, "service_name", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGD(TAG, "Parsed Service Name: %s", param);
            if ((strcmp(captive_cfg.service_name, param) != 0)) {
                if (captive_cfg.use_mDNS) {
                    if (mode == WIFI_MODE_STA) {
                        need_mdns_update = true;
                        ESP_LOGD(TAG, "mDNS service name changed, updating...");
                    }
                }
                strcpy(captive_cfg.service_name, param);
            }
        }
    }

    // Log the updated captive portal settings
    if (captive_cfg.wifi_mode == WIFI_MODE_STA) {
        char ipbuf[16] = "null";
        if (captive_cfg.use_static_ip) {
            esp_ip4addr_ntoa(&captive_cfg.static_ip, ipbuf, sizeof(ipbuf));
        }
        ESP_LOGI(TAG, "Settings updated: SSID=%s, authmode=%d, static_ip=%s, mDNS=%s", 
            captive_cfg.ssid, captive_cfg.authmode, ipbuf, captive_cfg.use_mDNS ? captive_cfg.mDNS_hostname : "null");
    } else if (captive_cfg.wifi_mode == WIFI_MODE_AP) {
        char ipbuf[16] = "null";
        if (captive_cfg.use_static_ip) {
            esp_ip4addr_ntoa(&captive_cfg.static_ip, ipbuf, sizeof(ipbuf));
        }
        ESP_LOGI(TAG, "Settings updated: AP SSID=%s, AP Password=%s, authmode=%d, static_ip=%s, mDNS=%s", 
            captive_cfg.ap_ssid, captive_cfg.ap_password, captive_cfg.authmode, ipbuf, captive_cfg.use_mDNS ? captive_cfg.mDNS_hostname : "null");
    }

    // Save settings to NVS
    set_nvs_wifi_settings(&captive_cfg);

    // Determine action based on mode
    if (mode_changed) {
        ESP_LOGD(TAG, "WiFi mode changed to: %d", captive_cfg.wifi_mode);
        if (captive_cfg.wifi_mode == WIFI_MODE_STA) {
            wifi_flags_set_bits(SWITCH_TO_STA_BIT);
        } else {
            wifi_flags_set_bits(SWITCH_TO_AP_BIT);
        }
    } else if (mode == WIFI_MODE_STA) {
        if (need_reconnect) {
            wifi_flags_set_bits(RECONECT_BIT);
        }
        if (need_mdns_update) {
            wifi_flags_set_bits(mDNS_CHANGE_BIT);
        }
    }
    
    // Redirect back to captive portal, method GET
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/captive");
    httpd_resp_send(req, "Redirected", HTTPD_RESP_USE_STRLEN);
    ESP_LOGD(TAG, "Redirecting to back captive portal, method GET");
    return ESP_OK;
}

/**
 * @brief Register captive portal HTTP handlers with the web server.
 * 
 * Registers the following endpoints:
 * - GET /captive - Main captive portal page
 * - POST /captive - Configuration submission handler
 * - GET /captive.json - Current configuration as JSON
 * - GET /scan.json - WiFi network scan results
 * 
 * @note Only registers if server handle is not NULL
 */
esp_err_t register_captive_portal_handlers(void) {

    httpd_uri_t captive_uri = {
        .uri = "/captive",
        .method = HTTP_GET,
        .handler = captive_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&captive_uri), TAG, "Failed to register captive handler");

    httpd_uri_t captive_post_uri = {
        .uri = "/captive",
        .method = HTTP_POST,
        .handler = captive_post_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&captive_post_uri), TAG, "Failed to register captive POST handler");

    httpd_uri_t captive_json_uri = {
        .uri = "/captive.json",
        .method = HTTP_GET,
        .handler = captive_json_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&captive_json_uri), TAG, "Failed to register captive JSON handler");

    httpd_uri_t scan_json_uri = {
        .uri = "/scan.json",
        .method = HTTP_GET,
        .handler = scan_json_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&scan_json_uri), TAG, "Failed to register scan JSON handler");
    return ESP_OK;
}


/**
 * @brief Start WiFi in captive portal AP mode.
 * 
 * Sets up the ESP32 as a WiFi access point, starts the HTTP server,
 * registers captive portal handlers, and starts a DNS server for redirection.
 */
esp_err_t wifi_start_captive() {
    ESP_LOGI(TAG, "Starting AP mode for captive portal...");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to set WiFi mode");
    wifi_config_t wifi_cfg = get_captive_ap_wifi_config(&captive_cfg);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "Failed to set WiFi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
    
    ESP_LOGI(TAG, "Setting max TX power to 44 (11dBm) for AP mode");
    esp_err_t err = esp_wifi_set_max_tx_power(44);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set max TX power: %s", esp_err_to_name(err));
    }


    // Log AP IP address
    esp_netif_ip_info_t ip_info;
    err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get IP info: %s", esp_err_to_name(err));
    } else {
        char ip_addr[16];
        inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
        ESP_LOGI(TAG, "Set up AP with IP: %s", ip_addr);
    }

    if (wifi_cfg.ap.authmode != WIFI_AUTH_OPEN) {
        ESP_LOGI(TAG, "SoftAP started: SSID: '%s' Password: '%s'", wifi_cfg.ap.ssid, wifi_cfg.ap.password);
    } else {
        ESP_LOGI(TAG, "SoftAP started: SSID: '%s' No password", wifi_cfg.ap.ssid);
    }

    // Start HTTP server and register handlers
    ESP_LOGD(TAG, "Starting web server on port: %d", server_mgr_get_port());
    err = server_mgr_start();
    if (err != ESP_OK) {
        esp_wifi_stop();
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to start HTTP server");
    }

    err = register_captive_portal_handlers();
    if (err != ESP_OK) {
        esp_wifi_stop();
        server_mgr_stop();
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to register captive portal handlers");
    }

    err = server_mgr_register_err_handler(HTTPD_404_NOT_FOUND, captive_error_redirect);
    if (err != ESP_OK) {
        esp_wifi_stop();
        server_mgr_stop();
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to register error handler");
    }

    // Start DNS server for captive portal redirection (hijack all DNS queries)
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    dns_server_handle_t h = start_dns_server(&dns_config);
    if (h == NULL) {
        esp_wifi_stop();
        server_mgr_stop();
        ESP_RETURN_ON_ERROR(ESP_FAIL, TAG, "Failed to start DNS server");
    }
    return ESP_OK;
}

esp_err_t wifi_init_captive() {
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);
    ESP_LOGI(TAG, "Initializing WiFi in captive portal mode...");
    fill_captive_portal_config_struct(&captive_cfg);
    
    // Initialize NVS
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize NVS");

    // Read NVS settings
    get_nvs_wifi_settings(&captive_cfg);
    ESP_LOGD(TAG, "STA SSID: %s, password: %s", captive_cfg.ssid, captive_cfg.password);
    ESP_LOGD(TAG, "AP SSID: %s, password: %s", captive_cfg.ap_ssid, captive_cfg.ap_password);

    return ESP_OK;
}


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
wifi_config_t get_sta_wifi_config() {
    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    strcpy((char *)wifi_cfg.sta.ssid, captive_cfg.ssid);
    if (captive_cfg.authmode == WIFI_AUTHMODE_OPEN) {
        strcpy((char *)wifi_cfg.sta.password, "");
        wifi_cfg.sta.threshold.authmode = WIFI_AUTHMODE_OPEN;
        ESP_LOGD(TAG, "STA config set: Authmode: 0, SSID: %s, open network (no password)", wifi_cfg.sta.ssid);
    } else {
        strcpy((char *)wifi_cfg.sta.password, captive_cfg.password);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTHMODE_WPA_PSK;
        ESP_LOGD(TAG, "STA config set: Authmode: 1, SSID: %s, password: %s", wifi_cfg.sta.ssid, wifi_cfg.sta.password);
    }

    return wifi_cfg;
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
wifi_config_t get_ap_wifi_config() {
    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_AP, &wifi_cfg);

    strcpy((char *)wifi_cfg.ap.ssid, captive_cfg.ap_ssid);
    strcpy((char *)wifi_cfg.ap.password, captive_cfg.ap_password);
    wifi_cfg.ap.ssid_len = strlen(captive_cfg.ap_ssid);
    wifi_cfg.ap.max_connection = 4;

    if (captive_cfg.ap_password[0] == 0) {
        wifi_cfg.ap.authmode = WIFI_AUTHMODE_OPEN;
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTHMODE_WPA_PSK;
    }

    ESP_LOGD(TAG, "AP config set: SSID: %s, password: %s, authmode: %d", wifi_cfg.ap.ssid, wifi_cfg.ap.password, wifi_cfg.ap.authmode);
    

    return wifi_cfg;
}

/**
 * @brief Create WiFi configuration for captive portal AP mode.
 * 
 * Configures an open access point with the hardcoded SSID "ESP32_Captive_Portal"
 * and no password, suitable for captive portal operation.
 * 
 * @param cfg Pointer to captive portal configuration (unused, for signature compatibility)
 * @return wifi_config_t WiFi configuration structure for captive AP
 */
wifi_config_t get_captive_ap_wifi_config() {
    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_AP, &wifi_cfg);

    strcpy((char *)wifi_cfg.ap.ssid, "ESP32_Captive_Portal");
    strcpy((char *)wifi_cfg.ap.password, "");
    wifi_cfg.ap.ssid_len = strlen("ESP32_Captive_Portal");
    wifi_cfg.ap.max_connection = 4;

    wifi_cfg.ap.authmode = WIFI_AUTHMODE_OPEN;

    ESP_LOGD(TAG, "AP config set: SSID: %s, password: %s, authmode: %d", wifi_cfg.ap.ssid, wifi_cfg.ap.password, wifi_cfg.ap.authmode);
    
    return wifi_cfg;
}

esp_err_t get_mdns_config(bool *use_mDNS, char *hostname, size_t hostname_len, char *service_name, size_t service_name_len) {
    if (use_mDNS == NULL || hostname == NULL || service_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *use_mDNS = captive_cfg.use_mDNS;
    strncpy(hostname, captive_cfg.mDNS_hostname, hostname_len);
    strncpy(service_name, captive_cfg.service_name, service_name_len);
    return ESP_OK;
}

esp_err_t get_static_ip_config(bool *use_static_ip, esp_ip4_addr_t *static_ip) {
    if (use_static_ip == NULL || static_ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *use_static_ip = captive_cfg.use_static_ip;
    *static_ip = captive_cfg.static_ip;
    return ESP_OK;
}

wifi_mode_t get_wifi_mode() {
    return captive_cfg.wifi_mode;
}
