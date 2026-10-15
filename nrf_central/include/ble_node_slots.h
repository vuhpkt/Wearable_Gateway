#ifndef BLE_NODE_SLOTS_H
#define BLE_NODE_SLOTS_H

/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>

/* Zephyr System Headers */
#include <zephyr/types.h>
#include <zephyr/bluetooth/bluetooth.h> 
#include <zephyr/bluetooth/conn.h>     
#include <zephyr/bluetooth/gatt.h>      

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
} ble_node_state_t ;

typedef struct {
    uint8_t slot_id;                   /* 0 .. 9 */
    bool is_used;
    ble_node_state_t state;
    bt_addr_le_t addr;                 /* MAC của Node */
    struct bt_conn *conn;              /* Con trỏ kết nối Zephyr */
    uint16_t tx_char_val_handle;      
    uint16_t cccd_handle;             
    struct bt_gatt_exchange_params mtu_params;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
} ble_node_slot_t;

void ble_node_slots_init(void);

  
ble_node_slot_t *ble_node_slot_alloc(const bt_addr_le_t *addr); 

  
void ble_node_slot_free(ble_node_slot_t *slot); 

  
ble_node_slot_t *ble_node_slot_find_by_addr(const bt_addr_le_t *addr);

  
ble_node_slot_t *ble_node_slot_find_by_conn(const struct bt_conn *conn);

  
uint8_t ble_node_slots_get_active_count(void);

  
uint8_t ble_node_slots_get_subscribed_count(void);

#endif