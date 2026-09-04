# ESP32 Captive WiFi Manager

A comprehensive WiFi management component for ESP32 devices with captive portal support, designed for ESP-IDF framework.

## Overview

The ESP32 Captive WiFi Manager provides an easy-to-use, yet complex solution for managing WiFi connectivity on ESP32 devices. It automatically handles WiFi connection attempts, falls back to a captive portal when needed, and exposes a small HTTP/WebSocket API for status and control.

## Use Cases

- **IoT Devices**: Consumer IoT products that need easy WiFi setup out of the box
- **Smart Home Devices**: Thermostats, sensors, and controllers requiring flexible network configuration
- **Prototyping**: Rapid development of WiFi-enabled ESP32 projects without hardcoded credentials
- **Field Deployment**: Devices deployed in various locations with different WiFi networks
- **Development & Testing**: Simplified WiFi configuration during development and testing phases

## Key Features

### Core Functionality

- **Automatic WiFi Management**: Attempts to connect to saved WiFi networks on boot
- **Captive Portal**: Launches a user-friendly web interface when WiFi connection fails
- **WiFi Scanning**: Scans and displays available networks in the captive portal
- **Credential Persistence**: Stores WiFi credentials in NVS flash memory
- **AP-Only Mode**: Can operate solely as an Access Point without attempting STA connection
- **Multiple Authentication Modes**: Supports Open and WPA2-Personal
- **Static IP Support**: Configure static IP addresses or use DHCP
- **mDNS Support**: Optional mDNS hostname configuration for easy device discovery
- **Status LED**: Visual feedback on connection status using SK6812 LED (configurable)
- **SD Card Support**: Optional SD card integration for file serving
- **Custom HTTP Handlers**: Register your own HTTP endpoints alongside the captive portal

### WebSocker server
- **WebSocket Support**: Provides a WebSocket server for real-time communication
- **RAP.v1 Protocol**: Implements the Realtime Action Protocol (RAP) for JSON-style messaging over WebSocket
    - Supports bidirectional commands, requests, and subscriptions
    - Documentation available in `RAP.v1.md` for message structure and usage

### Network Modes

1. **Station (STA) Mode**: Connects to an existing WiFi network
2. **Access Point (AP) Mode**: Creates its own WiFi network with captive portal
3. **Captive portal**: Switches to AP captive portal mode after failed connection attempts

## Configuration Options

The component provides extensive configuration through ESP-IDF's menuconfig system. Access these options via:

```bash
idf.py menuconfig
```

Navigate to: **WiFi Component Configuration**

### Available Configuration Options

#### WiFi Modes and Features
- **Enable STA mode**: Enable/disable Station mode (default: enabled)
- **Enable AP mode**: Enable/disable Access Point mode (default: enabled)
- **Enable Captive Portal**: Enable/disable captive portal functionality (default: enabled)
    - When enabled, ensure Python3 is available on the build host for preprocessing step
- **Enable WS-RAP (WebSocket Realtime Action Protocol)**: Enable/disable WS-RAP support (default: enabled)
- **Enable SD card support**: Enable/disable SD card integration (default: disabled)


#### Logging
- **WiFi component log level**: Control verbosity of component logs
  - None, Error, Warn, Info (default), Debug, Verbose

#### Connection Behavior
- **Maximum reconnect attempts**: Number of reconnection attempts before switching to AP mode (default: 5)
- **Maximum number of APs to store**: APs stored from WiFi scan, sorted by RSSI (default: 8)

#### SD Card Configuration
- **SD card MOSI pin**: GPIO pin for SD card MOSI (default: 11)
- **SD card MISO pin**: GPIO pin for SD card MISO (default: 13)
- **SD card SCK pin**: GPIO pin for SD card SCK (default: 12)
- **SD card CS pin**: GPIO pin for SD card CS (default: 10)
- **Format SD card on mount failure**: Auto-format SD on mount failure (default: disabled)

#### Status LED
- **Use SK6812 LED for status indication**: Enable/disable LED status indicator (default: enabled)
- **GPIO pin for SK6812 status LED**: GPIO pin for the status LED (default: 45)

### LED Status Indicators

The component uses an SK6812 RGB LED to provide visual feedback about the device's current state. The LED patterns are as follows:

| State | Color | Pattern | Description |
|-------|-------|---------|-------------|
| **Off** | None | Solid off | LED is disabled or device is powered off |
| **Loading** | White | Slow breathing (500ms) | System is initializing, loading configuration |
| **Loaded** | White | 2 quick blinks | System initialization complete |
| **WiFi Connecting** | Yellow/Orange | Slow breathing (500ms) | Attempting to connect to saved WiFi network |
| **WiFi Connected** | Yellow/Orange | 2 quick blinks | Successfully connected to WiFi network (STA mode) |
| **WiFi Disconnected** | Red | 3 quick blinks | Disconnected from WiFi network, will attempt reconnection |
| **AP Starting** | Blue | Slow breathing (500ms) | Captive portal AP is starting up |
| **AP Started** | Blue | 2 quick blinks | Captive portal AP is active and ready for configuration |

