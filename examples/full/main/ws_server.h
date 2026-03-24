#ifndef WS_SERVER_H
#define WS_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#include "sdkconfig.h"

typedef uint32_t ws_client_handle_t;

typedef struct ws_client_ctx ws_client_ctx_t;

/* ─── Callback types (client → server messages) ─── */

/** Called when a client opens a new WebSocket connection. */
typedef esp_err_t (*on_open_cb)(ws_client_handle_t handle, ws_client_ctx_t *ctx);

/** Called when the client sends a `cmd` message. Auto-acked if the callback
 *  does not call ws_respond/ws_send_error. */
typedef esp_err_t (*on_cmd_cb)(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id);

/** Called when the client sends a `req` message. Must call ws_respond or
 *  ws_send_error to complete the request. */
typedef esp_err_t (*on_req_cb)(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id);

/** Called when the client sends a `sub` message. sub_id is pre-assigned by the
 *  server. Must call ws_respond_sub (or ws_send_error) to complete the subscription. */
typedef esp_err_t (*on_sub_cb)(ws_client_handle_t handle, const char *name, cJSON *params, uint32_t req_id, uint16_t sub_id);

/** Called when the client sends an `unsub` message. The server automatically
 *  sends an acknowledgement response; this callback is for application cleanup only. */
typedef esp_err_t (*on_unsub_cb)(ws_client_handle_t handle, uint16_t sub_id);

/** Called when the WebSocket connection is closed. */
typedef esp_err_t (*on_close_cb)(ws_client_handle_t handle);

/* ─── Callback types (server → client responses) ─── */

/** Called when a server-initiated cmd or req receives a response (resp or err).
 *  success=true:  payload is the response payload (do not free; owned by caller).
 *  success=false: payload is NULL (request rejected or connection closed). */
typedef void (*ws_req_cb_t)(ws_client_handle_t handle, bool success, cJSON *payload, void *user_data);

/** Called when a server-initiated subscription receives its initial snapshot.
 *  success=true:  sub_id is the client-assigned subscription id; snapshot may be NULL.
 *  success=false: sub_id=0, the subscription was rejected by the client. */
typedef void (*ws_sub_snapshot_cb_t)(ws_client_handle_t handle, uint16_t sub_id, bool success, cJSON *snapshot, void *user_data);

/** Called when the client sends a delta for an active server-initiated subscription. */
typedef void (*ws_sub_delta_cb_t)(ws_client_handle_t handle, uint16_t sub_id, cJSON *payload, void *user_data);

/* ─── Internal per-client tracking (embedded in ws_client_ctx_t) ─── */

/** Pending server-initiated request (cmd or req) waiting for a client response. */
typedef struct {
    bool         in_use;
    uint32_t     req_id;
    ws_req_cb_t  cb;
    void        *user_data;
} ws_out_req_entry_t;

/** Pending server-initiated subscription waiting for client to assign a sub_id. */
typedef struct {
    bool                  in_use;
    uint32_t              req_id;
    ws_sub_snapshot_cb_t  snapshot_cb;
    ws_sub_delta_cb_t     delta_cb;
    void                 *user_data;
} ws_out_sub_entry_t;

/** Active subscription initiated by the server; the client sends deltas. */
typedef struct {
    bool               in_use;
    uint16_t           sub_id;   /* client-assigned */
    ws_sub_delta_cb_t  delta_cb;
    void              *user_data;
} ws_in_sub_entry_t;

struct ws_client_ctx {
    bool used;
    uint16_t gen;
    int fd;
    httpd_handle_t hd;

    /* server → client bookkeeping */
    uint32_t           last_req_id;        /* next req_id for server-initiated messages */
    SemaphoreHandle_t  in_flight_semaphore; /* counts in-flight server-initiated requests */
    ws_out_req_entry_t out_reqs[CONFIG_MAX_IN_FLIGHT_MSGS]; /* pending server cmd/req */
    ws_out_sub_entry_t out_subs[CONFIG_MAX_IN_FLIGHT_MSGS]; /* pending server subs */
    ws_in_sub_entry_t  in_subs[CONFIG_MAX_IN_FLIGHT_MSGS];  /* active server-init subs (client sends deltas) */

    /* client → server bookkeeping */
    uint32_t last_recv_req_id;   /* req_id of the last client message being processed */
    bool     last_recv_req_acked; /* true once a response has been sent for last_recv_req_id */
    uint16_t next_sub_id;        /* counter for server-assigned subscription ids */
};

/* ─── Lifecycle ─── */

/** Register callbacks for all WebSocket events. Any callback may be NULL. */
esp_err_t ws_register_callbacks(on_open_cb open_cb, on_cmd_cb cmd_cb, on_req_cb req_cb,
                                 on_sub_cb sub_cb, on_unsub_cb unsub_cb,
                                 on_close_cb close_cb);

/** Start the receive-processing task. Call once after ws_register_callbacks(). */
esp_err_t ws_start_task(void);

/** ESP-IDF HTTP server handler — register with .is_websocket = true. */
esp_err_t ws_handler(httpd_req_t *req);

/* ─── Respond to client-initiated messages ─── */

/** Send a `resp` message to the client. Takes ownership of payload (may be NULL). */
esp_err_t ws_respond(ws_client_handle_t handle, uint32_t req_id, cJSON *payload);

/** Send an `err` message to the client. */
esp_err_t ws_send_error(ws_client_handle_t handle, uint32_t req_id, const char *error_msg);

/** Respond to a client `sub` request with the server-assigned sub_id and an
 *  optional initial snapshot. Takes ownership of snapshot (may be NULL). */
esp_err_t ws_respond_sub(ws_client_handle_t handle, uint32_t req_id, uint16_t sub_id, cJSON *snapshot);

/** Send a streaming `delta` update for a client-initiated subscription.
 *  Takes ownership of payload. */
esp_err_t ws_send_sub_delta(ws_client_handle_t handle, uint16_t sub_id, cJSON *payload);

/* ─── Server-initiated messages ─── */

/** Send a `cmd` to the client. cb is called when the client responds (may be NULL).
 *  Takes ownership of params (may be NULL). */
esp_err_t ws_send_cmd(ws_client_handle_t handle, const char *name, cJSON *params,
                       ws_req_cb_t cb, void *user_data);

/** Send a `req` to the client. cb is called with the client's response.
 *  Takes ownership of params (may be NULL). */
esp_err_t ws_send_req(ws_client_handle_t handle, const char *name, cJSON *params,
                       ws_req_cb_t cb, void *user_data);

/** Subscribe to a client data stream. snapshot_cb is called when the client accepts
 *  or rejects the subscription. delta_cb is called for each subsequent delta.
 *  Takes ownership of params (may be NULL). */
esp_err_t ws_subscribe(ws_client_handle_t handle, const char *name, cJSON *params,
                        ws_sub_snapshot_cb_t snapshot_cb, ws_sub_delta_cb_t delta_cb,
                        void *user_data);

/** Unsubscribe from a client data stream (sub_id was assigned by the client). */
esp_err_t ws_unsubscribe(ws_client_handle_t handle, uint16_t sub_id);

#endif // WS_SERVER_H