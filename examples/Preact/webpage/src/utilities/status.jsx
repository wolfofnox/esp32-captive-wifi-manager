import './status.css';
import { useState, useEffect } from 'preact/hooks';
import { message } from './message.jsx';

export default function Status() {
  const [status, setStatus] = useState(null);

  async function fetchStatuses() {
    return Promise.all([
      fetch('/status.json').then(r => {
        if (!r.ok) {message('error', 'Failed to fetch status', 2000); return {}; }
        return r.json();
      }).catch(() => ({})),
      fetch('/wifi-status.json').then(r => {
        if (!r.ok) {message('error', 'Failed to fetch WiFi status', 2000); return {};} 
        return r.json();
      }).catch(() => ({}))
    ]).then(([status, wifi]) => {
      return { status: status, wifi: wifi };
    });
    }

    useEffect(() => {
    let mounted = true;
    const update = async () => {
      const data = await fetchStatuses();
      if (mounted) {
        setStatus(data);
      }
    };
    update();
    const interval = setInterval(update, 5000);
    return () => {
      mounted = false;
      clearInterval(interval);
    };
  }, []);

  return (
    <div id="status">
      {status && status.status && status.wifi ? (
        <>
          <span style={status.wifi.connected ? { color: '#388e3c' } : { color: '#c62828' }}>WiFi: {status.wifi.connected ? 'Connected' : 'Disconnected'}</span>
          <span>IP: {status.wifi.ip || 'N/A'}</span>
          <span>Heap: {status.status.totalHeap - status.status.freeHeap || 'N/A'}/{status.status.totalHeap || 'N/A'} bytes</span>
          <span>Uptime: {msToTime(status.status.uptime) || 'N/A'}</span>
          <span>FW: {status.status.version || 'N/A'}</span>
          <span style={{ fontSize: '0.9em' }}>Last update: {new Date().toLocaleTimeString()}</span>
        </>
      ) : <>Loading status...</>}
    </div>
  );
}

function msToTime(ms) {
    let totalSeconds = Math.floor(ms / 1000);
    let hours = Math.floor(totalSeconds / 3600);
    let minutes = Math.floor((totalSeconds % 3600) / 60);
    let seconds = totalSeconds % 60;
    return `${hours}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
}