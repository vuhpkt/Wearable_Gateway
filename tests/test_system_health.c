/**
 * @file test_system_health.c
 * @brief Host-based Unit Test for System Health Monitoring Module
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "system_health.h"

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING SYSTEM HEALTH UNIT TEST                  \n");
    printf("====================================================\n\n");

    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    system_health_config_t cfg = {
        .queue = &queue,
        .cloud_task_handle = NULL,
        .sensor_task_handle = NULL,
        .control_task_handle = NULL,
    };
    system_health_init(&cfg);

    system_health_snapshot_t snapshot;
    system_health_capture_snapshot(&snapshot);

    assert(snapshot.queue_stats.count == 0);
    assert(snapshot.queue_stats.dropped_count == 0);

    system_health_report_if_due(10000);

    printf("\n🎉 ALL SYSTEM HEALTH TESTS PASSED!\n");
    return 0;
}
