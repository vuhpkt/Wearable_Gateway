/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* FreeRTOS & ESP-IDF System Headers */
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>

/* Project Local Headers */
#include "thingsboard_mqtt.h"


static const char *TAG = "tb_mqtt";


#define MQTT_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_event_group = NULL;
static bool s_is_connected = false;
static bool s_is_paused = false;
static tb_mqtt_connect_cb_t s_connect_cb = NULL;

void tb_mqtt_register_connect_callback(tb_mqtt_connect_cb_t cb) {
    s_connect_cb = cb;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to Broker successfully! [State: READY]");
        s_is_connected = true;
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        if (s_connect_cb != NULL) {
            s_connect_cb();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected from Broker! Waiting auto-reconnect...");
        s_is_connected = false;
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT Publish ACK received (msg_id=%d)", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error event encountered (type=%d)", event->error_handle->error_type);
        break;
    default:
        break;
    }
}


esp_err_t tb_mqtt_init(const char *broker_uri, const char *access_token) {
    if (broker_uri == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        if (s_event_group == NULL)
            return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t config = {
        .broker.address.uri = broker_uri,
        .credentials.username = access_token,
        .network.timeout_ms = 5000,
        .session.keepalive = 15};

    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize esp_mqtt_client handle");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT Client initialized -> Target Broker: %s", broker_uri);
    return ESP_OK;
}

bool tb_mqtt_is_connected(void) { return s_is_connected; }

esp_err_t tb_mqtt_wait_connected(TickType_t xTicksToWait) {
    if (s_event_group == NULL)
        return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(s_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, xTicksToWait);
    return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int tb_mqtt_publish(const char *topic, const char *json_payload, int qos) {
    if (s_client == NULL || json_payload == NULL || !s_is_connected)
        return -1;
    int len = (int)strlen(json_payload);
    int msg_id = esp_mqtt_client_publish(s_client, topic, json_payload, len, qos, 0);
    return msg_id;
}

void tb_mqtt_stop(void) {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_is_connected = false;
    s_is_paused = false;
}

void tb_mqtt_pause(void) {
    if (s_client != NULL && !s_is_paused) {
        ESP_LOGI(TAG, "⏸ Pausing MQTT client (Wi-Fi offline / Provisioning mode)...");
        esp_mqtt_client_stop(s_client);
        s_is_paused = true;
        s_is_connected = false;
        if (s_event_group != NULL) {
            xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        }
    }
}

void tb_mqtt_resume(void) {
    if (s_client != NULL && s_is_paused) {
        ESP_LOGI(TAG, "▶ Resuming MQTT client connection...");
        esp_err_t err = esp_mqtt_client_start(s_client);
        if (err == ESP_OK) {
            s_is_paused = false;
        } else {
            ESP_LOGE(TAG, "Failed to resume MQTT client: %s", esp_err_to_name(err));
        }
    }
}