**Pattern Details:**
- **Breathing**: LED smoothly fades in and out (pulsing effect)
- **Quick blinks**: 100ms on/off cycles
- **Slow breathing**: 500ms fade in/out cycles

**Color Reference (HSV):**
- White: Neutral system operations
- Yellow/Orange (Hue 40°): Station (STA) mode operations
- Red (Hue 0°): Error or disconnected state
- Blue (Hue 210°): Access Point (AP) mode operations

You can override the LED color at any time using `wifi_set_led_rgb()` for custom application-specific status indication.

## How to Use as a Developer

### Prerequisites

- ESP-IDF v5.5.0 or later
- Python 3.x (for captive portal preprocessing if CONFIG_WIFI_ENABLE_CAPTIVE_PORTAL is enabled)
- ESP32 development board
- Basic knowledge of ESP-IDF build system

### Installation

#### Via IDF Component Manager

1. Add dependency to your project's `idf_component.yml` manifest:

```yml
dependencies:
  esp32-captive-wifi-manager:
    git: https://github.com/theMoonlitWolf/esp32-captive-wifi-manager.git
    path: "."
    version: v0.2.1 #Replace with desired version
```

2. Add it to your `INCLUDE_DIRS` in `CMakeLists.txt`:

```CMake
INCLUDE_DIRS managed_components/esp32-captive-wifi-manager/include
```

#### Manual Installation

1. Clone or download this repository into your project's `components` directory:

```bash
cd your_project/components
git clone https://github.com/theMoonlitWolf/esp32-captive-wifi-manager.git
```

2. Add it to your `INCLUDE_DIRS` in `CMakeLists.txt`:

```CMake
INCLUDE_DIRS components/esp32-captive-wifi-manager/include
```

### Basic Usage

#### Minimal Example

```c
#include "Wifi.h"

void app_main(void)
{
    // Initialize the WiFi manager
    // This handles everything: NVS, WiFi, captive portal, etc.
    wifi_init();
    
    // Your application code here
}
```

#### Registering Custom HTTP Handlers

You can register your own HTTP endpoints to serve custom content:

```c
#include "Wifi.h"
#include "esp_http_server.h"

// Your custom handler
esp_err_t my_custom_handler(httpd_req_t *req) {
    const char* response = "Hello from custom handler!";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

void app_main(void)
{
    wifi_init();
    
    // Register custom HTTP endpoint
    httpd_uri_t custom_uri = {
        .uri = "/custom",
        .method = HTTP_GET,
        .handler = my_custom_handler
    };
    wifi_register_http_handler(&custom_uri);
}
```

#### Using WebSocket Support

```c
esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;  // WebSocket handshake
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    // Receive WebSocket frame
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    
    // Handle WebSocket data...
    
    return ESP_OK;
}

void app_main(void)
{
    wifi_init();
    
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true
    };
    wifi_register_http_handler(&ws_uri);
}
```

#### Controlling Status LED

```c
#include "Wifi.h"

void app_main(void)
{
    wifi_init();
    
    // Set LED color (IRGB format), brightness 0-255
    // Example: Red at 50% brightness
    wifi_set_led_rgb(0x00FF0000, 128);
}
```

Note: This feature is depecated and Status led interface for other uses is to be wastly improved

## Exposed Interface (API)

### Main Functions

#### `esp_err_t wifi_init(void)`
Initializes the WiFi manager component. This function:
- Initializes NVS flash
- Sets up WiFi stack
- Loads saved credentials
- Attempts WiFi connection or starts captive portal
- Starts HTTP server

**Returns**: 
- `ESP_OK` on success
- Error code on failure

#### `esp_err_t wifi_register_http_handler(httpd_uri_t *uri)`
Registers a custom HTTP handler with the web server.

**Parameters**:
- `uri`: Pointer to `httpd_uri_t` structure defining the endpoint

**Returns**:
- `ESP_OK` on success
- `ESP_FAIL` if maximum handlers exceeded or registration failed

**Note**: Maximum 8 custom handlers can be registered (defined by `MAX_CUSTOM_HANDLERS`)

#### `void wifi_set_led_rgb(uint32_t irgb, uint8_t brightness)`
Sets the status LED color and brightness.

**Parameters**:
- `irgb`: Color in IRGB format (0x00RRGGBB for RGB LEDs)
- `brightness`: Brightness level (0-255)

#### `void url_decode(char *str)`
URL-decodes a string in-place. Useful for processing form data from HTTP POST requests.

**Parameters**:
- `str`: String to decode (modified in-place)

### Data Structures

