#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "mqtt_outbox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_OUTBOX_MAX_ITEMS        16U
#define GATEWAY_OUTBOX_ITEM_MAX_LEN     1536U
#define GATEWAY_OUTBOX_CONTROL_RESERVE  4U

typedef struct {
    uint32_t current_items;
    uint64_t current_bytes;
    uint32_t total_enqueued;
    uint32_t total_dequeued;
    uint32_t total_deleted;
    uint32_t total_expired;
    uint32_t dropped_full;
    uint32_t dropped_oversize;
    uint32_t dropped_control_reserve;
} gateway_outbox_stats_t;

/* Get synchronous statistics from static outbox pool */
void gateway_outbox_get_stats(outbox_handle_t outbox, gateway_outbox_stats_t *out_stats);

/* Reset pool statistics (for testing) */
void gateway_outbox_reset_stats(outbox_handle_t outbox);

/* Validate if an outbox item is still valid to retransmit (< max_age_ms) */
bool gateway_outbox_validate_retransmit(const struct outbox_item *item,
                                        outbox_tick_t now_tick,
                                        outbox_tick_t max_age);

#ifdef __cplusplus
}
#endif
