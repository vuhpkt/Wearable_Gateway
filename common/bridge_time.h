#pragma once

/* Standard C Libraries */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════
 *  Time Quality & Basis Definitions
 * ════════════════════════════════════════════════════════════ */
typedef enum {
    TIME_QUALITY_INVALID   = 0,  /* Uncertainty > budget, future timestamp, or epoch mismatch */
    TIME_QUALITY_UNKNOWN   = 1,  /* No SNTP or mapping not established */
    TIME_QUALITY_ESTIMATED = 2,  /* Monotonic delta mapped without high-confidence RTT */
    TIME_QUALITY_SYNCED    = 3,  /* High-confidence SNTP sync + monotonic delta */
} time_quality_t;

typedef enum {
    TIME_BASIS_CENTRAL_RX = 0,  /* Monotonic timestamp recorded at Central BLE notification */
    TIME_BASIS_ESP_RX     = 1,  /* Monotonic timestamp recorded at ESP Core 1 UART RX */
} time_basis_t;

/* Contract Constants */
#define BRIDGE_TIME_MAX_UNCERTAINTY_MS   250U   /* Maximum allowed uncertainty for live path (250ms) */
#define BRIDGE_TIME_LIVE_TTL_MS          5000U  /* 5-second live freshness window */
#define BRIDGE_TIME_MAX_FUTURE_SLACK_MS  100U   /* Allow up to 100ms clock jitter before invalidating */
#define ECG_SAMPLE_PERIOD_MS             4U     /* 250 Hz ECG sampling = 4 ms period */

/* ══════════════════════════════════════════════════════════════
 *  Bridge Time Synchronization State
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    bool     clock_anchored;          /* True when Central-to-ESP monotonic anchor is set */
    uint64_t anchor_central_ms;       /* Central monotonic timestamp at anchor */
    uint64_t anchor_esp_mono_ms;      /* ESP32 monotonic timestamp at anchor */
    uint64_t bridge_epoch;            /* Active bridge epoch */

    bool     sntp_synced;             /* True when ESP32 SNTP has synchronized */
    int64_t  sntp_offset_ms;          /* (esp_utc_ms - esp_mono_ms) */
    uint64_t last_sntp_sync_mono_ms;  /* ESP mono ms when SNTP was updated */

    uint16_t estimated_rtt_ms;        /* Round-trip time estimate / uncertainty in ms */

    /* Diagnostics / Counters */
    uint32_t sync_count;
    uint32_t invalid_time_drops;
    uint32_t clock_jump_resets;
    uint32_t stale_backlog_drops;
} bridge_time_sync_t;

/* ══════════════════════════════════════════════════════════════
 *  Lifecycle & Synchronization APIs
 * ════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize bridge time synchronization structure.
 */
static inline void bridge_time_init(bridge_time_sync_t *ts, uint64_t epoch) {
    if (!ts) return;
    ts->clock_anchored = false;
    ts->anchor_central_ms = 0;
    ts->anchor_esp_mono_ms = 0;
    ts->bridge_epoch = epoch;
    ts->sntp_synced = false;
    ts->sntp_offset_ms = 0;
    ts->last_sntp_sync_mono_ms = 0;
    ts->estimated_rtt_ms = 1; /* Default 1ms wire delay estimate */
    ts->sync_count = 0;
    ts->invalid_time_drops = 0;
    ts->clock_jump_resets = 0;
    ts->stale_backlog_drops = 0;
}

/**
 * @brief Reset bridge time anchor (e.g. upon link reconnect or epoch change).
 */
static inline void bridge_time_reset(bridge_time_sync_t *ts) {
    if (!ts) return;
    ts->clock_anchored = false;
    ts->anchor_central_ms = 0;
    ts->anchor_esp_mono_ms = 0;
}

/**
 * @brief Notify that a clock step/jump occurred (SNTP step > 1000ms).
 *        Invalidates monotonic anchor to prevent stale mapping.
 */
static inline void bridge_time_notify_clock_jump(bridge_time_sync_t *ts) {
    if (!ts) return;
    ts->clock_anchored = false;
    ts->clock_jump_resets++;
}

/**
 * @brief Update ESP32 SNTP real-time offset from UTC.
 *
 * @param ts Pointer to sync state
 * @param offset_ms (esp_utc_ms - esp_mono_ms)
 * @param current_esp_mono_ms Current ESP32 monotonic ms
 */