#### `captive_portal_config`
Configuration structure for WiFi and captive portal settings:

```c
typedef struct {
    char ssid[32];              // WiFi network SSID
    uint8_t authmode;           // Auth mode: 0=Open, 1=Password, 2=Enterprise
    char username[64];          // Username for WPA2-Enterprise
    char password[64];          // WiFi password
    bool use_static_ip;         // Use static IP (true) or DHCP (false)
    esp_ip4_addr_t static_ip;   // Static IP address (if enabled)
    bool use_mDNS;              // Enable mDNS
    char mDNS_hostname[32];     // mDNS hostname (e.g., "esp32")
    char service_name[64];      // mDNS service name (e.g., "ESP32 Web")
    char ap_ssid[32];           // Captive portal AP SSID
    char ap_password[64];       // Captive portal AP password
} captive_portal_config;
```
Note: This is normally not used, only if you want hard coded values on first start.

## Available Examples

### Full Example

Located in `examples/full/`, this comprehensive example demonstrates:

- Basic WiFi manager initialization
- Custom HTTP endpoint registration (`/status.json`)
- WebSocket communication (`/ws`)
- Binary and JSON WebSocket protocols
- Form handling with POST requests
- LED color control
- System status monitoring (uptime, heap memory)

**Usage:**

Connect an SD card with contents of the `webpage/` folder on its root.

Build, flash, and monitor:
```bash
cd examples/full
idf.py build
idf.py flash monitor
```

**Example features:**
1. Connects to saved WiFi or starts captive portal
2. Serves a web interface at `http://<device-ip>/`
3. Provides real-time system status via WebSocket
4. Demonstrates form handling and control
5. Shows LED status indication

## Captive Portal Usage

When the device cannot connect to a saved WiFi network (or on first boot), it automatically:

1. **Switches to AP Mode**: Creates an access point with SSID from configuration
2. **Starts DNS Server**: Redirects all DNS queries to the device IP
3. **Serves Configuration Page**: Displays a web interface for WiFi setup
4. **Scans Networks**: Shows available WiFi networks
5. **Saves Credentials**: Stores the configuration to NVS upon submission
6. **Attempts Connection**: Tries to connect to the configured network
7. **Falls Back if Needed**: Returns to AP mode if connection fails

### Default Captive Portal Access

- **SSID**: `ESP32_Captive_Portal`, not password. *This is not configurable*
  - If connected to the devide's AP, it should automatically launch the captive portal
  - Any URL shoud redirect you there
- **URI**: Navigate to `/captive` when connected to the device in runtime mode - STA or AP

## How It Works

1. **Boot Sequence**:
   - Initialize NVS and load saved WiFi settings
   - Initialize WiFi stack and event handlers
   - Initialize status LED (if enabled)
   - Mount SD card (if available)

2. **Connection Attempt**:
   - If saved credentials exist, attempt STA connection or launch AP if configured
   - Monitor connection success/failure
   - Count failed attempts

3. **Fallback to AP Mode**:
   - After max reconnection attempts, switch to AP captive portal mode
   - Start DNS server for captive portal
   - Launch HTTP server with configuration interface

4. **Configuration**:
   - User connects to AP
   - Opens browser (auto-redirected to config page)
   - Selects network and enters credentials
   - Submits form

5. **Connection Retry**:
   - Save new credentials to NVS
   - Restart WiFi in STA mode
   - Attempt connection with new credentials

## Dependencies

The component automatically manages these ESP-IDF component dependencies:

- `esp_wifi`: WiFi driver
- `esp_event`: Event handling
- `esp_http_server`: HTTP server
- `nvs_flash`: Non-volatile storage
- `lwip`: Lightweight IP stack
- `mdns`: mDNS service discovery (v1.8.2+)
- `led_indicator`: LED control (v1.1.1+)
- `fatfs`: FAT filesystem (for SD card)
- `cJSON`: JSON parsing and generation

The component also depends on this component, not available in ESP-IDF component registry:
- `dns_server`: Used for DNS hijacking, by Espressif systems

## Troubleshooting

### Cannot connect to captive portal
- Check that device is in AP mode (LED should indicate this)
- Look for the AP SSID configured in your device
- Ensure your phone/computer WiFi is enabled

### WiFi credentials not saving
- Check NVS partition is properly configured in partition table
- Enable debug logging to see NVS operations
- Verify SD card (if used) is properly mounted

### Status LED not working
- Verify `CONFIG_WIFI_USE_SK6812_STATUS_LED` is enabled
- Check GPIO pin configuration matches your hardware
- Ensure LED is properly connected and powered

### Build errors
- Verify ESP-IDF version is 5.5.0 or later
- Run `idf.py fullclean` and rebuild
- Check all dependencies are properly installed

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## Author

theMoonlitWolf

## Repository

https://github.com/theMoonlitWolf/esp32-captive-wifi-manager
