#include "telemetry.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *TAG = "telemetry";
static const char *HEARTBEAT_TOPIC = "esp32c6/task1/heartbeat";
static TaskHandle_t s_task_handle = nullptr;
static volatile bool s_mqtt_connected = false;

void telemetry_set_mqtt_connected(bool connected)
{
    s_mqtt_connected = connected;
}

static void telemetry_task(void *parameter)
{
    esp_mqtt_client_handle_t mqtt_client = static_cast<esp_mqtt_client_handle_t>(parameter);
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(5000));
        const int64_t uptime_seconds = esp_timer_get_time() / 1000000LL;
        const bool wifi_connected = wifi_manager_is_connected();
        char payload[192];
        snprintf(payload, sizeof(payload),
                 "{\"device\":\"esp32c6\",\"status\":\"%s\",\"uptime\":%lld,\"wifi\":%s,\"mqtt\":%s}",
                 (wifi_connected && s_mqtt_connected) ? "online" : "recovering",
                 static_cast<long long>(uptime_seconds),
                 wifi_connected ? "true" : "false",
                 s_mqtt_connected ? "true" : "false");

        if (!wifi_connected) {
            ESP_LOGW(TAG, "Heartbeat skipped: Wi-Fi is disconnected");
            continue;
        }
        if (!s_mqtt_connected) {
            ESP_LOGW(TAG, "Heartbeat skipped: MQTT is disconnected");
            continue;
        }

        int msg_id = esp_mqtt_client_publish(mqtt_client, HEARTBEAT_TOPIC, payload, 0, 1, 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "Heartbeat publish failed");
        } else {
            ESP_LOGI(TAG, "Heartbeat published: %s", payload);
        }
    }
}

esp_err_t telemetry_start(esp_mqtt_client_handle_t mqtt_client)
{
    if (mqtt_client == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_task_handle != nullptr) return ESP_ERR_INVALID_STATE;
    BaseType_t result = xTaskCreate(telemetry_task, "heartbeat_task", 4096,
                                    mqtt_client, 5, &s_task_handle);
    if (result != pdPASS) {
        s_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Heartbeat task started (5 second period)");
    return ESP_OK;
}
