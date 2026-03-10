#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "Wifi.h"


// --- Define variables, classes ---
const char *TAG = "main";  ///< Log tag for this module
uint8_t sliderValue = 0;
int64_t bootTime;

// --- Define functions ---
esp_err_t status_json_handler(httpd_req_t *req) {
    char json[300];
    int free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    snprintf(json, sizeof(json), "{\"uptime\": %lli, \"freeHeap\": %d, \"totalHeap\": %d, \"version\": \"%s\"}",
             (esp_timer_get_time() - bootTime) / 1000, free_heap, total_heap, "EXAMPLE");
    ESP_LOGD(TAG, "JSON data requested: %s", json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t control_post_handler(httpd_req_t *req) {
    char buf[100];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0; // Null-terminate the received data

    ESP_LOGI(TAG, "Received control data: %s", buf);


    if (len > 0) {
        buf[len] = '\0';
        // Parse key-value pairs
        char param[32];
        if (httpd_query_key_value(buf, "slider", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            sliderValue = (uint8_t)atoi(param);
            ESP_LOGI(TAG, "JSON slider updated to %d", sliderValue);
        }
        if (httpd_query_key_value(buf, "text", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGI(TAG, "Text value is %s", param);
        }
        if (httpd_query_key_value(buf, "number", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            int numberValue = (uint8_t)atoi(param);
            ESP_LOGI(TAG, "Number value is %d", numberValue);
        }
    }

    // Redirect back to /control, method GET
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/control");
    httpd_resp_send(req, "Control data received, redirecting", HTTPD_RESP_USE_STRLEN);
    ESP_LOGV(TAG, "Redirecting to back control page, method GET");
    return ESP_OK;
}

void app_main(void)
{
    // Set log level
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    ESP_LOGI(TAG, "START %s from %s", __FILE__, __DATE__);
    ESP_LOGI(TAG, "Setting up...");

    // Configure GPIO 3V3 bus output
    gpio_config_t v_bus_config = {
        .pin_bit_mask = (1ULL << 47),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&v_bus_config));
    ESP_ERROR_CHECK(gpio_set_level(47, 1));

   
    wifi_init();

    httpd_uri_t status_json_uri = {
        .uri = "/status.json",
        .method = HTTP_GET,
        .handler = status_json_handler
    };
    wifi_register_http_handler(&status_json_uri);

    httpd_uri_t control_post_uri = {
        .uri = "/control",
        .method = HTTP_POST,
        .handler = control_post_handler
    };
    wifi_register_http_handler(&control_post_uri);
    
    bootTime = esp_timer_get_time();
}

