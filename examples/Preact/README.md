# NEEDS TO BE FILLED IN, this is just a template for now, mostly coppied from examples/full/README.md

# ESP32 Captive WiFi Manager - Preact web app example

This example demonstrates how to use a preact WPA to have a better user experience, but aswell how to save ESP headspace by caching, ws API etc.

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [Software Prerequisites](#software-prerequisites)
- [Getting Started](#getting-started)
  - [Hardware Setup](#hardware-setup)
  - [Preparing the SD Card](#preparing-the-sd-card)
  - [Building and Flashing](#building-and-flashing)
- [Project Architecture](#project-architecture)
  - [Directory Structure](#directory-structure)
  - [Code Architecture](#code-architecture)
- [Web Interface](#web-interface)
  - [Pages Overview](#pages-overview)
  - [Navigation](#navigation)
- [API Endpoints](#api-endpoints)
  - [HTTP Endpoints](#http-endpoints)
  - [WebSocket Endpoint](#websocket-endpoint)
- [WebSocket Protocol](#websocket-protocol)
  - [Binary Protocol](#binary-protocol)
  - [Text/JSON Protocol](#textjson-protocol)
  - [Event Protocol](#event-protocol)
- [Configuration](#configuration)
- [Usage Guide](#usage-guide)
  - [First Boot](#first-boot)
  - [Accessing the Web Interface](#accessing-the-web-interface)
  - [Using the Control Page](#using-the-control-page)
  - [Using the WebSocket Page](#using-the-websocket-page)
- [Customization](#customization)
  - [Adding Custom Endpoints](#adding-custom-endpoints)
  - [Modifying the Web Interface](#modifying-the-web-interface)
  - [Extending WebSocket Protocol](#extending-websocket-protocol)
- [Troubleshooting](#troubleshooting)
- [Advanced Topics](#advanced-topics)

---

## Overview

## Features

### Core Functionality

### Web Interface Features

### Developer Features

## Hardware Requirements

### Mandatory Components

- **ESP32 Development Board**: Any ESP32 variant with:
  - At least 4MB flash (recommended: 8MB or more)
  - WiFi capability
  - GPIO pins available for peripherals
  
  Tested on:
  - ESP32-S3-DevKitC-1

- **microSD Card Module**: SPI-compatible SD card reader
  - Supported cards: microSD, microSDHC (up to 32GB recommended)
  - Format: FAT32

- **microSD Card**: 
  - Minimum 512MB (1GB+ recommended)
  - Class 4 or higher
  - Pre-formatted as FAT32

### Optional Components

- **SK6812 RGB LED**: For visual status indication, may be included on some dev boards
  - Default GPIO: 45 (configurable via menuconfig)
  - 5V compatible or with level shifter for 3.3V operation

- **Power Supply**: Adequate current capacity
  - Minimum 500mA (basic operation)
  - Recommended 1A+ (with SD card and LED)

### Default Pin Configuration

This example assumes the following pin configuration (can be modified in menuconfig):

| Peripheral | Function | Default GPIO |
|------------|----------|--------------|
| SD Card    | MOSI     | GPIO 11      |
| SD Card    | MISO     | GPIO 13      |
| SD Card    | SCK      | GPIO 12      |
| SD Card    | CS       | GPIO 10      |
| LED        | Data     | GPIO 45      |
| Power Bus  | 3V3 EN   | GPIO 47*     |

**Note**: GPIO 47 is used in this example to enable a 3.3V power bus for peripherals on some development boards. This is hardware-specific and may not be needed for your board. See `main.c` lines 247-256 to remove or modify.

## Software Prerequisites

### Required Software

1. **ESP-IDF v5.5.0 or later**
   ```bash
   # Install ESP-IDF if not already installed
   git clone -b v5.5.0 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   ./install.sh
   . ./export.sh
   ```

2. **Python 3.6+** (included with ESP-IDF installation)

3. **Git** (for cloning repositories)

### Component Dependencies

The following components are automatically managed by ESP-IDF Component Manager:

- `esp32-captive-wifi-manager` (this component)
- `esp_wifi` - WiFi driver
- `esp_http_server` - HTTP server with WebSocket support
- `nvs_flash` - Non-volatile storage
- `mdns` - mDNS service discovery
- `led_indicator` - LED control
- `fatfs` - FAT filesystem for SD card
- `dns_server` - DNS hijacking for captive portal

### Development Tools

Recommended but optional:
- **Visual Studio Code** with ESP-IDF extension
- **Serial Terminal**: PuTTY, minicom, or ESP-IDF Monitor

## Getting Started

### Hardware Setup

1. **Connect the SD Card Module**:
   - Wire the SD card module to your ESP32 using SPI:
     ```
     SD Card Module    ESP32
     --------------    -----
     MOSI             GPIO 11 (configurable)
     MISO             GPIO 13 (configurable)
     SCK              GPIO 12 (configurable)
     CS               GPIO 10 (configurable)
     VCC              3.3V
     GND              GND
     ```

2. **Connect the Status LED** (optional):
   - Connect SK6812 LED:
     ```
     LED Pin     ESP32
     --------    -----
     DIN         GPIO 45 (configurable)
     VCC         5V (or 3.3V with level shifter)
     GND         GND
     ```

3. **Power Supply**:
   - Connect ESP32 to USB for programming and power
   - Ensure stable power supply (USB 2.0 minimum, USB 3.0 recommended)

### Preparing the SD Card

1. **Format the SD Card**:
   - Format as FAT32 (not exFAT)
   - Use default allocation unit size

2. **Copy Web Files**:
   ```bash
   # Navigate to the example directory
   cd examples/full
   
   # Copy all files from webpage/ to SD card root
   cp webpage/* /path/to/sdcard/
   ```
   
   The SD card should contain:
   ```
   TBD
   ```

3. **Safely Eject**:
   - Safely eject the SD card from your computer
   - Insert it into the SD card module

### Building and Flashing

1. **Navigate to Example Directory**:
   ```bash
   cd /path/to/esp32-captive-wifi-manager/examples/preact
   ```

2. **Configure the Project** (optional):
   ```bash
   idf.py menuconfig
   ```
   
   Configure as needed:
   - SD card pin assignments (under "WiFi Component Configuration")
   - LED pin assignment
   - Log levels
   - WiFi settings

3. **Build the Project**:
   ```bash
   idf.py build
   ```

4. **Flash to ESP32**:
   ```bash
   # Replace COMx with your ESP32's serial port
   idf.py -p COMx flash
   ```

5. **Monitor Serial Output**:
   ```bash
   idf.py -p COMx monitor
   ```
   
   Press `Ctrl+]` to exit monitor.

## Project Architecture

### Directory Structure

```

```

### Code Architecture

#### Main Application (`main.c`)

#### Web Interface Architecture

## Web Interface

### Pages Overview

## WebSocket Protocol

The WebSocket implementation supports three distinct protocols optimized for different use cases.

### Binary Protocol

**Purpose**: Efficient transmission of control values (e.g., slider positions, sensor readings).

**Message Structure** (3 bytes):
```
Byte 0: Type ID (uint8)
Byte 1: Value low byte (int16, little-endian)
Byte 2: Value high byte (int16, little-endian)
```

**Type IDs**:
- `0x00`: VALUE_NONE (reserved)
- `0x01`: SLIDER_BINARY (binary slider value, 0-255)
- `0x02`: SLIDER_JSON (JSON slider value, 0-1023)

**Client → Server Example** (JavaScript):
```javascript
// Send binary slider value
function sendSliderBinary(value) {
  const buffer = new ArrayBuffer(3);
  const view = new DataView(buffer);
  view.setUint8(0, 0x01);        // Type: SLIDER_BINARY
  view.setInt16(1, value, true); // Value (little-endian)
  ws.send(buffer);
}
```

**Server → Client Example** (C):
```c
// Send value to client
uint8_t payload[3];
payload[0] = SLIDER_BINARY;              // Type
payload[1] = (uint8_t)(value & 0xFF);    // Low byte
payload[2] = (uint8_t)((value >> 8) & 0xFF); // High byte

httpd_ws_frame_t ws_frame = {
    .type = HTTPD_WS_TYPE_BINARY,
    .payload = payload,
    .len = sizeof(payload)
};
httpd_ws_send_frame_async(handle, sockfd, &ws_frame);
```

**Advantages**:
- Minimal bandwidth (3 bytes per message)
- Fast parsing (no JSON overhead)
- Ideal for high-frequency updates (e.g., joystick, sensors)

### Text/JSON Protocol

**Purpose**: Human-readable, structured data exchange.

**Client → Server Example** (JavaScript):
```javascript
// Send JSON slider value
function sendSliderJSON(value) {
  const message = JSON.stringify({ slider: value });
  ws.send(message);
}

// Send plain text
ws.send("Hello, ESP32!");
```

**Server-Side Parsing** (C):
```c
// Basic JSON parsing
char *sliderPtr = strstr(payload, "\"slider\":");
if (sliderPtr != NULL) {
    char *start = strchr(sliderPtr, ':');
    int value = atoi(start + 1);
    // Process value...
}
```

**Advantages**:
- Human-readable for debugging
- Flexible structure (easy to add fields)
- Compatible with standard JSON libraries
- Self-documenting

**Disadvantages**:
- Higher bandwidth
- Slower parsing
- Variable message size

### Event Protocol

**Purpose**: Simple control signals and acknowledgments.

**Message Structure** (1 byte):
```
Byte 0: Event ID (uint8)
```

**Event IDs**:
- `0x00`: EVENT_NONE (reserved)
- `0x01`: EVENT_TIMEOUT
- `0x02`: EVENT_RELOAD
- `0x03`: EVENT_REVERT_SETTINGS (reset to defaults)

**Client → Server Example** (JavaScript):
```javascript
// Request current state
function requestReload() {
  const buffer = new ArrayBuffer(1);
  const view = new DataView(buffer);
  view.setUint8(0, 0x02); // EVENT_RELOAD
  ws.send(buffer);
}
```

**Server Response**: On EVENT_RELOAD, server sends current state:
```c
case EVENT_RELOAD:
    // Send current state to client
    send_ws_value_to_req(req, SLIDER_BINARY, sliderBinaryValue);
    send_ws_value_to_req(req, SLIDER_JSON, sliderJSONValue);
    break;
```

**Advantages**:
- Minimal overhead (1 byte)
- Fast processing
- Clear semantics

### Protocol Selection Guidelines

| Use Case | Recommended Protocol | Reason |
|----------|---------------------|--------|
| Slider/potentiometer | Binary | High frequency, small values |
| Sensor readings | Binary | Efficient, predictable size |
| Configuration | JSON | Complex structures, readability |
| Debug messages | Text | Human-readable logging |
| Control signals | Event | Minimal overhead |

## Configuration

### SDK Configuration (`sdkconfig.defaults`)

The example includes optimized defaults:

```ini
# Enable WebSocket support
CONFIG_HTTPD_WS_SUPPORT=y

# Large partition for application
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y

# Increased stack sizes for WebSocket and HTTP
CONFIG_ESP_MAIN_TASK_STACK_SIZE=7168
CONFIG_HTTPD_STACK_SIZE=8192

# Enable long filenames for SD card
CONFIG_FATFS_LFN_HEAP=y

# Log level: INFO (DEBUG available)
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
```

### WiFi Component Configuration

Access via `idf.py menuconfig` → **WiFi Component Configuration**:

- **SD Card Pins**: Adjust if using different GPIO pins
- **Status LED Pin**: Change LED GPIO pin
- **Max Reconnect Attempts**: Number of retry attempts before falling back to AP mode
- **Log Levels**: Adjust verbosity for debugging

### Customizing for Your Hardware

1. **Different SD Card Pins**:
   ```bash
   idf.py menuconfig
   # Navigate to: WiFi Component Configuration → SD Card Configuration
   # Update MOSI, MISO, SCK, CS pins
   ```

2. **Different LED Pin**:
   ```bash
   idf.py menuconfig
   # Navigate to: WiFi Component Configuration → Status LED
   # Update GPIO pin
   ```

3. **Disable LED** (if not using):
   ```bash
   idf.py menuconfig
   # Navigate to: WiFi Component Configuration → Status LED
   # Uncheck "Use SK6812 LED for status indication"
   ```

4. **Power Bus GPIO** (hardware-specific):
   Edit `main.c` lines 247-256 to match your board or remove if not needed.

## Usage Guide

### First Boot

1. **Insert SD Card**: Ensure SD card with web files is inserted.

2. **Power On**: Connect ESP32 to power/USB.

3. **Monitor Boot**:
   ```bash
   idf.py monitor
   ```
   
   Expected log output:
   ```
   I (123) main: START main.c from Jan 1 2024
   I (124) main: Setting up...
   I (145) wifi_manager: Initializing WiFi manager...
   I (156) wifi_manager: No saved credentials, starting captive portal
   I (167) wifi_manager: AP started: ESP32_Captive_Portal
   ```

4. **LED Status**: Blue breathing pattern indicates AP mode.

### Accessing the Web Interface

#### Scenario 1: First Boot (No Saved WiFi)

1. **Connect to AP**:
   - SSID: `ESP32_Captive_Portal`
   - Password: None (open network)

2. **Captive Portal**: Should auto-open, or navigate to any URL.

3. **Configure WiFi**:
   - Select your WiFi network
   - Enter password
   - Submit

4. **Wait for Connection**: Device reboots and connects to your WiFi.

5. **Find IP Address**:
   - Check your router's DHCP client list, or
   - Check serial monitor for IP address:
     ```
     I (2345) wifi_manager: Connected to WiFi
     I (2346) wifi_manager: IP Address: 192.168.1.100
     ```

6. **Access Web Interface**:
   - Navigate to `http://192.168.1.100/`
   - Or use mDNS: `http://esp32.local/` (if configured)

#### Scenario 2: Subsequent Boots (WiFi Configured)

1. **Auto-Connect**: Device automatically connects to saved WiFi.

2. **LED Status**: Yellow/orange breathing during connection, 2 blinks when connected.

3. **Access**: Navigate to device IP address or mDNS hostname.

## Customization

### Adding Custom Endpoints

Add new HTTP endpoints by registering handlers:

```c
// Define your handler function
esp_err_t my_api_handler(httpd_req_t *req) {
    const char* response = "{\"status\":\"ok\",\"data\":42}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

// Register in app_main()
void app_main(void) {
    wifi_init();
    
    httpd_uri_t my_api_uri = {
        .uri = "/api/data",
        .method = HTTP_GET,
        .handler = my_api_handler
    };
    wifi_register_http_handler(&my_api_uri);
}
```

### Modifying the Web Interface

1. **Edit HTML/CSS/JS Files**: Modify files in `webpage/` directory.

2. **Copy to SD Card**: Update files on the SD card.

3. **Test**: Refresh browser (may need hard refresh: Ctrl+F5).

4. **No Recompilation Needed**: Web files are served from SD card, not compiled into firmware.

**Adding a New Page**:

1. Create `new-page.html` in `webpage/` with standard structure:
   ```html
   <!DOCTYPE html>
   <html>
   <head>
     <link rel="stylesheet" href="/styles.css">
   </head>
   <body>
     <div id="nav"></div>
     <div class="card">
       <h1>New Page</h1>
       <!-- Your content -->
     </div>
     <footer id="status"></footer>
     <script src="common.js"></script>
   </body>
   </html>
   ```

2. Update `nav.html` to include link to new page.

3. Copy to SD card and test.

### Extending WebSocket Protocol

**Add a New Binary Value Type**:

1. **Update Enum** in `main.c`:
   ```c
   typedef enum {
       VALUE_NONE,
       SLIDER_BINARY,
       SLIDER_JSON,
       MY_NEW_VALUE  // Add here
   } ws_value_type_t;
   ```

2. **Handle in Server** (`ws_handler()` function):
   ```c
   switch(packet->type) {
       case MY_NEW_VALUE:
           // Process new value
           int16_t myValue = packet->value;
           ESP_LOGI(TAG, "New value: %d", myValue);
           break;
   }
   ```

3. **Update Client** (`ws.js`):
   ```javascript
   const WS_value = {
       VALUE_NONE: 0,
       SLIDER_BINARY: 1,
       SLIDER_JSON: 2,
       MY_NEW_VALUE: 3  // Add here
   };
   ```

4. **Send from Client**:
   ```javascript
   sendWSBinary(WS_value.MY_NEW_VALUE, 42);
   ```

**Add a New Event Type**:

Follow similar pattern with `ws_event_type_t` enum.

## Troubleshooting

### Serial Monitor Shows "SD Card Mount Failed"

**Symptoms**: Cannot access web interface, SD card errors in log.

**Solutions**:
1. Check SD card is properly inserted.
2. Verify SD card is formatted as FAT32.
3. Check wiring connections (MOSI, MISO, SCK, CS, VCC, GND).
4. Try a different SD card (some cards are incompatible).
5. Check pin configuration in menuconfig matches wiring.
6. Enable debug logs: `idf.py menuconfig` → Component config → Log output → Default log verbosity → Debug.

### Cannot Connect to WebSocket

**Symptoms**: WebSocket errors in browser console, "WebSocket connection failed".

**Solutions**:
1. Verify WebSocket support is enabled: `CONFIG_HTTPD_WS_SUPPORT=y` in `sdkconfig`.
2. Check HTTP server started successfully (serial monitor logs).
3. Ensure using `ws://` (not `wss://` or `http://`).
4. Verify device IP address is correct.
5. Check firewall/network settings.
6. Try different browser (Firefox, Chrome, Edge).

### Web Page Loads but No Files

**Symptoms**: Browser shows "File not found" or directory listing.

**Solutions**:
1. Ensure all `webpage/*` files are copied to SD card root.
2. Check SD card is mounted (serial monitor shows success).
3. Verify filenames are correct (case-sensitive on some systems).
4. Check SD card is not corrupted (reformat and re-copy files).
5. Enable FAT long filename support: `CONFIG_FATFS_LFN_HEAP=y`.

### Status Footer Shows "N/A" for WiFi

**Symptoms**: Footer displays "WiFi: N/A" or "IP: N/A".

**Solutions**:
1. Check device is connected to WiFi (serial monitor logs).
2. Verify `/wifi-status.json` endpoint is accessible.
3. Check browser console for fetch errors.
4. Ensure captive-wifi-manager component is initialized properly.

### Slider Values Not Updating

**Symptoms**: Moving sliders has no effect, values don't change.

**Solutions**:
1. Check WebSocket is connected (green "WebSocket connected" message).
2. Verify serial monitor shows received messages.
3. Check browser console for JavaScript errors.
4. Ensure `ws.js` is loaded correctly.
5. Test with browser developer tools (Network tab, WS frames).

### Build Errors

**Symptoms**: Compilation fails with errors.

**Common Issues**:
- **ESP-IDF version**: Ensure v5.5.0 or later (`idf.py --version`).
- **Missing dependencies**: Run `idf.py reconfigure` to install component dependencies.
- **Configuration mismatch**: Run `idf.py fullclean` and rebuild.
- **Component path**: Verify `override_path` in `idf_component.yml` points to `../../..` (component root).

### Out of Memory / Heap Errors

**Symptoms**: Device crashes, heap allocation failures, watchdog resets.

**Solutions**:
1. Increase stack sizes in `sdkconfig.defaults`:
   ```ini
   CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
   CONFIG_HTTPD_STACK_SIZE=10240
   ```
2. Reduce concurrent WebSocket connections (close unused tabs).
3. Optimize WebSocket message frequency (avoid rapid updates).
4. Check for memory leaks (monitor heap in status footer).
5. Use smaller web files (minify CSS/JS).

### LED Not Working

**Symptoms**: No LED indication, LED stays off.

**Solutions**:
1. Check LED is enabled: `idf.py menuconfig` → WiFi Component Configuration → Status LED.
2. Verify LED pin configuration (default GPIO 45).
3. Check LED power supply (5V for SK6812, 3.3V may need level shifter).
4. Test LED separately (ensure it's not damaged).
5. Check wiring (DIN, VCC, GND).
6. Verify LED type (SK6812 RGB, not WS2812B or others).

---

## Additional Resources

- **Main Repository**: [esp32-captive-wifi-manager](https://github.com/theMoonlitWolf/esp32-captive-wifi-manager)
- **ESP-IDF Documentation**: [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/latest/)
- **ESP-HTTP-Server Guide**: [HTTP Server Component](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- **WebSocket Protocol**: [RFC 6455](https://tools.ietf.org/html/rfc6455)

## Contributing

Found a bug or have a suggestion? Please open an issue or submit a pull request to the main repository.

## License

This example is part of the ESP32 Captive WiFi Manager component and is licensed under the Apache License 2.0. See the [LICENSE](../../LICENSE) file for details.

---

**Happy Coding!** 🚀

For questions or support, please open an issue on GitHub.
