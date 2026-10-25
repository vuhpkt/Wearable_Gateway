#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "cloud_uplink.h"
#include "uart_bridge.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include "esp_log.h"
#include "esp_timer.h"
static const char *TAG = "cloud_uplink";
#else
#define ESP_LOGD(tag, fmt, ...) ((void)0)
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
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portMUX_INITIALIZE(&uplink->mux);
#endif
    mqtt_lifecycle_init(&uplink->lifecycle);
    for (uint32_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        uplink->ecg_stream_enabled[i] = true;
        uplink->node_explicitly_disconnected[i] = false;
    }
}

bool cloud_uplink_post_mqtt_event(cloud_uplink_t *uplink, const cloud_mqtt_evt_t *evt) {
    if (!uplink || !evt) return false;

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL(&uplink->mux);
#endif
    if (uplink->evt_count >= CLOUD_UPLINK_EVT_QUEUE_SIZE) {
        uplink->evt_overflow_count++;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portEXIT_CRITICAL(&uplink->mux);
#endif
        ESP_LOGW(TAG, "⚠️ [MQTT EVT OVERFLOW] Event queue full (count=%u), dropped event type=%d",
                 uplink->evt_count, evt->type);
        return false;
    }

    uplink->evt_queue[uplink->evt_tail] = *evt;
    uplink->evt_tail = (uplink->evt_tail + 1) % CLOUD_UPLINK_EVT_QUEUE_SIZE;
    uplink->evt_count++;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL(&uplink->mux);
#endif
    return true;
}

