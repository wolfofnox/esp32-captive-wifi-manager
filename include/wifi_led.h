#pragma once
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_WIFI_USE_SK6812_STATUS_LED

#include "led_indicator.h"

#else

/* Minimal no-op compatibility layer for led_indicator when the SK6812
 * status LED is disabled. This allows source files to call the LED API
 * without sprinkling #if guards throughout the codebase. */

typedef void* led_indicator_handle_t;

typedef struct {
    int type;
    int value;
    int duration_ms;
} blink_step_t;

/* Blink command types */
#define LED_BLINK_HOLD     0
#define LED_BLINK_STOP     1
#define LED_BLINK_BREATHE  2
#define LED_BLINK_HSV      3
#define LED_BLINK_LOOP     4

/* LED states and helpers */
#define LED_STATE_OFF      0
#define LED_STATE_ON       1
#define LED_STATE_75_PERCENT 2
#define MAX_SATURATION     255
#define SET_HSV(h,s,v) (0)

/* LED driver config placeholders used in initializers */
typedef struct { 
    int strip_gpio_num; 
    int max_leds; 
    int led_pixel_format; 
    int led_model; 
    struct { int invert_out; } flags; 
} led_strip_cfg_t;

typedef struct {
    led_strip_cfg_t led_strip_cfg;
    int led_strip_driver;
    struct { int clk_src; int spi_bus; } led_strip_spi_cfg;
} led_indicator_strips_config_t;

typedef struct {
    int mode;
    led_indicator_strips_config_t *led_indicator_strips_config;
    const blink_step_t **blink_lists;
    int blink_list_num;
} led_indicator_config_t;

/* Minimal constants used by Wifi.c initializers */
#define LED_STRIPS_MODE 0
#define LED_STRIP_SPI 1
#define LED_PIXEL_FORMAT_GRB 0
#define LED_MODEL_SK6812 0
#define SPI_CLK_SRC_DEFAULT 0
#define SPI3_HOST 0

/* No-op implementations */
static inline led_indicator_handle_t led_indicator_create(const led_indicator_config_t *cfg) { (void)cfg; return NULL; }
static inline esp_err_t led_indicator_start(led_indicator_handle_t handle, int blink) { (void)handle; (void)blink; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t led_indicator_stop(led_indicator_handle_t handle, int blink)  { (void)handle; (void)blink; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t led_indicator_set_rgb(led_indicator_handle_t handle, uint32_t rgb) { (void)handle; (void)rgb; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t led_indicator_set_brightness(led_indicator_handle_t handle, uint8_t brightness) { (void)handle; (void)brightness; return ESP_ERR_NOT_SUPPORTED; }
static inline void led_indicator_destroy(led_indicator_handle_t handle) { (void)handle; }

#endif // CONFIG_WIFI_USE_SK6812_STATUS_LED
