/*
 * Copyright (c) 2026 Wearable Gateway Project
 *
 * ble_central_mgr.c - BLE Central Manager Implementation
 * Encapsulates Active Scanning, Duty-Cycled Auto-Reconnect, Sequential Connection FSM,
 * ATT MTU Exchange, GATT Discovery (0x1400 / 0x1401 / 0x2902), and Multi-Node Notifications.
 */

/* Standard C Libraries */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Zephyr System Headers */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Project Local Headers */
#include "ble_central_mgr.h"
#include "ble_node_slots.h"
#include "ble_protocol_def.h"


LOG_MODULE_REGISTER(ble_central_mgr, LOG_LEVEL_INF);

/* ══════════════════════════════════════════════════════════════
 *  Duty-Cycled Scan Parameters (20% Duty Cycle for Safe Multi-Link)
 *  Interval = 160 * 0.625ms = 100ms
 *  Window   = 32  * 0.625ms = 20ms
 * ════════════════════════════════════════════════════════════ */
#define BLE_SCAN_PARAM_DUTY_CYCLED \
    BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_ACTIVE, BT_LE_SCAN_OPT_NONE, 160, 32)

/* ══════════════════════════════════════════════════════════════
 *  UUID Definitions for Wearable Device
 *  Base: 6E40xxxx-B5A3-F393-E0A9-E50E24DCCA9E
 * ════════════════════════════════════════════════════════════ */
static const struct bt_uuid_128 ecg_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e401400, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

static const struct bt_uuid_128 ecg_tx_char_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e401401, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

static const struct bt_uuid_16 cccd_uuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

/* ══════════════════════════════════════════════════════════════
 *  Module State & Persistent Parameters
 * ══════════════════════════════════════════════════════════════ */
#define BLE_HANDSHAKE_TIMEOUT_MS 5000

static const ble_central_callbacks_t *s_cbs = NULL;
static bool s_is_connecting = false;
static struct bt_conn *s_connecting_conn = NULL;

static struct k_timer s_handshake_timer;
static struct k_work s_handshake_timeout_work;

/* Forward declarations */
static void device_found_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad);
static void start_gatt_discovery(struct bt_conn *conn);
static void handshake_abort(struct bt_conn *conn);
static void subscribe_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params);
static void handshake_timeout_work_handler(struct k_work *work);
static void handshake_timer_expiry_fn(struct k_timer *timer);

/**
 * @brief Convert HCI disconnect reason codes to human-readable strings.
 */
static const char *hci_reason_to_str(uint8_t reason) {
    switch (reason) {
    case BT_HCI_ERR_CONN_TIMEOUT:
        return "Supervision Timeout (Signal lost or Power off)";
    case BT_HCI_ERR_REMOTE_USER_TERM_CONN:
        return "Remote User Terminated (Node powered off normally)";
    case BT_HCI_ERR_REMOTE_POWER_OFF:
        return "Remote Power Off";
    case BT_HCI_ERR_LOCALHOST_TERM_CONN:
        return "Local Host Terminated (Gateway disconnected)";
    case BT_HCI_ERR_LL_RESP_TIMEOUT:
        return "LL Response Timeout";
    case BT_HCI_ERR_CONN_FAIL_TO_ESTAB:
        return "Failed to Establish Connection";
    default:
        return "Other Disconnect Reason";
    }
}

/* ══════════════════════════════════════════════════════════════
 *  GATT Notification Callback
 * ══════════════════════════════════════════════════════════════ */
