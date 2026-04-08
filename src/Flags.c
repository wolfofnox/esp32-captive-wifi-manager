#include "Flags.h"

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CONFIG_LOG_LEVEL_WIFI
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "Wifi: Flags";

/** @brief FreeRTOS event group for WiFi state management and mode switching */
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_lock;

void wifi_flags_init(void)
{
    esp_log_level_set(TAG, CONFIG_LOG_LEVEL_WIFI);
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void wifi_flags_set_bits(EventBits_t bits_to_set)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, bits_to_set);
        } else {
            ESP_LOGE(TAG, "Event group not initialized, cannot set bits");
        }
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGE(TAG, "Failed to take mutex to set event bits");
    }
}

void wifi_flags_clear_bits(EventBits_t bits_to_clear)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, bits_to_clear);
        } else {
            ESP_LOGE(TAG, "Event group not initialized, cannot clear bits");
        }
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGE(TAG, "Failed to take mutex to clear event bits");
    }
}

EventBits_t wifi_flags_get_bits(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        EventBits_t bits = 0;
        if (s_wifi_events) {
            bits = xEventGroupGetBits(s_wifi_events);
        } else {
            ESP_LOGE(TAG, "Event group not initialized, cannot get bits");
        }
        xSemaphoreGive(s_lock);
        return bits;
    } else {
        ESP_LOGE(TAG, "Failed to take mutex to get event bits");
        return 0;
    }
}

EventBits_t wifi_flags_wait_for_bits(EventBits_t bits_to_wait, TickType_t ticks_to_wait, bool take_mutex_on_exit)
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
    if (take_mutex_on_exit) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to take mutex after waiting for event bits");
        }
    }
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

void wifi_flags_give_mutex(void)
{
    if (xSemaphoreGive(s_lock) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to give mutex");
    }
}