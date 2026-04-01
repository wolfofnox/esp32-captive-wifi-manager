#include "Flags.h"

/** @brief FreeRTOS event group for WiFi state management and mode switching */
static EventGroupHandle_t s_wifi_events;

void wifi_flags_init(void)
{
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
    }
}

void wifi_flags_set_bits(EventBits_t bits_to_set)
{
    if (s_wifi_events) {
        xEventGroupSetBits(s_wifi_events, bits_to_set);
    }
}

void wifi_flags_clear_bits(EventBits_t bits_to_clear)
{
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, bits_to_clear);
    }
}

EventBits_t wifi_flags_get_bits(void)
{
    if (s_wifi_events) {
        return xEventGroupGetBits(s_wifi_events);
    }
    return 0;
}

EventBits_t wifi_flags_wait_for_bits(EventBits_t bits_to_wait, TickType_t ticks_to_wait)
{
    return xEventGroupWaitBits(s_wifi_events, bits_to_wait, pdTRUE, pdFALSE, ticks_to_wait);
}