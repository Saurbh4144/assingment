#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

static const char *TAG = "wifi_manager";
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t s_wifi_event_group = nullptr;
static volatile bool s_wifi_connected = false;
static volatile bool s_provisioning_active = false;
static const char *PROV_POP = "abcd1234";

static void make_service_name(char *service_name, size_t max_len)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(service_name, max_len, "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        snprintf(service_name, max_len, "PROV_ESP32C6");
    }
}

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base;
    switch (event_id) {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "BLE provisioning started");
        break;
    case NETWORK_PROV_WIFI_CRED_RECV: {
        const wifi_sta_config_t *wifi_cfg = static_cast<const wifi_sta_config_t *>(event_data);
        ESP_LOGI(TAG, "Received Wi-Fi credentials for SSID: %.*s",
                 static_cast<int>(sizeof(wifi_cfg->ssid)),
                 reinterpret_cast<const char *>(wifi_cfg->ssid));
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL: {
        const network_prov_wifi_sta_fail_reason_t *reason =
            static_cast<const network_prov_wifi_sta_fail_reason_t *>(event_data);
        if (reason != nullptr && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) {
            ESP_LOGE(TAG, "Provisioning failed: Wi-Fi authentication error");
        } else {
            ESP_LOGE(TAG, "Provisioning failed: access point not found");
        }
        esp_err_t err = network_prov_mgr_reset_wifi_sm_state_on_failure();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not reset provisioning Wi-Fi state: %s", esp_err_to_name(err));
        }
        break;
    }
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning successful");
        s_provisioning_active = false;
        break;
    case NETWORK_PROV_END: {
        ESP_LOGI(TAG, "Provisioning service ended");
        s_provisioning_active = false;
        esp_err_t err = network_prov_mgr_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Provisioning deinit returned: %s", esp_err_to_name(err));
        }
        break;
    }
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base; (void)event_data;
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "Wi-Fi station started");
        if (!s_provisioning_active) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Initial esp_wifi_connect failed: %s", esp_err_to_name(err));
            }
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_wifi_connected = false;
        if (s_wifi_event_group != nullptr) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        if (!s_provisioning_active) {
            ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Reconnect call failed: %s", esp_err_to_name(err));
            }
        }
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(event_data);
    ESP_LOGI(TAG, "Wi-Fi connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_connected = true;
    if (s_wifi_event_group != nullptr) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == nullptr) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (esp_netif_create_default_wifi_sta() == nullptr) return ESP_FAIL;

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&wifi_cfg)) != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               &provisioning_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &ip_event_handler, nullptr));

    network_prov_mgr_config_t prov_cfg = {};
    prov_cfg.scheme = network_prov_scheme_ble;
    prov_cfg.scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    if ((err = network_prov_mgr_init(prov_cfg)) != ESP_OK) return err;

    bool provisioned = false;
    if ((err = network_prov_mgr_is_wifi_provisioned(&provisioned)) != ESP_OK) {
        network_prov_mgr_deinit();
        return err;
    }

    if (!provisioned) {
        s_provisioning_active = true;
        char service_name[20] = {0};
        make_service_name(service_name, sizeof(service_name));
        uint8_t custom_service_uuid[16] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02
        };
        network_prov_scheme_ble_set_service_uuid(custom_service_uuid);
        ESP_LOGI(TAG, "Device is not provisioned");
        ESP_LOGI(TAG, "BLE provisioning device: %s", service_name);
        ESP_LOGI(TAG, "Provisioning PoP: %s", PROV_POP);
        return network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1,
            static_cast<const void *>(PROV_POP),
            service_name,
            nullptr);
    }

    ESP_LOGI(TAG, "Wi-Fi credentials already stored in NVS");
    s_provisioning_active = false;
    if ((err = network_prov_mgr_deinit()) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    return esp_wifi_start();
}

bool wifi_manager_wait_connected(TickType_t timeout_ticks)
{
    if (s_wifi_event_group == nullptr) return false;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_connected;
}
