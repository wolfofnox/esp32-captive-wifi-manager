import { message } from '../utilities/message.jsx';

// constants for WebSocket binary events and values
const WS_event = {
    EVENT_NONE: 0,
    EVENT_TIMEOUT: 1,
    EVENT_RELOAD: 2,
    EVENT_REVERT_SETTINGS: 3
}

const WS_value = {
    VALUE_NONE: 0,
    SLIDER_BINARY: 1,
    SLIDER_JSON: 2
}

let ws = {}
function setupWebSocket() {
    if (ws && ws.readyState === WebSocket.OPEN) {
        console.warn('WebSocket already connected');
        return;
    }
    message('info', 'Setting up WebSocket connection...', 5000);
    ws = new WebSocket(`ws://${location.host}/ws`, 'binary.v1');
    ws.onopen = () => {
        console.log('WebSocket connected');
        message('info', 'WebSocket connected', 3000);
        if (window.onWSOpen) {
            window.onWSOpen();
        }
    };
    ws.onmessage = async (event) => {
        try {
            // Handle binary messages (Blob or ArrayBuffer)
            if (event.data instanceof Blob || event.data instanceof ArrayBuffer) {
                const buffer = event.data instanceof Blob ? await event.data.arrayBuffer() : event.data;
                const view = new DataView(buffer);

                // Detect message type based on size
                if (view.byteLength === 1) {
                    // Event message (1 byte header only)
                    const eventType = view.getUint8(0);
                    console.log('Event received:', eventType);
                    if (window.handleWSEvent) {
                        window.handleWSEvent(eventType);
                    }
                } else if (view.byteLength >= 3) {
                    // Value message (1 byte header + 2 bytes data)
                    const type = view.getUint8(0);
                    const value = view.getInt16(1, true); // little-endian
                    console.log('Binary message received:', { type, value });
                    if (window.handleWSBinaryData) {
                        window.handleWSBinaryData(type, value);
                    }
                } else {
                    console.warn('Invalid binary message size:', view.byteLength);
                }
            } else {
                // Text message
                console.log('Text message received:', event.data);
                if (window.handleWSText) {
                    window.handleWSText(event.data);
                }
            }
        } catch (e) {
            console.error('Error processing incoming message:', e);
        }
    };
    ws.onerror = (error) => {
        console.error('WebSocket error:', error); 
        message('error', 'WebSocket error: ' + error, 5000); 
    };
    ws.onclose = () => {
        console.log('WebSocket closed'); 
        message('warn', 'WebSocket closed', 5000);
        setTimeout(setupWebSocket, 10000);
    };
}

export function sendWSBinary(type, value) {
    const buffer = new ArrayBuffer(3);
    if (ws.readyState === WebSocket.OPEN) {
        const view = new DataView(buffer);
        view.setUint8(0, type);
        view.setInt16(1, value, true); // true for little-endian
        ws.send(buffer);
        console.log('Sent binary message: ', { type, value }, 'buffer: ', buffer);

    } else {
        console.warn('WebSocket not open, message not sent: ', buffer);
    }
}

export function sendWSEvent(eventType) {
    const buffer = new ArrayBuffer(1);
    if (ws.readyState === WebSocket.OPEN) {
        const view = new DataView(buffer);
        view.setUint8(0, eventType);
        ws.send(buffer);
        console.log('Sent event message: ', eventType, 'buffer: ', buffer);
    } else {
        console.warn('WebSocket not open, message not sent: ', buffer);
    }
}

export function sendWSMessage(msg) {
    console.log('Sending message ', msg);
    if (ws.readyState === WebSocket.OPEN) {
        ws.send(msg);
    } else {
        console.warn('WebSocket not open, message not sent:', msg);
    }
}
setupWebSocket();