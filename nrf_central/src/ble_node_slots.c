/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef HOST_TEST
/* Zephyr System Headers */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ble_node_slots, LOG_LEVEL_INF);
#endif

/* Project Local Headers */
#include "ble_node_slots.h"

static ble_node_slot_t s_slots[BLE_NODE_MAX_SLOTS];
static uint32_t s_global_session_counter = 1;

static void convert_addr_to_mac_be(uint8_t *mac_be, const bt_addr_le_t *addr) {
    if (!mac_be) return;
    if (addr) {
        /* Convert Zephyr Little-Endian (addr->a.val[0]=LSB) to Canonical Big-Endian (mac_be[0]=MSB) */
        mac_be[0] = addr->a.val[5];
        mac_be[1] = addr->a.val[4];
        mac_be[2] = addr->a.val[3];
        mac_be[3] = addr->a.val[2];
        mac_be[4] = addr->a.val[1];
        mac_be[5] = addr->a.val[0];
    } else {
        memset(mac_be, 0, 6);
    }
}

static uint32_t allocate_next_session_id(void) {
    uint32_t session = s_global_session_counter++;
    if (session == 0) {
        session = s_global_session_counter++;
    }
    return session;
}

void ble_node_slots_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        s_slots[i].slot_id = (uint8_t)i;
        s_slots[i].is_used = false;
        s_slots[i].state = NODE_STATE_FREE;
        s_slots[i].conn = NULL;
        s_slots[i].session_id = 0;
        s_slots[i].seq = 0;
    }
    s_global_session_counter = 1;
}

ble_node_slot_t *ble_node_slot_alloc(const bt_addr_le_t *addr) {
    /* 1. Ưu tiên tái cấp phát cho MAC cũ đã từng gắn với slot này */
    if (addr) {
        for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
            if (!s_slots[i].is_used && bt_addr_le_cmp(&s_slots[i].addr, addr) == 0) {
                s_slots[i].slot_id = (uint8_t)i;
                s_slots[i].is_used = true;
                s_slots[i].state = NODE_STATE_CONNECTING;
                s_slots[i].session_id = allocate_next_session_id();
                s_slots[i].seq = 0;
                s_slots[i].rx_char_val_handle = 0;
                s_slots[i].write_in_progress = false;
                convert_addr_to_mac_be(s_slots[i].mac_be, addr);
                return &s_slots[i];
            }
        }
    }

    /* 2. Cấp phát slot rảnh mới */
    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (!s_slots[i].is_used) {
            memset(&s_slots[i], 0, sizeof(ble_node_slot_t));
            s_slots[i].slot_id = (uint8_t)i;
            s_slots[i].is_used = true;
            s_slots[i].state = NODE_STATE_CONNECTING;
            s_slots[i].session_id = allocate_next_session_id();
            s_slots[i].seq = 0;

            if (addr) {
                bt_addr_le_copy(&s_slots[i].addr, addr);
                convert_addr_to_mac_be(s_slots[i].mac_be, addr);
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
    slot->session_id = 0;
    slot->seq = 0;
    slot->rx_char_val_handle = 0;
    slot->write_in_progress = false;
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

ble_node_slot_t *ble_node_slot_find_by_session(uint32_t session_id) {
    if (session_id == 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < BLE_NODE_MAX_SLOTS; i++) {
        if (s_slots[i].is_used && s_slots[i].session_id == session_id) {
            return &s_slots[i];
        }
    }
    return NULL;
}

ble_node_slot_t *ble_node_slot_get(uint8_t slot_id) {
    if (slot_id >= BLE_NODE_MAX_SLOTS) {
        return NULL;
    }
    return &s_slots[slot_id];
}

uint32_t ble_node_slot_next_seq(ble_node_slot_t *slot) {
    if (!slot) return 0;
    slot->seq++;
    return slot->seq;
}

bool ble_node_slot_capture_context(ble_node_slot_t *slot, ble_conn_context_t *ctx_out, uint64_t mono_ms) {
    if (!slot || !ctx_out || !slot->is_used || slot->session_id == 0) {
        return false;
    }

    ctx_out->slot_id = slot->slot_id;
    ctx_out->session_id = slot->session_id;
    ctx_out->seq = ble_node_slot_next_seq(slot);
    memcpy(ctx_out->mac, slot->mac_be, 6);
    ctx_out->rx_mono_ms = mono_ms;
    return true;
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
