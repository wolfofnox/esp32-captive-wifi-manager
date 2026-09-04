# Realtime Action Protocol (RAP) (JSON-style encoding)

This protocol is designed for JSON-style encodings over WebSocket. Versioning is negotiated in the WebSocket subprotocol (e.g., `rap.v1+json`), so messages do **not** include a per-message version field.

---

## Core Message Types

All request-like messages are **bidirectional** (client ↔ server) and use:

```json
{ "type": "...", "req_id": 1, "name": "string", "params": { ... } }
```

`req_id` is a **per-sender incrementing integer**. Responses echo the same `req_id`.

---

## 1) cmd (action / interrupt)

**Purpose:** Perform an action (may have side-effects).  
**Direction:** both directions.

**Message:**

```json
{ "type": "cmd", "req_id": 1, "name": "move", "params": { "x": 1, "y": 0, "speed": 0.6 } }
```

---

## 2) request (query / RPC)

**Purpose:** One-time query or fetch.

**Message:**

```json
{ "type": "req", "req_id": 2, "name": "wifi_stats", "params": { ... } }
```

**Result (snapshot):**

```json
{ "type": "resp", "req_id": 2, "payload": { "snapshot": { ... } } }
```

---

## 3) subscribe (start streaming updates)

**Purpose:** Begin a streaming subscription.

**Message:**

```json
{ "type": "sub", "req_id": 3, "name": "telemetry", "params": { "rate": 20, ... } }
```

**Result (with sub_id + optional snapshot):**

```json
{ "type": "resp", "req_id": 3, "payload": { "sub_id": 7, "snapshot": { ... } } }
```

**Note:** `sub_id` is assigned by the responder (server or client).

---

## 4) unsubscribe (stop streaming updates)

**Purpose:** End an existing subscription.

**Message:**

```json
{ "type": "unsub", "req_id": 4, "params": { "sub_id": 7 } }
```

---

## Responses

### ack (optional early accept)

```json
{ "type": "ack", "req_id": 3 }
```

### result (success)

```json
{ "type": "resp", "req_id": 3, "payload": { ... } }
```

### error (failure)

```json
{ "type": "err", "req_id": 3, "err": "reason" }
```

---

## Streaming Updates

### delta (incremental updates)

Deltas are sent **only for active subscriptions**.

```json
{ "type": "delta", "sub_id": 7, "payload": { ... } }
```

---

## Snapshot vs Delta

- **Snapshot** is a **full state** payload returned in the `resp` of:
  - `req` (one-time)
  - `sub` (initial state)
- **Delta** is a **streaming update** tied to a `sub_id`. It does not contain unchanged fields.

---

## Notes

- `req_id` must be unique per sender while in-flight.
- `sub_id` is assigned by the responder and remains valid until unsubscribed.
