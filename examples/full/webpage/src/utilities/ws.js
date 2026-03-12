// WebSocket client with request/response, events, reconnect hooks.
// Uses ES module export so you can import { WsClient } from './ws.js'
//
// For protocol details, see protocol.md. This client abstracts the protocol details
// and provides a higher-level API for sending requests/commands and handling
// responses/events.


import { message } from "./message.jsx";

// Event/message/cmd+req handler - setter, usage, typedef, call in _onMessage
//    onEvent(msg)
//    onCmd onRequest onSubscribe onDelta - (payload) (without metadata)
// messages: cmd, request {what fields}, subscribe {id, what fields}, delta

// Time sync
// common helpers - request snapshot, call cmd...
// review protocol

const PROTOCOL_VER = 1;

// small Subscription handle
class OutgoingSubscription {
  constructor(name, params, client, reqId) {
    this._client = client;
    this._name = name;
    this._params = params;
    this._reqId = reqId;
    this.id = null;
    this.snapshot = null;
    this._canceled = false;
    let res, rej;
    this.onceSnapshot = new Promise((r, j) => { res = r; rej = j; });
    this._resolveOnceSnapshot = res;
    this._rejectOnceSnapshot = rej;
    this._waitingForWsOpen = false;
    this.onDelta = (/*payload*/) => {}
  }

  unsubscribe() {
    if (this._canceled) return Promise.resolve();
    // pending subscribe (no assigned id yet)
    if (this.id == null && this._reqId != null && this._client._pendingSubs.has(this._reqId)) {
      this._client._pendingSubs.delete(this._reqId);
      this._canceled = true;
      this._client._subDescriptors.delete(this);
      // cancel underlying promise if present
      const p = this._client.pending.get(this._reqId);
      if (p) {
        clearTimeout(p.timer);
        try { if (p.reject) p.reject(new Error('cancelled')); } catch (e) {}
        try { if (p.ackReject) p.ackReject(new Error('cancelled')); } catch (e) {}
        this._client.pending.delete(this._reqId);
        this._client.inFlight--;
        this._client._drain();
      }
      try { this._resolveOnceSnapshot(null); } catch (e) {}
      return Promise.resolve();
    }
    // active subscription
    if (this.id != null) {
      const id = this.id;
      this._client.subscriptions.delete(id);
      this._client._subDescriptors.delete(this);
      const req_id = this._client.lastId++;
      const msg = { type: 'unsub', req_id, params: { sub_id: id } };
      return this._client._sendWithResponse(req_id, msg);
    }
    // otherwise, not pending or active - return error
    return Promise.reject(new Error('Subscription not found'));
  }
}

class IncomingSubscription {
  constructor(name, params, client, reqId) {
    this._client = client;
    this._name = name;
    this._params = params;
    this._reqId = reqId;
    this.id = client.lastSubId++ ?? null;
    this._dListeners = new Set();
    this._snapshotSent = false;
    this.active = true;
  }

  sendDelta(payload) {
    if (this.id == null) return Promise.reject(new Error('No subscription id assigned yet'));
    const msg = { type: 'delta', sub_id: this.id, payload };
    try { this._client._sendRaw(msg); } catch (e) { return Promise.reject(e); }
    return Promise.resolve();
  }

  sendError(err) {
    if (this.id == null) return Promise.reject(new Error('No subscription id assigned yet'));
    return this._client.error(this._reqId, err);
  }

  sendSnapshot(snapshot) {
    if (this.id == null) return Promise.reject(new Error('No subscription id assigned yet'));
    this._snapshotSent = true;
    return this._client.respond(this._reqId, { sub_id: this.id, snapshot });
  }

  _sendAck() {
    if (this.id == null) return Promise.reject(new Error('No subscription id assigned yet'));
    if (this._snapshotSent) return Promise.resolve();
    return this._client.ack(this._reqId);
  }
}

/**
 * @typedef {(method: string, params: any) => void} EventHandler
 */

