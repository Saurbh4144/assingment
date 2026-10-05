#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telemetry_start(esp_mqtt_client_handle_t mqtt_client);
void telemetry_set_mqtt_connected(bool connected);

#ifdef __cplusplus
}
#endif