static inline void bridge_time_set_sntp_offset(bridge_time_sync_t *ts,
                                              int64_t offset_ms,
                                              uint64_t current_esp_mono_ms) {
    if (!ts) return;
    if (ts->sntp_synced) {
        int64_t diff = offset_ms - ts->sntp_offset_ms;
        if (diff > 1000 || diff < -1000) {
            /* Clock stepped by more than 1 second */
            bridge_time_notify_clock_jump(ts);
        }
    }
    ts->sntp_offset_ms = offset_ms;
    ts->sntp_synced = true;
    ts->last_sntp_sync_mono_ms = current_esp_mono_ms;
}

/**
 * @brief Establish or update monotonic clock anchor between Central and ESP32.
 *
 * @param ts Pointer to sync state
 * @param epoch Active bridge epoch
 * @param central_mono_ms Central monotonic timestamp
 * @param esp_mono_ms ESP32 monotonic timestamp
 * @param rtt_ms Measured round-trip time in ms
 * @return 0 on success, negative error code on failure
 */
static inline int bridge_time_anchor(bridge_time_sync_t *ts,
                                     uint64_t epoch,
                                     uint64_t central_mono_ms,
                                     uint64_t esp_mono_ms,
                                     uint16_t rtt_ms) {
    if (!ts) return -1;
    if (epoch != ts->bridge_epoch) {
        ts->bridge_epoch = epoch;
        ts->clock_anchored = false;
    }
    ts->anchor_central_ms = central_mono_ms;
    ts->anchor_esp_mono_ms = esp_mono_ms;
    ts->estimated_rtt_ms = (rtt_ms > 0) ? rtt_ms : 1;
    ts->clock_anchored = true;
    ts->sync_count++;
    return 0;
}

/**
 * @brief Compute deterministic UTC event timestamp and quality from Central RX monotonic time.
 *
 * @param ts Pointer to sync state
 * @param epoch Frame envelope bridge epoch
 * @param central_rx_mono_ms Envelope central_rx_mono_ms
 * @param esp_rx_mono_ms Monotonic timestamp recorded at ESP Core 1 frame arrival
 * @param out_utc_ms Pointer to output UTC timestamp in milliseconds
 * @param out_quality Pointer to output time quality enum
 * @param out_uncertainty_ms Pointer to output uncertainty in milliseconds
 * @return 0 on success, negative error code on failure/invalid
 */
static inline int bridge_time_compute_event_ts(const bridge_time_sync_t *ts,
                                               uint64_t epoch,
                                               uint64_t central_rx_mono_ms,
                                               uint64_t esp_rx_mono_ms,
                                               uint64_t *out_utc_ms,
                                               uint8_t *out_quality,
                                               uint16_t *out_uncertainty_ms) {
    if (!ts || !out_utc_ms || !out_quality) {
        return -1;
    }

    /* Epoch mismatch rejects time mapping immediately */
    if (epoch != ts->bridge_epoch) {
        *out_utc_ms = 0;
        *out_quality = TIME_QUALITY_INVALID;
        if (out_uncertainty_ms) *out_uncertainty_ms = 0xFFFF;
        return -2;
    }

    /* If SNTP is not synced, UTC is unknown and must not produce LIVE */
    if (!ts->sntp_synced) {
        *out_utc_ms = 0;
        *out_quality = TIME_QUALITY_UNKNOWN;
        if (out_uncertainty_ms) *out_uncertainty_ms = 0xFFFF;
        return -3;
    }

    /* If clock anchor is not established, fallback to ESP arrival time as ESTIMATED */
    if (!ts->clock_anchored) {
        *out_utc_ms = (uint64_t)((int64_t)esp_rx_mono_ms + ts->sntp_offset_ms);
        *out_quality = TIME_QUALITY_ESTIMATED;
        if (out_uncertainty_ms) *out_uncertainty_ms = ts->estimated_rtt_ms + 10;
        return 0;
    }

    /* Delta since anchor on Central monotonic clock */
    int64_t central_delta_ms = (int64_t)central_rx_mono_ms - (int64_t)ts->anchor_central_ms;
    int64_t event_esp_mono_ms = (int64_t)ts->anchor_esp_mono_ms + central_delta_ms;

    /* Future timestamp guard: event cannot happen in the future beyond slack budget */
    if (event_esp_mono_ms > (int64_t)esp_rx_mono_ms + (int64_t)BRIDGE_TIME_MAX_FUTURE_SLACK_MS) {
        *out_utc_ms = 0;
        *out_quality = TIME_QUALITY_INVALID;
        if (out_uncertainty_ms) *out_uncertainty_ms = 0xFFFF;
        return -4;
    }

    /* Calculate wire transit latency */
    int64_t transit_ms = (int64_t)esp_rx_mono_ms - event_esp_mono_ms;
    if (transit_ms < 0) {
        transit_ms = 0;
    }

    uint16_t uncertainty = ts->estimated_rtt_ms + (uint16_t)(transit_ms > 65000 ? 65000 : transit_ms);
    if (out_uncertainty_ms) {
        *out_uncertainty_ms = uncertainty;
    }

    /* Uncertainty budget check */
    if (uncertainty > BRIDGE_TIME_MAX_UNCERTAINTY_MS) {
        *out_utc_ms = 0;
        *out_quality = TIME_QUALITY_INVALID;
        return -5;
    }

    /* Absolute UTC event timestamp */
    *out_utc_ms = (uint64_t)(event_esp_mono_ms + ts->sntp_offset_ms);
    *out_quality = TIME_QUALITY_SYNCED;
    return 0;
}