static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data,
                         uint16_t length) {
    ARG_UNUSED(params);
    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);
    if (!slot) {
        return BT_GATT_ITER_STOP;
    }

    if (data == NULL) {
        LOG_INF("[BLE] Node #%u Unsubscribed from TX Characteristic", slot->slot_id);
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    if (length == sizeof(vitals_packet_t)) {
        const vitals_packet_t *vitals = (const vitals_packet_t *)data;
        if (s_cbs && s_cbs->vitals_received) {
            s_cbs->vitals_received(slot->slot_id, vitals);
        }
    }

    if (length == ECG_DEFAULT_PAYLOAD_LEN || (length > sizeof(vitals_packet_t) && length % sizeof(int16_t) == 0)) {
        const int16_t *samples = (const int16_t *)data;
        uint16_t count = length / sizeof(int16_t);
        if (s_cbs && s_cbs->ecg_received) {
            s_cbs->ecg_received(slot->slot_id, samples, count);
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

/* ══════════════════════════════════════════════════════════════
 *  Handshake Lifetime & Recovery Helpers (H06, H07, M07)
 * ══════════════════════════════════════════════════════════════ */
static void handshake_abort(struct bt_conn *conn) {
    if (!conn) {
        return;
    }
    LOG_WRN("[BLE] Handshake failed or rejected; aborting connection %p", (void *)conn);
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void handshake_timeout_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (s_is_connecting && s_connecting_conn != NULL) {
        LOG_WRN("[BLE] Handshake timed out after %u ms. Aborting stuck connection...",
                BLE_HANDSHAKE_TIMEOUT_MS);
        handshake_abort(s_connecting_conn);
    }
}

static void handshake_timer_expiry_fn(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit(&s_handshake_timeout_work);
}

/**
 * @brief Completion callback for bt_gatt_subscribe (H07).
 * Confirms that remote CCC write was accepted by the peripheral.
 */
static void subscribe_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(params);
    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);
    if (!slot) {
        LOG_ERR("[GATT] Subscribe callback: slot not found for connection %p", (void *)conn);
        return;
    }

    if (err != 0) {
        LOG_ERR("[GATT] Node #%u CCC write rejected by peer (err %u)", slot->slot_id, err);
        handshake_abort(conn);
        return;
    }

    LOG_INF("[GATT] Node #%u CCC write confirmed! Subscribed successfully to TX notifications (0x1401)",
            slot->slot_id);
    slot->state = NODE_STATE_SUBSCRIBED;

    /* Handshake complete: cancel timer and release sequential connection lock */
    k_timer_stop(&s_handshake_timer);
    if (s_connecting_conn == conn) {
        s_is_connecting = false;
        s_connecting_conn = NULL;
    }

    /* If available slots remain, automatically resume scanning for next node */
    if (ble_node_slots_get_active_count() < BLE_NODE_MAX_SLOTS) {
        LOG_INF("[BLE] Resuming scan for additional nodes (%u/%u slots used)...",
                ble_node_slots_get_active_count(), BLE_NODE_MAX_SLOTS);
        ble_central_mgr_scan_start();
    }
}

/* ══════════════════════════════════════════════════════════════
 *  GATT Discovery Callback Hierarchy (3-Stage FSM)
 *  Stage 1: Primary Service (0x1400)
 *  Stage 2: TX Characteristic (0x1401)
 *  Stage 3: CCCD Descriptor (0x2902) -> Subscribe & Unlock Sequential FSM
 * ══════════════════════════════════════════════════════════════ */
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
    int err;
    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);
    if (!slot) {
        LOG_ERR("[GATT] Slot not found for active connection during discovery");
        return BT_GATT_ITER_STOP;
    }

    if (!attr) {
        LOG_DBG("[GATT] Node #%u Discovery stage ended (attr is NULL)", slot->slot_id);
        if (slot->state != NODE_STATE_SUBSCRIBED) {
            LOG_WRN("[GATT] Node #%u Discovery ended before subscription completed. Aborting...", slot->slot_id);
            handshake_abort(conn);
        }
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[GATT] Node #%u Discovered attr handle: 0x%04X", slot->slot_id, attr->handle);

    /* Stage 1: Matched Primary Service (0x1400) */
    if (bt_uuid_cmp(&ecg_service_uuid.uuid, params->uuid) == 0) {
        LOG_DBG("[GATT] Node #%u Found ECG Primary Service (0x1400) at handle 0x%04X", slot->slot_id, attr->handle);

        params->uuid = &ecg_tx_char_uuid.uuid;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

        err = bt_gatt_discover(conn, params);
        if (err != 0) {
            LOG_ERR("Node #%u: Failed to start TX characteristic discovery (err %d)", slot->slot_id, err);
            handshake_abort(conn);
        }
        return BT_GATT_ITER_STOP;
    }

    /* Stage 2: Matched TX Characteristic (0x1401) */
    if (bt_uuid_cmp(&ecg_tx_char_uuid.uuid, params->uuid) == 0) {
        slot->tx_char_val_handle = bt_gatt_attr_value_handle(attr);
        slot->subscribe_params.value_handle = slot->tx_char_val_handle;
        LOG_DBG("[GATT] Node #%u Found TX Char (0x1401)! Value handle: 0x%04X",
                slot->slot_id, slot->tx_char_val_handle);

        params->uuid = &cccd_uuid.uuid;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_DESCRIPTOR;

        err = bt_gatt_discover(conn, params);
        if (err != 0) {
            LOG_ERR("Node #%u: Failed to start CCCD discovery (err %d)", slot->slot_id, err);
            handshake_abort(conn);
        }
        return BT_GATT_ITER_STOP;
    }

    /* Stage 3: Matched CCCD Descriptor (0x2902) */
    if (bt_uuid_cmp(&cccd_uuid.uuid, params->uuid) == 0) {
        LOG_DBG("[GATT] Node #%u Found CCCD descriptor at handle 0x%04X", slot->slot_id, attr->handle);

        slot->cccd_handle = attr->handle;
        slot->subscribe_params.notify = notify_cb;
        slot->subscribe_params.subscribe = subscribe_cb; /* H07: completion callback */
        slot->subscribe_params.value = BT_GATT_CCC_NOTIFY;
        slot->subscribe_params.ccc_handle = attr->handle;
        slot->subscribe_params.value_handle = slot->tx_char_val_handle;

        err = bt_gatt_subscribe(conn, &slot->subscribe_params);
        if (err != 0 && err != -EALREADY) {
            LOG_ERR("Node #%u: Failed to subscribe to TX notifications (err %d)", slot->slot_id, err);
            handshake_abort(conn);
        } else if (err == -EALREADY) {
            LOG_INF("[GATT] Node #%u Already subscribed to TX notifications (0x1401)!", slot->slot_id);
            slot->state = NODE_STATE_SUBSCRIBED;
            k_timer_stop(&s_handshake_timer);
            if (s_connecting_conn == conn) {
                s_is_connecting = false;
                s_connecting_conn = NULL;
            }
            if (ble_node_slots_get_active_count() < BLE_NODE_MAX_SLOTS) {
                ble_central_mgr_scan_start();
            }
        } else {
            LOG_DBG("[GATT] Node #%u Subscribe request queued; waiting for CCC write confirmation...",
                    slot->slot_id);
        }
        return BT_GATT_ITER_STOP;
    }

    LOG_WRN("[GATT] Node #%u Unhandled attribute at handle 0x%04X", slot->slot_id, attr->handle);
    return BT_GATT_ITER_STOP;
}

