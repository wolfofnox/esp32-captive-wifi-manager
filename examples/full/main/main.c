#include "sdkconfig.h"

#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_EX
#include "esp_log.h"

#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "Wifi.h"
#include "time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>  
#include <string.h> 

#include "ws_server.h"

// --- Define variables, classes ---
const char *TAG = "main";  ///< Log tag for this module
uint8_t sliderSubValue = 0;
uint8_t sliderCmdValue = 0;
uint8_t sliderPostValue = 0;
int64_t bootTime;

/* Active subscriptions: handle + sub_id for sending deltas back to the client */
static ws_client_handle_t g_loopback_handle = 0;
static uint16_t           g_loopback_sub_id = 0;

static ws_client_handle_t g_status_handle = 0;
static uint16_t           g_status_sub_id = 0;

int last_free_heap_kb = 0;
int last_total_heap_kb = 0;
int64_t last_uptime_s = 0;
bool last_connected = false;
bool last_in_ap_mode = false;

esp_err_t control_post_handler(httpd_req_t *req) {
    char buf[100];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0; // Null-terminate the received data

    ESP_LOGD(TAG, "Received control POST data");
    ESP_LOGV(TAG, "Control POST data: %s", buf);


    if (len > 0) {
        buf[len] = '\0';
        // Parse key-value pairs
        char param[32];
        if (httpd_query_key_value(buf, "slider", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            sliderPostValue = (uint8_t)atoi(param);
            ESP_LOGI(TAG, "HTTP POST: Post slider updated to %d", sliderPostValue);
        }
        if (httpd_query_key_value(buf, "text", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            ESP_LOGI(TAG, "HTTP POST: Text value is %s", param);
        }
        if (httpd_query_key_value(buf, "number", param, sizeof(param)) == ESP_OK) {
            url_decode(param);
            int numberValue = (uint8_t)atoi(param);
            ESP_LOGI(TAG, "HTTP POST: Number value is %d", numberValue);
        }
    }

    // Redirect back to /control, method GET
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/control");
    httpd_resp_send(req, "Control data received, redirecting", HTTPD_RESP_USE_STRLEN);
    ESP_LOGD(TAG, "Redirecting to back control page, method GET");
    return ESP_OK;
}

esp_err_t on_req_handler(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id) {
    ESP_LOGD(TAG, "Received WS request: %s", name);
    if (esp_log_level_get(TAG) >= ESP_LOG_VERBOSE && params) {
        char *params_str = cJSON_Print(params);
        ESP_LOGV(TAG, "Request params: %s", params_str);
        if (params_str) cJSON_free(params_str);
    } else ESP_LOGV(TAG, "No params for this request");
    // Send a response back to the client
    if (strcmp(name, "reload") == 0) {
        ESP_LOGI(TAG, "Reload request received, responding with: Cmd: %d, Sub: %d", sliderCmdValue, sliderSubValue);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "sliderSubValue", sliderSubValue);
        cJSON_AddNumberToObject(resp, "sliderCmdValue", sliderCmdValue);
        ws_respond(handle, req_id, resp);
    } else {
        ESP_LOGW(TAG, "Unknown request: %s", name);
        ws_send_error(handle, req_id, "unknown request");
    }
    return ESP_OK;
};

esp_err_t on_cmd_handler(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id) {
    ESP_LOGD(TAG, "Received WS command: %s", name);
    if (esp_log_level_get(TAG) >= ESP_LOG_VERBOSE) {
        char *params_str = params ? cJSON_Print(params) : "null";
        ESP_LOGV(TAG, "Command params: %s", params_str);
        if (params_str) cJSON_free(params_str);
    }
    if (strcmp(name, "sliderCmd") == 0) {
        cJSON *value_j = cJSON_GetObjectItemCaseSensitive(params, "value");
        if (cJSON_IsNumber(value_j)) {
            sliderCmdValue = (uint8_t)value_j->valuedouble;
            ESP_LOGI(TAG, "Command slider updated to %d", sliderCmdValue);
            ws_respond(handle, req_id, NULL); // Send empty response to acknowledge command
            /* Forward the updated value to all clients subscribed to "loopback" */
            if (g_loopback_handle && g_loopback_sub_id) {
                cJSON *delta = cJSON_CreateObject();
                cJSON_AddNumberToObject(delta, "value", sliderCmdValue);
                ws_send_sub_delta(g_loopback_handle, g_loopback_sub_id, delta);
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
    ESP_LOGD(TAG, "Received WS sub: %s (sub_id=%d)", name, sub_id);
    if (strcmp(name, "loopback") == 0) {
        g_loopback_handle = handle;
        g_loopback_sub_id = sub_id;
        /* Respond with the current sliderCmdValue as the initial snapshot */
        cJSON *snapshot = cJSON_CreateObject();
        cJSON_AddNumberToObject(snapshot, "value", sliderCmdValue);
        
        ESP_LOGI(TAG, "Client subscribed to loopback with sub_id=%d", sub_id);
        if (esp_log_level_get(TAG) >= ESP_LOG_VERBOSE && snapshot) {
            char *snapshot_str = cJSON_Print(snapshot);
            ESP_LOGV(TAG, "Loopback sub snapshot: %s", snapshot_str);
            if (snapshot_str) cJSON_free(snapshot_str);
        }

        ws_respond_sub(handle, req_id, sub_id, snapshot);
    } else if (strcmp(name, "status") == 0) {
        g_status_handle = handle;
        g_status_sub_id = sub_id;
        /* Respond with a snapshot containing some status info */
        last_free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024;
        last_total_heap_kb = heap_caps_get_total_size(MALLOC_CAP_DEFAULT) / 1024;
        last_uptime_s = (esp_timer_get_time() - bootTime) / 1000000;
        bool connected, in_ap_mode;
        char *ip_str = NULL;
        char *ssid = NULL;
        char *ap_ssid = NULL;
        wifi_get_status(&connected, &in_ap_mode, &ip_str, &ssid, &ap_ssid);
        cJSON *snapshot = cJSON_CreateObject();
        cJSON_AddNumberToObject(snapshot, "uptime", last_uptime_s);
        cJSON_AddNumberToObject(snapshot, "freeHeap", last_free_heap_kb);
        cJSON_AddNumberToObject(snapshot, "totalHeap", last_total_heap_kb);
        cJSON_AddStringToObject(snapshot, "version", "EXAMPLE");
        cJSON_AddBoolToObject(snapshot, "connected", connected);
        cJSON_AddStringToObject(snapshot, "ip", ip_str ? ip_str : "");
        cJSON_AddStringToObject(snapshot, "ssid", ssid ? ssid : "");
        cJSON_AddBoolToObject(snapshot, "in_ap_mode", in_ap_mode);
        cJSON_AddStringToObject(snapshot, "ap_ssid", ap_ssid ? ap_ssid : "");

        ESP_LOGI(TAG, "Client subscribed to status with sub_id=%d", sub_id);
        if (esp_log_level_get(TAG) >= ESP_LOG_VERBOSE && snapshot) {
            char *snapshot_str = cJSON_Print(snapshot);
            ESP_LOGV(TAG, "Status sub snapshot: %s", snapshot_str);
            if (snapshot_str) cJSON_free(snapshot_str);
        }

        ws_respond_sub(handle, req_id, sub_id, snapshot);
        if (ip_str) free(ip_str);
        if (ssid) free(ssid);
        if (ap_ssid) free(ap_ssid);
    } else {
        ws_send_error(handle, req_id, "unknown subscription");
    }
    return ESP_OK;
}

/* Called when the client unsubscribes from a server-side data stream */
esp_err_t on_unsub_handler(ws_client_handle_t handle, uint16_t sub_id) {
    ESP_LOGD(TAG, "Client unsubscribed sub_id=%d", sub_id);
    if (sub_id == g_loopback_sub_id) {
        g_loopback_handle = 0;
        g_loopback_sub_id = 0;
        ESP_LOGI(TAG, "Client unsubscribed from loopback (sub_id=%d)", sub_id);
    } else if (sub_id == g_status_sub_id) {
        g_status_handle = 0;
        g_status_sub_id = 0;
        ESP_LOGI(TAG, "Client unsubscribed from status (sub_id=%d)", sub_id);
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
        ESP_LOGI(TAG, "sliderSub delta: value=%d", sliderSubValue);
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

esp_err_t on_close_handler(ws_client_handle_t handle) {
    ESP_LOGI(TAG, "WebSocket client disconnected: handle=0x%08X", handle);

     /* Clear any global subscription state associated with this handle */  
    if (handle == g_loopback_handle) {   
        g_loopback_handle = 0;  
        g_loopback_sub_id = 0;  
    }  
    if (handle == g_status_handle) {  
        g_status_handle = 0;  
        g_status_sub_id = 0;  
    } 
    return ESP_OK;
}

static void status_delta_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (g_status_handle && g_status_sub_id) {
            cJSON *delta = NULL;
            int64_t uptime_s = (esp_timer_get_time() - bootTime) / 1000000;
            if ((last_uptime_s < 120 && uptime_s-last_uptime_s >= 2) || (last_uptime_s >= 120 && uptime_s-last_uptime_s >= 10)) {
                if (!delta) {
                    delta = cJSON_CreateObject();
                }
                if (!delta) continue;
                cJSON_AddNumberToObject(delta, "uptime", uptime_s);
                last_uptime_s = uptime_s;
            }
            int free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024;
            if (abs(free_heap_kb - last_free_heap_kb) >= 5)
            {
                if (!delta) {
                    delta = cJSON_CreateObject();
                }
                if (!delta) continue;
                cJSON_AddNumberToObject(delta, "freeHeap", free_heap_kb);
                last_free_heap_kb = free_heap_kb;
            }
            int total_heap_kb = heap_caps_get_total_size(MALLOC_CAP_DEFAULT) / 1024;
            if (total_heap_kb != last_total_heap_kb)
            {
                if (!delta) {
                    delta = cJSON_CreateObject();
                }
                if (!delta) continue;
                cJSON_AddNumberToObject(delta, "totalHeap", total_heap_kb);
                last_total_heap_kb = total_heap_kb;
            }
            bool connected, in_ap_mode;
            char *ip_str = NULL;
            char *ssid = NULL;
            char *ap_ssid = NULL;
            wifi_get_status(&connected, &in_ap_mode, &ip_str, &ssid, &ap_ssid);
            if (connected != last_connected) {
                if (!delta) {
                    delta = cJSON_CreateObject();
                }
                if (!delta) continue;
                cJSON_AddBoolToObject(delta, "connected", connected);
                cJSON_AddStringToObject(delta, "ip", connected ? (ip_str ? ip_str : "") : "");
                cJSON_AddStringToObject(delta, "ssid", connected ? (ssid ? ssid : "") : "");
                last_connected = connected;
            }
            if (in_ap_mode != last_in_ap_mode) {
                if (!delta) {
                    delta = cJSON_CreateObject();
                }
                if (!delta) continue;
                cJSON_AddBoolToObject(delta, "in_ap_mode", in_ap_mode);
                cJSON_AddStringToObject(delta, "ap_ssid", in_ap_mode ? (ap_ssid ? ap_ssid : "") : "");
                last_in_ap_mode = in_ap_mode;
            }
            if (delta) ws_send_sub_delta(g_status_handle, g_status_sub_id, delta);
            if (ip_str) free(ip_str);
            if (ssid) free(ssid);
            if (ap_ssid) free(ap_ssid);
        }
    }       
}

void app_main(void)
{
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
                          on_sub_handler, on_unsub_handler, on_close_handler);
    ESP_ERROR_CHECK(ws_start_task());

    xTaskCreate(status_delta_task, "status_delta_task", 2048, NULL, 5, NULL);
    
    bootTime = esp_timer_get_time();
}

