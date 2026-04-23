#include "Flags.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"

static const char *TAG = "Wifi: Flags";

/** @brief FreeRTOS event group for WiFi state management and mode switching */
static EventGroupHandle_t s_wifi_events;

void wifi_flags_init(void)
{
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
    }
}

void wifi_flags_set_bits(EventBits_t bits_to_set)
{
    if (s_wifi_events) {
        xEventGroupSetBits(s_wifi_events, bits_to_set);
    } else {
        ESP_LOGE(TAG, "Event group not initialized, cannot set bits");
    }
}

void wifi_flags_clear_bits(EventBits_t bits_to_clear)
{
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, bits_to_clear);
    } else {
        ESP_LOGE(TAG, "Event group not initialized, cannot clear bits");
    }
}

EventBits_t wifi_flags_get_bits(void)
{
    EventBits_t bits = 0;
    if (s_wifi_events) {
        bits = xEventGroupGetBits(s_wifi_events);
    } else {
        ESP_LOGE(TAG, "Event group not initialized, cannot get bits");
    }
    return bits;
}

EventBits_t wifi_flags_wait_for_bits(EventBits_t bits_to_wait, TickType_t ticks_to_wait)
{
    ESP_LOGV(TAG, "Waiting for event bits: %c%c%c%c%c%c%c%c",
            bits_to_wait & BIT7 ? '1' : '0',
            bits_to_wait & BIT6 ? '1' : '0',
            bits_to_wait & BIT5 ? '1' : '0',
            bits_to_wait & BIT4 ? '1' : '0',
            bits_to_wait & BIT3 ? '1' : '0',
            bits_to_wait & BIT2 ? '1' : '0',
            bits_to_wait & BIT1 ? '1' : '0',
            bits_to_wait & BIT0 ? '1' : '0');
    int ret = xEventGroupWaitBits(s_wifi_events, bits_to_wait, pdFALSE, pdFALSE, ticks_to_wait);
    ESP_LOGV(TAG, "Received event bits: %c%c%c%c%c%c%c%c",
            ret & BIT7 ? '1' : '0',
            ret & BIT6 ? '1' : '0',
            ret & BIT5 ? '1' : '0',
            ret & BIT4 ? '1' : '0',
            ret & BIT3 ? '1' : '0',
            ret & BIT2 ? '1' : '0',
            ret & BIT1 ? '1' : '0',
            ret & BIT0 ? '1' : '0');
    return ret;
}