/**
 * @brief Calculate queue backlog age in milliseconds.
 *
 * @param esp_rx_mono_ms Frame arrival monotonic ms at Core 1
 * @param current_esp_mono_ms Current monotonic ms at dequeue/processing
 * @return Backlog age in milliseconds
 */
static inline uint32_t bridge_time_calc_backlog_ms(uint64_t esp_rx_mono_ms,
                                                   uint64_t current_esp_mono_ms) {
    if (current_esp_mono_ms < esp_rx_mono_ms) {
        return 0;
    }
    return (uint32_t)(current_esp_mono_ms - esp_rx_mono_ms);
}

/**
 * @brief Evaluate whether a metric timestamp is currently fresh (< 5000ms age).
 *
 * @param event_utc_ms Recorded event UTC timestamp
 * @param server_now_utc_ms Current UTC time
 * @param freshness_window_ms Window duration in ms (typically 5000)
 * @return true if fresh, false if stale or future
 */
static inline bool bridge_time_is_fresh(uint64_t event_utc_ms,
                                       uint64_t server_now_utc_ms,
                                       uint32_t freshness_window_ms) {
    if (event_utc_ms == 0 || server_now_utc_ms < event_utc_ms) {
        return false; /* Unset or future */
    }
    uint64_t age = server_now_utc_ms - event_utc_ms;
    return (age < freshness_window_ms);
}

/**
 * @brief Compute ECG batch start timestamp from end timestamp and sample parameters.
 *
 * @param batch_end_utc_ms UTC timestamp of the last sample in the batch
 * @param sample_count Number of samples in the batch (e.g. 50)
 * @param period_ms Sample period in ms (e.g. 4 for 250 Hz)
 * @param out_start_utc_ms Output start timestamp
 * @return 0 on success, -1 on invalid args
 */
static inline int bridge_time_compute_ecg_batch(uint64_t batch_end_utc_ms,
                                                uint16_t sample_count,
                                                uint16_t period_ms,
                                                uint64_t *out_start_utc_ms) {
    if (!out_start_utc_ms || sample_count == 0 || period_ms == 0) {
        return -1;
    }
    uint64_t duration = (uint64_t)(sample_count - 1) * (uint64_t)period_ms;
    if (batch_end_utc_ms < duration) {
        *out_start_utc_ms = 0;
    } else {
        *out_start_utc_ms = batch_end_utc_ms - duration;
    }
    return 0;
}

/**
 * @brief Resolve same-millisecond collision to enforce strictly monotonic stream order.
 *
 * @param proposed_utc_ms Proposed UTC timestamp for this event
 * @param last_published_utc_ms Previously published UTC timestamp on this stream
 * @return Strictly monotonic timestamp >= proposed_utc_ms and > last_published_utc_ms
 */
static inline uint64_t bridge_time_resolve_collision(uint64_t proposed_utc_ms,
                                                     uint64_t last_published_utc_ms) {
    if (proposed_utc_ms <= last_published_utc_ms) {
        return last_published_utc_ms + 1;
    }
    return proposed_utc_ms;
}

#ifdef __cplusplus
}
#endif
