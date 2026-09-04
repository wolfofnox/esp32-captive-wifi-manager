#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include "helpers.h"


esp_err_t get_nvs_wifi_settings(captive_portal_config *cfg);
esp_err_t set_nvs_wifi_settings(captive_portal_config *cfg);
esp_err_t init_nvs();