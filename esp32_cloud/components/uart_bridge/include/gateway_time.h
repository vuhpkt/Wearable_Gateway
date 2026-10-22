#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "bridge_time.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════
 *  Time Observation Mailbox (Core 0 -> Core 1)
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    int64_t  utc_ms;
    uint64_t mono_ms;
    uint32_t sequence;
} gateway_time_obs_t;

typedef struct {
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_TYPE mux;
#endif
    gateway_time_obs_t obs;
    bool has_pending;
} gateway_time_mailbox_t;

/* ══════════════════════════════════════════════════════════════
 *  Gateway Time State (Owned exclusively by Core 1)
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    bridge_time_sync_t sync;
    uint32_t           last_applied_seq;

    /* Metrics & Drop Accounting */
    uint32_t           total_mapped;
    uint32_t           invalid_epoch_drops;
    uint32_t           no_sntp_drops;
    uint32_t           future_ts_drops;
    uint32_t           uncertainty_drops;
} gateway_time_t;

/* Mailbox APIs */
void gateway_time_mailbox_init(gateway_time_mailbox_t *mb);
void gateway_time_mailbox_post(gateway_time_mailbox_t *mb, int64_t utc_ms, uint64_t mono_ms);
bool gateway_time_mailbox_poll(gateway_time_mailbox_t *mb, gateway_time_obs_t *out_obs);

/* Gateway Time Engine APIs (Core 1) */
void gateway_time_init(gateway_time_t *gt, uint64_t initial_epoch);
void gateway_time_reset_anchor(gateway_time_t *gt);
void gateway_time_set_epoch(gateway_time_t *gt, uint64_t new_epoch);
void gateway_time_apply_obs(gateway_time_t *gt, const gateway_time_obs_t *obs);
void gateway_time_poll_and_apply_mailbox(gateway_time_t *gt, gateway_time_mailbox_t *mb);

/**
 * @brief Map a received frame to UTC timestamp and calculate uncertainty.
 *        Enforces deterministic re-anchor rules:
 *        - Re-anchor ONLY when unanchored or when Central monotonic clock moves backward.
 *        - Uncertainty > 250ms or future timestamp returns INVALID without forcing re-anchor.
 *        - SNTP not synced returns UNKNOWN without forcing re-anchor.
 */
int gateway_time_map_tuple(gateway_time_t *gt,
                           uint64_t bridge_epoch,
                           uint64_t central_rx_mono_ms,
                           uint64_t esp_rx_mono_ms,
                           uint64_t *out_utc_ms,
                           uint8_t *out_quality,
                           uint16_t *out_uncertainty_ms);

#ifdef __cplusplus
}
#endif