static void start_gatt_discovery(struct bt_conn *conn) {
    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);
    if (!slot) {
        LOG_ERR("[GATT] Cannot start discovery: slot not found");
        return;
    }

    LOG_DBG("[GATT] Node #%u Starting GATT discovery for ECG Service (0x1400)...", slot->slot_id);

    memset(&slot->discover_params, 0, sizeof(slot->discover_params));
    slot->discover_params.uuid = &ecg_service_uuid.uuid;
    slot->discover_params.func = discover_func;
    slot->discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    slot->discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err != 0) {
        LOG_ERR("Node #%u: Failed to start GATT discovery (err %d)", slot->slot_id, err);
        handshake_abort(conn);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  ATT MTU Exchange Callback
 * ══════════════════════════════════════════════════════════════ */
static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params) {
    ARG_UNUSED(params);
    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);

    if (err != 0) {
        LOG_WRN("MTU exchange negotiation failed (err %u), continuing at default MTU", err);
    } else {
        uint16_t current_mtu = bt_gatt_get_mtu(conn);
        LOG_INF("[BLE] Node #%u MTU exchange successful! Effective MTU: %u bytes",
                slot ? slot->slot_id : 0xFF, current_mtu);
    }

    /* MTU settled, begin GATT Discovery */
    start_gatt_discovery(conn);
}

/* ══════════════════════════════════════════════════════════════
 *  Connection Lifecycle Callbacks
 * ══════════════════════════════════════════════════════════════ */
