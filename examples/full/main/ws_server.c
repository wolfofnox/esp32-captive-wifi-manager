#include "ws_server.h"

#include "sdkconfig.h"

#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_EX
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <strings.h>
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

QueueHandle_t ws_rx_q;

on_cmd_cb   g_on_cmd_cb   = NULL;
on_req_cb   g_on_req_cb   = NULL;
on_close_cb g_on_close_cb = NULL;
on_open_cb  g_on_open_cb  = NULL;
on_sub_cb   g_on_sub_cb   = NULL;
on_unsub_cb g_on_unsub_cb = NULL;

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
        s_slots[i].next_sub_id = 0;
        memset(s_slots[i].out_reqs, 0, sizeof(s_slots[i].out_reqs));
        memset(s_slots[i].out_subs, 0, sizeof(s_slots[i].out_subs));
        memset(s_slots[i].in_subs,  0, sizeof(s_slots[i].in_subs));
    }
    
    ws_rx_q = xQueueCreate(CONFIG_WS_RX_QUEUE_SIZE, sizeof(struct rx_job));
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

/* allocate the first free slot */
static esp_err_t ws_slot_alloc(httpd_handle_t hd, int sockfd, ws_client_handle_t *out_handle)
{
    if (!s_lock) ws_pool_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    // first try to find an existing slot for this sockfd (reused connection)
    for (int i = 0; i < CONFIG_MAX_WS_CLIENTS; ++i) {
        if (s_slots[i].fd == sockfd) {
            if (s_slots[i].used) ESP_RETURN_ON_ERROR(ESP_ERR_INVALID_STATE, WS_TAG, "Sockfd %d already has an allocated slot, same socket can not have two open connections", sockfd);
            s_slots[i].used = true;
            s_slots[i].hd = hd;
            s_slots[i].next_sub_id = 0;
            memset(s_slots[i].out_reqs, 0, sizeof(s_slots[i].out_reqs));
            memset(s_slots[i].out_subs, 0, sizeof(s_slots[i].out_subs));
            memset(s_slots[i].in_subs,  0, sizeof(s_slots[i].in_subs));
            if (s_slots[i].in_flight_semaphore == NULL) {
                s_slots[i].in_flight_semaphore = xSemaphoreCreateCounting(CONFIG_MAX_IN_FLIGHT_MSGS, CONFIG_MAX_IN_FLIGHT_MSGS);
            }
            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            ESP_LOGD(WS_TAG, "Reusing slot %d for sockfd=%d", i, sockfd);
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
            s_slots[i].next_sub_id = 0;
            memset(s_slots[i].out_reqs, 0, sizeof(s_slots[i].out_reqs));
            memset(s_slots[i].out_subs, 0, sizeof(s_slots[i].out_subs));
            memset(s_slots[i].in_subs,  0, sizeof(s_slots[i].in_subs));
            /* bump generation; avoid generation==0 */
            s_slots[i].gen = (s_slots[i].gen + 1) ? (s_slots[i].gen + 1) : 1;

            if (s_slots[i].in_flight_semaphore == NULL) {
                s_slots[i].in_flight_semaphore = xSemaphoreCreateCounting(CONFIG_MAX_IN_FLIGHT_MSGS, CONFIG_MAX_IN_FLIGHT_MSGS);
            }

            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            ESP_LOGD(WS_TAG, "allocated slot %d -> handle 0x%08X (sockfd=%d)", i, h, sockfd);
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
            s_slots[i].next_sub_id = 0;
            memset(s_slots[i].out_reqs, 0, sizeof(s_slots[i].out_reqs));
            memset(s_slots[i].out_subs, 0, sizeof(s_slots[i].out_subs));
            memset(s_slots[i].in_subs,  0, sizeof(s_slots[i].in_subs));
            /* bump generation; avoid generation==0 */
            s_slots[i].gen = (s_slots[i].gen + 1) ? (s_slots[i].gen + 1) : 1;

            if (s_slots[i].in_flight_semaphore == NULL) {
                s_slots[i].in_flight_semaphore = xSemaphoreCreateCounting(CONFIG_MAX_IN_FLIGHT_MSGS, CONFIG_MAX_IN_FLIGHT_MSGS);
            }

            ws_client_handle_t h = make_handle(i, s_slots[i].gen);
            xSemaphoreGive(s_lock);
            ESP_LOGD(WS_TAG, "allocated slot %d -> handle 0x%08X (sockfd=%d)", i, h, sockfd);
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
            ESP_LOGD(WS_TAG, "freeing slot %d (sockfd=%d)", i, sockfd);
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

esp_err_t ws_register_callbacks(on_open_cb open_cb, on_cmd_cb cmd_cb, on_req_cb req_cb,
                                 on_sub_cb sub_cb, on_unsub_cb unsub_cb,
                                 on_close_cb close_cb)
{
    if (!s_lock) ws_pool_init();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    g_on_open_cb  = open_cb;
    g_on_cmd_cb   = cmd_cb;
    g_on_req_cb   = req_cb;
    g_on_sub_cb   = sub_cb;
    g_on_unsub_cb = unsub_cb;
    g_on_close_cb = close_cb;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t ws_send_json(ws_client_handle_t handle, cJSON *json) {
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t out = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
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

static esp_err_t ws_ack(ws_client_handle_t handle, uint32_t req_id) {
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
    if (!resp) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "type", "resp");
    cJSON_AddNumberToObject(resp, "req_id", req_id);
    if (payload) {
        cJSON_AddItemToObject(resp, "payload", payload); /* takes ownership */
    } else {
        cJSON_AddItemToObject(resp, "payload", cJSON_CreateObject()); /* empty payload */
    }
    esp_err_t r = ws_send_json(handle, resp);
    cJSON_Delete(resp);
    ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (slot && slot->last_recv_req_id == req_id && !slot->last_recv_req_acked) {
        slot->last_recv_req_acked = true;
    }
    xSemaphoreGive(s_lock);
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
    ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (slot && slot->last_recv_req_id == req_id && !slot->last_recv_req_acked) {
        slot->last_recv_req_acked = true;
    }
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t ws_respond_sub(ws_client_handle_t handle, uint32_t req_id, uint16_t sub_id, cJSON *snapshot) {
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        cJSON_Delete(snapshot);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload, "sub_id", sub_id);
    if (snapshot) {
        cJSON_AddItemToObject(payload, "snapshot", snapshot); /* takes ownership */
    }
    return ws_respond(handle, req_id, payload);
}

esp_err_t ws_send_sub_delta(ws_client_handle_t handle, uint16_t sub_id, cJSON *payload) {
    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(msg, "type", "delta");
    cJSON_AddNumberToObject(msg, "sub_id", sub_id);
    if (payload) {
        cJSON_AddItemToObject(msg, "payload", payload); /* takes ownership */
    }
    esp_err_t r = ws_send_json(handle, msg);
    cJSON_Delete(msg);
    return r;
}

/* ─── Server-initiated messaging helpers ─── */

/* Common implementation for ws_send_cmd / ws_send_req */
static esp_err_t ws_send_out_req(ws_client_handle_t handle, const char *type, const char *name,
                                  cJSON *params, ws_req_cb_t cb, void *user_data)
{
    ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
    if (!slot) {
        cJSON_Delete(params);
        return ESP_ERR_NOT_FOUND;
    }

    /* Wait for a free in-flight slot (1-second timeout to avoid blocking callers forever) */
    if (xSemaphoreTake(slot->in_flight_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(WS_TAG, "In-flight limit reached for handle 0x%08X", handle);
        cJSON_Delete(params);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Find a free out_req slot */
    int idx = -1;
    for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
        if (!slot->out_reqs[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(slot->in_flight_semaphore);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }

    uint32_t req_id = ++slot->last_req_id;
    if (req_id == 0) req_id = ++slot->last_req_id; /* skip 0 */

    slot->out_reqs[idx].in_use    = true;
    slot->out_reqs[idx].req_id    = req_id;
    slot->out_reqs[idx].cb        = cb;
    slot->out_reqs[idx].user_data = user_data;

    xSemaphoreGive(s_lock);

    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot->out_reqs[idx].in_use = false;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(slot->in_flight_semaphore);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(msg, "type", type);
    cJSON_AddNumberToObject(msg, "req_id", (double)req_id);
    cJSON_AddStringToObject(msg, "name", name);
    if (params) {
        cJSON_AddItemToObject(msg, "params", params); /* takes ownership */
    }

    esp_err_t r = ws_send_json(handle, msg);
    cJSON_Delete(msg);

    if (r != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot->out_reqs[idx].in_use = false;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(slot->in_flight_semaphore);
    }
    return r;
}

esp_err_t ws_send_cmd(ws_client_handle_t handle, const char *name, cJSON *params,
                       ws_req_cb_t cb, void *user_data)
{
    return ws_send_out_req(handle, "cmd", name, params, cb, user_data);
}

esp_err_t ws_send_req(ws_client_handle_t handle, const char *name, cJSON *params,
                       ws_req_cb_t cb, void *user_data)
{
    return ws_send_out_req(handle, "req", name, params, cb, user_data);
}

esp_err_t ws_subscribe(ws_client_handle_t handle, const char *name, cJSON *params,
                        ws_sub_snapshot_cb_t snapshot_cb, ws_sub_delta_cb_t delta_cb,
                        void *user_data)
{
    ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
    if (!slot) {
        cJSON_Delete(params);
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(slot->in_flight_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(WS_TAG, "In-flight limit reached for handle 0x%08X", handle);
        cJSON_Delete(params);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int idx = -1;
    for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
        if (!slot->out_subs[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(slot->in_flight_semaphore);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }

    uint32_t req_id = ++slot->last_req_id;
    if (req_id == 0) req_id = ++slot->last_req_id;

    slot->out_subs[idx].in_use      = true;
    slot->out_subs[idx].req_id      = req_id;
    slot->out_subs[idx].snapshot_cb = snapshot_cb;
    slot->out_subs[idx].delta_cb    = delta_cb;
    slot->out_subs[idx].user_data   = user_data;

    xSemaphoreGive(s_lock);

    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot->out_subs[idx].in_use = false;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(slot->in_flight_semaphore);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(msg, "type", "sub");
    cJSON_AddNumberToObject(msg, "req_id", (double)req_id);
    cJSON_AddStringToObject(msg, "name", name);
    if (params) {
        cJSON_AddItemToObject(msg, "params", params); /* takes ownership */
    }

    esp_err_t r = ws_send_json(handle, msg);
    cJSON_Delete(msg);

    if (r != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot->out_subs[idx].in_use = false;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(slot->in_flight_semaphore);
    }
    return r;
}

esp_err_t ws_unsubscribe(ws_client_handle_t handle, uint16_t sub_id)
{
    ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
    if (!slot) return ESP_ERR_NOT_FOUND;

    /* Remove from active incoming subs */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
        if (slot->in_subs[i].in_use && slot->in_subs[i].sub_id == sub_id) {
            slot->in_subs[i].in_use = false;
            break;
        }
    }
    uint32_t req_id = ++slot->last_req_id;
    if (req_id == 0) req_id = ++slot->last_req_id;
    xSemaphoreGive(s_lock);

    cJSON *params = cJSON_CreateObject();
    if (!params) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(params, "sub_id", sub_id);

    cJSON *msg = cJSON_CreateObject();
    if (!msg) { cJSON_Delete(params); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(msg, "type", "unsub");
    cJSON_AddNumberToObject(msg, "req_id", (double)req_id);
    cJSON_AddItemToObject(msg, "params", params);

    esp_err_t r = ws_send_json(handle, msg);
    cJSON_Delete(msg);
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
                    ESP_LOGE(WS_TAG, "Failed to receive WebSocket frame payload from socket %d, freeing slot", fd);
                    return ESP_OK;
                }
            }
        } else {  
            // Allocation failed: treat CLOSE frame as having an empty payload  
            ESP_LOGW(WS_TAG, "Failed to allocate memory for WebSocket CLOSE payload from socket %d, treating as empty", fd);  
            ws_pkt.len = 0;  
        }  

        ws_pkt.payload[ws_pkt.len] = 0; // null-terminate for logging

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

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW(WS_TAG, "Unsupported WebSocket frame type %d (non-TEXT) from socket %d, ignoring", ws_pkt.type, httpd_req_to_sockfd(req));
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

/* Parse and dispatch a single received RAP message. */
static void ws_dispatch_message(ws_client_handle_t handle, cJSON *root)
{
    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j) || !type_j->valuestring) {
        ESP_LOGW(WS_TAG, "Message from handle 0x%08X missing or invalid 'type'", handle);
        return;
    }
    const char *type = type_j->valuestring;

    /* ── delta: streaming update from client for a server-initiated subscription ── */
    if (strcmp(type, "delta") == 0) {
        cJSON *sub_id_j = cJSON_GetObjectItemCaseSensitive(root, "sub_id");
        cJSON *payload_j = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (!cJSON_IsNumber(sub_id_j)) {
            ESP_LOGW(WS_TAG, "delta from handle 0x%08X missing 'sub_id'", handle);
            return;
        }
        uint16_t sub_id = (uint16_t)sub_id_j->valuedouble;
        ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
        if (!slot) return;

        ws_sub_delta_cb_t cb = NULL;
        void *ud = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
            if (slot->in_subs[i].in_use && slot->in_subs[i].sub_id == sub_id) {
                cb = slot->in_subs[i].delta_cb;
                ud = slot->in_subs[i].user_data;
                break;
            }
        }
        xSemaphoreGive(s_lock);

        if (cb) {
            cb(handle, sub_id, payload_j, ud);
        } else {
            ESP_LOGW(WS_TAG, "delta from handle 0x%08X for unknown sub_id=%"PRIu16, handle, sub_id);
        }
        return;
    }

    /* ── ack: client received the message; release the in-flight slot so the
     *        server can send new messages, but keep the bookkeeping entry alive
     *        until the final resp/err arrives. ── */
    if (strcmp(type, "ack") == 0) {
        cJSON *req_id_j = cJSON_GetObjectItemCaseSensitive(root, "req_id");
        if (!cJSON_IsNumber(req_id_j)) return;
        double req_id_d = req_id_j->valuedouble;
        if (req_id_d < 0 || req_id_d > (double)UINT32_MAX || req_id_d != (double)(uint32_t)req_id_d) return;
        uint32_t req_id = (uint32_t)req_id_d;

        ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
        if (!slot) return;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool released = false;
        for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
            if (slot->out_reqs[i].in_use && !slot->out_reqs[i].acked && slot->out_reqs[i].req_id == req_id) {
                slot->out_reqs[i].acked = true;
                released = true;
                break;
            }
        }
        if (!released) {
            for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
                if (slot->out_subs[i].in_use && !slot->out_subs[i].acked && slot->out_subs[i].req_id == req_id) {
                    slot->out_subs[i].acked = true;
                    released = true;
                    break;
                }
            }
        }
        xSemaphoreGive(s_lock);

        if (released) {
            xSemaphoreGive(slot->in_flight_semaphore);
            ESP_LOGD(WS_TAG, "ack from handle 0x%08X for req_id=%"PRIu32" — in-flight slot freed", handle, req_id);
        } else {
            ESP_LOGW(WS_TAG, "ack from handle 0x%08X for unknown or already-acked req_id=%"PRIu32, handle, req_id);
        }
        return;
    }

    /* ── resp / err: response to a server-initiated request or subscription ── */
    if (strcmp(type, "resp") == 0 || strcmp(type, "err") == 0) {
        cJSON *req_id_j = cJSON_GetObjectItemCaseSensitive(root, "req_id");
        if (!cJSON_IsNumber(req_id_j)) {
            ESP_LOGW(WS_TAG, "%s from handle 0x%08X missing 'req_id'", type, handle);
            return;
        }
        double req_id_d = req_id_j->valuedouble;
        if (req_id_d < 0 || req_id_d > (double)UINT32_MAX || req_id_d != (double)(uint32_t)req_id_d) {
            ESP_LOGW(WS_TAG, "%s from handle 0x%08X invalid 'req_id': %g", type, handle, req_id_d);
            return;
        }
        uint32_t req_id = (uint32_t)req_id_d;
        bool success = (strcmp(type, "resp") == 0);

        cJSON *payload_j = cJSON_GetObjectItemCaseSensitive(root, "payload");

        ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
        if (!slot) return;

        /* Check for a matching pending subscription (resp with sub_id in payload) */
        if (success) {
            cJSON *sub_id_j = payload_j ? cJSON_GetObjectItemCaseSensitive(payload_j, "sub_id") : NULL;
            if (cJSON_IsNumber(sub_id_j)) {
                uint16_t sub_id = (uint16_t)sub_id_j->valuedouble;
                cJSON *snapshot_j = payload_j ? cJSON_GetObjectItemCaseSensitive(payload_j, "snapshot") : NULL;

                xSemaphoreTake(s_lock, portMAX_DELAY);
                for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
                    if (slot->out_subs[i].in_use && slot->out_subs[i].req_id == req_id) {
                        ws_sub_snapshot_cb_t scb  = slot->out_subs[i].snapshot_cb;
                        ws_sub_delta_cb_t    dcb  = slot->out_subs[i].delta_cb;
                        void                *ud   = slot->out_subs[i].user_data;
                        bool                 was_acked = slot->out_subs[i].acked;
                        slot->out_subs[i].in_use = false;

                        /* Promote to active incoming sub */
                        for (int j = 0; j < CONFIG_MAX_IN_FLIGHT_MSGS; j++) {
                            if (!slot->in_subs[j].in_use) {
                                slot->in_subs[j].in_use    = true;
                                slot->in_subs[j].sub_id    = sub_id;
                                slot->in_subs[j].delta_cb  = dcb;
                                slot->in_subs[j].user_data = ud;
                                break;
                            }
                        }
                        xSemaphoreGive(s_lock);
                        if (!was_acked) xSemaphoreGive(slot->in_flight_semaphore);
                        ESP_LOGD(WS_TAG, "server sub accepted by client: req_id=%"PRIu32" -> sub_id=%"PRIu16, req_id, sub_id);
                        if (scb) scb(handle, sub_id, true, snapshot_j, ud);
                        return;
                    }
                }
                xSemaphoreGive(s_lock);
                ESP_LOGW(WS_TAG, "resp with sub_id from handle 0x%08X but no matching pending sub (req_id=%"PRIu32")", handle, req_id);
                return;
            }
        } else {
            /* err: check for a pending subscription first */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
                if (slot->out_subs[i].in_use && slot->out_subs[i].req_id == req_id) {
                    ws_sub_snapshot_cb_t scb = slot->out_subs[i].snapshot_cb;
                    void                *ud  = slot->out_subs[i].user_data;
                    bool                 was_acked = slot->out_subs[i].acked;
                    slot->out_subs[i].in_use = false;
                    xSemaphoreGive(s_lock);
                    if (!was_acked) xSemaphoreGive(slot->in_flight_semaphore);
                    if (scb) scb(handle, 0, false, NULL, ud);
                    return;
                }
            }
            xSemaphoreGive(s_lock);
        }

        /* Fall through: must be a pending cmd/req */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < CONFIG_MAX_IN_FLIGHT_MSGS; i++) {
            if (slot->out_reqs[i].in_use && slot->out_reqs[i].req_id == req_id) {
                ws_req_cb_t cb = slot->out_reqs[i].cb;
                void       *ud = slot->out_reqs[i].user_data;
                bool        was_acked = slot->out_reqs[i].acked;
                slot->out_reqs[i].in_use = false;
                xSemaphoreGive(s_lock);
                if (!was_acked) xSemaphoreGive(slot->in_flight_semaphore);
                if (cb) cb(handle, success, success ? payload_j : NULL, ud);
                return;
            }
        }
        xSemaphoreGive(s_lock);
        ESP_LOGW(WS_TAG, "%s from handle 0x%08X for unknown req_id=%"PRIu32, type, handle, req_id);
        return;
    }

    /* ── Messages that require req_id and come from the client ── */
    cJSON *req_id_j = cJSON_GetObjectItemCaseSensitive(root, "req_id");
    if (!cJSON_IsNumber(req_id_j)) {
        ESP_LOGW(WS_TAG, "'%s' from handle 0x%08X missing or invalid 'req_id'", type, handle);
        return;
    }
    double req_id_d = req_id_j->valuedouble;
    if (req_id_d < 0 || req_id_d > (double)UINT32_MAX || req_id_d != (double)(uint32_t)req_id_d) {
        ESP_LOGW(WS_TAG, "'%s' from handle 0x%08X has out-of-range 'req_id': %g", type, handle, req_id_d);
        return;
    }
    uint32_t req_id = (uint32_t)req_id_d;

    cJSON *name_j   = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *params_j = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsObject(params_j)) params_j = NULL;

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

    } else if (strcmp(type, "sub") == 0) {
        if (!cJSON_IsString(name_j) || !name_j->valuestring) {
            ESP_LOGW(WS_TAG, "sub from handle 0x%08X missing 'name'", handle);
            ws_send_error(handle, req_id, "missing 'name' field");
            return;
        }
        /* Assign a server-side sub_id */
        ws_client_ctx_t *slot = ws_slot_get_by_handle(handle);
        if (!slot) return;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint16_t sub_id = ++slot->next_sub_id;
        if (sub_id == 0) sub_id = ++slot->next_sub_id; /* avoid 0 */
        xSemaphoreGive(s_lock);

        ESP_LOGD(WS_TAG, "sub '%s' from handle 0x%08X (req_id=%"PRIu32", assigned sub_id=%"PRIu16")",
                 name_j->valuestring, handle, req_id, sub_id);

        if (g_on_sub_cb) {
            esp_err_t r = g_on_sub_cb(handle, name_j->valuestring, params_j, req_id, sub_id);
            if (r != ESP_OK) {
                ESP_LOGW(WS_TAG, "on_sub_cb returned %s for sub '%s'", esp_err_to_name(r), name_j->valuestring);
            }
        } else {
            ws_send_error(handle, req_id, "subscriptions not supported");
        }

    } else if (strcmp(type, "unsub") == 0) {
        cJSON *sub_id_j = params_j ? cJSON_GetObjectItemCaseSensitive(params_j, "sub_id") : NULL;
        if (!cJSON_IsNumber(sub_id_j)) {
            ESP_LOGW(WS_TAG, "unsub from handle 0x%08X missing 'params.sub_id'", handle);
            ws_send_error(handle, req_id, "missing 'params.sub_id'");
            return;
        }
        uint16_t sub_id = (uint16_t)sub_id_j->valuedouble;
        ESP_LOGD(WS_TAG, "unsub sub_id=%"PRIu16" from handle 0x%08X (req_id=%"PRIu32")", sub_id, handle, req_id);
        if (g_on_unsub_cb) {
            g_on_unsub_cb(handle, sub_id);
        }
        /* Auto-respond to confirm the unsubscribe */
        ws_respond(handle, req_id, cJSON_CreateObject());

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

        /* Determine if this is a client-initiated message (needs req_id tracking + auto-ack for cmd).
         * Responses from the client (ack/resp/err) and deltas do not need this tracking. */
        cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
        const char *type_str = cJSON_IsString(type_j) ? type_j->valuestring : "";

        bool is_client_request = (strcmp(type_str, "cmd")   == 0 ||
                                   strcmp(type_str, "req")   == 0 ||
                                   strcmp(type_str, "sub")   == 0 ||
                                   strcmp(type_str, "unsub") == 0);

        uint32_t recv_req_id = 0;
        if (is_client_request) {
            cJSON *req_id_j = cJSON_GetObjectItemCaseSensitive(root, "req_id");
            if (cJSON_IsNumber(req_id_j)) {
                double d = req_id_j->valuedouble;
                if (d >= 0 && d <= (double)UINT32_MAX && d == (double)(uint32_t)d) {
                    recv_req_id = (uint32_t)d;
                    ws_client_ctx_t *slot = ws_slot_get_by_handle(job.handle);
                    if (slot) {
                        xSemaphoreTake(s_lock, portMAX_DELAY);
                        slot->last_recv_req_id    = recv_req_id;
                        slot->last_recv_req_acked = false;
                        xSemaphoreGive(s_lock);
                    }
                }
            }
        }

        ws_dispatch_message(job.handle, root);

        /* Auto-ack `cmd` messages if the callback did not already respond */
        if (strcmp(type_str, "cmd") == 0 && recv_req_id != 0) {
            ws_client_ctx_t *slot = ws_slot_get_by_handle(job.handle);
            if (slot) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                bool need_ack = (slot->last_recv_req_id == recv_req_id && !slot->last_recv_req_acked);
                xSemaphoreGive(s_lock);
                if (need_ack) {
                    ESP_LOGV(WS_TAG, "auto-acking cmd req_id=%"PRIu32" from handle 0x%08X", recv_req_id, job.handle);
                    ws_ack(job.handle, recv_req_id);
                }
            }
        }

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
