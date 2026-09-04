#pragma once

#include "esp_err.h"
#include "stdbool.h"

esp_err_t register_runtime_handlers(bool sd_card_present);