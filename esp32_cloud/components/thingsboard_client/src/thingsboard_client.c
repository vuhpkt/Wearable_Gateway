#include "thingsboard_client.h"

#ifndef HOST_UNIT_TEST
#include "esp_log.h"

static const char *TAG = "TB_CLIENT";

esp_err_t tb_client_init(void) {
    esp_err_t err = tb_mqtt_init(TB_MQTT_BROKER_URI, TB_GATEWAY_ACCESS_TOKEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ThingsBoard MQTT: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ThingsBoard client initialized successfully");
    return ESP_OK;
}
#endif
