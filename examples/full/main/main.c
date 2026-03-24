#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "Wifi.h"
#include "time.h"

#include "ws_server.h"

// --- Define variables, classes ---
const char *TAG = "main";  ///< Log tag for this module
uint8_t sliderSubValue = 0;
uint8_t sliderCmdValue = 0;
uint8_t sliderPostValue = 0;
int64_t bootTime;

/* Active subscriptions: handle + sub_id for sending deltas back to the client */
static ws_client_handle_t g_sliderBin_handle = 0;
static uint16_t           g_sliderBin_sub_id = 0;

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
            sliderPostValue = (uint8_t)atoi(param);
            ESP_LOGI(TAG, "Post slider updated to %d", sliderPostValue);
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

esp_err_t on_req_handler(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id) {
    ESP_LOGI(TAG, "Received WS request: %s", name);
    ESP_LOGD(TAG, "Request params: %s", cJSON_Print(params));
    // Send a response back to the client
    if (strcmp(name, "reload") == 0) {
        ESP_LOGD(TAG, "Reload request received, responding with: Cmd: %d, Sub: %d", sliderCmdValue, sliderSubValue);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "sliderSubValue", sliderSubValue);
        cJSON_AddNumberToObject(resp, "sliderCmdValue", sliderCmdValue);
        ws_respond(handle, req_id, resp);
    } else {
        ws_send_error(handle, req_id, "unknown request");
    }
    return ESP_OK;
};

esp_err_t on_cmd_handler(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id) {
    ESP_LOGI(TAG, "Received WS command: %s", name);
    ESP_LOGD(TAG, "Command params: %s", cJSON_Print(params));
    if (strcmp(name, "sliderCmd") == 0) {
        cJSON *value_j = cJSON_GetObjectItemCaseSensitive(params, "value");
        if (cJSON_IsNumber(value_j)) {
            sliderCmdValue = (uint8_t)value_j->valuedouble;
            ESP_LOGI(TAG, "Command slider updated to %d", sliderCmdValue);
            ws_respond(handle, req_id, NULL); // Send empty response to acknowledge command
            /* Forward the updated value to all clients subscribed to "sliderBin" */
            if (g_sliderBin_handle && g_sliderBin_sub_id) {
                cJSON *delta = cJSON_CreateObject();
                cJSON_AddNumberToObject(delta, "value", sliderCmdValue);
                ws_send_sub_delta(g_sliderBin_handle, g_sliderBin_sub_id, delta);
            }
        } else {
            ESP_LOGW(TAG, "sliderCmd missing 'value' field or 'value' is not a number");
            ws_send_error(handle, req_id, "missing or invalid 'value' field");
        }
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", name);
        ws_send_error(handle, req_id, "unknown command");
    }
    return ESP_OK;
}

/* Called when the client subscribes to a server-side data stream */
esp_err_t on_sub_handler(ws_client_handle_t handle, const char *name, cJSON *params,
                          uint32_t req_id, uint16_t sub_id) {
    ESP_LOGI(TAG, "Received WS sub: %s (sub_id=%d)", name, sub_id);
    if (strcmp(name, "sliderBin") == 0) {
        g_sliderBin_handle = handle;
        g_sliderBin_sub_id = sub_id;
        /* Respond with the current sliderCmdValue as the initial snapshot */
        cJSON *snapshot = cJSON_CreateObject();
        cJSON_AddNumberToObject(snapshot, "value", sliderCmdValue);
        ws_respond_sub(handle, req_id, sub_id, snapshot);
    } else {
        ws_send_error(handle, req_id, "unknown subscription");
    }
    return ESP_OK;
}

/* Called when the client unsubscribes from a server-side data stream */
esp_err_t on_unsub_handler(ws_client_handle_t handle, uint16_t sub_id) {
    ESP_LOGI(TAG, "Client unsubscribed sub_id=%d", sub_id);
    if (sub_id == g_sliderBin_sub_id) {
        g_sliderBin_handle = 0;
        g_sliderBin_sub_id = 0;
    }
    return ESP_OK;
}

/* Called when client accepts the server-initiated "sliderSub" subscription */
static void on_sliderSub_snapshot(ws_client_handle_t handle, uint16_t sub_id, bool success,
                                   cJSON *snapshot, void *user_data)
{
    if (!success) {
        ESP_LOGW(TAG, "sliderSub subscription rejected by client (handle=0x%08X)", handle);
        return;
    }
    ESP_LOGI(TAG, "sliderSub accepted by client: handle=0x%08X, sub_id=%d", handle, sub_id);
    if (snapshot) {
        cJSON *val = cJSON_GetObjectItemCaseSensitive(snapshot, "value");
        if (cJSON_IsNumber(val)) {
            sliderSubValue = (uint8_t)val->valuedouble;
            ESP_LOGI(TAG, "sliderSub snapshot value: %d", sliderSubValue);
        }
    }
}

/* Called when the client sends a delta for the server-initiated "sliderSub" subscription */
static void on_sliderSub_delta(ws_client_handle_t handle, uint16_t sub_id,
                                cJSON *payload, void *user_data)
{
    cJSON *val = cJSON_GetObjectItemCaseSensitive(payload, "value");
    if (cJSON_IsNumber(val)) {
        sliderSubValue = (uint8_t)val->valuedouble;
        ESP_LOGD(TAG, "sliderSub delta: value=%d", sliderSubValue);
    }
}

esp_err_t on_open_handler(ws_client_handle_t handle, ws_client_ctx_t *ctx) {
    ESP_LOGI(TAG, "WebSocket client connected: handle=0x%08X", handle);
    /* Subscribe to the client's sliderSub data stream */
    esp_err_t r = ws_subscribe(handle, "sliderSub", NULL,
                                on_sliderSub_snapshot, on_sliderSub_delta, NULL);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "ws_subscribe(sliderSub) failed: %s", esp_err_to_name(r));
    }
    return ESP_OK;
}

void app_main(void)
{
    // Set log level
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_EX);
    esp_log_level_set("ws_server", CONFIG_LOG_LEVEL_EX); 
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

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .supported_subprotocol = "rap.v1+json",
        .handle_ws_control_frames = true
    };
    wifi_register_http_handler(&ws_uri);

    ws_register_callbacks(on_open_handler, on_cmd_handler, on_req_handler,
                          on_sub_handler, on_unsub_handler, NULL);
    ESP_ERROR_CHECK(ws_start_task());
    
    bootTime = esp_timer_get_time();
}

