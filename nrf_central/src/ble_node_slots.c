/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Zephyr System Headers */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Project Local Headers */
#include "ble_node_slots.h"


LOG_MODULE_REGISTER(ble_node_slots, LOG_LEVEL_INF);

static ble_node_slot_t s_slots[BLE_NODE_MAX_SLOTS];

void ble_node_slots_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        s_slots[i].slot_id = i;
        s_slots[i].is_used = false;
        s_slots[i].state = NODE_STATE_FREE;
        s_slots[i].conn = NULL;
    }
}

ble_node_slot_t *ble_node_slot_alloc(const bt_addr_le_t *addr) {
    /* 1. Ưu tiên tái cấp phát cho MAC cũ đã từng gắn với slot này */
    if (addr) {
        for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
            if (!s_slots[i].is_used && bt_addr_le_cmp(&s_slots[i].addr, addr) == 0) {
                s_slots[i].slot_id = i;
                s_slots[i].is_used = true;
                s_slots[i].state = NODE_STATE_CONNECTING;
                return &s_slots[i];
            }
        }
    }

    /* 2. Cấp phát slot rảnh mới */
    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (!s_slots[i].is_used) {
            memset(&s_slots[i], 0, sizeof(ble_node_slot_t));
            s_slots[i].slot_id = i;
            s_slots[i].is_used = true;
            s_slots[i].state = NODE_STATE_CONNECTING;

            if (addr) {
                bt_addr_le_copy(&s_slots[i].addr, addr);
            }
            return &s_slots[i];
        }
    }
    return NULL;
}

void ble_node_slot_free(ble_node_slot_t *slot) {
    if (!slot) {
        return;
    }

    if (slot->conn) {
        bt_conn_unref(slot->conn);
        slot->conn = NULL;
    }

    /* Giữ nguyên slot->addr để phục vụ cơ chế Static MAC Reconnect */
    slot->is_used = false;
    slot->state = NODE_STATE_FREE;
}

ble_node_slot_t *ble_node_slot_find_by_addr(const bt_addr_le_t *addr) {
    if (!addr) {
        return NULL;
    }

    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (s_slots[i].is_used && bt_addr_le_cmp(addr, &s_slots[i].addr) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

ble_node_slot_t *ble_node_slot_find_by_conn(const struct bt_conn *conn) {
    if (!conn) {
        return NULL;
    }

    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (s_slots[i].is_used && s_slots[i].conn == conn) {
            return &s_slots[i];
        }
    }
    return NULL;
}

uint8_t ble_node_slots_get_active_count(void) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (s_slots[i].is_used) {
            count++;
        }
    }
    return count;
}

uint8_t ble_node_slots_get_subscribed_count(void) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (s_slots[i].is_used && s_slots[i].state == NODE_STATE_SUBSCRIBED) {
            count++;
        }
    }
    return count;
}