void cloud_uplink_process_events(cloud_uplink_t *uplink) {
    if (!uplink) return;

    while (1) {
        cloud_mqtt_evt_t evt;
        bool has_evt = false;

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portENTER_CRITICAL(&uplink->mux);
#endif
        if (uplink->evt_count > 0) {
            evt = uplink->evt_queue[uplink->evt_head];
            uplink->evt_head = (uplink->evt_head + 1) % CLOUD_UPLINK_EVT_QUEUE_SIZE;
            uplink->evt_count--;
            has_evt = true;
        }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portEXIT_CRITICAL(&uplink->mux);
#endif

        if (!has_evt) {
            break;
        }

        switch (evt.type) {
        case CLOUD_MQTT_EVT_CONNECTED:
            ESP_LOGI(TAG, "🔄 MQTT (Re)Connected: Resetting node lifecycle state & requesting Central snapshot");
            mqtt_lifecycle_on_broker_connected(&uplink->lifecycle);
            for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
                uplink->pending_announces[i].has_pending = false;
                uplink->node_explicitly_disconnected[i] = false;
            }
            uart_bridge_trigger_snapshot_sync();
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
                        if (attr_msg_id > 0) {
                            mqtt_lifecycle_set_attributes_msg_id(&uplink->lifecycle, ack_node_id, attr_msg_id);
                        } else {
                            ESP_LOGW(TAG, "⚠️ [NODE %u ATTR] Failed to publish attributes (err=%d); reverting to IDLE",
                                     ack_node_id, attr_msg_id);
                            uplink->lifecycle.nodes[ack_node_id].state = NODE_MQTT_STATE_IDLE;
                            uplink->pending_announces[ack_node_id].has_pending = false;
                        }
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

    node_mqtt_tracker_t *n = &uplink->lifecycle.nodes[tuple->node_id];
    frame_snapshot_tuple_t local_tuple = *tuple;

    if (!is_valid_mac_address(local_tuple.mac)) {
        /* If Central reports slot disconnected/empty with zero MAC, but our tracker
         * still has an ACTIVE or in-flight connection for this slot, use the known MAC
         * to cleanly disconnect the node and notify ThingsBoard. */
        if (!is_connected && n->state != NODE_MQTT_STATE_IDLE && is_valid_mac_address(n->mac)) {
            memcpy(local_tuple.mac, n->mac, 6);
        } else {
            return;
        }
    }

    const char *topic = NULL;

    if (is_connected) {
        /* Idempotence Guard:
         * If the node is already ACTIVE with the identical MAC and session_id
         * in the current MQTT generation, DO NOT DEMOTE OR RE-ANNOUNCE! */
        if (n->state == NODE_MQTT_STATE_ACTIVE &&
            n->generation == uplink->lifecycle.mqtt_generation &&
            n->session_id == local_tuple.session_id &&
            memcmp(n->mac, local_tuple.mac, 6) == 0) {
            ESP_LOGD(TAG, "🟢 [NODE %u STATUS] Already ACTIVE (sess=%lu), ignoring redundant connect",
                     tuple->node_id, (unsigned long)local_tuple.session_id);
            return;
        }

        /* In-flight Handshake Guard:
         * If an announce is already in flight for this node with the same MAC, session,
         * and generation, DO NOT overwrite msg_id and reset state! */
        if ((n->state == NODE_MQTT_STATE_WAIT_CONNECT_ACK || n->state == NODE_MQTT_STATE_WAIT_ATTR_ACK) &&
            n->generation == uplink->lifecycle.mqtt_generation &&
            n->session_id == local_tuple.session_id &&
            memcmp(n->mac, local_tuple.mac, 6) == 0) {
            ESP_LOGD(TAG, "🟢 [NODE %u STATUS] Announce already in flight (state=%d, sess=%lu), ignoring duplicate",
                     tuple->node_id, (int)n->state, (unsigned long)local_tuple.session_id);
            return;
        }

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portENTER_CRITICAL(&uplink->mux);
#endif
        uplink->node_explicitly_disconnected[tuple->node_id] = false;
        uplink->ecg_stream_enabled[tuple->node_id] = true; /* Reset to enabled for new connection */
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portEXIT_CRITICAL(&uplink->mux);
#endif

        int len = synth_format_mac_connect(uplink->tx_buffer, sizeof(uplink->tx_buffer), local_tuple.mac, &topic);
        if (len > 0 && topic != NULL) {
            int msg_id = tb_mqtt_publish(topic, uplink->tx_buffer, 1);
            if (msg_id > 0) {
                ESP_LOGI(TAG, "📤 [NODE %u CONNECT] Published to %s (msg_id=%d)", tuple->node_id, topic, msg_id);
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
                portENTER_CRITICAL(&uplink->mux);
#endif
                mqtt_lifecycle_start_announce(&uplink->lifecycle, tuple->node_id, local_tuple.mac, local_tuple.session_id, msg_id);
                uplink->pending_announces[tuple->node_id].tuple = local_tuple;
                uplink->pending_announces[tuple->node_id].has_pending = true;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
                portEXIT_CRITICAL(&uplink->mux);
#endif
            } else {
                ESP_LOGW(TAG, "⚠️ [NODE %u CONNECT] Failed to publish connect to %s (err=%d)", tuple->node_id, topic, msg_id);
            }
            /* Note: Attributes are published ONLY after connect ACK in on_puback, strictly adhering to barrier */
        }
    } else {
        /* Disconnect Guard: If node is already IDLE and marked disconnected, ignore redundant disconnect frames */
        if (n->state == NODE_MQTT_STATE_IDLE && uplink->node_explicitly_disconnected[tuple->node_id]) {
            return;
        }

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portENTER_CRITICAL(&uplink->mux);
#endif
        uplink->node_explicitly_disconnected[tuple->node_id] = true;
        uplink->ecg_stream_enabled[tuple->node_id] = true; /* Reset to default on disconnect */
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portEXIT_CRITICAL(&uplink->mux);
#endif
        /* Immediate offline attribute latch */
        int attr_len = synth_format_mac_attributes(uplink->attr_buffer, sizeof(uplink->attr_buffer), &local_tuple, false, "gw-01");
        if (attr_len > 0) {
            tb_mqtt_publish("v1/gateway/attributes", uplink->attr_buffer, 1);
            ESP_LOGI(TAG, "📤 [NODE %u DISCONNECT ATTR] Published bleConnected=false to v1/gateway/attributes", tuple->node_id);
        }

        int len = synth_format_mac_disconnect(uplink->tx_buffer, sizeof(uplink->tx_buffer), local_tuple.mac, &topic);
        if (len > 0 && topic != NULL) {
            tb_mqtt_publish(topic, uplink->tx_buffer, 1);
            ESP_LOGI(TAG, "📤 [NODE %u DISCONNECT] Published to %s", tuple->node_id, topic);
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
            portENTER_CRITICAL(&uplink->mux);
#endif
            mqtt_lifecycle_handle_node_disconnect(&uplink->lifecycle, tuple->node_id);
            uplink->pending_announces[tuple->node_id].has_pending = false;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
            portEXIT_CRITICAL(&uplink->mux);
#endif
        } else {
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
            portENTER_CRITICAL(&uplink->mux);
#endif
            mqtt_lifecycle_handle_node_disconnect(&uplink->lifecycle, tuple->node_id);
            uplink->pending_announces[tuple->node_id].has_pending = false;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
            portEXIT_CRITICAL(&uplink->mux);
#endif
        }
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
        const node_mqtt_tracker_t *n = &uplink->lifecycle.nodes[tuple->node_id];
        if (!mqtt_lifecycle_is_node_active(&uplink->lifecycle, tuple->node_id, tuple->session_id)) {
            if (n->state == NODE_MQTT_STATE_IDLE &&
                !uplink->node_explicitly_disconnected[tuple->node_id] &&
                uplink->lifecycle.mqtt_connected &&
                tuple->session_id != 0 &&
                is_valid_mac_address(tuple->mac)) {
                ESP_LOGI(TAG, "📢 [NODE %u] Active stream received while unannounced on MQTT; triggering auto-announce...", tuple->node_id);
                cloud_uplink_handle_node_status(uplink, tuple, true);
            } else {
                ESP_LOGW(TAG, "⏱️ [NODE %u STREAM %u] Dropped: Inactive (state=%d, node_sess=%lu, frame_sess=%lu, gen=%lu/%lu)",
                         tuple->node_id, tuple->stream_type, (int)n->state,
                         (unsigned long)n->session_id, (unsigned long)tuple->session_id,
                         (unsigned long)n->generation, (unsigned long)uplink->lifecycle.mqtt_generation);
            }
        } else if (tuple->timestamp_utc_ms == 0 || tuple->timestamp_utc_ms > current_utc_ms + 2000ULL) {
            ESP_LOGW(TAG, "⏱️ [NODE %u STREAM %u] Dropped: Future/Invalid TS (event=%llu, now=%llu)",
                     tuple->node_id, tuple->stream_type,
                     (unsigned long long)tuple->timestamp_utc_ms, (unsigned long long)current_utc_ms);
        } else {
            uint64_t age = (current_utc_ms >= tuple->timestamp_utc_ms) ? (current_utc_ms - tuple->timestamp_utc_ms) : 0;
            (void)age;
            ESP_LOGW(TAG, "⏱️ [NODE %u STREAM %u] Dropped: TTL expired (age=%llums >= 5000ms)",
                     tuple->node_id, tuple->stream_type, (unsigned long long)age);
        }
        return;
    }

    switch (tuple->stream_type) {
    case STREAM_TYPE_VITALS: {
        if (tuple->payload_len >= sizeof(vitals_packet_t)) {
            const vitals_packet_t *v = (const vitals_packet_t *)tuple->payload;
            float temp_c = (float)v->temp_x10 / 10.0f;
            (void)temp_c;
            if (v->battery <= 100) {
                ESP_LOGI(TAG, "💓 [NODE %u VITALS] HR_ECG: %u bpm | HR_PPG: %u bpm | SpO2: %u%% | Temp: %.1f°C | Bat: %u%% | MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                         tuple->node_id, v->hr_ecg, v->hr_ppg, v->spo2, temp_c, v->battery,
                         tuple->mac[0], tuple->mac[1], tuple->mac[2],
                         tuple->mac[3], tuple->mac[4], tuple->mac[5]);
            } else {
                ESP_LOGI(TAG, "💓 [NODE %u VITALS] HR_ECG: %u bpm | HR_PPG: %u bpm | SpO2: %u%% | Temp: %.1f°C | Bat: N/A | MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                         tuple->node_id, v->hr_ecg, v->hr_ppg, v->spo2, temp_c,
                         tuple->mac[0], tuple->mac[1], tuple->mac[2],
                         tuple->mac[3], tuple->mac[4], tuple->mac[5]);
            }

            int len = synth_format_mac_vitals(uplink->tx_buffer, sizeof(uplink->tx_buffer), tuple, v, "gw-01");
            if (len > 0 && tb_mqtt_is_connected()) {
                tb_mqtt_publish("v1/gateway/telemetry", uplink->tx_buffer, 0);
            }
        }
        break;
    }

    case STREAM_TYPE_ECG: {
        bool ecg_enabled = true;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portENTER_CRITICAL(&uplink->mux);
        ecg_enabled = uplink->ecg_stream_enabled[tuple->node_id];
        portEXIT_CRITICAL(&uplink->mux);
#else
        ecg_enabled = uplink->ecg_stream_enabled[tuple->node_id];
#endif
        if (!ecg_enabled) {
            return; /* Gateway-level Gating: drop ECG telemetry when stream is disabled */
        }
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
    uint64_t current_mono_ms = 0;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    current_utc_ms = (uint64_t)((int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000));
    current_mono_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
#else
    current_utc_ms = tuple->timestamp_utc_ms;
    current_mono_ms = tuple->esp_rx_mono_ms;
#endif

    mqtt_lifecycle_check_timeouts(&uplink->lifecycle, current_mono_ms, 5000);
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        if (uplink->lifecycle.nodes[i].state == NODE_MQTT_STATE_IDLE && uplink->pending_announces[i].has_pending) {
            uplink->pending_announces[i].has_pending = false;
        }
    }

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
    int slot = -1;
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portENTER_CRITICAL((portMUX_TYPE *)&uplink->mux);
#endif
    for (uint32_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        if (uplink->lifecycle.nodes[i].state != NODE_MQTT_STATE_IDLE &&
            memcmp(uplink->lifecycle.nodes[i].mac, target_mac, 6) == 0) {
            slot = (int)i;
            break;
        }
    }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
    portEXIT_CRITICAL((portMUX_TYPE *)&uplink->mux);
#endif
    return slot;
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

    if (cmd_buf[0] == CMD_MODE_CFG && cmd_len >= 7) {
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portENTER_CRITICAL(&uplink->mux);
#endif
        if (strstr(json_payload, "\"showEcg\"") != NULL || strstr(json_payload, "\"ecgEnabled\"") != NULL) {
            uplink->ecg_stream_enabled[slot_id] = (cmd_buf[6] != 0);
        } else {
            cmd_buf[6] = uplink->ecg_stream_enabled[slot_id] ? 1 : 0;
        }
#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
        portEXIT_CRITICAL(&uplink->mux);
#endif
    }

    int frame_len = uart_frame_pack(out_uart_frame, max_frame_len,
                                    (uint8_t)slot_id, UART_PKT_TYPE_DOWNLINK,
                                    cmd_buf, cmd_len);
    return frame_len;
}

void cloud_uplink_on_mqtt_data(cloud_uplink_t *uplink,
                               const char *topic, size_t topic_len,
                               const char *data, size_t data_len) {
    if (!uplink || !topic || !data || data_len == 0) return;
    if (topic_len != strlen("v1/gateway/attributes") ||
        strncmp(topic, "v1/gateway/attributes", topic_len) != 0) {
        return;
    }

    char json_buf[512];
    if (data_len >= sizeof(json_buf)) {
        ESP_LOGW(TAG, "⚠️ [DOWNLINK] Attributes payload too large (%u >= %u bytes), dropped",
                 (unsigned)data_len, (unsigned)sizeof(json_buf));
        return;
    }
    memcpy(json_buf, data, data_len);
    json_buf[data_len] = '\0';

    uint8_t uart_frame[UART_MAX_FRAME_LEN];
    int frame_len = cloud_uplink_handle_downlink_attributes(uplink, json_buf, uart_frame, sizeof(uart_frame));
    if (frame_len > 0) {
        int sent = uart_bridge_tx_raw(uart_frame, (size_t)frame_len);
        if (sent > 0) {
            ESP_LOGI(TAG, "📥 [DOWNLINK] Forwarded CMD 0x%02X to Slot %u (%d bytes)",
                     uart_frame[4], uart_frame[1], sent);
        } else {
            ESP_LOGE(TAG, "❌ [DOWNLINK] Failed to transmit UART frame (err=%d)", sent);
        }
    } else {
        ESP_LOGW(TAG, "⚠️ [DOWNLINK] Failed to process attributes (err=%d)", frame_len);
    }
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

void cloud_uplink_on_mqtt_data_cb(const char *topic, int topic_len, const char *data, int data_len) {
    if (topic_len > 0 && data_len > 0) {
        cloud_uplink_on_mqtt_data(&s_default_uplink, topic, (size_t)topic_len, data, (size_t)data_len);
    }
}
