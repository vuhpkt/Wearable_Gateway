#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#include "wifi_config.h"
#include "wifi_sta_mgr.h"
#include "wifi_ap.h"
#include "wifi_portal.h"
#include "boot_button.h"
#include "nvs_storage.h"
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
 * @brief Initialize all Wi-Fi manager subsystems (NVS, Boot Button, Wi-Fi Drivers)
 * @param on_ip_cb Callback fired when IP is acquired (can resume MQTT client)
 */
esp_err_t wifi_mgr_init(wifi_mgr_ip_callback_t on_ip_cb);

/**
 * @brief Initialize provisioning subsystem and bind callbacks to control queue
 * @param control_queue FreeRTOS queue for prov_event_t
 * @param fsm Pointer to prov_fsm_t to initialize
 * @param pause_mqtt_cb Optional callback to pause MQTT during AP mode
 */
void wifi_mgr_prov_init(QueueHandle_t control_queue, prov_fsm_t *fsm, void (*pause_mqtt_cb)(void));

/**
 * @brief Dispatch a provisioning event to the active Provisioning FSM
 * @param evt Pointer to event to dispatch
 */
void wifi_mgr_prov_dispatch(const prov_event_t *evt);

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
 * @brief Start Wi-Fi station mode with stored or default credentials
 */
esp_err_t wifi_mgr_start(void);

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