static void connected_cb(struct bt_conn *conn, uint8_t err) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);
    if (!slot) {
        slot = ble_node_slot_find_by_addr(bt_conn_get_dst(conn));
    }

    if (err != 0) {
        LOG_ERR("Failed to connect to %s (err %u)", addr_str, err);
        if (slot) {
            ble_node_slot_free(slot);
        }
        if (s_connecting_conn == conn) {
            k_timer_stop(&s_handshake_timer);
            s_connecting_conn = NULL;
            s_is_connecting = false;
        }
        ble_central_mgr_scan_start();
        return;
    }

    if (!slot) {
        LOG_ERR("No slot allocated for connected node [%s]", addr_str);
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (s_connecting_conn == conn) {
            k_timer_stop(&s_handshake_timer);
            s_connecting_conn = NULL;
            s_is_connecting = false;
        }
        ble_central_mgr_scan_start();
        return;
    }

    slot->state = NODE_STATE_CONNECTED;
    if (!slot->conn) {
        slot->conn = bt_conn_ref(conn);
    }

    LOG_INF("[BLE] Node #%u Connected successfully! MAC: %s", slot->slot_id, addr_str);

    if (s_cbs && s_cbs->connected) {
        s_cbs->connected(slot->slot_id, bt_conn_get_dst(conn));
    }

    /* Initiate ATT MTU exchange up to configured limit */
    slot->mtu_params.func = mtu_exchange_cb;
    int ret = bt_gatt_exchange_mtu(conn, &slot->mtu_params);
    if (ret != 0) {
        LOG_ERR("Node #%u: Failed to request MTU exchange (err %d)", slot->slot_id, ret);
        start_gatt_discovery(conn);
    } else {
        LOG_DBG("[BLE] Node #%u Requesting ATT MTU exchange...", slot->slot_id);
    }
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    ble_node_slot_t *slot = ble_node_slot_find_by_conn(conn);
    uint8_t slot_id = slot ? slot->slot_id : 0xFF;

    LOG_INF("[BLE] Node #%u Disconnected from [%s] | Reason: 0x%02X (%s)",
            slot_id, addr_str, reason, hci_reason_to_str(reason));

    if (slot) {
        ble_node_slot_free(slot);
    }

    if (s_cbs && s_cbs->disconnected) {
        s_cbs->disconnected(slot_id, bt_conn_get_dst(conn), reason);
    }

    /* M07: Reset connecting lock and timer ONLY if the disconnected connection was in handshake */
    if (s_connecting_conn == conn) {
        LOG_DBG("[BLE] Handshake connection terminated. Releasing lock and stopping timer.");
        k_timer_stop(&s_handshake_timer);
        s_connecting_conn = NULL;
        s_is_connecting = false;
        /* Safe to resume scanning now */
        ble_central_mgr_scan_start();
    } else {
        /* An already-subscribed node disconnected. If not currently handshaking, resume scan if slots open */
        if (!s_is_connecting && ble_node_slots_get_active_count() < BLE_NODE_MAX_SLOTS) {
            ble_central_mgr_scan_start();
        }
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
};

/* ══════════════════════════════════════════════════════════════
 *  Active BLE Scanning & Target Node Filtering
 * ══════════════════════════════════════════════════════════════ */
struct adv_parse_context {
    bool match_found;
    char dev_name[32];
};

static bool parse_cb(struct bt_data *data, void *user_data) {
    struct adv_parse_context *ctx = (struct adv_parse_context *)user_data;

    switch (data->type) {
    case BT_DATA_NAME_COMPLETE:
    case BT_DATA_NAME_SHORTENED: {
        const size_t prefix_len = sizeof(BLE_DEV_NAME_PREFIX) - 1;

        if (data->data_len >= prefix_len && memcmp(data->data, BLE_DEV_NAME_PREFIX, prefix_len) == 0) {
            size_t copy_len = MIN(data->data_len, sizeof(ctx->dev_name) - 1);
            memcpy(ctx->dev_name, data->data, copy_len);
            ctx->dev_name[copy_len] = '\0';

            ctx->match_found = true;
            return false; /* Stop parsing further fields */
        }
        break;
    }

    case BT_DATA_UUID128_ALL:
    case BT_DATA_UUID128_SOME: {
        if (data->data_len >= 16) {
            struct bt_uuid_128 u;
            u.uuid.type = BT_UUID_TYPE_128;
            memcpy(u.val, data->data, 16);

            if (bt_uuid_cmp(&u.uuid, &ecg_service_uuid.uuid) == 0) {
                ctx->match_found = true;
                return false; /* Stop parsing further fields */
            }
        }
        break;
    }

    default:
        break;
    }

    return true; /* Continue parsing next AD field */
}

