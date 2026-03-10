import { WsClient } from "../utilities/ws.js"
import { useState, useEffect, useRef } from 'preact/hooks';


export default function WebSocket() {
    const [sliderBinValue, setSliderBinValue] = useState(0);
    const [sliderJsonValue, setSliderJsonValue] = useState(0);
    const [textInputValue, setTextInputValue] = useState('');

    useEffect(() => {
        // Send binary slider value
    }, [sliderBinValue]);

    useEffect(() => {
        // Send JSON slider value
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
                <button id="sendTextBtn" onClick={ null }>Send</button>
            </div>
        </div>
    )
}