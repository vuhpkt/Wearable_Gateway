#include <string.h>
#include <stdio.h>
#include "gateway_ingest.h"

#if defined(ESP_PLATFORM) && !defined(HOST_UNIT_TEST)
#include "esp_log.h"
#include "esp_timer.h"
static const char *TAG = "gateway_ingest";
#else
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
static uint64_t s_mock_timer_ms = 1000;
#define esp_timer_get_time() ((int64_t)(s_mock_timer_ms * 1000ULL))
#endif

void gateway_ingest_init(gateway_ingest_t *ingest,
                         frame_snapshot_queue_t *queue,
                         revocation_table_t *rev,
                         gateway_time_t *gt,
                         gateway_time_mailbox_t *time_mb) {
    if (!ingest) return;
    memset(ingest, 0, sizeof(*ingest));
    uart_bridge_fsm_init(&ingest->bridge_fsm, 1000);
    core1_gate_init(&ingest->core1_gate, 1000);
    ingest->snapshot_queue = queue;
    ingest->rev_table = rev;
    ingest->gateway_time = gt;
    ingest->time_mailbox = time_mb;
    ingest->last_sync_req_mono = -100000;
}

bool gateway_ingest_is_link_ready(const gateway_ingest_t *ingest) {
    if (!ingest) return false;
    return (ingest->bridge_fsm.state == BRIDGE_LINK_READY);
}

void gateway_ingest_step_retry(gateway_ingest_t *ingest, int64_t now_mono_ms) {
    if (!ingest) return;

    /* Drain pending SNTP observations from mailbox */
    if (ingest->gateway_time && ingest->time_mailbox) {
        gateway_time_poll_and_apply_mailbox(ingest->gateway_time, ingest->time_mailbox);
    }

    if (ingest->bridge_fsm.state != BRIDGE_LINK_READY) {
        if (now_mono_ms - ingest->last_sync_req_mono >= 2000) {
            ingest->last_sync_req_mono = now_mono_ms;
            uart_bridge_send_sync_req();
        }
    }
}

