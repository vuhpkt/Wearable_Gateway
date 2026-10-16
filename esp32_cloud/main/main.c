/*
 * Copyright (c) 2026 Wearable Gateway Project
 *
 * ESP32-S3 Dual-Core Wearable Gateway
 * Target SoC: ESP32-S3-WROOM-1-N16R8 (Dual-core Xtensa LX7 @ 240MHz)
 *
 * Core Allocation:
 *   - Core 0 (PRO_CPU): Network, Wi-Fi Manager & System Health Supervisor
 *   - Core 1 (APP_CPU): Sensor Producer Task (Real-time 460800 Baud UART RX & JSON Dispatch)
 */

/* Standard C Libraries */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* FreeRTOS & ESP-IDF System Headers */
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

/* Project Local Headers */
#include "ble_protocol_def.h"
#include "data_synthesizer.h"
#include "nvs_storage.h"
#include "thingsboard_config.h"
#include "thingsboard_mqtt.h"
#include "tb_rest_client.h"
#include "uart_bridge_rx.h"
#include "uart_frame_def.h"
#include "uart_rx_parser.h"
#include "wifi_config.h"
#include "wifi_portal.h"
#include "wifi_sta_mgr.h"
#include "wifi_ap.h"
#include "boot_button.h"
#include "gateway_metrics.h"

static const char *TAG = "esp32_gateway";


#define FRAME_QUEUE_LEN         128
#define SENSOR_TASK_STACK_SIZE  4096
#define CLOUD_TASK_STACK_SIZE   4096

static QueueHandle_t s_frame_queue = NULL;
static StaticQueue_t s_frame_queue_struct;
static uint8_t s_frame_queue_storage[FRAME_QUEUE_LEN * sizeof(uart_rx_frame_t)];

/* ══════════════════════════════════════════════════════════════
 *  UART Rx Frame Callback & Performance Metrics
 * ════════════════════════════════════════════════════════════ */
static char s_synth_tx_buf[2048];
static bool s_node_announced[MAX_NODES_SUPPORTED] = {0};

static TaskHandle_t s_cloud_task_handle = NULL;
static StaticTask_t s_cloud_task_tcb;
static StackType_t s_cloud_task_stack[CLOUD_TASK_STACK_SIZE];

static TaskHandle_t s_sensor_task_handle = NULL;
static StaticTask_t s_sensor_task_tcb;
static StackType_t s_sensor_task_stack[SENSOR_TASK_STACK_SIZE];

static gateway_metrics_t s_metrics = {0};
static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;

static inline void metrics_inc_mqtt_published(void) {
    portENTER_CRITICAL(&s_metrics_lock);
    s_metrics.mqtt_published++;
    portEXIT_CRITICAL(&s_metrics_lock);
}

static inline void metrics_inc_mqtt_dropped(void) {
    portENTER_CRITICAL(&s_metrics_lock);
    s_metrics.mqtt_dropped++;
    portEXIT_CRITICAL(&s_metrics_lock);
}

static inline void metrics_get_snapshot(gateway_metrics_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_metrics_lock);
    *out = s_metrics;
    portEXIT_CRITICAL(&s_metrics_lock);
}

static void on_mqtt_connected(void) {
    memset(s_node_announced, 0, sizeof(s_node_announced));
    tb_rest_mark_all_mqtt_dirty();
    ESP_LOGI(TAG, "🔄 MQTT (Re)Connected: Reset announcement cache and marked all seen nodes dirty");
}

static void reconcile_mqtt_node_status(void) {
    if (!tb_mqtt_is_connected()) {
        return;
    }
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        tb_node_state_snapshot_t snap;
        if (tb_rest_get_mqtt_pending_sync(i, &snap)) {
            const char *topic = NULL;
            char status_buf[64];
            int len = synth_format_node_status(status_buf, sizeof(status_buf), i, snap.desired_connected, &topic);
            if (len > 0 && topic != NULL) {
                int msg_id = tb_mqtt_publish(topic, status_buf, 1);
                if (msg_id >= 0) {
                    metrics_inc_mqtt_published();
                    s_node_announced[i] = snap.desired_connected;
                    tb_rest_ack_mqtt_synced(i, snap.generation);
                    ESP_LOGI(TAG, "📡 Reconciled MQTT v1/gateway/%s for Node%u (gen=%lu)",
                             snap.desired_connected ? "connect" : "disconnect", i + 1, (unsigned long)snap.generation);
                } else {
                    metrics_inc_mqtt_dropped();
                }
            }
        }
    }
}

