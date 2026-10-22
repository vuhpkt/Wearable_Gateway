#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "cloud_uplink.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include "esp_log.h"
#include "esp_timer.h"
static const char *TAG = "cloud_uplink";
#else
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#endif

static bool is_valid_mac_address(const uint8_t *mac) {
    if (!mac) return false;
    uint8_t zero_mac[6] = {0};
    uint8_t ff_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(mac, zero_mac, 6) == 0) return false;
    if (memcmp(mac, ff_mac, 6) == 0) return false;
    return true;
}

void cloud_uplink_init(cloud_uplink_t *uplink) {
    if (!uplink) return;
    memset(uplink, 0, sizeof(*uplink));
    mqtt_lifecycle_init(&uplink->lifecycle);
}

bool cloud_uplink_post_mqtt_event(cloud_uplink_t *uplink, const cloud_mqtt_evt_t *evt) {
    if (!uplink || !evt) return false;

    if (uplink->evt_count >= CLOUD_UPLINK_EVT_QUEUE_SIZE) {
        uplink->evt_overflow_count++;
        ESP_LOGW(TAG, "⚠️ [MQTT EVT OVERFLOW] Event queue full (count=%u), dropped event type=%d",
                 uplink->evt_count, evt->type);
        return false;
    }

    uplink->evt_queue[uplink->evt_tail] = *evt;
    uplink->evt_tail = (uplink->evt_tail + 1) % CLOUD_UPLINK_EVT_QUEUE_SIZE;
    uplink->evt_count++;
    return true;
}

void cloud_uplink_process_events(cloud_uplink_t *uplink) {
    if (!uplink) return;

    while (uplink->evt_count > 0) {
        cloud_mqtt_evt_t evt = uplink->evt_queue[uplink->evt_head];
        uplink->evt_head = (uplink->evt_head + 1) % CLOUD_UPLINK_EVT_QUEUE_SIZE;
        uplink->evt_count--;

        switch (evt.type) {
        case CLOUD_MQTT_EVT_CONNECTED:
            ESP_LOGI(TAG, "🔄 MQTT (Re)Connected: Resetting node lifecycle state");
            mqtt_lifecycle_on_broker_connected(&uplink->lifecycle);
            for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
                uplink->pending_announces[i].has_pending = false;
            }
            break;

        case CLOUD_MQTT_EVT_DISCONNECTED:
            ESP_LOGW(TAG, "🔌 MQTT Disconnected from broker");
            mqtt_lifecycle_on_broker_disconnected(&uplink->lifecycle);
            break;

        case CLOUD_MQTT_EVT_PUBACK: {
            uint8_t ack_node_id = 0;
            bool ready_for_attr = false;
            int ret = mqtt_lifecycle_handle_puback(&uplink->lifecycle, evt.msg_id, &ack_node_id, &ready_for_attr);
            if (ret == 0) {
                if (ready_for_attr) {
                    ESP_LOGI(TAG, "🟢 Node %u connect ACKed; publishing attributes...", ack_node_id);
                    frame_snapshot_tuple_t temp_tuple;
                    memset(&temp_tuple, 0, sizeof(temp_tuple));
                    temp_tuple.node_id = ack_node_id;

                    if (uplink->pending_announces[ack_node_id].has_pending) {
                        temp_tuple = uplink->pending_announces[ack_node_id].tuple;
                    } else {
                        memcpy(temp_tuple.mac, uplink->lifecycle.nodes[ack_node_id].mac, sizeof(temp_tuple.mac));
                        temp_tuple.session_id = uplink->lifecycle.nodes[ack_node_id].session_id;
                    }

                    int len = synth_format_mac_attributes(uplink->attr_buffer, sizeof(uplink->attr_buffer), &temp_tuple, true, "gw-01");
                    if (len > 0) {
                        int attr_msg_id = tb_mqtt_publish("v1/gateway/attributes", uplink->attr_buffer, 1);
                        mqtt_lifecycle_set_attributes_msg_id(&uplink->lifecycle, ack_node_id, attr_msg_id);
                    }
                } else {
                    ESP_LOGI(TAG, "🟢 Node %u attributes ACKed; node is ACTIVE for telemetry", ack_node_id);
                    uplink->pending_announces[ack_node_id].has_pending = false;
                }
            }
            break;
        }

        default:
            break;
        }
    }
}

