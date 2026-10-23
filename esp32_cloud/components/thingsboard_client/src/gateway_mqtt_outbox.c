#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "gateway_mqtt_outbox.h"
#include "sys/queue.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include "esp_log.h"
static const char *TAG = "gw_outbox";
#else
#ifndef ESP_LOGD
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#endif
#ifndef ESP_LOGE
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#endif
#endif

typedef struct outbox_item {
    char            buffer[GATEWAY_OUTBOX_ITEM_MAX_LEN];
    int             len;
    int             msg_id;
    int             msg_type;
    int             msg_qos;
    outbox_tick_t   tick;
    pending_state_t pending;
    bool            in_use;
    STAILQ_ENTRY(outbox_item) next;
} outbox_item_t;

STAILQ_HEAD(outbox_list_t, outbox_item);

struct outbox_t {
    uint64_t                size;
    struct outbox_list_t    list;
    outbox_item_t           pool[GATEWAY_OUTBOX_MAX_ITEMS];
    gateway_outbox_stats_t  stats;
    bool                    initialized;
};

static struct outbox_t s_static_outbox;

outbox_handle_t outbox_init(void) {
    memset(&s_static_outbox, 0, sizeof(s_static_outbox));
    STAILQ_INIT(&s_static_outbox.list);
    s_static_outbox.initialized = true;
    return &s_static_outbox;
}

outbox_item_handle_t outbox_enqueue(outbox_handle_t outbox, outbox_message_handle_t message, outbox_tick_t tick) {
    if (!outbox || !message) return NULL;
    int total_len = message->len + message->remaining_len;
    if (total_len > (int)GATEWAY_OUTBOX_ITEM_MAX_LEN) {
        outbox->stats.dropped_oversize++;
        ESP_LOGW(TAG, "Packet size %d exceeds outbox item limit %u, rejected", total_len, GATEWAY_OUTBOX_ITEM_MAX_LEN);
        return NULL;
    }

    /* Count available free slots */
    int free_slots = 0;
    int free_idx = -1;
    for (int i = 0; i < (int)GATEWAY_OUTBOX_MAX_ITEMS; i++) {
        if (!outbox->pool[i].in_use) {
            free_slots++;
            if (free_idx < 0) free_idx = i;
        }
    }

    if (free_slots == 0 || free_idx < 0) {
        outbox->stats.dropped_full++;
        static bool s_has_logged = false;
        static outbox_tick_t s_last_outbox_log_tick = 0;
        if (!s_has_logged || tick >= s_last_outbox_log_tick + 1000) {
            s_has_logged = true;
            s_last_outbox_log_tick = tick;
            ESP_LOGW(TAG, "Outbox pool exhausted (%u items), dropping packet msg_id=%d", GATEWAY_OUTBOX_MAX_ITEMS, message->msg_id);
        }
        return NULL;
    }

    outbox_item_t *item = &outbox->pool[free_idx];
    item->in_use = true;
    item->msg_id = message->msg_id;
    item->msg_type = message->msg_type;
    item->msg_qos = message->msg_qos;
    item->tick = tick;
    item->len = total_len;
    item->pending = QUEUED;

    memcpy(item->buffer, message->data, message->len);
    if (message->remaining_data && message->remaining_len > 0) {
        memcpy(item->buffer + message->len, message->remaining_data, message->remaining_len);
    }

    STAILQ_INSERT_TAIL(&outbox->list, item, next);
    outbox->size += item->len;
    outbox->stats.current_items++;
    outbox->stats.current_bytes = outbox->size;
    outbox->stats.total_enqueued++;

    ESP_LOGD(TAG, "ENQUEUE msgid=%d, len=%d, outbox_items=%u, size=%"PRIu64,
             message->msg_id, total_len, outbox->stats.current_items, outbox->size);
    return item;
}

outbox_item_handle_t outbox_get(outbox_handle_t outbox, int msg_id) {
    if (!outbox) return NULL;
    outbox_item_t *item;
    STAILQ_FOREACH(item, &outbox->list, next) {
        if (item->in_use && item->msg_id == msg_id) {
            return item;
        }
    }
    return NULL;
}

outbox_item_handle_t outbox_dequeue(outbox_handle_t outbox, pending_state_t pending, outbox_tick_t *tick) {
    if (!outbox) return NULL;
    outbox_item_t *item;
    STAILQ_FOREACH(item, &outbox->list, next) {
        if (item->in_use && item->pending == pending) {
            if (tick) *tick = item->tick;
            outbox->stats.total_dequeued++;
            return item;
        }
    }
    return NULL;
}

uint8_t *outbox_item_get_data(outbox_item_handle_t item, size_t *len, uint16_t *msg_id, int *msg_type, int *qos) {
    if (!item) return NULL;
    if (len) *len = item->len;
    if (msg_id) *msg_id = item->msg_id;
    if (msg_type) *msg_type = item->msg_type;
    if (qos) *qos = item->msg_qos;
    return (uint8_t *)item->buffer;
}

