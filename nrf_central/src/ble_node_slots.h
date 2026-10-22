#ifndef BLE_NODE_SLOTS_H
#define BLE_NODE_SLOTS_H

/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Zephyr System Headers */
#include <zephyr/types.h>
#include <zephyr/bluetooth/bluetooth.h> 
#include <zephyr/bluetooth/conn.h>     
#include <zephyr/bluetooth/gatt.h>      

#include "uart_frame_def.h"

#ifndef BLE_NODE_MAX_SLOTS
#if defined(CONFIG_BT_MAX_CONN)
#define BLE_NODE_MAX_SLOTS CONFIG_BT_MAX_CONN
#else
#define BLE_NODE_MAX_SLOTS 10U
#endif
#endif

typedef enum {
    NODE_STATE_FREE = 0,
    NODE_STATE_CONNECTING,
    NODE_STATE_CONNECTED,
    NODE_STATE_DISCOVERING,
    NODE_STATE_SUBSCRIBED
} ble_node_state_t;

typedef struct {
    uint8_t slot_id;                   /* 0 .. 9 */
    bool is_used;
    ble_node_state_t state;
    bt_addr_le_t addr;                 /* Native BLE Address (Little-Endian in addr.a.val) */
    uint8_t mac_be[6];                 /* Big-Endian display format matching GC9A01 LCD */
    uint32_t session_id;               /* Unique session id for this connection */
    uint32_t seq;                      /* Monotonically increasing sequence number */
    struct bt_conn *conn;              /* Zephyr connection pointer */
    uint16_t tx_char_val_handle;      
    uint16_t cccd_handle;             
    uint16_t rx_char_val_handle;       /* RX Char 0x1402 value handle for downlink write */
    bool write_in_progress;            /* In-flight bt_gatt_write lock */
    struct bt_gatt_write_params write_params; /* Persistent Zephyr write parameters struct */
    uint8_t write_buf[UART_MAX_PAYLOAD_LEN];  /* Static payload buffer for write */
    struct bt_gatt_exchange_params mtu_params;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
} ble_node_slot_t;

/* Context captured in callbacks */
typedef struct {
    uint8_t  slot_id;                  /* 0 .. 9 */
    uint32_t session_id;               /* Non-zero session id */
    uint32_t seq;                      /* Sequence number for this sample */
    uint8_t  mac[6];                   /* Canonical Big-Endian MAC */
    uint64_t rx_mono_ms;               /* Monotonic timestamp */
} ble_conn_context_t;

void ble_node_slots_init(void);

ble_node_slot_t *ble_node_slot_alloc(const bt_addr_le_t *addr);

void ble_node_slot_free(ble_node_slot_t *slot);

ble_node_slot_t *ble_node_slot_find_by_addr(const bt_addr_le_t *addr);

ble_node_slot_t *ble_node_slot_find_by_conn(const struct bt_conn *conn);

ble_node_slot_t *ble_node_slot_find_by_session(uint32_t session_id);

ble_node_slot_t *ble_node_slot_get(uint8_t slot_id);

uint32_t ble_node_slot_next_seq(ble_node_slot_t *slot);

bool ble_node_slot_capture_context(ble_node_slot_t *slot, ble_conn_context_t *ctx_out, uint64_t mono_ms);

uint8_t ble_node_slots_get_active_count(void);

uint8_t ble_node_slots_get_subscribed_count(void);

#endif /* BLE_NODE_SLOTS_H */