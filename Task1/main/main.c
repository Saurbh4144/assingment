#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "telemetry.h"
#include "wifi_manager.h"

static const char *TAG = "task1_main";
#define MQTT_BROKER_URI "mqtt://test.mosquitto.org:1883"

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase/reinitialize");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
        return;
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");
}

static void synchronize_time(void)
{
    ESP_LOGI(TAG, "Starting SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (err == ESP_OK) {
        time_t now;
        struct tm utc_time;
        time(&now);
        gmtime_r(&now, &utc_time);
        char buffer[40];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &utc_time);
        ESP_LOGI(TAG, "SNTP synchronized: %s", buffer);
    } else {
        ESP_LOGW(TAG, "SNTP sync timed out: %s", esp_err_to_name(err));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base; (void)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        telemetry_set_mqtt_connected(true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        telemetry_set_mqtt_connected(false);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        telemetry_set_mqtt_connected(false);
        break;
    default:
        break;
    }
}

static esp_mqtt_client_handle_t start_mqtt(void)
{
    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "MQTT client initialization failed");
        return NULL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    ESP_LOGI(TAG, "MQTT client started: %s", MQTT_BROKER_URI);
    return client;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Native IoT Task 1 starting");
    initialize_nvs();
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_LOGI(TAG, "Waiting for Wi-Fi/IP");
    if (!wifi_manager_wait_connected(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Wi-Fi connection wait failed");
        return;
    }
    synchronize_time();
    esp_mqtt_client_handle_t mqtt_client = start_mqtt();
    if (mqtt_client == NULL) return;
    ESP_ERROR_CHECK(telemetry_start(mqtt_client));
    ESP_LOGI(TAG, "Task 1 initialization complete");
}
