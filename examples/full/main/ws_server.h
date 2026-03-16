#ifndef WS_SERVER_H
#define WS_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include "cJSON.h"

typedef uint32_t ws_client_handle_t;

typedef struct ws_client_ctx ws_client_ctx_t;

/* callback prototypes you can set per-client */
typedef esp_err_t (*on_open_cb)(ws_client_handle_t handle, ws_client_ctx_t *ctx);
typedef esp_err_t (*on_cmd_cb)(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id);
typedef esp_err_t (*on_req_cb)(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id);
typedef esp_err_t (*on_close_cb)(ws_client_handle_t handle);

struct ws_client_ctx {
    bool used;
    uint16_t gen;
    int fd;
    httpd_handle_t hd;
    /* bookkeeping: pending requests, subscriptions, mutex, etc. */
    uint32_t               last_req_id; // if server issues requests to client
    SemaphoreHandle_t      in_flight_semaphore; // counts in-flight requests initiated by server
};


esp_err_t ws_register_callbacks(on_open_cb open_cb, on_cmd_cb cmd_cb, on_req_cb req_cb, on_close_cb close_cb);

esp_err_t ws_handler(httpd_req_t *req);

esp_err_t ws_respond(ws_client_handle_t handle, uint32_t req_id, cJSON *payload);
esp_err_t ws_send_error(ws_client_handle_t handle, uint32_t req_id, const char *error_msg);
// other send a register handler funcs

#endif // WS_SERVER_H