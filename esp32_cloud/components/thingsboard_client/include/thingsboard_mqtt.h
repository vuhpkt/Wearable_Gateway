#pragma once

/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "uart_frame_def.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
/* FreeRTOS & ESP-IDF System Headers */
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#else
#ifndef ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_TIMEOUT -3
#endif
#ifndef TICK_TYPE_T_DEFINED
#define TICK_TYPE_T_DEFINED
typedef uint32_t TickType_t;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════
 *  MQTT Lifecycle & PUBACK Tracking Definitions
 * ════════════════════════════════════════════════════════════ */
typedef enum {
    NODE_MQTT_STATE_IDLE = 0,
    NODE_MQTT_STATE_WAIT_CONNECT_ACK,
    NODE_MQTT_STATE_WAIT_ATTR_ACK,
    NODE_MQTT_STATE_ACTIVE,
} node_mqtt_state_t;

typedef struct {
    uint8_t           mac[6];
    uint32_t          session_id;
    int               connect_msg_id;
    int               attr_msg_id;
    uint32_t          generation;
    node_mqtt_state_t state;
} node_mqtt_tracker_t;

typedef struct {
    uint32_t            mqtt_generation;
    bool                mqtt_connected;
    node_mqtt_tracker_t nodes[MAX_NODES_SUPPORTED];

    /* Diagnostics / Counters */
    uint32_t connect_published_count;
    uint32_t connect_ack_count;
    uint32_t attr_published_count;
    uint32_t attr_ack_count;
    uint32_t stale_ack_drops;
    uint32_t telemetry_blocked_count;
    uint32_t ttl_dropped_count;
} mqtt_lifecycle_mgr_t;

/* ══════════════════════════════════════════════════════════════
 *  MQTT Lifecycle APIs
 * ════════════════════════════════════════════════════════════ */
void mqtt_lifecycle_init(mqtt_lifecycle_mgr_t *mgr);
void mqtt_lifecycle_on_broker_connected(mqtt_lifecycle_mgr_t *mgr);
void mqtt_lifecycle_on_broker_disconnected(mqtt_lifecycle_mgr_t *mgr);

int mqtt_lifecycle_start_announce(mqtt_lifecycle_mgr_t *mgr,
                                  uint8_t node_id,
                                  const uint8_t *mac,
                                  uint32_t session_id,
                                  int pub_msg_id);

int mqtt_lifecycle_handle_puback(mqtt_lifecycle_mgr_t *mgr,
                                 int pub_msg_id,
                                 uint8_t *out_node_id,
                                 bool *out_ready_for_attributes);

int mqtt_lifecycle_set_attributes_msg_id(mqtt_lifecycle_mgr_t *mgr,
                                         uint8_t node_id,
                                         int pub_msg_id);

bool mqtt_lifecycle_is_node_active(const mqtt_lifecycle_mgr_t *mgr,
                                   uint8_t node_id,
                                   uint32_t session_id);

void mqtt_lifecycle_handle_node_disconnect(mqtt_lifecycle_mgr_t *mgr,
                                           uint8_t node_id);

bool mqtt_lifecycle_can_publish_telemetry(mqtt_lifecycle_mgr_t *mgr,
                                          uint8_t node_id,
                                          uint32_t session_id,
                                          uint64_t event_utc_ms,
                                          uint64_t current_utc_ms,
                                          uint32_t ttl_ms);

/* ══════════════════════════════════════════════════════════════
 *  Hardware MQTT Client APIs
 * ════════════════════════════════════════════════════════════ */
esp_err_t tb_mqtt_init(const char *broker_uri, const char *access_token);
bool tb_mqtt_is_connected(void);
esp_err_t tb_mqtt_wait_connected(TickType_t xTicksToWait);
int tb_mqtt_publish(const char *topic, const char *json_payload, int qos);

typedef void (*tb_mqtt_connect_cb_t)(void);
void tb_mqtt_register_connect_callback(tb_mqtt_connect_cb_t cb);

typedef void (*tb_mqtt_publish_cb_t)(int msg_id);
void tb_mqtt_register_publish_callback(tb_mqtt_publish_cb_t cb);

typedef void (*tb_mqtt_data_cb_t)(const char *topic, int topic_len, const char *data, int data_len);
void tb_mqtt_register_data_callback(tb_mqtt_data_cb_t cb);

void tb_mqtt_stop(void);
void tb_mqtt_pause(void);
void tb_mqtt_resume(void);

#ifdef __cplusplus
}
#endif