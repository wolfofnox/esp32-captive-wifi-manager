import { WsClient } from '../utilities/ws.js';

export default function WebSocket() {

    const ws = new WsClient('/ws');
    ws.connect();

    return (
        <div class="card">
            <h1>WebSocket control example</h1>

            {/* Binary sending example */}
            <div class="slider-group">
                <div class="slider-label-row">
                    <label for="sliderBin">Slider (Binary):</label>
                    <span class="value" id="sliderBinValue">0</span>
                </div>
                <input type="range" id="sliderBin" min="0" max="255" value="0"></input>
            </div>

            {/* JSON sending example */}
            <div class="slider-group">
                <div class="slider-label-row">
                    <label for="sliderJson">Slider (JSON):</label>
                    <span class="value" id="sliderJsonValue">0</span>
                </div>
                <input type="range" id="sliderJson" min="0" max="1023" value="0"></input>
            </div>

            {/* Text input example */}
            <div class="text-input-group">
                <label for="textinput">Text Input:</label>
                <input type="text" id="textinput" value="Hello, ESP32!"></input>
                <button id="sendTextBtn">Send</button>
            </div>
        </div>
    )
}