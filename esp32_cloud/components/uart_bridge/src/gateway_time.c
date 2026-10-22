#include <string.h>
#include "gateway_time.h"

void gateway_time_mailbox_init(gateway_time_mailbox_t *mb) {
    if (!mb) return;
    memset(mb, 0, sizeof(*mb));
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    mb->mux = init_mux;
#endif
}

void gateway_time_mailbox_post(gateway_time_mailbox_t *mb, int64_t utc_ms, uint64_t mono_ms) {
    if (!mb) return;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&mb->mux);
#endif
    mb->obs.sequence++;
    mb->obs.utc_ms = utc_ms;
    mb->obs.mono_ms = mono_ms;
    mb->has_pending = true;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&mb->mux);
#endif
}

bool gateway_time_mailbox_poll(gateway_time_mailbox_t *mb, gateway_time_obs_t *out_obs) {
    if (!mb || !out_obs) return false;
    bool had_update = false;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&mb->mux);
#endif
    if (mb->has_pending) {
        *out_obs = mb->obs;
        mb->has_pending = false;
        had_update = true;
    }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&mb->mux);
#endif
    return had_update;
}

void gateway_time_init(gateway_time_t *gt, uint64_t initial_epoch) {
    if (!gt) return;
    memset(gt, 0, sizeof(*gt));
    bridge_time_init(&gt->sync, initial_epoch);
}

void gateway_time_reset_anchor(gateway_time_t *gt) {
    if (!gt) return;
    bridge_time_reset(&gt->sync);
}

void gateway_time_set_epoch(gateway_time_t *gt, uint64_t new_epoch) {
    if (!gt) return;
    gt->sync.bridge_epoch = new_epoch;
    bridge_time_reset(&gt->sync);
}

void gateway_time_apply_obs(gateway_time_t *gt, const gateway_time_obs_t *obs) {
    if (!gt || !obs) return;
    int64_t offset_ms = obs->utc_ms - (int64_t)obs->mono_ms;
    bridge_time_set_sntp_offset(&gt->sync, offset_ms, obs->mono_ms);
    gt->last_applied_seq = obs->sequence;
}

void gateway_time_poll_and_apply_mailbox(gateway_time_t *gt, gateway_time_mailbox_t *mb) {
    if (!gt || !mb) return;
    gateway_time_obs_t obs;
    if (gateway_time_mailbox_poll(mb, &obs)) {
        gateway_time_apply_obs(gt, &obs);
    }
}

int gateway_time_map_tuple(gateway_time_t *gt,
                           uint64_t bridge_epoch,
                           uint64_t central_rx_mono_ms,
                           uint64_t esp_rx_mono_ms,
                           uint64_t *out_utc_ms,
                           uint8_t *out_quality,
                           uint16_t *out_uncertainty_ms) {
    if (!gt || !out_utc_ms || !out_quality) return -1;

    /* 1. Epoch check */
    if (bridge_epoch != gt->sync.bridge_epoch) {
        *out_utc_ms = 0;
        *out_quality = TIME_QUALITY_INVALID;
        if (out_uncertainty_ms) *out_uncertainty_ms = 0xFFFF;
        gt->invalid_epoch_drops++;
        return -2;
    }

    /* 2. SNTP sync check */
    if (!gt->sync.sntp_synced) {
        *out_utc_ms = 0;
        *out_quality = TIME_QUALITY_UNKNOWN;
        if (out_uncertainty_ms) *out_uncertainty_ms = 0xFFFF;
        gt->no_sntp_drops++;
        return -3;
    }

    /* 3. Re-anchor check:
     * Only re-anchor if not anchored yet or Central monotonic clock moved backwards
     */
    if (!gt->sync.clock_anchored || (central_rx_mono_ms < gt->sync.anchor_central_ms)) {
        bridge_time_anchor(&gt->sync, bridge_epoch, central_rx_mono_ms, esp_rx_mono_ms, 2);
    }

    /* 4. Compute UTC event timestamp */
    int rc = bridge_time_compute_event_ts(&gt->sync, bridge_epoch,
                                          central_rx_mono_ms, esp_rx_mono_ms,
                                          out_utc_ms, out_quality, out_uncertainty_ms);
    if (rc == -4) {
        gt->future_ts_drops++;
        return -4;
    } else if (rc == -5) {
        gt->uncertainty_drops++;
        return -5;
    } else if (rc != 0) {
        return rc;
    }

    gt->total_mapped++;
    return 0;
}
