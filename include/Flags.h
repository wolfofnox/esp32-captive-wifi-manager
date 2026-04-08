#pragma once
#include "esp_event.h"

/** @brief Event bit indicating WiFi is connected to an AP (STA mode) */
#define CONNECTED_BIT (1u << 0)

/** @brief Event bit to trigger switch to STA (station/client) mode */
#define SWITCH_TO_STA_BIT (1u << 1)

/** @brief Event bit to trigger switch to AP (access point) mode */
#define SWITCH_TO_AP_BIT (1u << 2)

/** @brief Event bit to trigger switch to captive portal AP mode */
#define SWITCH_TO_CAPTIVE_AP_BIT (1u << 3)

/** @brief Event bit to trigger reconnection in STA mode */
#define RECONECT_BIT (1u << 4)

/** @brief Event bit to trigger mDNS configuration update */
#define mDNS_CHANGE_BIT (1u << 5)

/** @brief Event bit to trigger time synchronization */
#define SYNC_TIME_BIT (1u << 6)

/** @brief Event bit indicating AP mode is active */
#define AP_MODE_BIT (1u << 7)

void wifi_flags_init(void);
void wifi_flags_set_bits(EventBits_t bits_to_set);
void wifi_flags_clear_bits(EventBits_t bits_to_clear);
EventBits_t wifi_flags_get_bits(void);
EventBits_t wifi_flags_wait_for_bits(EventBits_t bits_to_wait, TickType_t ticks_to_wait, bool take_mutex_on_exit);
void wifi_flags_give_mutex(void);