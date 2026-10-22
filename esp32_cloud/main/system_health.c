#include "system_health.h"
#include <string.h>
#include <stdio.h>

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_sta_mgr.h"
#include "thingsboard_mqtt.h"

static const char *TAG = "system_health";
#else
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

static system_health_config_t s_config;
static uint32_t s_last_report_ms = 0;

void system_health_init(const system_health_config_t *config) {
    if (config) {
        s_config = *config;
    } else {
        memset(&s_config, 0, sizeof(s_config));
    }
    s_last_report_ms = 0;
}

void system_health_capture_snapshot(system_health_snapshot_t *snapshot) {
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    snapshot->free_heap_bytes = (uint32_t)esp_get_free_heap_size();
    snapshot->min_heap_bytes = (uint32_t)esp_get_minimum_free_heap_size();
    snapshot->largest_free_block_bytes = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    if (s_config.cloud_task_handle) {
        snapshot->cloud_hwm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)s_config.cloud_task_handle);
    }
    if (s_config.sensor_task_handle) {
        snapshot->sensor_hwm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)s_config.sensor_task_handle);
    }
    if (s_config.control_task_handle) {
        snapshot->control_hwm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)s_config.control_task_handle);
    }

    snapshot->wifi_connected = wifi_sta_is_connected();
    snapshot->mqtt_connected = tb_mqtt_is_connected();
    snapshot->sntp_synced = wifi_sta_is_time_synced();
#endif

    if (s_config.queue) {
        frame_snapshot_queue_get_stats(s_config.queue, &snapshot->queue_stats);
    }

    gateway_outbox_get_stats(NULL, &snapshot->outbox_stats);
}

void system_health_report_if_due(uint32_t period_ms) {
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now_ms - s_last_report_ms < period_ms) {
        return;
    }
    s_last_report_ms = now_ms;

    system_health_snapshot_t s;
    system_health_capture_snapshot(&s);

    ESP_LOGI(TAG, "🩺 [HEALTH] Heap: %luB (Min: %luB, MaxBlock: %luB) | Stacks(HWM): Cloud:%lu Sensor:%lu Ctrl:%lu | Wi-Fi: %s | MQTT: %s | SNTP: %s",
             (unsigned long)s.free_heap_bytes,
             (unsigned long)s.min_heap_bytes,
             (unsigned long)s.largest_free_block_bytes,
             (unsigned long)s.cloud_hwm,
             (unsigned long)s.sensor_hwm,
             (unsigned long)s.control_hwm,
             s.wifi_connected ? "ONLINE" : "OFFLINE",
             s.mqtt_connected ? "CONNECTED" : "DISCONNECTED",
             s.sntp_synced ? "SYNCED" : "WAITING");

    ESP_LOGI(TAG, "📊 [METRICS] Q: %lu items (drop=%lu, prio_drop=%lu) | Outbox: %lu items (%lluB, drop_full=%lu, drop_ovsz=%lu)",
             (unsigned long)s.queue_stats.count,
             (unsigned long)s.queue_stats.dropped_count,
             (unsigned long)s.queue_stats.prio_dropped_count,
             (unsigned long)s.outbox_stats.current_items,
             (unsigned long long)s.outbox_stats.current_bytes,
             (unsigned long)s.outbox_stats.dropped_full,
             (unsigned long)s.outbox_stats.dropped_oversize);
#else
    (void)period_ms;
#endif
}
