/*
 * Copyright (c) 2026 Wearable Gateway Project
 *
 * ble_central_mgr.h - BLE Central Manager for Wearable Gateway
 * Handles Active Scanning, Target Node Matching, Connection Lifecycle,
 * ATT MTU Negotiation, GATT Discovery, and Characteristic Subscription.
 */

#ifndef BLE_CENTRAL_MGR_H
#define BLE_CENTRAL_MGR_H

/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>

/* Zephyr System Headers */
#include <zephyr/types.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

/* Project Local Headers */
#include "ble_node_slots.h"
#include "ble_protocol_def.h"

/**
 * @brief Application event callbacks registered with BLE Central Manager.
 */
typedef struct {
    void (*connected)(const ble_conn_context_t *ctx, const bt_addr_le_t *addr);
    void (*disconnected)(const ble_conn_context_t *ctx, const bt_addr_le_t *addr, uint8_t reason);
    void (*vitals_received)(const ble_conn_context_t *ctx, const vitals_packet_t *vitals);
    void (*ecg_received)(const ble_conn_context_t *ctx, const int16_t *samples, uint16_t count);
    void (*raw_received)(const ble_conn_context_t *ctx, const uint8_t *data, uint16_t len);
} ble_central_callbacks_t;

/**
 * @brief Initialize the Bluetooth stack and start BLE Central manager.
 *
 * @param cbs Pointer to callback structure (must have static/persistent lifetime).
 * @return 0 on success, or negative error code on failure.
 */
int ble_central_mgr_init(const ble_central_callbacks_t *cbs);

/**
 * @brief Check if a peripheral node is currently connected.
 *
 * @return true if connected, false otherwise.
 */
bool ble_central_mgr_is_connected(void);

/**
 * @brief Start active BLE scanning for target nodes.
 *
 * @return 0 on success, or negative error code on failure.
 */
int ble_central_mgr_scan_start(void);

/**
 * @brief Stop active BLE scanning.
 *
 * @return 0 on success, or negative error code on failure.
 */
int ble_central_mgr_scan_stop(void);

uint8_t ble_central_mgr_get_connected_count(void);

uint8_t ble_central_mgr_get_subscribed_count(void);

/**
 * @brief Send a downlink command payload to a node via BLE GATT Write Request (0x1402).
 *
 * @param slot_id Slot index (0 .. BLE_NODE_MAX_SLOTS - 1)
 * @param data    Command payload bytes (e.g. CMD_MODE_CFG, CMD_THR, etc.)
 * @param len     Length of command payload in bytes
 * @return 0 on success, -EINVAL on invalid args, -ENOTCONN if not connected/discovered,
 *         -EBUSY if a write is already in progress for this slot.
 */
int ble_central_mgr_write_cmd(uint8_t slot_id, const uint8_t *data, uint16_t len);

#endif /* BLE_CENTRAL_MGR_H */
