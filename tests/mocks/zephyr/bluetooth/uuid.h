#pragma once

#include <stdint.h>
#include <string.h>

#define BT_UUID_TYPE_128 1
#define BT_UUID_TYPE_16  2

struct bt_uuid {
    uint8_t type;
};

struct bt_uuid_16 {
    struct bt_uuid uuid;
    uint16_t val;
};

struct bt_uuid_128 {
    struct bt_uuid uuid;
    uint8_t val[16];
};

#define BT_UUID_INIT_16(value) { .uuid = { .type = BT_UUID_TYPE_16 }, .val = (value) }
#define BT_UUID_128_ENCODE(a, b, c, d, e) { 0 }
#define BT_UUID_INIT_128(...) { .uuid = { .type = BT_UUID_TYPE_128 }, .val = {0} }
#define BT_UUID_GATT_CCC_VAL 0x2902

static inline int bt_uuid_cmp(const struct bt_uuid *u1, const struct bt_uuid *u2) {
    if (!u1 || !u2) return -1;
    if (u1->type != u2->type) return (int)u1->type - (int)u2->type;
    return 0;
}