void gateway_ingest_on_frame(gateway_ingest_t *ingest, const uart_rx_frame_t *frame) {
    if (!ingest || !frame) return;

    /* Apply any pending SNTP updates from Core 0 before mapping */
    if (ingest->gateway_time && ingest->time_mailbox) {
        gateway_time_poll_and_apply_mailbox(ingest->gateway_time, ingest->time_mailbox);
    }

    /* 1. Bridge Control Packets */
    if (frame->pkt_type == UART_PKT_TYPE_CONTROL_HELLO) {
        if (frame->len >= sizeof(uart_ctrl_hello_pkt_t)) {
            uart_ctrl_sync_pkt_t sync_out;
            const uart_ctrl_hello_pkt_t *hello = (const uart_ctrl_hello_pkt_t *)frame->payload;
            int ret = uart_bridge_handle_hello(&ingest->bridge_fsm, hello, &sync_out);
            if (ret == 0) {
                core1_gate_set_epoch(&ingest->core1_gate, sync_out.bridge_epoch);
                if (ingest->gateway_time) {
                    gateway_time_set_epoch(ingest->gateway_time, sync_out.bridge_epoch);
                }
                if (ingest->rev_table) {
                    revocation_table_reset_epoch(ingest->rev_table, sync_out.bridge_epoch);
                }
                uart_bridge_tx_frame(0, UART_PKT_TYPE_CONTROL_SYNC, &sync_out, sizeof(sync_out));
                ESP_LOGI(TAG, "🤝 Handshake synced: Central Nonce=0x%08lX | Epoch=%llu",
                         (unsigned long)sync_out.boot_nonce, (unsigned long long)sync_out.bridge_epoch);

                /* Request slot snapshot from Central */
                uart_snapshot_req_pkt_t req;
                uart_bridge_request_snapshot(&ingest->bridge_fsm, &req);
                uart_bridge_tx_frame(0, UART_PKT_TYPE_SNAPSHOT_REQ, &req, sizeof(req));
            }
        }
        return;
    }

    if (frame->pkt_type == UART_PKT_TYPE_SNAPSHOT_END) {
        if (frame->len >= sizeof(uart_snapshot_end_pkt_t)) {
            const uart_snapshot_end_pkt_t *end = (const uart_snapshot_end_pkt_t *)frame->payload;
            int ret = uart_bridge_handle_snapshot_end(&ingest->bridge_fsm, end);
            if (ret == 0) {
                ESP_LOGI(TAG, "📋 Snapshot committed: %u slots active (Ingest OPEN)", end->slot_count);
            }
        }
        return;
    }

    /* 2. Node Status (Connect / Disconnect) */
    if (frame->pkt_type == UART_PKT_TYPE_NODE_STATUS || frame->pkt_type == UART_PKT_TYPE_NODE_STATUS_V2) {
        frame_snapshot_tuple_t tuple;
        memset(&tuple, 0, sizeof(tuple));
        tuple.node_id = frame->node_id;
        tuple.stream_type = STREAM_TYPE_STATUS;
        tuple.esp_rx_mono_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

        if (frame->len >= sizeof(uart_v2_envelope_t) + sizeof(uart_node_status_pkt_t)) {
            const uart_v2_envelope_t *env = (const uart_v2_envelope_t *)frame->payload;
            const uart_node_status_pkt_t *status = (const uart_node_status_pkt_t *)(frame->payload + sizeof(uart_v2_envelope_t));

            int ret = core1_gate_handle_status(&ingest->core1_gate, frame->node_id, env, status);
            if (ret != CORE1_GATE_OK) {
                ESP_LOGW(TAG, "Core 1 Gate rejected status frame for slot %u (rc=%d)", frame->node_id, ret);
                return;
            }

            if (ingest->rev_table) {
                if (status->status == 0) {
                    revocation_table_revoke_slot(ingest->rev_table, frame->node_id, env->bridge_epoch, env->session_id);
                } else {
                    revocation_table_bind_session(ingest->rev_table, frame->node_id, env->bridge_epoch, env->session_id);
                }
            }

            uart_bridge_handle_snapshot_slot(&ingest->bridge_fsm, frame->node_id, env->bridge_epoch);

            uint64_t event_utc_ms = 0;
            uint8_t quality = TIME_QUALITY_UNKNOWN;
            uint16_t uncertainty = 0;

            if (ingest->gateway_time) {
                gateway_time_map_tuple(ingest->gateway_time, env->bridge_epoch,
                                       env->central_rx_mono_ms, tuple.esp_rx_mono_ms,
                                       &event_utc_ms, &quality, &uncertainty);
            }

            memcpy(tuple.mac, status->mac, sizeof(tuple.mac));
            tuple.session_id = env->session_id;
            tuple.bridge_epoch = env->bridge_epoch;
            tuple.central_rx_mono_ms = env->central_rx_mono_ms;
            tuple.timestamp_utc_ms = event_utc_ms;
            tuple.time_quality = quality;
            tuple.uncertainty_ms = uncertainty;
            tuple.seq = env->seq;
            tuple.payload_len = sizeof(uart_node_status_pkt_t);
            memcpy(tuple.payload, status, sizeof(uart_node_status_pkt_t));
        } else {
            tuple.session_id = 1;
            size_t copy_len = (frame->len > sizeof(tuple.payload)) ? sizeof(tuple.payload) : frame->len;
            tuple.payload_len = (uint8_t)copy_len;
            memcpy(tuple.payload, frame->payload, copy_len);
        }

        if (ingest->snapshot_queue) {
            if (!frame_snapshot_queue_push_control(ingest->snapshot_queue, &tuple)) {
                ESP_LOGW(TAG, "⚠️ [CONTROL QUEUE FULL] Dropped control frame for slot %u (type=%u) -> Triggering Snapshot Resync",
                         frame->node_id, tuple.stream_type);
                uart_snapshot_req_pkt_t req;
                uart_bridge_request_snapshot(&ingest->bridge_fsm, &req);
                uart_bridge_tx_frame(0, UART_PKT_TYPE_SNAPSHOT_REQ, &req, sizeof(req));
            }
        }
        return;
    }

    /* 3. Telemetry Frames (Vitals & ECG Batch) */
    if (frame->pkt_type == UART_PKT_TYPE_VITALS || frame->pkt_type == UART_PKT_TYPE_VITALS_V2 ||
        frame->pkt_type == UART_PKT_TYPE_ECG_BATCH || frame->pkt_type == UART_PKT_TYPE_ECG_V2) {
        frame_snapshot_tuple_t tuple;
        memset(&tuple, 0, sizeof(tuple));
        tuple.node_id = frame->node_id;
        tuple.stream_type = (frame->pkt_type == UART_PKT_TYPE_VITALS || frame->pkt_type == UART_PKT_TYPE_VITALS_V2) ? STREAM_TYPE_VITALS : STREAM_TYPE_ECG;
        tuple.esp_rx_mono_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

        if (frame->len >= sizeof(uart_v2_envelope_t)) {
            const uart_v2_envelope_t *env = (const uart_v2_envelope_t *)frame->payload;
            uint8_t mac[sizeof(tuple.mac)];

            int ret = core1_gate_validate_telemetry(&ingest->core1_gate, frame->node_id, env, mac);
            if (ret != CORE1_GATE_OK) {
                if (core1_gate_is_snapshot_requested(&ingest->core1_gate)) {
                    uart_snapshot_req_pkt_t req;
                    uart_bridge_request_snapshot(&ingest->bridge_fsm, &req);
                    uart_bridge_tx_frame(0, UART_PKT_TYPE_SNAPSHOT_REQ, &req, sizeof(req));
                    core1_gate_clear_snapshot_request(&ingest->core1_gate);
                }
                return;
            }

            if (!uart_bridge_is_ingest_allowed(&ingest->bridge_fsm, env->bridge_epoch)) {
                return;
            }

            uint64_t event_utc_ms = 0;
            uint8_t quality = TIME_QUALITY_UNKNOWN;
            uint16_t uncertainty = 0;

            if (ingest->gateway_time) {
                gateway_time_map_tuple(ingest->gateway_time, env->bridge_epoch,
                                       env->central_rx_mono_ms, tuple.esp_rx_mono_ms,
                                       &event_utc_ms, &quality, &uncertainty);
            }

            memcpy(tuple.mac, mac, sizeof(tuple.mac));
            tuple.session_id = env->session_id;
            tuple.bridge_epoch = env->bridge_epoch;
            tuple.central_rx_mono_ms = env->central_rx_mono_ms;
            tuple.timestamp_utc_ms = event_utc_ms;
            tuple.time_quality = quality;
            tuple.uncertainty_ms = uncertainty;
            tuple.seq = env->seq;

            size_t payload_bytes = frame->len - sizeof(uart_v2_envelope_t);
            if (payload_bytes > sizeof(tuple.payload)) {
                payload_bytes = sizeof(tuple.payload);
            }
            tuple.payload_len = (uint8_t)payload_bytes;
            memcpy(tuple.payload, frame->payload + sizeof(uart_v2_envelope_t), tuple.payload_len);
        } else {
            tuple.session_id = 1;
            size_t copy_len = (frame->len > sizeof(tuple.payload)) ? sizeof(tuple.payload) : frame->len;
            tuple.payload_len = (uint8_t)copy_len;
            memcpy(tuple.payload, frame->payload, copy_len);
        }

        if (ingest->snapshot_queue) {
            frame_snapshot_queue_push(ingest->snapshot_queue, &tuple);
        }
        return;
    }
}

#ifndef HOST_UNIT_TEST
#include "esp_timer.h"

static gateway_ingest_t *s_active_ingest = NULL;

static void on_uart_frame_received(const uart_rx_frame_t *frame) {
    if (s_active_ingest) {
        gateway_ingest_on_frame(s_active_ingest, frame);
    }
}

void gateway_ingest_task(void *pvParameters) {
    gateway_ingest_t *ingest = (gateway_ingest_t *)pvParameters;
    s_active_ingest = ingest;

    uart_bridge_init();
    uart_rx_parser_init(on_uart_frame_received);
    static uint8_t rx_buf[4096];

    while (1) {
        int64_t now_mono = (int64_t)(esp_timer_get_time() / 1000ULL);
        gateway_ingest_step_retry(ingest, now_mono);

        int len = uart_bridge_rx_read(rx_buf, sizeof(rx_buf), 20);
        if (len > 0) {
            uart_rx_parser_process(rx_buf, len);
        }
    }
}
#endif