void cloud_uplink_handle_node_status(cloud_uplink_t *uplink, const frame_snapshot_tuple_t *tuple, bool is_connected) {
    if (!uplink || !tuple || tuple->node_id >= MAX_NODES_SUPPORTED) return;
    if (!is_valid_mac_address(tuple->mac)) return;

    const char *topic = NULL;
    if (is_connected) {
        int len = synth_format_mac_connect(uplink->tx_buffer, sizeof(uplink->tx_buffer), tuple->mac, &topic);
        if (len > 0 && topic != NULL) {
            int msg_id = tb_mqtt_publish(topic, uplink->tx_buffer, 1);
            ESP_LOGI(TAG, "📤 [NODE %u CONNECT] Published to %s (msg_id=%d)", tuple->node_id, topic, msg_id);
            
            mqtt_lifecycle_start_announce(&uplink->lifecycle, tuple->node_id, tuple->mac, tuple->session_id, msg_id);
            uplink->pending_announces[tuple->node_id].tuple = *tuple;
            uplink->pending_announces[tuple->node_id].has_pending = true;
            /* Note: Attributes are published ONLY after connect ACK in on_puback, strictly adhering to barrier */
        }
    } else {
        /* Immediate offline attribute latch */
        int attr_len = synth_format_mac_attributes(uplink->attr_buffer, sizeof(uplink->attr_buffer), tuple, false, "gw-01");
        if (attr_len > 0) {
            tb_mqtt_publish("v1/gateway/attributes", uplink->attr_buffer, 1);
            ESP_LOGI(TAG, "📤 [NODE %u DISCONNECT ATTR] Published bleConnected=false to v1/gateway/attributes", tuple->node_id);
        }

        int len = synth_format_mac_disconnect(uplink->tx_buffer, sizeof(uplink->tx_buffer), tuple->mac, &topic);
        if (len > 0 && topic != NULL) {
            tb_mqtt_publish(topic, uplink->tx_buffer, 1);
            ESP_LOGI(TAG, "📤 [NODE %u DISCONNECT] Published to %s", tuple->node_id, topic);
            mqtt_lifecycle_handle_node_disconnect(&uplink->lifecycle, tuple->node_id);
        }
        uplink->pending_announces[tuple->node_id].has_pending = false;
    }
}

void cloud_uplink_process_telemetry(cloud_uplink_t *uplink, const frame_snapshot_tuple_t *tuple, uint64_t current_utc_ms) {
    if (!uplink || !tuple || tuple->node_id >= MAX_NODES_SUPPORTED) return;
    if (!is_valid_mac_address(tuple->mac)) return;

    /* Reject frames without valid synchronized UTC timestamps from live publish */
    if (tuple->timestamp_utc_ms == 0 ||
        tuple->time_quality == TIME_QUALITY_UNKNOWN ||
        tuple->time_quality == TIME_QUALITY_INVALID) {
        return;
    }

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    uint64_t current_mono_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (tuple->esp_rx_mono_ms > 0 && current_mono_ms >= tuple->esp_rx_mono_ms) {
        uint64_t queue_age_ms = current_mono_ms - tuple->esp_rx_mono_ms;
        if (queue_age_ms >= 5000) {
            uplink->lifecycle.ttl_dropped_count++;
            return;
        }
    }
#endif

    bool can_pub = mqtt_lifecycle_can_publish_telemetry(&uplink->lifecycle,
                                                        tuple->node_id,
                                                        tuple->session_id,
                                                        tuple->timestamp_utc_ms,
                                                        current_utc_ms,
                                                        5000);
    if (!can_pub) {
        ESP_LOGW(TAG, "⏱️ [NODE %u STREAM %u] Dropped by lifecycle guard (inactive, wrong session, or TTL expired)",
                 tuple->node_id, tuple->stream_type);
        return;
    }

    switch (tuple->stream_type) {
    case STREAM_TYPE_VITALS: {
        if (tuple->payload_len >= sizeof(vitals_packet_t)) {
            const vitals_packet_t *v = (const vitals_packet_t *)tuple->payload;
            float temp_c = (float)v->temp_x10 / 10.0f;
            (void)temp_c;
            ESP_LOGI(TAG, "💓 [NODE %u VITALS] HR_ECG: %u bpm | HR_PPG: %u bpm | SpO2: %u%% | Temp: %.1f°C | MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     tuple->node_id, v->hr_ecg, v->hr_ppg, v->spo2, temp_c,
                     tuple->mac[0], tuple->mac[1], tuple->mac[2],
                     tuple->mac[3], tuple->mac[4], tuple->mac[5]);

            int len = synth_format_mac_vitals(uplink->tx_buffer, sizeof(uplink->tx_buffer), tuple, v, "gw-01");
            if (len > 0 && tb_mqtt_is_connected()) {
                tb_mqtt_publish("v1/gateway/telemetry", uplink->tx_buffer, 1);
            }
        }
        break;
    }

    case STREAM_TYPE_ECG: {
        uint16_t sample_count = tuple->payload_len / sizeof(int16_t);
        const int16_t *samples = (const int16_t *)tuple->payload;
        if (sample_count > 0) {
            ESP_LOGI(TAG, "📈 [NODE %u ECG BATCH] Received %u samples (First: %d, Last: %d) | MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     tuple->node_id, sample_count, samples[0], samples[sample_count - 1],
                     tuple->mac[0], tuple->mac[1], tuple->mac[2],
                     tuple->mac[3], tuple->mac[4], tuple->mac[5]);

            int len = synth_format_mac_ecg_batch(uplink->tx_buffer, sizeof(uplink->tx_buffer), tuple, samples, sample_count, "gw-01");
            if (len > 0 && tb_mqtt_is_connected()) {
                tb_mqtt_publish("v1/gateway/telemetry", uplink->tx_buffer, 0);
            }
        }
        break;
    }

    default:
        break;
    }
}

