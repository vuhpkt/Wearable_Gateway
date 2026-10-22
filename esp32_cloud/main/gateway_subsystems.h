#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef HOST_UNIT_TEST
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#else
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#endif

#include "wifi_manager.h"
#include "uart_bridge.h"
#include "thingsboard_client.h"
#include "cloud_uplink.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Consolidated Gateway Subsystems Initialization
 *
 * Enforces mandatory dependency initialization order:
 *   1. uart_bridge_subsystem_init(): Initializes Spinlocks, Mailbox, Time Engine,
 *      Snapshot Queue, and Revocation Table (ready before any SNTP event can fire).
 *   2. cloud_uplink_default_init(): Initializes MQTT Lifecycle, Announce, and Buffers
 *      (ready before any Wi-Fi IP is acquired).
 *   3. wifi_mgr_setup(): Initializes NVS, Provisioning FSM, Wi-Fi STA/AP, and hooks
 *      SNTP callback directly to forward UTC time to Core 1 via uart_bridge_post_sntp.
 *
 * @param control_queue Queue to receive provisioning events (from boot button / portal timeout)
 * @return ESP_OK on success, or error code on failure
 */
esp_err_t gateway_subsystems_init(QueueHandle_t control_queue);

#ifdef __cplusplus
}
#endif
