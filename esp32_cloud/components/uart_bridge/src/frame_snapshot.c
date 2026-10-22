#include <string.h>
#include "frame_snapshot.h"

void frame_snapshot_queue_init(frame_snapshot_queue_t *q) {
    if (!q) return;
    memset(q, 0, sizeof(*q));
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    q->mux = init_mux;
    q->sem_items = xSemaphoreCreateCountingStatic(
        FRAME_SNAPSHOT_QUEUE_CAPACITY + FRAME_SNAPSHOT_PRIORITY_CAPACITY,
        0,
        &q->sem_buffer);
#endif
}

bool frame_snapshot_queue_push(frame_snapshot_queue_t *q, const frame_snapshot_tuple_t *item) {
    if (!q || !item) return false;
    bool ok = false;

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&q->mux);
#endif

    if (q->count >= FRAME_SNAPSHOT_QUEUE_CAPACITY) {
        q->dropped_count++;
        ok = false; /* Non-blocking drop on queue full */
    } else {
        /* Immutable copy-by-value */
        q->items[q->tail] = *item;
        q->tail = (q->tail + 1U) % FRAME_SNAPSHOT_QUEUE_CAPACITY;
        q->count++;
        ok = true;
    }

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&q->mux);
    if (ok && q->sem_items) {
        xSemaphoreGive(q->sem_items);
    }
#endif

    return ok;
}

bool frame_snapshot_queue_push_priority(frame_snapshot_queue_t *q, const frame_snapshot_tuple_t *item) {
    if (!q || !item) return false;
    bool ok = false;

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&q->mux);
#endif

    if (q->prio_count >= FRAME_SNAPSHOT_PRIORITY_CAPACITY) {
        q->prio_dropped_count++;
        ok = false;
    } else {
        q->priority_items[q->prio_tail] = *item;
        q->prio_tail = (q->prio_tail + 1U) % FRAME_SNAPSHOT_PRIORITY_CAPACITY;
        q->prio_count++;
        ok = true;
    }

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&q->mux);
    if (ok && q->sem_items) {
        xSemaphoreGive(q->sem_items);
    }
#endif

    return ok;
}

bool frame_snapshot_queue_pop(frame_snapshot_queue_t *q, frame_snapshot_tuple_t *item_out, uint32_t timeout_ms) {
    if (!q) return false;

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    if (q->sem_items) {
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        if (xSemaphoreTake(q->sem_items, ticks) != pdTRUE) {
            return false;
        }
    }
    portENTER_CRITICAL(&q->mux);
#else
    (void)timeout_ms;
#endif

    bool found = false;
    /* Priority mailbox serviced first */
    if (q->prio_count > 0) {
        if (item_out) {
            *item_out = q->priority_items[q->prio_head];
        }
        q->prio_head = (q->prio_head + 1U) % FRAME_SNAPSHOT_PRIORITY_CAPACITY;
        q->prio_count--;
        found = true;
    } else if (q->count > 0) {
        if (item_out) {
            *item_out = q->items[q->head];
        }
        q->head = (q->head + 1U) % FRAME_SNAPSHOT_QUEUE_CAPACITY;
        q->count--;
        found = true;
    }

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&q->mux);
#endif

    return found;
}

uint32_t frame_snapshot_queue_count(const frame_snapshot_queue_t *q) {
    if (!q) return 0;
    return q->count + q->prio_count;
}

uint32_t frame_snapshot_queue_dropped(const frame_snapshot_queue_t *q) {
    if (!q) return 0;
    return q->dropped_count;
}

uint32_t frame_snapshot_queue_prio_dropped(const frame_snapshot_queue_t *q) {
    if (!q) return 0;
    return q->prio_dropped_count;
}

void frame_snapshot_queue_get_stats(const frame_snapshot_queue_t *q, frame_snapshot_queue_stats_t *out_stats) {
    if (!q || !out_stats) return;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL((portMUX_TYPE *)&q->mux);
#endif
    out_stats->count = q->count;
    out_stats->dropped_count = q->dropped_count;
    out_stats->prio_count = q->prio_count;
    out_stats->prio_dropped_count = q->prio_dropped_count;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL((portMUX_TYPE *)&q->mux);
#endif
}

void revocation_table_init(revocation_table_t *tbl) {
    if (!tbl) return;
    memset(tbl, 0, sizeof(*tbl));
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    tbl->rev_mux = init_mux;
#endif
}

void revocation_table_reset_epoch(revocation_table_t *tbl, uint64_t new_epoch) {
    if (!tbl) return;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&tbl->rev_mux);
#endif
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        tbl->active_epoch[i] = new_epoch;
        tbl->active_session[i] = 0;
    }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&tbl->rev_mux);
#endif
}

void revocation_table_set_active_session(revocation_table_t *tbl, uint8_t node_id, uint64_t bridge_epoch, uint32_t session_id) {
    if (!tbl || node_id >= MAX_NODES_SUPPORTED) return;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&tbl->rev_mux);
#endif
    tbl->active_epoch[node_id] = bridge_epoch;
    tbl->active_session[node_id] = session_id;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&tbl->rev_mux);
#endif
}

bool revocation_table_revoke_slot(revocation_table_t *tbl, uint8_t node_id, uint64_t bridge_epoch, uint32_t session_id) {
    if (!tbl || node_id >= MAX_NODES_SUPPORTED) return false;
    bool revoked = false;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&tbl->rev_mux);
#endif
    if ((bridge_epoch == 0 || tbl->active_epoch[node_id] == bridge_epoch) &&
        (session_id == 0 || tbl->active_session[node_id] == session_id)) {
        tbl->active_epoch[node_id] = 0;
        tbl->active_session[node_id] = 0;
        revoked = true;
    }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&tbl->rev_mux);
#endif
    return revoked;
}

bool revocation_table_is_session_valid(const revocation_table_t *tbl, uint8_t node_id, uint64_t bridge_epoch, uint32_t session_id) {
    if (!tbl || node_id >= MAX_NODES_SUPPORTED || session_id == 0) return false;
    bool valid = false;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL((portMUX_TYPE *)&tbl->rev_mux);
#endif
    if (bridge_epoch != 0 && tbl->active_epoch[node_id] != bridge_epoch) {
        valid = false;
    } else {
        valid = (tbl->active_session[node_id] == session_id);
    }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL((portMUX_TYPE *)&tbl->rev_mux);
#endif
    return valid;
}
