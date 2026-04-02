#include "Server-mgr.h"

#include "sdkconfig.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"


/** @brief HTTP server handle, NULL when server is not running */
httpd_handle_t server = NULL;

/** @brief HTTP server configuration structure */
httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();

/** @brief Registry array for storing custom HTTP handlers registered by the application */
static httpd_uri_t custom_handlers[CONFIG_WIFI_MAX_CUSTOM_HTTP_HANDLERS];

/** @brief Count of currently registered custom HTTP handlers */
static size_t custom_handler_count = 0;

static const char *TAG = "Server-mgr";

esp_err_t server_mgr_start(){
    if (server == NULL) {
        // Configure HTTP server
        httpd_config.lru_purge_enable = true;
        httpd_config.max_uri_handlers = CONFIG_WIFI_MAX_CUSTOM_HTTP_HANDLERS + 8;
        httpd_config.uri_match_fn = httpd_uri_match_wildcard;
        httpd_config.stack_size = 6144;  // Increase from default 4096 to handle captive portal detection bursts
    }
    return httpd_start(&server, &httpd_config);
}

esp_err_t server_mgr_stop(){
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    return ESP_OK;
}
esp_err_t server_mgr_register_handler(const httpd_uri_t *uri_handler) {
    if (server == NULL) {
        ESP_LOGE(TAG, "Cannot register handler: server is not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (uri_handler == NULL || uri_handler->uri == NULL || uri_handler->handler == NULL) {
        ESP_LOGE(TAG, "Cannot register handler: uri or handler is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = httpd_register_uri_handler(server, uri_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register handler for %s: %s", uri_handler->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t server_mgr_register_err_handler(httpd_err_code_t err_code, httpd_err_handler_func_t handler_fn) {
    if (server == NULL) {
        ESP_LOGE(TAG, "Cannot register error handler: server is not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (handler_fn == NULL) {
        ESP_LOGE(TAG, "Cannot register error handler: handler function is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = httpd_register_err_handler(server, err_code, handler_fn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register error handler for code %d: %s", err_code, esp_err_to_name(err));
    }
    return err;
}

uint16_t server_mgr_get_port() {
    return httpd_config.server_port;
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
esp_err_t register_custom_http_handlers() {
    if (server == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < custom_handler_count; ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &custom_handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register custom handler for %s: %s", custom_handlers[i].uri, esp_err_to_name(err));
            ret = err;
        }
    }
    return ret;
}