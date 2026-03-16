#include "ws_server.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

const char *WS_TAG = "ws_server";

struct ws_client_ctx s_slots[CONFIG_MAX_WS_CLIENTS];
SemaphoreHandle_t s_lock;

struct rx_job {
    ws_client_handle_t handle;
    char *payload;
    size_t payload_len;
};
typedef struct rx_job rx_job_t;

struct tx_job {
    ws_client_handle_t handle;
    bool needs_ack;
    //...
};
typedef struct tx_job tx_job_t;

QueueHandle_t ws_rx_q, ws_tx_q;

on_cmd_cb g_on_cmd_cb = NULL;
on_req_cb g_on_req_cb = NULL;
on_close_cb g_on_close_cb = NULL;
on_open_cb g_on_open_cb = NULL;

/* helper - build handle from index+1 and generation:
   handle layout: [ generation (16 bits) | index+1 (16 bits) ]
   index+1 used so 0 handle == invalid */
static inline ws_client_handle_t make_handle(int idx, uint16_t gen)
{
    return ((ws_client_handle_t)gen << 16) | (ws_client_handle_t)(idx + 1);
}

static inline void decode_handle(ws_client_handle_t h, int *out_idx, uint16_t *out_gen)
{
    if (out_idx) *out_idx = (int)((h & 0xffff) - 1);
    if (out_gen) *out_gen = (uint16_t)(h >> 16);
}

static inline ws_client_handle_t get_handle_for_sockfd(int sockfd)
{
    if (!s_lock) ws_pool_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (s_slots[i].used && s_slots[i].fd == sockfd) {
            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            return h;
        }
    }
    xSemaphoreGive(s_lock);
    return 0;
}

static inline void ws_pool_init()
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(s_slots, 0, sizeof(s_slots));

    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; i++) {
        s_slots[i].used = false;
        s_slots[i].gen = 0;
        s_slots[i].fd = 0;
        s_slots[i].hd = NULL;
        s_slots[i].last_req_id = 0;
        s_slots[i].in_flight_semaphore = NULL;
    }

    ws_rx_q = xQueueCreate(CONFIG_WS_RX_QUEUE_SIZE, sizeof(struct rx_job));
    ws_tx_q = xQueueCreate(CONFIG_WS_TX_QUEUE_SIZE, sizeof(struct tx_job));
}

/* allocate the first free slot */
static esp_err_t ws_slot_alloc(httpd_handle_t hd, int sockfd, ws_client_handle_t *out_handle)
{
    if(!s_lock) ws_pool_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    // first try to find an existing slot for this sockfd (reused connection)
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (s_slots[i].fd == sockfd) {
            if (s_slots[i].used) ESP_RETURN_ON_ERROR(ESP_ERR_INVALID_STATE, WS_TAG, "Sockfd %d already has an allocated slot, same socket can not have two open connections", sockfd);
            s_slots[i].used = true;
            s_slots[i].hd = hd;
            if (s_slots[i].in_flight_semaphore == NULL) {
                s_slots[i].in_flight_semaphore = xSemaphoreCreateCounting(CONFIG_MAX_IN_FLIGHT_MSGS, CONFIG_MAX_IN_FLIGHT_MSGS);
            }
            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            ESP_LOGI(WS_TAG, "Reusing slot %d for sockfd=%d", i, sockfd);
            if (out_handle) *out_handle = h;
            return ESP_OK;
        }
    }

    // no existing slot for this sockfd, allocate a new one
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (!s_slots[i].used && s_slots[i].gen == 0) {
            s_slots[i].used = true;
            s_slots[i].hd = hd;
            s_slots[i].fd = sockfd;
            s_slots[i].last_req_id = 0;
            /* bump generation; avoid generation==0 */
            s_slots[i].gen = (s_slots[i].gen + 1) ? (s_slots[i].gen + 1) : 1;

            if (s_slots[i].in_flight_semaphore == NULL) {
                s_slots[i].in_flight_semaphore = xSemaphoreCreateCounting(CONFIG_MAX_IN_FLIGHT_MSGS, CONFIG_MAX_IN_FLIGHT_MSGS);
            }

            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            ESP_LOGI(WS_TAG, "allocated slot %d -> handle 0x%08X (sockfd=%d)", i, h, sockfd);
            if (out_handle) *out_handle = h;
            return ESP_OK;
        }
    }

    // no unused slots, try to find a free slot
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (!s_slots[i].used) {
            s_slots[i].used = true;
            s_slots[i].hd = hd;
            s_slots[i].fd = sockfd;
            s_slots[i].last_req_id = 0;
            /* bump generation; avoid generation==0 */
            s_slots[i].gen = (s_slots[i].gen + 1) ? (s_slots[i].gen + 1) : 1;

            if (s_slots[i].in_flight_semaphore == NULL) {
                s_slots[i].in_flight_semaphore = xSemaphoreCreateCounting(CONFIG_MAX_IN_FLIGHT_MSGS, CONFIG_MAX_IN_FLIGHT_MSGS);
            }

            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            ESP_LOGI(WS_TAG, "allocated slot %d -> handle 0x%08X (sockfd=%d)", i, h, sockfd);
            if (out_handle) *out_handle = h;
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_ERROR(ESP_ERR_NO_MEM, WS_TAG, "No free client slots");
    return ESP_ERR_NO_MEM;
}

