#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_http_server.h"

esp_err_t server_mgr_init();
esp_err_t server_mgr_start();
esp_err_t server_mgr_stop();
esp_err_t server_mgr_register_handler(const httpd_uri_t *uri_handler);
esp_err_t server_mgr_register_err_handler(httpd_err_code_t err_code, httpd_err_handler_func_t handler_fn);
uint16_t server_mgr_get_port();

esp_err_t wifi_register_http_handler(httpd_uri_t *uri);

/**
 * @brief Register all custom HTTP handlers with the server.
 * 
 * Called when transitioning to STA or AP mode to activate custom handlers.
 */
esp_err_t register_custom_http_handlers();