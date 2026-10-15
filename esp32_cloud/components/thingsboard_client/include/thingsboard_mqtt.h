#pragma once

/* Standard C Libraries */
#include <stdbool.h>


/* FreeRTOS & ESP-IDF System Headers */
#include <esp_err.h>
#include <freertos/FreeRTOS.h>


esp_err_t tb_mqtt_init(const char *broker_uri, const char *access_token);

bool tb_mqtt_is_connected(void);

esp_err_t tb_mqtt_wait_connected(TickType_t xTicksToWait);

int tb_mqtt_publish(const char *topic, const char *json_payload, int qos);

typedef void (*tb_mqtt_connect_cb_t)(void);
void tb_mqtt_register_connect_callback(tb_mqtt_connect_cb_t cb);

void tb_mqtt_stop(void);
void tb_mqtt_pause(void);
void tb_mqtt_resume(void);