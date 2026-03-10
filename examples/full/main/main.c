#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "Wifi.h"
#include "time.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_http_server.h"


// --- Define variables, classes ---
const char *TAG = "main";  ///< Log tag for this module
uint8_t sliderBinaryValue = 0;
uint8_t sliderJSONValue = 0;
int64_t bootTime;

// Control packet types for binary WebSocket protocol
typedef enum {
    VALUE_NONE,
    SLIDER_BINARY,
    SLIDER_JSON
} ws_value_type_t;

typedef enum {
    EVENT_NONE,
    EVENT_TIMEOUT,
    EVENT_RELOAD,
    EVENT_REVERT_SETTINGS
} ws_event_type_t;

// Binary control packet structure
typedef struct __attribute__((packed)) {
    uint8_t type;  // Control type (1 byte)
    int16_t value;       // Control value (2 bytes)
} ws_control_packet_t;

static const char *k_supported_protocols[] = {
    "binary.v1"
};
static const size_t k_supported_protocols_count =
sizeof(k_supported_protocols) / sizeof(k_supported_protocols[0]);

// Trim leading/trailing spaces in-place; returns pointer to trimmed start.
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

// Return the first supported protocol found in the client's header list.
// The returned pointer is one of the constants in k_supported_protocols.
static const char *pick_subprotocol_from_header(const char *header_value) {
    if (!header_value || !*header_value) return NULL;

    // Copy header so we can tokenize safely.
    char *buf = strdup(header_value);
    if (!buf) return NULL;

    const char *selected = NULL;
    char *saveptr = NULL;

    // Client list is comma-separated per spec.
    for (char *token = strtok_r(buf, ",", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ",", &saveptr)) {

        char *proto = trim(token);

        for (size_t i = 0; i < k_supported_protocols_count; i++) {
            if (strcasecmp(proto, k_supported_protocols[i]) == 0) {
                selected = k_supported_protocols[i];
                break;
            }
        }
        if (selected) break; // pick first match in client order
    }

    free(buf);
    return selected;
}

// Pre-handshake callback: decide protocol or reject
static esp_err_t ws_pre_handshake_cb(httpd_req_t *req) {
    // Read Sec-WebSocket-Protocol header (if present)
    size_t len = httpd_req_get_hdr_value_len(req, "Sec-WebSocket-Protocol");
    if (len == 0) {
        // Client sent no protocol list; accept connection without a protocol.
        return ESP_OK;
    }

    char *hdr = malloc(len + 1);
    if (!hdr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    esp_err_t err = httpd_req_get_hdr_value_str(
        req, "Sec-WebSocket-Protocol", hdr, len + 1);
    if (err != ESP_OK) {
        free(hdr);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Invalid WebSocket subprotocol header");
    }

    const char *selected = pick_subprotocol_from_header(hdr);
    free(hdr);

    if (!selected) {
        // None of the client's protocols are supported
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Unsupported WebSocket subprotocol");
    }

    // Set the chosen protocol in the handshake response
    httpd_resp_set_hdr(req, "Sec-WebSocket-Protocol", selected);
    return ESP_OK;
}

// Helper: send a 1-byte event back to the client that sent the request
static esp_err_t send_ws_event_to_req(httpd_req_t *req, uint8_t eventType)
{
    uint8_t payload = (uint8_t)eventType;
    httpd_ws_frame_t ws_frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = &payload,
        .len = sizeof(payload)
    };

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Invalid socket file descriptor");
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(httpd_ws_send_frame_async(req->handle, sockfd, &ws_frame), TAG, "Failed to send ws event packet");
    return ESP_OK;
}

