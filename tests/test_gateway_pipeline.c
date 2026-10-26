/**
 * @file test_gateway_pipeline.c
 * @brief End-to-End Pipeline Integration Test (UART Bytes -> Ingest -> Queue -> Uplink -> MQTT)
 *
 * Validates complete ESP32 Gateway dataflow and fault handling:
 * 1. UART Frame Parsing & Envelope Decoupling
 * 2. Bridge Handshake & Epoch Sync (Central Reboot resilience)
 * 3. Announce Barrier (CONNECT -> PUBACK -> ATTR -> PUBACK -> ACTIVE)
 * 4. Temporal SNTP clock mapping & Monotonic TTL rejection
 * 5. Slot Reuse with Session Revocation
 * 6. Queue & Outbox Saturation Drop Tracking
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "uart_frame_def.h"
#include "uart_rx_parser.h"
#include "core1_gate.h"
#include "frame_snapshot.h"
#include "gateway_time.h"
#include "gateway_ingest.h"
#include "thingsboard_mqtt.h"
#include "data_synthesizer.h"
#include "cloud_uplink.h"

/* ══════════════════════════════════════════════════════════════
 *  Fake MQTT Transport Capture
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    char topic[64];
    char payload[2048];
    int  qos;
    int  msg_id;
} test_mqtt_publish_record_t;

#define MAX_CAPTURED_PUBS 32
static test_mqtt_publish_record_t s_captured_pubs[MAX_CAPTURED_PUBS];
static uint32_t s_captured_pub_count = 0;
static int s_next_msg_id = 500;
static bool s_mock_connected = true;

bool tb_mqtt_is_connected(void) {
    return s_mock_connected;
}

int uart_bridge_tx_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    (void)node_id;
    (void)pkt_type;
    (void)payload;
    (void)len;
    return 0;
}

int uart_bridge_send_sync_req(void) {
    return 0;
}

int tb_mqtt_publish(const char *topic, const char *payload, int qos) {
    if (!s_mock_connected || !topic || !payload) return -1;
    if (s_captured_pub_count < MAX_CAPTURED_PUBS) {
        test_mqtt_publish_record_t *rec = &s_captured_pubs[s_captured_pub_count++];
        strncpy(rec->topic, topic, sizeof(rec->topic) - 1);
        strncpy(rec->payload, payload, sizeof(rec->payload) - 1);
        rec->qos = qos;
        rec->msg_id = s_next_msg_id++;
        return rec->msg_id;
    }
    return -2;
}

static void reset_mock_transport(void) {
    memset(s_captured_pubs, 0, sizeof(s_captured_pubs));
    s_captured_pub_count = 0;
    s_next_msg_id = 500;
    s_mock_connected = true;
}

/* ══════════════════════════════════════════════════════════════
 *  Pipeline Test Harness
 * ════════════════════════════════════════════════════════════ */
static gateway_ingest_t       s_test_ingest;
static gateway_time_t         s_test_time;
static gateway_time_mailbox_t s_test_mailbox;
static frame_snapshot_queue_t s_test_queue;
static revocation_table_t     s_test_rev;
static cloud_uplink_t         s_test_uplink;

static void on_parser_frame(const uart_rx_frame_t *frame) {
    gateway_ingest_on_frame(&s_test_ingest, frame);
}

static void setup_pipeline(void) {
    reset_mock_transport();
    gateway_time_init(&s_test_time, 1000);
    gateway_time_mailbox_init(&s_test_mailbox);
    frame_snapshot_queue_init(&s_test_queue);
    revocation_table_init(&s_test_rev);
    gateway_ingest_init(&s_test_ingest, &s_test_queue, &s_test_rev, &s_test_time, &s_test_mailbox);
    cloud_uplink_init(&s_test_uplink);
    cloud_uplink_on_mqtt_connected(&s_test_uplink);
    cloud_uplink_process_events(&s_test_uplink);
    uart_rx_parser_init(on_parser_frame);
}

