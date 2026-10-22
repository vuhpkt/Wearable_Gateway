#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "frame_snapshot.h"
#include "gateway_mqtt_outbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Memory metrics */
    uint32_t free_heap_bytes;
    uint32_t min_heap_bytes;
    uint32_t largest_free_block_bytes;

    /* Task Stack High Water Marks */
    uint32_t cloud_hwm;
    uint32_t sensor_hwm;
    uint32_t control_hwm;

    /* Queue metrics */
    frame_snapshot_queue_stats_t queue_stats;

    /* Outbox metrics */
    gateway_outbox_stats_t outbox_stats;

    /* Status flags */
    bool wifi_connected;
    bool mqtt_connected;
    bool sntp_synced;
} system_health_snapshot_t;

typedef struct {
    const frame_snapshot_queue_t *queue;
    void *cloud_task_handle;
    void *sensor_task_handle;
    void *control_task_handle;
} system_health_config_t;

void system_health_init(const system_health_config_t *config);
void system_health_capture_snapshot(system_health_snapshot_t *snapshot);
void system_health_report_if_due(uint32_t period_ms);

#ifdef __cplusplus
}
#endif
