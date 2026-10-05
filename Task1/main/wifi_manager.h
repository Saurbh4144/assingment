#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
bool wifi_manager_wait_connected(TickType_t timeout_ticks);
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