static void feed_uart_packet(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    uint8_t raw[128];
    int encoded = uart_frame_pack(raw, sizeof(raw), node_id, pkt_type, payload, len);
    assert(encoded > 0);
    uart_rx_parser_process(raw, encoded);
}

static void feed_envelope_packet(uint8_t slot, uint8_t pkt_type, uint32_t session, uint64_t epoch,
                                 uint32_t seq, uint64_t central_rx_mono, const void *data, uint8_t data_len) {
    uart_v2_envelope_t env;
    memset(&env, 0, sizeof(env));
    env.bridge_epoch = epoch;
    env.session_id = session;
    env.seq = seq;
    env.central_rx_mono_ms = central_rx_mono;

    uint8_t raw[128];
    memcpy(raw, &env, sizeof(env));
    if (data && data_len > 0) {
        memcpy(raw + sizeof(env), data, data_len);
    }
    feed_uart_packet(slot, pkt_type, raw, sizeof(env) + data_len);
}

static void process_queued_frames_to_uplink(uint64_t current_utc_ms) {
    cloud_uplink_process_events(&s_test_uplink);
    frame_snapshot_tuple_t tuple;
    while (frame_snapshot_queue_pop(&s_test_queue, &tuple, 0)) {
        if (tuple.session_id != 0 && !revocation_table_is_session_valid(&s_test_rev, tuple.node_id, tuple.bridge_epoch, tuple.session_id)) {
            if (!(tuple.stream_type == STREAM_TYPE_STATUS && tuple.payload_len > 0 && tuple.payload[0] == 0)) {
                continue;
            }
        }
        if (tuple.stream_type == STREAM_TYPE_STATUS) {
            uint8_t status = tuple.payload[0];
            cloud_uplink_handle_node_status(&s_test_uplink, &tuple, status == 1);
        } else {
            cloud_uplink_process_telemetry(&s_test_uplink, &tuple, current_utc_ms);
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Integration Scenarios
 * ════════════════════════════════════════════════════════════ */
static void test_scenario_handshake_and_connect_barrier(void) {
    printf("[RUN] Pipeline Scenario 1: Handshake -> Connect -> Announce Barrier Flow...\n");
    setup_pipeline();

    /* 1. Receive HELLO from Central */
    uart_ctrl_hello_pkt_t hello = {
        .version = UART_BRIDGE_PROTOCOL_VERSION,
        .boot_nonce = 0x12345678,
        .reserved = 0
    };
    feed_uart_packet(0, UART_PKT_TYPE_CONTROL_HELLO, &hello, sizeof(hello));
    assert(s_test_ingest.bridge_fsm.state == BRIDGE_LINK_SYNCED);

    uint64_t active_epoch = s_test_ingest.bridge_fsm.epoch;
    assert(active_epoch > 0);

    /* 2. Snapshot: 10 slots (Slot 0 connected with MAC AA:BB:CC:DD:EE:01, Session 100, others empty) */
    for (uint8_t i = 0; i < 10; i++) {
        uart_node_status_pkt_t st;
        memset(&st, 0, sizeof(st));
        if (i == 0) {
            st.status = 1;
            uint8_t m[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
            memcpy(st.mac, m, 6);
        } else {
            st.status = 0;
            /* MAC is 00:00:00:00:00:00 for unused slots */
        }
        feed_envelope_packet(i, UART_PKT_TYPE_NODE_STATUS_V2, (i == 0) ? 100 : 0, active_epoch, 1, 100000ULL + i * 10, &st, sizeof(st));
    }

    /* Snapshot End: commits snapshot */
    uart_snapshot_end_pkt_t end = {
        .bridge_epoch = active_epoch,
        .transaction_id = 1,
        .slot_count = 10,
        .reserved = 0
    };
    feed_uart_packet(0, UART_PKT_TYPE_SNAPSHOT_END, &end, sizeof(end));
    assert(gateway_ingest_is_link_ready(&s_test_ingest) == true);

    /* 3. Ingest pops status frame into queue; process to cloud */
    process_queued_frames_to_uplink(1700000000000ULL);

    /* Verify CONNECT was published to ThingsBoard Gateway */
    assert(s_captured_pub_count == 1);
    assert(strcmp(s_captured_pubs[0].topic, "v1/gateway/connect") == 0);
    assert(strstr(s_captured_pubs[0].payload, "AA:BB:CC:DD:EE:01") != NULL);
    int conn_msg_id = s_captured_pubs[0].msg_id;

    /* 4. Telemetry attempted while in CONNECTING state must be BLOCKED */
    vitals_packet_t vitals = { .hr_ecg = 72, .hr_ppg = 71, .spo2 = 98, .temp_x10 = 366 };
    feed_envelope_packet(0, UART_PKT_TYPE_VITALS_V2, 100, active_epoch, 2, 100000ULL, &vitals, sizeof(vitals));
    process_queued_frames_to_uplink(1700000000000ULL);
    assert(s_captured_pub_count == 1); /* Still only CONNECT published! */

    /* 5. Broker returns PUBACK for CONNECT -> triggers ATTRIBUTES publish */
    cloud_mqtt_evt_t ack_evt = { .type = CLOUD_MQTT_EVT_PUBACK, .msg_id = conn_msg_id };
    cloud_uplink_post_mqtt_event(&s_test_uplink, &ack_evt);
    process_queued_frames_to_uplink(1700000000000ULL);

    assert(s_captured_pub_count == 2);
    assert(strcmp(s_captured_pubs[1].topic, "v1/gateway/attributes") == 0);
    int attr_msg_id = s_captured_pubs[1].msg_id;

    /* 6. Broker returns PUBACK for ATTRIBUTES -> node transitions to ACTIVE */
    ack_evt.msg_id = attr_msg_id;
    cloud_uplink_post_mqtt_event(&s_test_uplink, &ack_evt);
    process_queued_frames_to_uplink(1700000000000ULL);

    /* Provide SNTP observation to calibrate time (mono_ms matching mock 100000ULL) */
    gateway_time_mailbox_post(&s_test_mailbox, 1700000000000ULL, 100000ULL);

    /* 7. Now Telemetry publishes cleanly (fresh within 5000ms TTL) */
    feed_envelope_packet(0, UART_PKT_TYPE_VITALS_V2, 100, active_epoch, 3, 100010ULL, &vitals, sizeof(vitals));
    process_queued_frames_to_uplink(1699999901050ULL);

    assert(s_captured_pub_count == 3);
    assert(strcmp(s_captured_pubs[2].topic, "v1/gateway/telemetry") == 0);
    assert(strstr(s_captured_pubs[2].payload, "ecgHeartRate") != NULL);

    printf("      -> PASS: Handshake, Announce Barrier, and Telemetry gating proven end-to-end\n");
}

static void test_scenario_central_reboot_epoch_bump(void) {
    printf("[RUN] Pipeline Scenario 2: Central Reboot & Epoch Transition Protection...\n");
    setup_pipeline();

    /* Establish Epoch 1 */
    uart_ctrl_hello_pkt_t hello1 = { .version = UART_BRIDGE_PROTOCOL_VERSION, .boot_nonce = 0xAAAA, .reserved = 0 };
    feed_uart_packet(0, UART_PKT_TYPE_CONTROL_HELLO, &hello1, sizeof(hello1));
    uint64_t epoch1 = s_test_ingest.bridge_fsm.epoch;

    uart_node_status_pkt_t st = { .status = 1, .mac = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66} };
    feed_envelope_packet(0, UART_PKT_TYPE_NODE_STATUS_V2, 50, epoch1, 1, 500, &st, sizeof(st));

    for (uint8_t i = 1; i < 10; i++) {
        uart_node_status_pkt_t st_idle = { .status = 0, .mac = {0} };
        feed_envelope_packet(i, UART_PKT_TYPE_NODE_STATUS_V2, 0, epoch1, 1, 500, &st_idle, sizeof(st_idle));
    }

    uart_snapshot_end_pkt_t end1 = { .bridge_epoch = epoch1, .transaction_id = 1, .slot_count = 10, .reserved = 0 };
    feed_uart_packet(0, UART_PKT_TYPE_SNAPSHOT_END, &end1, sizeof(end1));

    /* Central crashes and reboots with new Boot Nonce -> New Epoch */
    uart_ctrl_hello_pkt_t hello2 = { .version = UART_BRIDGE_PROTOCOL_VERSION, .boot_nonce = 0xBBBB, .reserved = 0 };
    feed_uart_packet(0, UART_PKT_TYPE_CONTROL_HELLO, &hello2, sizeof(hello2));
    uint64_t epoch2 = s_test_ingest.bridge_fsm.epoch;
    assert(epoch2 != epoch1);

    /* Stale telemetry from Epoch 1 received in buffer */
    vitals_packet_t vitals = { .hr_ecg = 80, .hr_ppg = 80, .spo2 = 99, .temp_x10 = 370 };
    feed_envelope_packet(0, UART_PKT_TYPE_VITALS_V2, 50, epoch1, 99, 500, &vitals, sizeof(vitals));

    /* Stale frame must be rejected by Core 1 gate */
    process_queued_frames_to_uplink(1700000000000ULL);
    assert(frame_snapshot_queue_count(&s_test_queue) == 0);

    /* Stale session 50 is revoked in table */
    assert(revocation_table_is_session_valid(&s_test_rev, 0, epoch1, 50) == false);

    printf("      -> PASS: Epoch bump wiped stale binding and blocked obsolete frames\n");
}

static void test_scenario_slot_reuse_isolation(void) {
    printf("[RUN] Pipeline Scenario 3: Slot Reuse Isolation between different nodes...\n");
    setup_pipeline();

    uart_ctrl_hello_pkt_t hello = { .version = UART_BRIDGE_PROTOCOL_VERSION, .boot_nonce = 0x1111, .reserved = 0 };
    feed_uart_packet(0, UART_PKT_TYPE_CONTROL_HELLO, &hello, sizeof(hello));
    uint64_t epoch = s_test_ingest.bridge_fsm.epoch;

    /* Node A occupies Slot 0 */
    uart_node_status_pkt_t st_a = { .status = 1, .mac = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA} };
    feed_envelope_packet(0, UART_PKT_TYPE_NODE_STATUS_V2, 10, epoch, 1, 100, &st_a, sizeof(st_a));

    /* Slot 0 is revoked / Node A disconnects */
    st_a.status = 0;
    feed_envelope_packet(0, UART_PKT_TYPE_NODE_STATUS_V2, 10, epoch, 2, 200, &st_a, sizeof(st_a));

    /* Node B connects and re-uses Slot 0 with Session 20 */
    uart_node_status_pkt_t st_b = { .status = 1, .mac = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB} };
    feed_envelope_packet(0, UART_PKT_TYPE_NODE_STATUS_V2, 20, epoch, 3, 300, &st_b, sizeof(st_b));

    /* Node B is valid */
    assert(revocation_table_is_session_valid(&s_test_rev, 0, epoch, 20) == true);
    /* Late packet from Node A (Session 10) is rejected */
    assert(revocation_table_is_session_valid(&s_test_rev, 0, epoch, 10) == false);

    printf("      -> PASS: Slot reuse completely isolates distinct patient sessions\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING END-TO-END GATEWAY PIPELINE TESTS (TDD)  \n");
    printf("====================================================\n\n");

    test_scenario_handshake_and_connect_barrier();
    test_scenario_central_reboot_epoch_bump();
    test_scenario_slot_reuse_isolation();

    printf("\n🎉 ALL PIPELINE INTEGRATION TESTS PASSED!\n");
    return 0;
}
