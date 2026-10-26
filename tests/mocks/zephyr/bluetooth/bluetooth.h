#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define BT_ADDR_LE_STR_LEN 30

typedef struct {
    uint8_t val[6];
} bt_addr_t;

typedef struct {
    uint8_t type;
    bt_addr_t a;
} bt_addr_le_t;

static inline int bt_addr_le_cmp(const bt_addr_le_t *a1, const bt_addr_le_t *a2) {
    return memcmp(a1, a2, sizeof(bt_addr_le_t));
}

static inline void bt_addr_le_copy(bt_addr_le_t *dst, const bt_addr_le_t *src) {
    memcpy(dst, src, sizeof(bt_addr_le_t));
}

static inline void bt_addr_le_to_str(const bt_addr_le_t *addr, char *str, size_t len) {
    if (addr && str && len > 0) {
        snprintf(str, len, "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr->a.val[0], addr->a.val[1], addr->a.val[2],
                 addr->a.val[3], addr->a.val[4], addr->a.val[5]);
    }
}

#define BT_LE_SCAN_TYPE_ACTIVE 1
#define BT_LE_SCAN_OPT_NONE 0
#define BT_LE_SCAN_PARAM(t, o, i, w) 0

#define BT_DATA_NAME_COMPLETE 0x09
#define BT_DATA_NAME_SHORTENED 0x08
#define BT_DATA_UUID128_ALL 0x07
#define BT_DATA_UUID128_SOME 0x06

struct net_buf_simple {
    uint8_t *data;
    uint16_t len;
};

struct bt_data {
    uint8_t type;
    uint8_t data_len;
    const uint8_t *data;
};

static inline void bt_data_parse(struct net_buf_simple *ad,
                                 bool (*func)(struct bt_data *data, void *user_data),
                                 void *user_data) {
    (void)ad; (void)func; (void)user_data;
}

#define BT_CONN_LE_CREATE_CONN 0
#define BT_LE_CONN_PARAM_DEFAULT 0

struct bt_conn;
static inline int bt_enable(void *cb) { (void)cb; return 0; }
static inline int bt_le_scan_start(int param, void *cb) { (void)param; (void)cb; return 0; }
static inline int bt_le_scan_stop(void) { return 0; }
static inline int bt_conn_le_create(const bt_addr_le_t *peer, void *param, void *conn_param, struct bt_conn **conn) {
    (void)peer; (void)param; (void)conn_param; (void)conn;
    return 0;
}
static inline const bt_addr_le_t *bt_conn_get_dst(const struct bt_conn *conn) {
    static bt_addr_le_t dummy_addr = {0};
    (void)conn;
    return &dummy_addr;
}