/* free by sockfd (called when you detect close or error) */
static esp_err_t ws_slot_free_by_sockfd(int sockfd)
{
    if (!s_lock) ws_pool_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (s_slots[i].used && s_slots[i].fd == sockfd) {
            ESP_LOGI(WS_TAG, "freeing slot %d (sockfd=%d)", i, sockfd);
            s_slots[i].used = false;
            /* don't clear generation - used to invalidate old handles */
            /* don't clear fd - used to hand out the same handle if the same fd reconnects */
            /* don't clear last_req_id - reused if slot is reused */
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

/* lookup by handle - validates generation and used flag */
static ws_client_ctx_t *ws_slot_get_by_handle(ws_client_handle_t h)
{
    if (h == 0) return NULL;
    int idx;
    uint16_t gen;
    decode_handle(h, &idx, &gen);
    if (idx < 0 || idx >= CONFIG_MAX_WS_CLIENTS) return NULL;
    if (!s_lock) ws_pool_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ws_client_ctx_t *slot = NULL;

    if (s_slots[idx].used && s_slots[idx].gen == gen) {
        slot = &s_slots[idx];
    }
    xSemaphoreGive(s_lock);
    return slot;
}

/* linear scan by sockfd: simple and cheap for small pools */
static ws_client_ctx_t *ws_pool_get_by_sockfd(int sockfd)
{
    if (!s_lock) ws_pool_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (s_slots[i].used && s_slots[i].fd == sockfd) {
            ws_client_ctx_t *s = &s_slots[i];
            xSemaphoreGive(s_lock);
            return s;
        }
    }
    xSemaphoreGive(s_lock);
    return NULL;
}


esp_err_t ws_register_callbacks(on_open_cb open_cb, on_cmd_cb cmd_cb, on_req_cb req_cb, on_close_cb close_cb)
{
    if(!s_lock) ws_pool_init();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    g_on_open_cb = open_cb;
    g_on_cmd_cb = cmd_cb;
    g_on_req_cb = req_cb;
    g_on_close_cb = close_cb;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ws_send_json(ws_client_handle_t handle, cJSON *json) {
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t out = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = payload,
        .len = strlen(payload)
    };

    ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
    if (!slot) {
        free(payload);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t r = httpd_ws_send_frame_async(slot->hd, slot->fd, &out);
    free(payload);
    return r;
}

esp_err_t ws_ack(ws_client_handle_t handle, uint32_t req_id) {
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(resp, "type", "ack");
    cJSON_AddNumberToObject(resp, "req_id", req_id);
    esp_err_t r = ws_send_json(handle, resp);
    cJSON_Delete(resp);
    return r;
}

esp_err_t ws_respond(ws_client_handle_t handle, uint32_t req_id, cJSON *payload) {
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(resp, "type", "resp");
    cJSON_AddNumberToObject(resp, "req_id", req_id);
    cJSON_AddItemToObject(resp, "payload", payload); // takes ownership of payload
    esp_err_t r = ws_send_json(handle, resp);
    cJSON_Delete(resp);
    return r;
}

esp_err_t ws_send_error(ws_client_handle_t handle, uint32_t req_id, const char *error_msg) {
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(resp, "type", "err");
    cJSON_AddNumberToObject(resp, "req_id", req_id);
    cJSON_AddStringToObject(resp, "err", error_msg);
    esp_err_t r = ws_send_json(handle, resp);
    cJSON_Delete(resp);
    return r;
}

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {

        ws_client_handle_t handle;

        ESP_RETURN_ON_ERROR(ws_slot_alloc(req->handle, httpd_req_to_sockfd(req), &handle), WS_TAG, "Failed to allocate client slot");

        ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
        if (!slot) {
            return ESP_ERR_NOT_FOUND;
        }

        if (g_on_open_cb) {
            esp_err_t r = g_on_open_cb(handle, slot);
            if (r != ESP_OK) {
                ws_slot_free_by_sockfd(slot->fd);
                ESP_RETURN_ON_ERROR(r, WS_TAG, "on_open_cb failed");
            }
        }

        ESP_LOGI(WS_TAG, "New WebSocket connection: handle=0x%08X, sockfd=%d", handle, slot->fd);

        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = NULL;

    // First, get frame metadata
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &ws_pkt, 0), WS_TAG, "Failed to receive WebSocket frame");

    ESP_LOGV(WS_TAG, "Received WebSocket frame:\n        type=%d, len=%d, fragmented=%d, final=%d", ws_pkt.type, ws_pkt.len, ws_pkt.fragmented, ws_pkt.final);
    
    // Handle WS packets
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        
        // allocate buffer for payload (if any) and receive it
        if (ws_pkt.len > 0) {
            ws_pkt.payload = calloc(1, ws_pkt.len + 1);
            if (ws_pkt.payload) {
                esp_err_t r = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
                if (r != ESP_OK) {
                    free(ws_pkt.payload);
                    ws_slot_free_by_sockfd(fd);
                    return ESP_OK;
                }
            }
        }

        ESP_LOGV(WS_TAG, "Received CLOSE frame from socket %d, payload len=%d\n    payload: %s", fd, ws_pkt.len, (char*)ws_pkt.payload);

        // echo the client's close payload back (if present) as our CLOSE response
        httpd_ws_frame_t out = {
            .type = HTTPD_WS_TYPE_CLOSE,
            .payload = ws_pkt.payload,
            .len = ws_pkt.len
        };
        httpd_ws_send_frame_async(req->handle, fd, &out);

        if (g_on_close_cb) {
            g_on_close_cb(get_handle_for_sockfd(fd));
        }
        
        ESP_LOGI(WS_TAG, "WebSocket connection closed: sockfd=%d", fd);
        ws_slot_free_by_sockfd(fd);
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
        ESP_LOGE(WS_TAG, "Failed to receive WebSocket frame payload: %s", esp_err_to_name(ret));
        ESP_RETURN_ON_ERROR(ret, WS_TAG, "Failed to receive ws packet");
    }
    
    ESP_LOGD(WS_TAG, "Received WebSocket text payload from socket %d: %s", httpd_req_to_sockfd(req), (char*)ws_pkt.payload);

    rx_job_t job = {
        .handle = get_handle_for_sockfd(httpd_req_to_sockfd(req)),
        .payload = (char*)ws_pkt.payload,
        .payload_len = ws_pkt.len
    };

    if (xQueueSend(ws_rx_q, &job, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(WS_TAG, "RX queue full, dropping message from handle 0x%08X", job.handle);
        free(ws_pkt.payload);
    } else {
        ESP_LOGV(WS_TAG, "Enqueued message from handle 0x%08X to RX queue", job.handle);
        /* ownership of payload transferred to the RX task; do NOT free here */
    }
    
    return ESP_OK;
}