static void ensure_node_connected(uint8_t node_id) {
    if (node_id >= MAX_NODES_SUPPORTED) return;
    if (!s_node_announced[node_id] && tb_mqtt_is_connected()) {
        tb_node_state_snapshot_t snap;
        if (tb_rest_get_node_snapshot(node_id, &snap) && snap.desired_connected) {
            const char *topic = NULL;
            char connect_buf[64];
            int len = synth_format_node_status(connect_buf, sizeof(connect_buf), node_id, true, &topic);
            if (len > 0 && topic != NULL) {
                int msg_id = tb_mqtt_publish(topic, connect_buf, 1);
                if (msg_id >= 0) {
                    metrics_inc_mqtt_published();
                    s_node_announced[node_id] = true;
                    tb_rest_ack_mqtt_synced(node_id, snap.generation);
                    ESP_LOGI(TAG, "📡 Auto-announced v1/gateway/connect activation for Node%u", node_id + 1);
                } else {
                    metrics_inc_mqtt_dropped();
                }
            }
        }
    }
}

static void on_uart_frame_received(const uart_rx_frame_t *frame) {
    if (frame == NULL) {
        return;
    }

    /* Node Status packets are latched directly into the canonical state table */
    if (frame->pkt_type == UART_PKT_TYPE_NODE_STATUS) {
        if (frame->len == 1 && (frame->payload[0] == 0 || frame->payload[0] == 1)) {
            bool is_conn = (frame->payload[0] == 1);
            tb_rest_set_node_connected(frame->node_id, is_conn);
            portENTER_CRITICAL(&s_metrics_lock);
            s_metrics.status_recv++;
            portEXIT_CRITICAL(&s_metrics_lock);
            ESP_LOGI(TAG, "🟢 [PRODUCER STATUS LATCH] Node %u -> %s (bypasses telemetry FIFO)",
                     frame->node_id + 1, is_conn ? "CONNECTED" : "DISCONNECTED");
        }
        /* Never enqueue status into telemetry FIFO */
        return;
    }

    /* Count ingress telemetry at producer before queue */
    if (frame->pkt_type == UART_PKT_TYPE_VITALS) {
        portENTER_CRITICAL(&s_metrics_lock);
        s_metrics.vitals_recv++;
        portEXIT_CRITICAL(&s_metrics_lock);
        tb_rest_infer_node_connected_if_unseen(frame->node_id);
    } else if (frame->pkt_type == UART_PKT_TYPE_ECG_BATCH) {
        portENTER_CRITICAL(&s_metrics_lock);
        s_metrics.ecg_batches_recv++;
        portEXIT_CRITICAL(&s_metrics_lock);
        tb_rest_infer_node_connected_if_unseen(frame->node_id);
    }

    if (s_frame_queue != NULL) {
        BaseType_t ret = xQueueSend(s_frame_queue, frame, 0);
        if (ret != pdTRUE) {
            portENTER_CRITICAL(&s_metrics_lock);
            s_metrics.queue_overflow++;
            portEXIT_CRITICAL(&s_metrics_lock);
        }
    }
}