export class WsClient {
  /**
   * @param {string} uri websocket uri (e.g., "/ws")
   * @param {{ackTimeoutMs?:number, respTimeoutMs?:number, maxInFlight?:number, autoClose?:boolean}} opts
   */
  constructor(uri, opts = {}) {
    const isSecure = window.location.protocol === 'https:';
    this.url = (isSecure ? "wss://" : "ws://") + window.location.host + uri;
    this.ackTimeoutMs = opts.ackTimeoutMs ?? 5000;
    this.respTimeoutMs = opts.respTimeoutMs ?? 10000;
    this.maxInFlight = opts.maxInFlight ?? 8;
    // whether to auto-close socket on unload/navigation (default: true)
    this._autoClose = opts.autoClose ?? true;

    this.protocols = [`rap.v${PROTOCOL_VER}+json`, 'rap+json'];

    this.ws = null;
    this.lastId = 0;
    this.pending = new Map(); // id -> {resolve, reject, ackResolve, ackReject, timer, acknowledged}
    this.inFlight = 0;
    this.queue = []; // queued sends when at capacity
    this.onEvent = (/*method, params*/) => {}; // override
    this.onOpen = () => {};
    this.onClose = (/*event*/) => {};
    // convenience callbacks — override as needed
    this.onCmd = (/*cmd, params, req_id, meta*/) => {};
    this.onRequest = (/*name, params, req_id, meta*/) => {};
    this.onSubscribe = (/*name, params, sub*/) => {};

    this.reconnectDelay = 1000;
    this._closedExplicitly = false;

    // outgoing subscription bookkeeping
    this._pendingOutSubs = new Map(); // req_id -> Subscription (waiting for sub_id)
    this.outSubscriptions = new Map(); // sub_id -> Subscription (active)
    this._outSubDescriptors = new Map(); // Subscription -> {name, params}
    this._autoResubscribe = opts.autoResubscribe ?? true;

    // incoming subscription bookkeeping
    this.inSubscriptions = new Map(); // sub_id -> IncomingSubscription
    this.lastSubId = 0;

    // Auto-register unload handlers in browser contexts if requested
    if (this._autoClose && typeof window !== 'undefined' && window.addEventListener) {
      this._boundBeforeUnload = () => this.close();
      this._boundPagehide = (e) => { if (!e.persisted) this.close(); };
      try {
        window.addEventListener('beforeunload', this._boundBeforeUnload, { passive: true });
        window.addEventListener('pagehide', this._boundPagehide, { passive: true });
      } catch (e) {
        // some environments may reject options; fall back
        try { window.addEventListener('beforeunload', this._boundBeforeUnload); } catch (e) {}
        try { window.addEventListener('pagehide', this._boundPagehide); } catch (e) {}
      }
    }
  }

  open() {
    this._closedExplicitly = false;
    this._openInternal();
  }

  _openInternal() {
    this.ws = new WebSocket(this.url, this.protocols);
    this.ws.onopen = () => {
      if (this.ws.protocol) {
        console.log(`[WS] Connected. Negotiated subprotocol: ${this.ws.protocol}`);
        message('success', 'WebSocket connected', 2000);
      } else {
        console.warn(`[WS] Connected without subprotocol. Server did not select one.`);
      }
      // re-subscribe active descriptors after successful open
      if (this._autoResubscribe) this._resubscribeAll();
      this.onOpen();
      // drain any queued sends that were waiting for the socket to open
      this._drain();
    };
    this.ws.onmessage = (ev) => this._onMessage(ev.data);
    this.ws.onerror = () => {
      // swallow; close will attempt reconnect
    };
    this.ws.onclose = (ev) => {
      // Handshake rejection often shows as code 1006 with empty reason
      const code = ev.code ?? "unknown";
      const reason = ev.reason || "(no reason provided by browser)";
      if (!this._closedExplicitly) {
        console.warn(`[WS] Connection closed/rejected. code=${code}, reason=${reason}`);
        message('error', `WebSocket connection closed (code ${code})`, 3000);
        // Attempt reconnect
        setTimeout(() => this._openInternal(), this.reconnectDelay);
      }
      this.onClose(ev);
      this._clearPendingWithError(new Error("connection closed"));
    };
  }