void cloud_uplink_on_mqtt_connected(cloud_uplink_t *uplink) {
    cloud_mqtt_evt_t evt = { .type = CLOUD_MQTT_EVT_CONNECTED, .msg_id = 0 };
    cloud_uplink_post_mqtt_event(uplink, &evt);
}

void cloud_uplink_on_mqtt_published(cloud_uplink_t *uplink, int msg_id) {
    cloud_mqtt_evt_t evt = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = msg_id };
    cloud_uplink_post_mqtt_event(uplink, &evt);
}

void cloud_uplink_dispatch_tuple(cloud_uplink_t *uplink, const frame_snapshot_tuple_t *tuple) {
    if (!uplink || !tuple) return;

    uint64_t current_utc_ms = 0;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    current_utc_ms = (uint64_t)((int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000));
#else
    current_utc_ms = tuple->timestamp_utc_ms;
#endif

    if (tuple->stream_type == STREAM_TYPE_STATUS) {
        uint8_t status = tuple->payload[0];
        bool is_conn = (status == 1);
        ESP_LOGI(TAG, "🟢 [NODE %u STATUS] %s | Session: %lu | MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 tuple->node_id, is_conn ? "CONNECTED" : "DISCONNECTED",
                 (unsigned long)tuple->session_id,
                 tuple->mac[0], tuple->mac[1], tuple->mac[2],
                 tuple->mac[3], tuple->mac[4], tuple->mac[5]);
        cloud_uplink_handle_node_status(uplink, tuple, is_conn);
    } else {
        cloud_uplink_process_telemetry(uplink, tuple, current_utc_ms);
    }
}

int cloud_uplink_find_slot_by_mac(const cloud_uplink_t *uplink, const uint8_t target_mac[6]) {
    if (!uplink || !target_mac) {
        return -1;
    }
    for (uint32_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        if (uplink->lifecycle.nodes[i].state == NODE_MQTT_STATE_ACTIVE &&
            memcmp(uplink->lifecycle.nodes[i].mac, target_mac, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int cloud_uplink_handle_downlink_attributes(cloud_uplink_t *uplink,
                                           const char *json_payload,
                                           uint8_t *out_uart_frame,
                                           size_t max_frame_len) {
    if (!uplink || !json_payload || !out_uart_frame || max_frame_len < (UART_FRAME_OVERHEAD + 1)) {
        return -1;
    }

    char device_mac_str[32] = {0};
    uint8_t cmd_buf[UART_MAX_PAYLOAD_LEN];
    uint8_t cmd_len = 0;

    int ret = downlink_synth_extract_device_and_cmd(json_payload,
                                                    device_mac_str, sizeof(device_mac_str),
                                                    cmd_buf, sizeof(cmd_buf), &cmd_len);
    if (ret != 0) {
        return -2;
    }

    uint8_t target_mac[6];
    if (mac_ascii_to_bytes(device_mac_str, target_mac) != 0) {
        return -3;
    }

    int slot_id = cloud_uplink_find_slot_by_mac(uplink, target_mac);
    if (slot_id < 0) {
        return -4; // Target node not connected
    }

    int frame_len = uart_frame_pack(out_uart_frame, max_frame_len,
                                    (uint8_t)slot_id, UART_PKT_TYPE_DOWNLINK,
                                    cmd_buf, cmd_len);
    return frame_len;
}

/* ══════════════════════════════════════════════════════════════
 *  Hybrid Facade Implementation (Default Module Instance)
 * ════════════════════════════════════════════════════════════ */
static cloud_uplink_t s_default_uplink;

cloud_uplink_t* cloud_uplink_get_default_instance(void) {
    return &s_default_uplink;
}

void cloud_uplink_default_init(void) {
    cloud_uplink_init(&s_default_uplink);
}

void cloud_uplink_on_mqtt_connected_cb(void) {
    cloud_uplink_on_mqtt_connected(&s_default_uplink);
}

void cloud_uplink_on_mqtt_published_cb(int msg_id) {
    cloud_uplink_on_mqtt_published(&s_default_uplink, msg_id);
}

void cloud_uplink_default_process_events(void) {
    cloud_uplink_process_events(&s_default_uplink);
}

void cloud_uplink_default_dispatch(const frame_snapshot_tuple_t *tuple) {
    cloud_uplink_dispatch_tuple(&s_default_uplink, tuple);
}

int cloud_uplink_default_find_slot_by_mac(const uint8_t target_mac[6]) {
    return cloud_uplink_find_slot_by_mac(&s_default_uplink, target_mac);
}
