import { WsClient } from "../utilities/ws.js"
import { useState, useEffect, useRef } from 'preact/hooks';


export default function WebSocket() {
    const [sliderBinValue, setSliderBinValue] = useState(0);
    const [sliderJsonValue, setSliderJsonValue] = useState(0);
    const [textInputValue, setTextInputValue] = useState('');

    const wsRef = useRef(null);

    useEffect(() => {
        console.log('[WS page] mount - creating client');
        const wsClient = new WsClient("/ws");
        wsRef.current = wsClient;

        wsClient.onOpen = () => {
            console.log('[WS client] onOpen - sending reload request');
            const p = wsClient.request("reload");
            p.ack.then((res) => console.log('WebSocket ack response:', res)).catch((err) => console.error('WebSocket ack error:', err));
            p.then((msg) => console.log('WebSocket request completed with message:', msg)).catch((err) => console.error('WebSocket request failed with error:', err));
        };

        wsClient.open();

        return () => {
            console.log('[WS page] unmount - closing client');
            wsClient.close();
            wsRef.current = null;
        }
    }, []);

    // initial request is sent from client.onOpen above

    useEffect(() => {
        if (!wsRef.current) return;
        // Send binary slider value
        try { wsRef.current.cmd("sliderBin", { value: sliderBinValue }).ack.catch((err) => console.error('WebSocket ack error:', err)); } catch (e) { console.error('WebSocket command error:', e); }
    }, [sliderBinValue]);

    useEffect(() => {
        if (!wsRef.current) return;
        // Send JSON slider value
        try { wsRef.current.cmd("sliderJson", { value: sliderJsonValue }).ack.catch((err) => console.error('WebSocket ack error:', err)); } catch (e) { console.error('WebSocket command error:', e); }
    }, [sliderJsonValue]);

    return (
        <div class="card">
            <h1>WebSocket control example</h1>

            {/* Binary sending example */}
            <div class="slider-group">
                <div class="slider-label-row">
                    <label for="sliderBin">Slider (Binary):</label>
                    <span class="value" id="sliderBinValue">{sliderBinValue}</span>
                </div>
                <input type="range" id="sliderBin" min="0" max="255" value={sliderBinValue} onInput={(e) => setSliderBinValue(e.target.value)}></input>
            </div>

            {/* JSON sending example */}
            <div class="slider-group">
                <div class="slider-label-row">
                    <label for="sliderJson">Slider (JSON):</label>
                    <span class="value" id="sliderJsonValue">{sliderJsonValue}</span>
                </div>
                <input type="range" id="sliderJson" min="0" max="1023" value={sliderJsonValue} onInput={(e) => setSliderJsonValue(e.target.value)}></input>
            </div>

            {/* Text input example */}
            <div class="text-input-group">
                <label for="textinput">Text Input:</label>
                <input type="text" id="textinput" value={textInputValue} onInput={(e) => setTextInputValue(e.target.value)}></input>
                <button id="sendTextBtn" onClick={async () => { if (wsRef.current) await wsRef.current.cmd("text", { value: textInputValue }).ack; }}>Send</button>
            </div>
        </div>
    )
}