  close() {
    this._closedExplicitly = true;
    if (this.ws) this.ws.close();
    this._clearPendingWithError(new Error("connection closed"));
    // remove any auto-registered handlers
    if (typeof window !== 'undefined' && window.removeEventListener) {
      if (this._boundBeforeUnload) {
        try { window.removeEventListener('beforeunload', this._boundBeforeUnload); } catch (e) {}
        this._boundBeforeUnload = null;
      }
      if (this._boundPagehide) {
        try { window.removeEventListener('pagehide', this._boundPagehide); } catch (e) {}
        this._boundPagehide = null;
      }
    }
  }

  // alias for explicit lifecycle management
  destroy() { this.close(); }

  _clearPendingWithError(err) {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      // reject both final result and ack (if present)
      if (p.reject) p.reject(err);
      if (p.ackReject) p.ackReject(err);
    }
    this.pending.clear();
    this.inFlight = 0;
    for (const item of this.queue) {
      if (!item) continue;
      try { item(true); } catch (e) {}
    }
    this.queue = [];
  }

  _onMessage(raw) {
    let msg;
    try { msg = JSON.parse(raw); } catch (e) { console.warn("invalid json", e); return; }
  
    if (msg.type === "req") {
      // server -> client request (RPC). Call application handler.
      try { this.onRequest(msg.name ?? null, msg.params ?? null, msg.req_id ?? null, msg); } catch (e) { console.warn('onRequest handler error', e); }
      return;
    } else if (msg.type === "cmd") {
      // server -> client command (action expected). Call application handler.
      try { this.onCmd(msg.name ?? null, msg.params ?? null, msg.req_id ?? null, msg); } catch (e) { console.warn('onCmd handler error', e); }
      return;
    } else if (msg.type === "ack" && msg.req_id !== undefined) {
      const p = this.pending.get(msg.req_id);
      if (p) {
        if (p.ackResolve && !p.acknowledged) {
          p.ackResolve(msg);
          p.acknowledged = true;
          this._resetTimer(msg.req_id, this.respTimeoutMs);
          this.inFlight--; // ack means request is no longer in-flight
        }
        this._drain();
      }
      return;
    } else if ((msg.type === "resp" || msg.type === "err") && msg.req_id !== undefined) {
      const p = this.pending.get(msg.req_id);
      const payload = msg.payload ?? msg;
      // If this response contains a subscription id, and we have a pending
      // subscribe, promote it to an active subscription.
      if (payload && payload.sub_id != null) {
        const subId = payload.sub_id;
        const sub = this._pendingOutSubs.get(msg.req_id);
        if (sub) {
          this._pendingOutSubs.delete(msg.req_id);
          if (msg.type === "err") {
            try { sub._rejectOnceSnapshot(new Error(msg.err || 'subscribe error')); } catch (e) {}
          } else if (!sub._canceled) {
            sub.id = subId;
            sub.snapshot = payload.snapshot ?? null;
            try { sub._resolveOnceSnapshot(sub.snapshot); } catch (e) {}
            this.outSubscriptions.set(subId, sub);
            this._outSubDescriptors.set(sub, { name: sub._name, params: sub._params });
          } else {
            // canceled before assignment: resolve onceSnapshot with null
            try { sub._resolveOnceSnapshot(null); } catch (e) {}
          }
        }
      }

      if (p) {
        if (!p.acknowledged) {
          if (p.ackResolve) p.ackResolve(msg);
          this.inFlight--; // ack means request is no longer in-flight
        }
        clearTimeout(p.timer);
        this.pending.delete(msg.req_id);
        if (msg.type === "resp") {
          if (p.resolve) p.resolve(payload);
        } else if (msg.type === "err") {
          if (p.reject) p.reject(new Error(msg.err || "server error"));
          // if this was a failed subscribe, and we had a pending sub, reject it
          const sub = this._pendingOutSubs.get(msg.req_id);
          if (sub) {
            this._pendingOutSubs.delete(msg.req_id);
            try { sub._rejectOnceSnapshot(new Error(msg.err || 'subscribe error')); } catch (e) {}
            this._outSubDescriptors.delete(sub);
          }
        }
        this._drain();
      }
      return;
    } else if (msg.type === "delta") {
      const payload = msg.payload ?? msg;
      const subId = msg.sub_id ?? null;
      // route to matching subscription if present
      const sub = this.outSubscriptions.get(subId);
      if (sub) {
        try { sub.onDelta(payload); } catch (e) { console.warn('subscription emit error', e); }
      } else {
        console.warn('[WS] delta for unknown sub_id', subId);
      }
      return;
    } else if (msg.type === "sub" && msg.name) {
      const sub = new IncomingSubscription(msg.name ?? null, msg.params ?? null, this, msg.req_id ?? null);
      this.inSubscriptions.set(sub.id, sub);
      try { this.onSubscribe(msg.name ?? null, msg.params ?? null, sub); sub._sendAck() }
      catch (e) {
        console.warn('onSubscribe handler error', e);
        sub.sendError('Subscription handler error: ' + e.toString()).catch((err) => console.warn('Failed to send subscription error response', err));
      }
      return;
    } else if (msg.type === "unsub" && msg.params?.sub_id != null) {
      const sub = this.inSubscriptions.get(msg.params.sub_id);
      if (sub) {
        sub.active = false;
        this.inSubscriptions.delete(msg.params.sub_id);
        this.respond(msg.req_id, {}).catch((err) => console.warn('Failed to send unsub response', err));
      } else {
        console.warn('[WS] unsub for unknown sub_id', msg.params.sub_id);
        this.error(msg.req_id, 'Subscription not found').catch((err) => console.warn('Failed to send unsub error response', err));
      }
      return;
    }

    // fallback
    this.onEvent(msg.method ?? msg.type, msg.payload ?? msg);
  }

  // Subscription helpers
  subscribe(name, params = {}) {
    const req_id = this.lastId++;
    const sub = new OutgoingSubscription(name, params, this, req_id);
    this._pendingOutSubs.set(req_id, sub);
    this._outSubDescriptors.set(sub, { name, params });
    if (this.ws?.readyState !== WebSocket.OPEN) { this._waitingForWsOpen = true; }
    // send but return subscription handle immediately
    const msg = { type: 'sub', req_id, name, params };
    const sendPromise = this._sendWithResponse(req_id, msg);
    // expose the underlying promise and its ack on the subscription for callers
    try { sub._sendPromise = sendPromise; sub.ack = sendPromise.ack; } catch (e) {}
    sendPromise.catch((e) => {
      // errors will be handled in _onMessage, but ensure pending subs cleaned
      const s = this._pendingOutSubs.get(req_id);
      if (s) {
        this._pendingOutSubs.delete(req_id);
        try { s._rejectOnceSnapshot(e); } catch (er) {}
        this._outSubDescriptors.delete(s);
      }
    });
    return sub;
  }


  _resubscribeAll() {
    // Re-issue subscriptions for all descriptors
    for (const [sub, desc] of Array.from(this._outSubDescriptors.entries())) {
      if (sub._reqId != null) sub._client._pendingOutSubs.delete(sub._reqId);
      if (sub.id != null) this.outSubscriptions.delete(sub.id);
      if (sub._canceled) continue;
      if (sub._waitingForWsOpen) { sub._waitingForWsOpen = false; continue; } // Already in queue
      // clear existing id/snapshot
      sub.id = null;
      sub.snapshot = null; 
      // send new subscribe
      const req_id = this.lastId++;
      sub._reqId = req_id;
      this._pendingOutSubs.set(req_id, sub);
      sub.onceSnapshot = new Promise((res, rej) => { sub._resolveOnceSnapshot = res; sub._rejectOnceSnapshot = rej; });
      try {
        const msg = { type: 'sub', req_id, name: desc.name, params: desc.params };
        this._sendWithResponse(req_id, msg).catch((e) => {
          if (this._pendingOutSubs.has(req_id)) {
            this._pendingOutSubs.delete(req_id);
            try { sub._rejectOnceSnapshot(e); } catch (er) {}
          }
        });
      } catch (e) { /* swallow */ }
    }
  }

  // high-level RPC call (cmd/request)
  cmd(command, params = {}) {
    const req_id = this.lastId++;
    const msg = { type: "cmd", req_id, name: command, params: params};
    return this._sendWithResponse(req_id, msg);
  }

  // request-type (e.g., get_snapshot)
  request(name, params = {}) {
    const req_id = this.lastId++;
    const msg = { type: "req", req_id, name, params };
    return this._sendWithResponse(req_id, msg);
  }

  error(req_id, err) {
    const msg = { type: "err", req_id, err: err.toString() };
    try { this._sendRaw(msg); } catch (e) { return Promise.reject(e); }
    return Promise.resolve();
  }

  respond(req_id, payload) {
    const msg = { type: "resp", req_id, payload };
    try { this._sendRaw(msg); } catch (e) { return Promise.reject(e); }
    return Promise.resolve();
  }

  ack(req_id) {
    const msg = { type: "ack", req_id };
    try { this._sendRaw(msg); } catch (e) { return Promise.reject(e); }
    return Promise.resolve();
  }

  _sendWithResponse(req_id, msg) {
    // Provide backward-compatible main Promise that resolves with the
    // final result, and attach an `.ack` Promise for immediate
    // acknowledgements. Callers can either `await p` for the final
    // response or `await p.ack` to know the server accepted the
    // request.
    let ackResolve, ackReject;
    const ackPromise = new Promise((res, rej) => { ackResolve = res; ackReject = rej; });

    const mainPromise = new Promise((resolve, reject) => {
      const doSend = (shouldReject = false) => {
        if (shouldReject) {
          reject(new Error("request rejected from queue"));
          ackReject(new Error("request rejected from queue"));
          return;
        }
        try {
          this.ws.send(JSON.stringify(msg));
        } catch (e) {
          // reject both promises on immediate send failure
          reject(e);
          ackReject(e);
          return;
        }
        this.inFlight++;
        const timer = setTimeout(() => this._onRequestTimeout(req_id), this.ackTimeoutMs);
        this.pending.set(req_id, { resolve, reject, ackResolve, ackReject, timer, acknowledged: false });
      };
      this.queue.push(doSend);
      this._drain();
    });
    mainPromise.ack = ackPromise;
    return mainPromise;
  }

  _drain() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    while (this.inFlight < this.maxInFlight && this.queue.length) {
      const fn = this.queue.shift();
      fn();
    }
  }

  _sendRaw(obj) {
    try {
      this.ws?.send(JSON.stringify(obj));
    } catch (e) {
      console.warn("send failed", e);
    }
  }

  _onRequestTimeout(req_id) {
    const p = this.pending.get(req_id);
    if (p && !p.acknowledged) {
      p.acknowledged = true; // prevent ack resolution after timeout
      if (p.ackReject) p.ackReject(new Error("timeout"));
      this.inFlight--;
    }
    if (p) {
      this.pending.delete(req_id);
      if (p.reject) p.reject(new Error("timeout"));
      this._drain();
    }
  }

  _resetTimer(req_id, ms) {
    const p = this.pending.get(req_id);
    if (p) {
      clearTimeout(p.timer);
      p.timer = setTimeout(() => this._onRequestTimeout(req_id), ms);
    }
  }
}