static void publish_frame(const uart_rx_frame_t *frame) {
    switch (frame->pkt_type) {
    case UART_PKT_TYPE_NODE_STATUS: {
        /* Status frames are latched at producer; should not be present in telemetry FIFO */
        ESP_LOGW(TAG, "⚠️ Unexpected status packet in telemetry FIFO for Node%u", frame->node_id + 1);
        break;
    }
    case UART_PKT_TYPE_VITALS: {
        if (frame->len >= sizeof(vitals_packet_t)) {
            ensure_node_connected(frame->node_id);
            const vitals_packet_t *v = (const vitals_packet_t *)frame->payload;
            float temp_c = (float)v->temp_x10 / 10.0f;
            ESP_LOGD(TAG, "💓 [NODE %u VITALS] HR_ECG: %u bpm | HR_PPG: %u bpm | SpO2: %u%% | Temp: %.1f°C (CRC OK)",
                     frame->node_id, v->hr_ecg, v->hr_ppg, v->spo2, temp_c);

            int len = synth_format_vitals(s_synth_tx_buf, sizeof(s_synth_tx_buf), frame->node_id, v);
            if (len > 0 && tb_mqtt_is_connected()) {
                int msg_id = tb_mqtt_publish(TB_GATEWAY_TELEMETRY_TOPIC, s_synth_tx_buf, 0);
                if (msg_id >= 0) {
                    metrics_inc_mqtt_published();
                } else {
                    metrics_inc_mqtt_dropped();
                }
            } else if (len == -2) {
                ESP_LOGW(TAG, "⏳ [NODE %u VITALS] Waiting for SNTP real-time clock synchronization...", frame->node_id);
            }
        }
        break;
    }
    case UART_PKT_TYPE_ECG_BATCH: {
        ensure_node_connected(frame->node_id);
        uint16_t sample_count = frame->len / sizeof(int16_t);
        const int16_t *samples = (const int16_t *)frame->payload;
        if (sample_count > 0) {
            ESP_LOGD(TAG, "📈 [NODE %u ECG BATCH] Received %u samples (First: %d, Last: %d) (CRC OK)", frame->node_id,
                     sample_count, samples[0], samples[sample_count - 1]);
        }

        int len = synth_format_ecg_batch(s_synth_tx_buf, sizeof(s_synth_tx_buf), frame->node_id, samples, sample_count);
        if (len > 0 && tb_mqtt_is_connected()) {
            int msg_id = tb_mqtt_publish(TB_GATEWAY_TELEMETRY_TOPIC, s_synth_tx_buf, 0);
            if (msg_id >= 0) {
                metrics_inc_mqtt_published();
            } else {
                metrics_inc_mqtt_dropped();
            }
        } else if (len == -2) {
            ESP_LOGW(TAG, "⏳ [NODE %u ECG] Waiting for SNTP real-time clock synchronization...", frame->node_id);
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "📦 [NODE %u PKT 0x%02X] Received %u bytes payload (CRC OK)", frame->node_id, frame->pkt_type,
                 frame->len);
        break;
    }
}

static void sensor_producer_task(void *pvParameters) {
    (void)pvParameters;
    uart_bridge_rx_init();
    uart_rx_parser_init(on_uart_frame_received);
    static uint8_t rx_buf[4096];

    while (1) {
        int len = uart_bridge_rx_read(rx_buf, sizeof(rx_buf), 20);
        if (len > 0) {
            uart_rx_parser_process(rx_buf, len);
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Wi-Fi Provisioning & Mode Switching Handlers (Zero Reboot)
 * ════════════════════════════════════════════════════════════ */
#define SOFTAP_SSID "ESP32_HEALTH_PROV"

static void on_new_credentials_received(const char *new_ssid, const char *new_pass);
static void on_softap_timeout(void);

static void on_softap_timeout(void) {
    ESP_LOGW(TAG, "⏰ SoftAP idle timeout expired (5 min). Stopping portal and restoring Station...");
    wifi_portal_stop();
    wifi_ap_stop();

    char ssid[33] = {0};
    char pass[65] = {0};
    if (nvs_storage_read_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Restoring Station connection to '%s'...", ssid);
        wifi_sta_init_with_creds(ssid, pass);
    }
}

static void on_boot_button_long_press(void) {
    if (wifi_ap_is_active()) {
        ESP_LOGW(TAG, "SoftAP is already active, ignoring button trigger.");
        return;
    }

    ESP_LOGI(TAG, "===============================================================");
    ESP_LOGI(TAG, "🔘 [TRIGGER] BOOT BUTTON 2-SECOND HOLD DETECTED!");
    ESP_LOGI(TAG, "   Stopping Station mode and transitioning to SoftAP...");
    ESP_LOGI(TAG, "===============================================================");

    /* 1. Pause MQTT client so it doesn't attempt connection or log socket errors while offline */
    tb_mqtt_pause();

    /* 2. Stop Station mode */
    wifi_sta_stop();

    /* 3. Start SoftAP */
    wifi_ap_start(SOFTAP_SSID, NULL);

    /* 4. Start Web Portal */
    wifi_portal_start(on_new_credentials_received);

    ESP_LOGI(TAG, ">>> SoftAP & Web Portal ready: Connect to '%s', open http://192.168.4.1/ <<<", SOFTAP_SSID);
}

static void on_new_credentials_received(const char *new_ssid, const char *new_pass) {
    ESP_LOGI(TAG, "===============================================================");
    ESP_LOGI(TAG, "🔑 [PROVISIONING] New credentials received via Web Portal!");
    ESP_LOGI(TAG, "   SSID:     '%s'", new_ssid);
    ESP_LOGI(TAG, "   Password: '%s'", new_pass ? new_pass : "");
    ESP_LOGI(TAG, "===============================================================");

    /* 1. Save to NVS */
    nvs_storage_save_wifi(new_ssid, new_pass);

    /* 2. Stop Web Portal and SoftAP */
    wifi_portal_stop();
    wifi_ap_stop();

    /* 3. Connect to the new Station in RAM without restarting MCU */
    ESP_LOGI(TAG, "Transitioning back to Station mode to connect to '%s'...", new_ssid);
    wifi_sta_init_with_creds(new_ssid, new_pass);
}

static void cloud_consumer_task(void *pvParameters) {
    if (!wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "Waiting for Wi-Fi Station connection before starting Cloud MQTT...");
        while (!wifi_sta_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

#if USE_MOCK_MQTT_SERVER
    ESP_LOGI(TAG, "🚀 [MODE: LOCAL MOCK MQTT] Target Broker: %s", TB_MQTT_BROKER_URI);
#else
    ESP_LOGI(TAG, "🌐 [MODE: THINGSBOARD CLOUD] Target Broker: %s", TB_MQTT_BROKER_URI);
#endif

    tb_mqtt_register_connect_callback(on_mqtt_connected);
    tb_mqtt_init(TB_MQTT_BROKER_URI, TB_GATEWAY_ACCESS_TOKEN);
    tb_mqtt_wait_connected(portMAX_DELAY);

    /* Wait for SNTP time sync up to 5s before streaming */
    if (wifi_sta_wait_time_sync(5000) == ESP_OK) {
        ESP_LOGI(TAG, "⏰ Real-time synchronized successfully via SNTP!");
    } else {
        ESP_LOGW(TAG, "⚠️ Continuing SNTP synchronization in background...");
    }

    /* Start ThingsBoard REST background client */
    tb_rest_client_init();

    static uart_rx_frame_t frame;
    int64_t last_health_log_us = esp_timer_get_time();
    gateway_metrics_t last_metrics;
    metrics_get_snapshot(&last_metrics);

    while (1) {
        reconcile_mqtt_node_status();

        if (xQueueReceive(s_frame_queue, &frame, pdMS_TO_TICKS(100))) {
            publish_frame(&frame);
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_health_log_us >= 10000000ULL) { /* 10s monotonic window */
            gateway_metrics_t curr_metrics;
            metrics_get_snapshot(&curr_metrics);

            gateway_throughput_snapshot_t snap;
            if (gateway_metrics_calculate_throughput(&last_metrics, last_health_log_us,
                                                     &curr_metrics, now_us, &snap)) {
                last_metrics = curr_metrics;
                last_health_log_us = now_us;

                UBaseType_t c_hwm = s_cloud_task_handle ? uxTaskGetStackHighWaterMark(s_cloud_task_handle) : 0;
                UBaseType_t s_hwm = s_sensor_task_handle ? uxTaskGetStackHighWaterMark(s_sensor_task_handle) : 0;
                TaskHandle_t rest_h = (TaskHandle_t)tb_rest_get_task_handle();
                UBaseType_t r_hwm = rest_h ? uxTaskGetStackHighWaterMark(rest_h) : 0;
                uint32_t q_waiting = s_frame_queue ? (uint32_t)uxQueueMessagesWaiting(s_frame_queue) : 0;

                ESP_LOGI(TAG, "🩺 [HEALTH] Heap: %luB (Min: %luB) | Stacks(HWM): C:%u S:%u R:%u | Wi-Fi: %s | MQTT: %s | SNTP: %s",
                         (unsigned long)esp_get_free_heap_size(),
                         (unsigned long)esp_get_minimum_free_heap_size(),
                         (unsigned)c_hwm, (unsigned)s_hwm, (unsigned)r_hwm,
                         wifi_sta_is_connected() ? "ONLINE" : "OFFLINE",
                         tb_mqtt_is_connected() ? "CONNECTED" : "DISCONNECTED",
                         wifi_sta_is_time_synced() ? "SYNCED" : "WAITING");

                ESP_LOGI(TAG, "📊 [THROUGHPUT %lums] Ingress: ECG=%lu Vit=%lu (Rate: %lu/s) [Tot: E=%lu V=%lu S=%lu] | MQTT: Pub=%lu Drop=%lu | Q_Wait: %lu | Q_Overfl: %lu",
                         (unsigned long)(snap.delta_us / 1000ULL),
                         (unsigned long)snap.ecg_batches_delta,
                         (unsigned long)snap.vitals_delta,
                         (unsigned long)snap.rate_per_sec,
                         (unsigned long)snap.ecg_batches_total,
                         (unsigned long)snap.vitals_total,
                         (unsigned long)snap.status_total,
                         (unsigned long)snap.mqtt_published_total,
                         (unsigned long)snap.mqtt_dropped_total,
                         (unsigned long)q_waiting,
                         (unsigned long)snap.queue_overflow_total);
            }
        }
    }
}


/* ══════════════════════════════════════════════════════════════
 *  Gateway Application Entry Point
 * ════════════════════════════════════════════════════════════ */
void app_main(void) {
    nvs_storage_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[33] = {0};
    char pass[65] = {0};

    /*
     * NVS Credential Check:
     * Check NVS first. Only load compile-time default if NVS is completely empty.
     * Never overwrite user credentials on reboot!
     */
    if (nvs_storage_read_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "🔑 [NVS LOADED] Found Wi-Fi credentials in NVS: SSID='%s' (pass_len=%u)", ssid, (unsigned)strlen(pass));
    } else {
        ESP_LOGI(TAG, "No Wi-Fi credentials in NVS. Initializing with defaults: '%s'...", CONFIG_WIFI_SSID);
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, CONFIG_WIFI_PASSWORD, sizeof(pass) - 1);
        nvs_storage_save_wifi(ssid, pass);
    }

    ESP_LOGI(TAG, "🔑 [CREDENTIALS] SSID='%s' | PASSWORD='%s' (Length: %u chars)", ssid, pass, (unsigned)strlen(pass));

    /* Initialize BOOT Button Poller (GPIO 0, 2s hold detection) */
    ESP_ERROR_CHECK(boot_button_init(on_boot_button_long_press));

    /* Level 1 Power Optimization: Arm 5-minute SoftAP idle timeout to prevent continuous RF heating */
    wifi_ap_set_timeout(300, on_softap_timeout);

    /* [M01 FIX] Create 128-item queue (~2.56s buffer) and spawn UART ingestion early with static allocation */
    s_frame_queue = xQueueCreateStatic(
        FRAME_QUEUE_LEN,
        sizeof(uart_rx_frame_t),
        s_frame_queue_storage,
        &s_frame_queue_struct
    );
    if (s_frame_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create static s_frame_queue!");
    }

    s_sensor_task_handle = xTaskCreateStaticPinnedToCore(
        sensor_producer_task,
        "sensor_task",
        SENSOR_TASK_STACK_SIZE,
        NULL,
        5,
        s_sensor_task_stack,
        &s_sensor_task_tcb,
        1
    );
    if (s_sensor_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create static sensor_task!");
    }
    ESP_LOGI(TAG, "🚀 Sensor Producer Task spawned statically on Core 1 (Ingestion Active immediately)");

    if (strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Starting Wi-Fi connection to '%s' (Channels 1..13 dynamic scan)...", ssid);
        wifi_sta_init_with_creds(ssid, pass);

        esp_err_t wait_err = wifi_sta_wait_connected(pdMS_TO_TICKS(15000));
        if (wait_err == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi Station connected successfully!");
        } else if (wifi_ap_is_active()) {
            ESP_LOGI(TAG, "SoftAP Provisioning is active; Station connection halted for setup.");
        } else {
            ESP_LOGW(TAG, "Station not connected within initial 15s. Background reconnect timer is active.");
        }
    }

    s_cloud_task_handle = xTaskCreateStaticPinnedToCore(
        cloud_consumer_task,
        "cloud_task",
        CLOUD_TASK_STACK_SIZE,
        NULL,
        5,
        s_cloud_task_stack,
        &s_cloud_task_tcb,
        0
    );
    if (s_cloud_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create static cloud_task!");
    }

    ESP_LOGI(TAG, "Dual-Core static tasks spawned successfully!");
}