// Helper: send a typed value (1 byte type + 2 byte little-endian int16 value)
static esp_err_t send_ws_value_to_req(httpd_req_t *req, uint8_t type, int16_t value)
{
    uint8_t payload[3];
    payload[0] = (uint8_t)type;
    payload[1] = (uint8_t)(value & 0xFF);
    payload[2] = (uint8_t)((value >> 8) & 0xFF);
    httpd_ws_frame_t ws_frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = &payload[0],
        .len = sizeof(payload)
    };

    int sockfd = httpd_req_to_sockfd(req);
    ESP_RETURN_ON_ERROR(httpd_ws_send_frame_async(req->handle, sockfd, &ws_frame), TAG, "Failed to send ws packet: type:%u val:%d", type, value);
    return ESP_OK;
}

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
            sliderJSONValue = (uint8_t)atoi(param);
            ESP_LOGI(TAG, "JSON slider updated to %d", sliderJSONValue);
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

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        return ws_pre_handshake_cb(req);
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = NULL;

    // First, get frame metadata
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &ws_pkt, 0), TAG, "Failed to receive WebSocket frame");

    if (ws_pkt.type == HTTPD_WS_TYPE_BINARY && ws_pkt.len == 1) {
        ws_pkt.payload = malloc(1);
        esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 1);
        if (ret != ESP_OK) {
            free(ws_pkt.payload);
            ESP_RETURN_ON_ERROR(ret, TAG, "Failed to receive ws event packet");
        }
        uint8_t event_id = ((uint8_t*)ws_pkt.payload)[0];
        switch (event_id) {
            case EVENT_TIMEOUT:
                ESP_LOGV(TAG, "WebSocket timeout event received");
                break;
            case EVENT_RELOAD:
                ESP_LOGV(TAG, "Reload event received; sending current slider value back to client");
                // send current binary slider value back as a typed value
                send_ws_value_to_req(req, SLIDER_BINARY, (int16_t)sliderBinaryValue);
                send_ws_value_to_req(req, SLIDER_JSON, (int16_t)sliderJSONValue);
                break;
            case EVENT_REVERT_SETTINGS:
                ESP_LOGV(TAG, "Reverting to default settings");
                sliderBinaryValue = 0;
                sliderJSONValue = 0;
                send_ws_value_to_req(req, SLIDER_BINARY, (int16_t)sliderBinaryValue);
                send_ws_value_to_req(req, SLIDER_JSON, (int16_t)sliderJSONValue);
                break;
            default:
                ESP_LOGW(TAG, "Unknown event id: 0x%2X", event_id);
        }
        free(ws_pkt.payload);
        return ESP_OK;
    }

    // Check if this is a binary control packet with a numerical value
    if (ws_pkt.type == HTTPD_WS_TYPE_BINARY && ws_pkt.len == sizeof(ws_control_packet_t)) {
        ws_pkt.payload = malloc(ws_pkt.len);
        esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            free(ws_pkt.payload);
            ESP_RETURN_ON_ERROR(ret, TAG, "Failed to receive ws control packet");
        }
        ws_control_packet_t *packet = (ws_control_packet_t *)ws_pkt.payload;
        switch(packet->type) {
            case SLIDER_BINARY:
                sliderBinaryValue = (uint8_t)(packet->value & 0xFF);
                ESP_LOGI(TAG, "Binary slider updated to %d", sliderBinaryValue);
                break;
            case SLIDER_JSON:
                ESP_LOGW(TAG, "JSON slider is not supposed to be handled in binary packets");
                break;
            default:
                ESP_LOGW(TAG, "Unknown packet type: 0x%2X, value: 0x%4X", packet->type, packet->value);
                break;
        }
        free(ws_pkt.payload);
        return ESP_OK;
    }

    // Handle WS packets
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket connection closed");
        free(ws_pkt.payload);
        return ESP_OK;
    }

    // Fallback for other messages, null terminate payload
    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;
    ws_pkt.payload[ws_pkt.len] = 0;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to receive ws packet");
    }

    ESP_LOGD(TAG, "Received WebSocket text payload: %s", (char*)ws_pkt.payload);

    // Try to parse as JSON
    // Basic JSON parsing for {"slider": number}
    char *sliderPtr = strstr((char*)ws_pkt.payload, "\"slider\":");
    if (sliderPtr != NULL) {
        char *start = strchr(sliderPtr, ':'); // Skip to colon
        if (start) {
            // Skip whitespace after colon
            start++;
            while (*start == ' ') start++;
            
            // Parse the number
            int value = atoi(start);
            sliderJSONValue = value & 0x3FF; // Clamp to 10 bits (0-1023)
            ESP_LOGI(TAG, "JSON slider updated to %d", sliderJSONValue);
            free(ws_pkt.payload);
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Received WebSocket text: %s", (char*)ws_pkt.payload);

    free(ws_pkt.payload);
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

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    wifi_register_http_handler(&ws_uri);
    
    bootTime = esp_timer_get_time();
}