/* Parse and dispatch a single received RAP message. Frees root on return. */
static void ws_dispatch_message(ws_client_handle_t handle, cJSON *root)
{
    cJSON *type_j   = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *req_id_j = cJSON_GetObjectItemCaseSensitive(root, "req_id");
    cJSON *name_j   = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *params_j = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (!cJSON_IsString(type_j) || !type_j->valuestring) {
        ESP_LOGW(WS_TAG, "Message from handle 0x%08X missing or invalid 'type'", handle);
        return;
    }

    if (!cJSON_IsNumber(req_id_j)) {
        ESP_LOGW(WS_TAG, "Message from handle 0x%08X missing or invalid 'req_id'", handle);
        return;
    }

    double req_id_d = req_id_j->valuedouble;
    if (req_id_d < 0 || req_id_d > (double)UINT32_MAX || req_id_d != (double)(uint32_t)req_id_d) {
        ESP_LOGW(WS_TAG, "Message from handle 0x%08X has out-of-range or non-integer 'req_id': %g", handle, req_id_d);
        return;
    }

    const char *type   = type_j->valuestring;
    uint32_t    req_id = (uint32_t)req_id_d;

    /* params is optional; pass NULL to callbacks if absent */
    if (!cJSON_IsObject(params_j)) {
        params_j = NULL;
    }

    if (strcmp(type, "cmd") == 0) {
        if (!cJSON_IsString(name_j) || !name_j->valuestring) {
            ESP_LOGW(WS_TAG, "cmd from handle 0x%08X missing 'name'", handle);
            ws_send_error(handle, req_id, "missing 'name' field");
            return;
        }
        ESP_LOGD(WS_TAG, "cmd '%s' from handle 0x%08X (req_id=%"PRIu32")", name_j->valuestring, handle, req_id);
        if (g_on_cmd_cb) {
            esp_err_t r = g_on_cmd_cb(handle, name_j->valuestring, params_j, req_id);
            if (r != ESP_OK) {
                ESP_LOGW(WS_TAG, "on_cmd_cb returned %s for cmd '%s'", esp_err_to_name(r), name_j->valuestring);
            }
        }
    } else if (strcmp(type, "req") == 0) {
        if (!cJSON_IsString(name_j) || !name_j->valuestring) {
            ESP_LOGW(WS_TAG, "req from handle 0x%08X missing 'name'", handle);
            ws_send_error(handle, req_id, "missing 'name' field");
            return;
        }
        ESP_LOGD(WS_TAG, "req '%s' from handle 0x%08X (req_id=%"PRIu32")", name_j->valuestring, handle, req_id);
        if (g_on_req_cb) {
            esp_err_t r = g_on_req_cb(handle, name_j->valuestring, params_j, req_id);
            if (r != ESP_OK) {
                ESP_LOGW(WS_TAG, "on_req_cb returned %s for req '%s'", esp_err_to_name(r), name_j->valuestring);
            }
        }
    } else {
        ESP_LOGW(WS_TAG, "Unhandled message type '%s' from handle 0x%08X (req_id=%"PRIu32")", type, handle, req_id);
    }
}

