export default function Control() {
  return (
    <div class="card">
      <h1>HTML Post Control</h1>
      <form id="controlForm" method="POST" action="/control">
        <div class="text-input-group">
          <label for="number">Number (0-255):</label>
          <input type="number" id="number" name="number" min="0" max="255" value="128" required></input>
        </div>
        <div class="slider-group">
          <div class="slider-label-row">
            <label for="slider">Slider (0-100):</label>
            <span class="value" id="sliderValue">50</span>
          </div>
          <input type="range" id="slider" name="slider" min="0" max="100" value="50" oninput={(e) => { document.getElementById('sliderValue').innerText = e.target.value; }}></input>
        </div>
        <div class="text-input-group">
          <label for="text">Text:</label>
          <input type="text" id="text" name="text" value="ESP32 Device" required></input>
        </div>
        <button type="submit">Submit</button>
      </form>
    </div>
  );
}
