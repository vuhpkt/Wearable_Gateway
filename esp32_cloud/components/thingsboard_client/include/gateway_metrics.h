#ifndef GATEWAY_METRICS_H
#define GATEWAY_METRICS_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * @brief Raw gateway telemetry and transport counters.
 */
typedef struct {
    uint32_t ecg_batches_recv;   /* Producer ingress count */
    uint32_t vitals_recv;        /* Producer ingress count */
    uint32_t status_recv;        /* Producer status frame count */
    uint32_t queue_overflow;     /* Producer FIFO overflow drops */
    uint32_t mqtt_published;     /* Cloud consumer publish count */
    uint32_t mqtt_dropped;       /* Cloud consumer publish failures */
} gateway_metrics_t;

/**
 * @brief Snapshot of throughput rates computed over a measured window.
 */
typedef struct {
    /* Cumulative totals at snapshot time */
    uint32_t ecg_batches_total;
    uint32_t vitals_total;
    uint32_t status_total;
    uint32_t queue_overflow_total;
    uint32_t mqtt_published_total;
    uint32_t mqtt_dropped_total;

    /* Window deltas */
    uint32_t ecg_batches_delta;
    uint32_t vitals_delta;
    uint32_t telemetry_delta;    /* (ecg_delta + vitals_delta) */
    uint32_t rate_per_sec;       /* Telemetry frames per second */
    uint64_t delta_us;           /* Actual measured duration in microseconds */
} gateway_throughput_snapshot_t;

/**
 * @brief Calculate throughput rate from monotonic microsecond delta.
 *
 * Implements non-integer window rate calculation:
 *   rate_per_sec = (telemetry_delta * 1,000,000 + delta_us / 2) / delta_us
 *
 * Handles:
 * 1. Monotonic microsecond clock (esp_timer_get_time), unaffected by SNTP clock stepping.
 * 2. Unsigned 32-bit counter wrap via standard modular arithmetic (curr - prev).
 * 3. Intermediate 64-bit precision to prevent overflow during multiplication.
 * 4. Division by zero protection (delta_us == 0).
 * 5. Nearest-integer rounding (+ delta_us / 2).
 *
 * @param prev Previous metrics snapshot
 * @param prev_us Microsecond timestamp of previous snapshot
 * @param curr Current metrics snapshot
 * @param curr_us Microsecond timestamp of current snapshot
 * @param out_snap Destination snapshot structure
 * @return true if valid duration (delta_us > 0), false if duration <= 0
 */
static inline bool gateway_metrics_calculate_throughput(
    const gateway_metrics_t *prev,
    uint64_t prev_us,
    const gateway_metrics_t *curr,
    uint64_t curr_us,
    gateway_throughput_snapshot_t *out_snap)
{
    if (!prev || !curr || !out_snap) {
        return false;
    }

    if (curr_us <= prev_us) {
        memset(out_snap, 0, sizeof(*out_snap));
        out_snap->ecg_batches_total = curr->ecg_batches_recv;
        out_snap->vitals_total = curr->vitals_recv;
        out_snap->status_total = curr->status_recv;
        out_snap->queue_overflow_total = curr->queue_overflow;
        out_snap->mqtt_published_total = curr->mqtt_published;
        out_snap->mqtt_dropped_total = curr->mqtt_dropped;
        return false;
    }

    uint64_t delta_us = curr_us - prev_us;
    uint32_t ecg_delta = curr->ecg_batches_recv - prev->ecg_batches_recv;
    uint32_t vitals_delta = curr->vitals_recv - prev->vitals_recv;
    uint32_t telem_delta = ecg_delta + vitals_delta;

    /* Intermediate 64-bit multiplication with rounding */
    uint64_t rate_num = ((uint64_t)telem_delta * 1000000ULL) + (delta_us / 2ULL);
    uint32_t rate_per_sec = (uint32_t)(rate_num / delta_us);

    out_snap->ecg_batches_total = curr->ecg_batches_recv;
    out_snap->vitals_total = curr->vitals_recv;
    out_snap->status_total = curr->status_recv;
    out_snap->queue_overflow_total = curr->queue_overflow;
    out_snap->mqtt_published_total = curr->mqtt_published;
    out_snap->mqtt_dropped_total = curr->mqtt_dropped;

    out_snap->ecg_batches_delta = ecg_delta;
    out_snap->vitals_delta = vitals_delta;
    out_snap->telemetry_delta = telem_delta;
    out_snap->rate_per_sec = rate_per_sec;
    out_snap->delta_us = delta_us;

    return true;
}

#endif /* GATEWAY_METRICS_H */