static void ws_rx_task(void *arg)
{
    rx_job_t job;
    for (;;) {
        xQueueReceive(ws_rx_q, &job, portMAX_DELAY);

        ESP_LOGV(WS_TAG, "RX task processing message from handle 0x%08X (%u bytes)", job.handle, job.payload_len);

        cJSON *root = cJSON_Parse(job.payload);
        free(job.payload);
        job.payload = NULL;

        if (!root) {
            ESP_LOGW(WS_TAG, "Failed to parse JSON from handle 0x%08X", job.handle);
            continue;
        }

        ws_dispatch_message(job.handle, root);
        cJSON_Delete(root);
    }
}

esp_err_t ws_start_task(void)
{
    if (!s_lock) ws_pool_init();

    BaseType_t ret = xTaskCreate(
        ws_rx_task,
        "ws_rx",
        CONFIG_WS_RX_TASK_STACK_SIZE,
        NULL,
        CONFIG_WS_RX_TASK_PRIORITY,
        NULL
    );

    if (ret != pdPASS) {
        ESP_LOGE(WS_TAG, "Failed to create ws_rx task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(WS_TAG, "WS receive task started (stack=%d, prio=%d)",
             CONFIG_WS_RX_TASK_STACK_SIZE, CONFIG_WS_RX_TASK_PRIORITY);
    return ESP_OK;
}
