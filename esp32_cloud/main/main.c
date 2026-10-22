/*
 * Copyright (c) 2026 Wearable Gateway Project
 * Target SoC: ESP32-S3-WROOM-1-N16R8 (Dual-core Xtensa LX7 @ 240MHz)
 * Zero Heap Application Composition Root:
 *   - Core 0 (PRO_CPU): Control Task (Provisioning FSM & Health), Cloud Uplink Task
 *   - Core 1 (APP_CPU): Sensor Producer Ingest Task (460800 Baud UART RX & Bridge FSM)
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "wifi_manager.h"
#include "uart_bridge.h"
#include "thingsboard_client.h"
#include "cloud_uplink.h"
#include "system_health.h"

static const char *TAG = "gateway_root";

/* Wi-Fi IP acquisition callback -> Resumes MQTT client */
static void on_wifi_ip_assigned(void) {
    ESP_LOGI(TAG, "Wi-Fi IP acquired -> resuming MQTT client");
    tb_mqtt_resume();
}

/* SNTP Time Sync callback -> Forwards UTC clock observation to Core 1 via mailbox */
static void on_sntp_sync(struct timeval *tv) {
    if (!tv) return;
    int64_t now_utc_ms = (int64_t)tv->tv_sec * 1000LL + (int64_t)(tv->tv_usec / 1000);
    uint64_t esp_mono_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    uart_bridge_post_sntp(now_utc_ms, esp_mono_ms);
}

/* Control Queue (Static Allocation) */
#define CONTROL_QUEUE_SIZE      8
static QueueHandle_t            s_control_queue = NULL;
static StaticQueue_t            s_control_queue_struct;
static uint8_t                  s_control_queue_storage[CONTROL_QUEUE_SIZE * sizeof(prov_event_t)];

/* Task Allocations (100% static stacks, exactly 4096 bytes each) */
#define TASK_STACK_SIZE         4096
static TaskHandle_t             s_control_task_handle = NULL;
static StaticTask_t             s_control_task_tcb;
static StackType_t              s_control_task_stack[TASK_STACK_SIZE];

static TaskHandle_t             s_cloud_task_handle = NULL;
static StaticTask_t             s_cloud_task_tcb;
static StackType_t              s_cloud_task_stack[TASK_STACK_SIZE];

static TaskHandle_t             s_sensor_task_handle = NULL;
static StaticTask_t             s_sensor_task_tcb;
static StackType_t              s_sensor_task_stack[TASK_STACK_SIZE];

/* Core 0 Control Supervisor Task */
static void control_task(void *pvParameters) {
    (void)pvParameters;
    prov_event_t evt;

    while (1) {
        if (xQueueReceive(s_control_queue, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (evt.type == PROV_EVT_CREDENTIALS_SUBMITTED) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            wifi_mgr_prov_dispatch(&evt);
        }
        system_health_report_if_due(10000);
    }
}

/* Core 0 Cloud Uplink Consumer Task */
static void cloud_consumer_task(void *pvParameters) {
    (void)pvParameters;
    while (!wifi_mgr_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    tb_mqtt_register_connect_callback(cloud_uplink_on_mqtt_connected_cb);
    tb_mqtt_register_publish_callback(cloud_uplink_on_mqtt_published_cb);
    tb_mqtt_init(TB_MQTT_BROKER_URI, TB_GATEWAY_ACCESS_TOKEN);
    tb_mqtt_wait_connected(portMAX_DELAY);

    if (wifi_sta_wait_time_sync(5000) == ESP_OK) {
        ESP_LOGI(TAG, "⏰ Real-time synchronized successfully via SNTP!");
    } else {
        ESP_LOGW(TAG, "⚠️ Continuing SNTP synchronization in background...");
    }

    static frame_snapshot_tuple_t tuple;
    while (1) {
        cloud_uplink_default_process_events();

        if (uart_bridge_pop_snapshot(&tuple, 100)) {
            if (tuple.session_id != 0 && !uart_bridge_is_session_valid(tuple.node_id, tuple.bridge_epoch, tuple.session_id)) {
                if (!(tuple.stream_type == STREAM_TYPE_STATUS && tuple.payload_len > 0 && tuple.payload[0] == 0)) {
                    ESP_LOGW(TAG, "⚠️ Dropping queued frame from revoked session %lu (Slot %u)",
                             (unsigned long)tuple.session_id, tuple.node_id);
                    continue;
                }
            }
            cloud_uplink_default_dispatch(&tuple);
        }
    }
}

/* Composition Root */
void app_main(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_control_queue = xQueueCreateStatic(CONTROL_QUEUE_SIZE, sizeof(prov_event_t),
                                         s_control_queue_storage, &s_control_queue_struct);
    assert(s_control_queue != NULL);

    /* 1. Ingest & Bridge Subsystem (Spinlocks, Mailbox, Time, Queue, Rev Table) */
    ESP_ERROR_CHECK(uart_bridge_subsystem_init());

    /* 2. Cloud Uplink Subsystem (Default Instance, MQTT Lifecycle, Buffers) */
    cloud_uplink_default_init();

    /* 3. Wi-Fi Manager (NVS, STA/AP, Provisioning FSM, SNTP Callback) */
    ESP_ERROR_CHECK(wifi_mgr_setup(s_control_queue, on_wifi_ip_assigned, tb_mqtt_pause, on_sntp_sync));

    s_control_task_handle = xTaskCreateStaticPinnedToCore(
        control_task, "control_task", TASK_STACK_SIZE, NULL, 4,
        s_control_task_stack, &s_control_task_tcb, 0);

    s_cloud_task_handle = xTaskCreateStaticPinnedToCore(
        cloud_consumer_task, "cloud_task", TASK_STACK_SIZE, NULL, 5,
        s_cloud_task_stack, &s_cloud_task_tcb, 0);

    s_sensor_task_handle = xTaskCreateStaticPinnedToCore(
        gateway_ingest_task, "sensor_task", TASK_STACK_SIZE, uart_bridge_get_ingest_ctx(), 5,
        s_sensor_task_stack, &s_sensor_task_tcb, 1);

    assert(s_control_task_handle && s_cloud_task_handle && s_sensor_task_handle);

    system_health_config_t health_cfg = {
        .queue = uart_bridge_get_snapshot_queue(),
        .cloud_task_handle = s_cloud_task_handle,
        .sensor_task_handle = s_sensor_task_handle,
        .control_task_handle = s_control_task_handle,
    };
    system_health_init(&health_cfg);

    ESP_LOGI(TAG, "All Static Tasks spawned successfully (Zero-Heap Composition Root, 4096B stacks)!");
}
