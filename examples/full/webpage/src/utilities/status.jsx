import './status.css';
import { useState, useEffect } from 'preact/hooks';
import { WsClient } from './ws.js';

export default function Status() {
  const [status, setStatus] = useState(null);

  useEffect(() => {
    console.log('Connecting to WebSocket for status updates...');
    const wsClient = new WsClient("/ws");
    let sub;
    wsClient.onOpen = () => {
      console.log('WebSocket connection opened');
      sub = wsClient.subscribe("status");
      sub.ack.then((res) => console.log('WebSocket ack response:', res)).catch((err) => console.error('WebSocket ack error:', err));
      sub.onDelta = (delta) => {
        console.log('Received status delta:', delta);
        setStatus(prevStatus => ({ ...(prevStatus || {}), ...delta }));
      };
      sub.onceSnapshot.then((snapshot) => {
        console.log('Received status snapshot:', snapshot);
        setStatus(snapshot);
      }).catch((err) => console.error('WebSocket snapshot error:', err));
    };

    wsClient.open();

    return () => {
      if (sub) sub.unsubscribe().catch((err) => console.error('WebSocket unsubscribe error:', err));
      wsClient.close();
      console.log('WebSocket connection closed');
    };
  }, []);

  return (
    <div id="status">
      {status /*&& status.wifi*/ ? (
        <>
          <span style={status.in_ap_mode ? { color: '#2d7cfb' } : status.connected ? { color: '#388e3c' } : { color: '#c62828' }}>WiFi: {status.in_ap_mode ? 'AP Mode' : status.connected ? 'Connected' : 'Disconnected'}</span>
          <span>IP: {status.ip || 'N/A'}</span>
          <span>SSID: {status.ssid || (status.in_ap_mode ? status.ap_ssid : 'N/A')}</span>
          <span>Heap: {status.totalHeap - status.freeHeap || 'N/A'}/{status.totalHeap || 'N/A'} kb</span>
          <span>Uptime: {sToTime(status.uptime) || 'N/A'}</span>
          <span>FW: {status.version || 'N/A'}</span>
          <span style={{ fontSize: '0.9em' }}>Last update: {new Date().toLocaleTimeString()}</span>
        </>
      ) : <>Loading status...</>}
    </div>
  );
}

function sToTime(s) {
    let totalSeconds = Math.floor(s);
    let hours = Math.floor(totalSeconds / 3600);
    let minutes = Math.floor((totalSeconds % 3600) / 60);
    let seconds = totalSeconds % 60;
    return `${hours}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
}