import { WsClient } from "../utilities/ws.js"
import { useState, useEffect, useRef } from 'preact/hooks';


export default function WebSocket() {
    const [sliderCmdValue, setSliderCmdValue] = useState(0);
    const [sliderSubValue, setSliderSubValue] = useState(0);
    const [textInputValue, setTextInputValue] = useState('');

    const wsRef = useRef(null);

    useEffect(() => {
        const wsClient = new WsClient("/ws");
        wsRef.current = wsClient;

        wsClient.onOpen = () => {
            const p = wsClient.request("reload");
            p.ack.then((res) => console.log('WebSocket ack response:', res)).catch((err) => console.error('WebSocket ack error:', err));
            p.then((msg) => console.log('WebSocket request completed with message:', msg)).catch((err) => console.error('WebSocket request failed with error:', err));
            // use the WsClient subscribe method (not firebase/data-connect)
            const sub = wsClient.subscribe("sliderBin");
            sub.ack.then((res) => console.log('WebSocket ack response:', res)).catch((err) => console.error('WebSocket ack error:', err));
            sub.onDelta = (delta) => {
                console.log('Received sliderBin delta:', delta);
                if (delta.value != null) setSliderSubValue(delta.value);
            };
            sub.onceSnapshot().then((snapshot) => {
                console.log('Received sliderBin snapshot:', snapshot);
                if (snapshot.value != null) setSliderSubValue(snapshot.value);
            }).catch((err) => console.error('WebSocket snapshot error:', err));
        };

        wsClient.open();

        return () => {
            wsClient.close();
            wsRef.current = null;
        }
    }, []);

    // initial request is sent from client.onOpen above

    useEffect(() => {
        if (!wsRef.current) return;
        try { wsRef.current.cmd("sliderCmd", { value: sliderCmdValue }).ack.catch((err) => console.error('WebSocket ack error:', err)); } catch (e) { console.error('WebSocket command error:', e); }
    }, [sliderCmdValue]);

    useEffect(() => {
        if (!wsRef.current) return;
        // Send slider value as a delta to a subscription

    }, [sliderSubValue]);

    return (
        <div class="card">
            <h1>WebSocket control example</h1>

            {/* Binary sending example */}
            <div class="slider-group">
                <div class="slider-label-row">
                    <label for="sliderCmd">Slider (Binary):</label>
                    <span class="value" id="sliderCmdValue">{sliderCmdValue}</span>
                </div>
                <input type="range" id="sliderCmd" min="0" max="255" value={sliderCmdValue} onInput={(e) => setSliderCmdValue(e.target.value)}></input>
            </div>

            {/* JSON sending example */}
            <div class="slider-group">
                <div class="slider-label-row">
                    <label for="sliderSub">Slider (JSON):</label>
                    <span class="value" id="sliderSubValue">{sliderSubValue}</span>
                </div>
                <input type="range" id="sliderSub" min="0" max="1023" value={sliderSubValue} onInput={(e) => setSliderSubValue(e.target.value)}></input>
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