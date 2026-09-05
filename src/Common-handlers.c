#include "Common-handlers.h"

#include "Wifi.h"
#include "Server-mgr.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "errno.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Wifi: Common-handlers";

// Restart from a background task so the HTTP server can finish sending
// the response and close the connection gracefully before reboot.
static void restart_delayed_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    vTaskDelete(NULL);
}

/**
 * @brief HTTP GET handler for /restart endpoint (reboots device).
 * 
 * @param req HTTP request handle
 * @return ESP_OK on success
 */
esp_err_t restart_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, "Restarting...", HTTPD_RESP_USE_STRLEN);

    BaseType_t r = xTaskCreate(restart_delayed_task, "restart_delayed", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) {
        // If task creation failed, fall back to delaying in-place (best-effort)
        esp_restart();
    }

    return ESP_OK;
}

esp_err_t register_common_handlers(void) {
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);

    ESP_LOGD(TAG, "Registering common HTTP handlers");

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler
    };
    ESP_RETURN_ON_ERROR(server_mgr_register_handler(&restart_uri), TAG, "Failed to register /restart handler");

    return ESP_OK;
}