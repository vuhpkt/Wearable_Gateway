#pragma once

#include <stdint.h>
#include "uuid.h"
#include "conn.h"

#define BT_GATT_ITER_STOP 0
#define BT_GATT_ITER_CONTINUE 1
#define BT_GATT_DISCOVER_PRIMARY 1
#define BT_GATT_DISCOVER_CHARACTERISTIC 2
#define BT_GATT_DISCOVER_DESCRIPTOR 3
#define BT_GATT_CCC_NOTIFY 0x0001
#define BT_ATT_FIRST_ATTRIBUTE_HANDLE 0x0001
#define BT_ATT_LAST_ATTRIBUTE_HANDLE 0xffff

struct bt_gatt_attr {
    uint16_t handle;
};

static inline uint16_t bt_gatt_attr_value_handle(const struct bt_gatt_attr *attr) {
    return attr ? attr->handle : 0;
}

struct bt_gatt_exchange_params {
    void (*func)(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params);
};

struct bt_gatt_discover_params {
    const struct bt_uuid *uuid;
    uint8_t (*func)(struct bt_conn *conn, const struct bt_gatt_attr *attr, struct bt_gatt_discover_params *params);
    uint16_t start_handle;
    uint16_t end_handle;
    uint8_t type;
};

struct bt_gatt_subscribe_params {
    uint8_t (*notify)(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data, uint16_t length);
    void (*subscribe)(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params);
    uint16_t value_handle;
    uint16_t ccc_handle;
    uint16_t value;
};

struct bt_gatt_write_params;
typedef void (*bt_gatt_write_func_t)(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params);

struct bt_gatt_write_params {
    bt_gatt_write_func_t func;
    uint16_t handle;
    uint16_t offset;
    const void *data;
    uint16_t length;
};

static inline int bt_gatt_exchange_mtu(struct bt_conn *conn, struct bt_gatt_exchange_params *params) {
    (void)conn; (void)params; return 0;
}
static inline uint16_t bt_gatt_get_mtu(struct bt_conn *conn) { (void)conn; return 247; }
static inline int bt_gatt_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params) {
    (void)conn; (void)params; return 0;
}
static inline int bt_gatt_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    (void)conn; (void)params; return 0;
}

#if defined(__GNUC__)
__attribute__((weak)) int g_mock_bt_gatt_write_ret = 0;
__attribute__((weak)) uint32_t g_mock_bt_gatt_write_call_count = 0;
__attribute__((weak)) struct bt_gatt_write_params *g_mock_last_write_params = 0;
#else
extern int g_mock_bt_gatt_write_ret;
extern uint32_t g_mock_bt_gatt_write_call_count;
extern struct bt_gatt_write_params *g_mock_last_write_params;
#endif

static inline int bt_gatt_write(struct bt_conn *conn, struct bt_gatt_write_params *params) {
    (void)conn;
    g_mock_bt_gatt_write_call_count++;
    g_mock_last_write_params = params;
    return g_mock_bt_gatt_write_ret;
}
