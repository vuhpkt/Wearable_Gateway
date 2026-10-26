#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

struct bt_conn {
    uint32_t id;
    int ref_count;
    bool disconnected;
    uint8_t disconnect_reason;
};

extern uint32_t g_host_bt_disconnect_count;
extern struct bt_conn *g_host_last_disconnected_conn;

static inline void bt_conn_unref(struct bt_conn *conn) {
    (void)conn;
}

static inline struct bt_conn *bt_conn_ref(struct bt_conn *conn) {
    return conn;
}

#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_HCI_ERR_CONN_TIMEOUT          0x08
#define BT_HCI_ERR_REMOTE_POWER_OFF      0x15
#define BT_HCI_ERR_LOCALHOST_TERM_CONN   0x16
#define BT_HCI_ERR_LL_RESP_TIMEOUT       0x22
#define BT_HCI_ERR_CONN_FAIL_TO_ESTAB    0x3E

static inline int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason) {
    if (!conn) return -EINVAL;
    conn->disconnected = true;
    conn->disconnect_reason = reason;
    g_host_bt_disconnect_count++;
    g_host_last_disconnected_conn = conn;
    return 0;
}

#define BT_CONN_CB_DEFINE(_name) \
    static const struct bt_conn_cb _name __attribute__((unused))

struct bt_conn_cb {
    void (*connected)(struct bt_conn *conn, uint8_t err);
    void (*disconnected)(struct bt_conn *conn, uint8_t reason);
};
