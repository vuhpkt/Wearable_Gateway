#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#include "wifi_config.h"
#include "wifi_sta_mgr.h"
#include "provisioning_fsm.h"

#ifndef HOST_UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#else
#ifndef QUEUE_HANDLE_T_DEFINED
#define QUEUE_HANDLE_T_DEFINED
typedef void* QueueHandle_t;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_mgr_ip_callback_t)(void);

/**
 * @brief Consolidated Wi-Fi & Provisioning setup (NVS, STA, AP, Portal, SNTP)
 * @param control_queue Queue to receive provisioning events
 * @param on_ip_cb Callback fired when IP is acquired
 * @param pause_mqtt_cb Callback fired when SoftAP opens to pause MQTT
 * @param sntp_cb Callback fired when SNTP sync completes
 */
esp_err_t wifi_mgr_setup(QueueHandle_t control_queue,
                         wifi_mgr_ip_callback_t on_ip_cb,
                         void (*pause_mqtt_cb)(void),
                         wifi_sta_sntp_cb_t sntp_cb);

/**
 * @brief Dispatch a provisioning event to the active Provisioning FSM
 * @param evt Pointer to event to dispatch
 */
void wifi_mgr_prov_dispatch(const prov_event_t *evt);

/**
 * @brief Check if Wi-Fi station is connected and has valid IP
 */
bool wifi_mgr_is_connected(void);

/**
 * @brief Check if SNTP real-time clock is synchronized
 */
bool wifi_mgr_is_time_synced(void);

#ifdef __cplusplus
}
#endif