esp_err_t outbox_delete_item(outbox_handle_t outbox, outbox_item_handle_t item_to_delete) {
    if (!outbox || !item_to_delete) return ESP_FAIL;
    outbox_item_t *item;
    STAILQ_FOREACH(item, &outbox->list, next) {
        if (item == item_to_delete) {
            STAILQ_REMOVE(&outbox->list, item, outbox_item, next);
            outbox->size -= item->len;
            item->in_use = false;
            outbox->stats.current_items--;
            outbox->stats.current_bytes = outbox->size;
            outbox->stats.total_deleted++;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t outbox_delete(outbox_handle_t outbox, int msg_id, int msg_type) {
    if (!outbox) return ESP_FAIL;
    outbox_item_t *item, *tmp;
    STAILQ_FOREACH_SAFE(item, &outbox->list, next, tmp) {
        if (item->in_use && item->msg_id == msg_id && (0xFF & item->msg_type) == msg_type) {
            STAILQ_REMOVE(&outbox->list, item, outbox_item, next);
            outbox->size -= item->len;
            item->in_use = false;
            outbox->stats.current_items--;
            outbox->stats.current_bytes = outbox->size;
            outbox->stats.total_deleted++;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t outbox_set_pending(outbox_handle_t outbox, int msg_id, pending_state_t pending) {
    outbox_item_t *item = (outbox_item_t *)outbox_get(outbox, msg_id);
    if (item) {
        item->pending = pending;
        return ESP_OK;
    }
    return ESP_FAIL;
}

pending_state_t outbox_item_get_pending(outbox_item_handle_t item) {
    return item ? item->pending : QUEUED;
}

esp_err_t outbox_set_tick(outbox_handle_t outbox, int msg_id, outbox_tick_t tick) {
    outbox_item_t *item = (outbox_item_t *)outbox_get(outbox, msg_id);
    if (item) {
        item->tick = tick;
        return ESP_OK;
    }
    return ESP_FAIL;
}

int outbox_delete_single_expired(outbox_handle_t outbox, outbox_tick_t current_tick, outbox_tick_t timeout) {
    if (!outbox) return -1;
    outbox_item_t *item;
    STAILQ_FOREACH(item, &outbox->list, next) {
        if (item->in_use && (current_tick - item->tick > timeout)) {
            STAILQ_REMOVE(&outbox->list, item, outbox_item, next);
            outbox->size -= item->len;
            item->in_use = false;
            outbox->stats.current_items--;
            outbox->stats.current_bytes = outbox->size;
            outbox->stats.total_expired++;
            return item->msg_id;
        }
    }
    return -1;
}

int outbox_delete_expired(outbox_handle_t outbox, outbox_tick_t current_tick, outbox_tick_t timeout) {
    if (!outbox) return 0;
    int deleted = 0;
    outbox_item_t *item, *tmp;
    STAILQ_FOREACH_SAFE(item, &outbox->list, next, tmp) {
        if (item->in_use && (current_tick - item->tick > timeout)) {
            STAILQ_REMOVE(&outbox->list, item, outbox_item, next);
            outbox->size -= item->len;
            item->in_use = false;
            outbox->stats.current_items--;
            outbox->stats.current_bytes = outbox->size;
            outbox->stats.total_expired++;
            deleted++;
        }
    }
    return deleted;
}

uint64_t outbox_get_size(outbox_handle_t outbox) {
    return outbox ? outbox->size : 0;
}

void outbox_delete_all_items(outbox_handle_t outbox) {
    if (!outbox) return;
    outbox_item_t *item, *tmp;
    STAILQ_FOREACH_SAFE(item, &outbox->list, next, tmp) {
        STAILQ_REMOVE(&outbox->list, item, outbox_item, next);
        item->in_use = false;
    }
    outbox->size = 0;
    outbox->stats.current_items = 0;
    outbox->stats.current_bytes = 0;
}

void outbox_destroy(outbox_handle_t outbox) {
    if (!outbox) return;
    outbox_delete_all_items(outbox);
    outbox->initialized = false;
}

void gateway_outbox_get_stats(outbox_handle_t outbox, gateway_outbox_stats_t *out_stats) {
    if (!out_stats) return;
    if (outbox) {
        *out_stats = outbox->stats;
    } else {
        *out_stats = s_static_outbox.stats;
    }
}

void gateway_outbox_reset_stats(outbox_handle_t outbox) {
    if (!outbox) return;
    memset(&outbox->stats, 0, sizeof(outbox->stats));
    outbox->stats.current_items = (uint32_t)(outbox->size > 0 ? 1 : 0);
    outbox->stats.current_bytes = outbox->size;
}

bool gateway_outbox_validate_retransmit(const struct outbox_item *item,
                                        outbox_tick_t now_tick,
                                        outbox_tick_t max_age) {
    if (!item || !item->in_use) return false;
    if (now_tick < item->tick) return false;
    outbox_tick_t age = now_tick - item->tick;
    return (age < max_age);
}