static void device_found_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad) {
    struct adv_parse_context ctx = {.match_found = false, .dev_name = {0}};

    bt_data_parse(ad, parse_cb, &ctx);

    if (ctx.match_found) {
        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

        /* 1. If node is already connected or registered in a slot, ignore */
        if (ble_node_slot_find_by_addr(addr) != NULL) {
            return;
        }

        /* 2. If sequential connection FSM is busy connecting another node, ignore */
        if (s_is_connecting) {
            return;
        }

        LOG_INF("[BLE Scan] Target Node Detected! MAC: %s | RSSI: %d dBm | Name: %s | Type: 0x%02X",
                addr_str, rssi, ctx.dev_name[0] != '\0' ? ctx.dev_name : "(UUID Matched in SR)", type);

        /* 3. Allocate a free slot in the connection matrix (prioritizes previous MAC binding) */
        ble_node_slot_t *slot = ble_node_slot_alloc(addr);
        if (!slot) {
            LOG_WRN("All %d connection slots are full!", BLE_NODE_MAX_SLOTS);
            return;
        }

        /* 4. Lock sequential FSM and stop scanning */
        s_is_connecting = true;
        int err = bt_le_scan_stop();
        if (err != 0) {
            LOG_ERR("Failed to stop scan (err %d)", err);
            ble_node_slot_free(slot);
            s_is_connecting = false;
            return;
        }

        LOG_INF("[BLE] Initiating connection to Node #%u [%s]...", slot->slot_id, addr_str);

        err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &slot->conn);
        if (err != 0) {
            LOG_ERR("Connection initiation failed (err %d)", err);
            ble_node_slot_free(slot);
            s_is_connecting = false;
            s_connecting_conn = NULL;
            ble_central_mgr_scan_start();
        } else {
            /* Handshake lock acquired: track connection and arm 5-second deadline */
            s_connecting_conn = slot->conn;
            k_timer_start(&s_handshake_timer, K_MSEC(BLE_HANDSHAKE_TIMEOUT_MS), K_NO_WAIT);
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Public API Implementation
 * ══════════════════════════════════════════════════════════════ */
int ble_central_mgr_init(const ble_central_callbacks_t *cbs) {
    ble_node_slots_init();
    s_cbs = cbs;
    s_is_connecting = false;
    s_connecting_conn = NULL;

    k_timer_init(&s_handshake_timer, handshake_timer_expiry_fn, NULL);
    k_work_init(&s_handshake_timeout_work, handshake_timeout_work_handler);

    int err = bt_enable(NULL);
    if (err != 0) {
        LOG_ERR("Bluetooth stack initialization failed (err %d)", err);
        return err;
    }

    LOG_INF("Bluetooth Central stack initialized successfully!");

    return ble_central_mgr_scan_start();
}

bool ble_central_mgr_is_connected(void) {
    return (ble_node_slots_get_active_count() > 0);
}

uint8_t ble_central_mgr_get_connected_count(void) {
    return ble_node_slots_get_active_count();
}

uint8_t ble_central_mgr_get_subscribed_count(void) {
    return ble_node_slots_get_subscribed_count();
}

int ble_central_mgr_scan_start(void) {
    int err = bt_le_scan_start(BLE_SCAN_PARAM_DUTY_CYCLED, device_found_cb);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Failed to start BLE active scan (err %d)", err);
    } else {
        LOG_INF("BLE Active Scanning started (Duty Cycle 20%%)! Listening for '%s' nodes...", BLE_DEV_NAME_PREFIX);
    }
    return (err == -EALREADY) ? 0 : err;
}

int ble_central_mgr_scan_stop(void) {
    return bt_le_scan_stop();
}
