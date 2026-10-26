/**
 * @file test_gateway_ingest.c
 * @brief Host-based Unit Tests for Gateway Core 1 Ingest Engine (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Hello handshake -> Epoch set -> Snapshot requested.
 * 2. Status frame handling -> Core 1 gate binding + Revocation table update.
 * 3. Snapshot end -> Ingest opened -> Telemetry validated and pushed to queue.
 * 4. Periodic bridge SYNC retry progresses on Core 1 independent of network state.
 * 5. Pre-SNTP telemetry yields TIME_QUALITY_UNKNOWN; post-SNTP yields SYNCED.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "gateway_ingest.h"
#include "ble_protocol_def.h"

/* Mock Tx Frame tracking */
static uint8_t s_last_tx_type = 0;
static int s_tx_count = 0;

int uart_bridge_tx_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len) {
    (void)node_id;
    (void)payload;
    (void)len;
    s_last_tx_type = pkt_type;
    s_tx_count++;
    return 0;
}

int uart_bridge_send_sync_req(void) {
    s_last_tx_type = UART_PKT_TYPE_CONTROL_SYNC;
    s_tx_count++;
    return 0;
}

static void test_hello_and_snapshot_flow(void) {
    printf("[RUN] Test 1: HELLO handshake -> Snapshot -> Ingest OPEN...\n");
    gateway_ingest_t ingest;
    frame_snapshot_queue_t queue;
    revocation_table_t rev;
    gateway_time_t gt;
    gateway_time_mailbox_t mb;

    frame_snapshot_queue_init(&queue);
    revocation_table_init(&rev);
    gateway_time_init(&gt, 1000);
    gateway_time_mailbox_init(&mb);

    gateway_ingest_init(&ingest, &queue, &rev, &gt, &mb);

    s_tx_count = 0;

    /* 1. Send Central HELLO packet */
    uart_ctrl_hello_pkt_t hello = {
        .version = UART_BRIDGE_PROTOCOL_VERSION,
        .boot_nonce = 0x12345678,
        .reserved = 0,
    };
    uart_rx_frame_t frame_hello = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_CONTROL_HELLO,
        .len = sizeof(hello),
    };
    memcpy(frame_hello.payload, &hello, sizeof(hello));

    gateway_ingest_on_frame(&ingest, &frame_hello);

    /* Assert: SYNC and SNAPSHOT_REQ transmitted */
    assert(s_tx_count == 2);
    assert(s_last_tx_type == UART_PKT_TYPE_SNAPSHOT_REQ);
    assert(gateway_ingest_is_link_ready(&ingest) == false);

    /* 2. Send 10 Snapshot Status slots */
    uint64_t current_epoch = ingest.bridge_fsm.epoch;
    for (uint8_t i = 0; i < 10; i++) {
        uart_v2_envelope_t env = {
            .bridge_epoch = current_epoch,
            .session_id = (i == 0) ? 101 : 0,
            .seq = 1,
            .central_rx_mono_ms = 1000 + i * 10,
        };
        uart_node_status_pkt_t status = {
            .status = (i == 0) ? 1 : 0,
            .mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, (uint8_t)(0x10 + i) }
        };
        uart_rx_frame_t frame_slot = {
            .node_id = i,
            .pkt_type = UART_PKT_TYPE_NODE_STATUS_V2,
            .len = sizeof(env) + sizeof(status),
        };
        memcpy(frame_slot.payload, &env, sizeof(env));
        memcpy(frame_slot.payload + sizeof(env), &status, sizeof(status));

        gateway_ingest_on_frame(&ingest, &frame_slot);
    }

    /* 3. Send SNAPSHOT_END */
    uart_snapshot_end_pkt_t end_pkt = {
        .bridge_epoch = current_epoch,
        .transaction_id = 1,
        .slot_count = 10,
        .reserved = 0,
    };
    uart_rx_frame_t frame_end = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_SNAPSHOT_END,
        .len = sizeof(end_pkt),
    };
    memcpy(frame_end.payload, &end_pkt, sizeof(end_pkt));

    gateway_ingest_on_frame(&ingest, &frame_end);

    /* Assert: Link is now READY and Ingest is OPEN */
    assert(gateway_ingest_is_link_ready(&ingest) == true);
    printf("      -> PASS: Hello -> Snapshot sequence completed, ingest link ready\n");
}

static void test_telemetry_ingest_to_queue(void) {
    printf("[RUN] Test 2: Telemetry frame validated and pushed to snapshot queue...\n");
    gateway_ingest_t ingest;
    frame_snapshot_queue_t queue;
    revocation_table_t rev;
    gateway_time_t gt;
    gateway_time_mailbox_t mb;

    frame_snapshot_queue_init(&queue);
    revocation_table_init(&rev);
    gateway_time_init(&gt, 500ULL);
    gateway_time_mailbox_init(&mb);
    gateway_ingest_init(&ingest, &queue, &rev, &gt, &mb);

    /* Simulate link ready with epoch 500 */
    ingest.bridge_fsm.state = BRIDGE_LINK_READY;
    ingest.bridge_fsm.epoch = 500ULL;
    core1_gate_set_epoch(&ingest.core1_gate, 500ULL);

    /* Connect slot 0 with session 202 */
    uart_v2_envelope_t env_c = { .bridge_epoch = 500ULL, .session_id = 202, .seq = 1, .central_rx_mono_ms = 1000 };
    uart_node_status_pkt_t st = { .status = 1, .mac = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
    core1_gate_handle_status(&ingest.core1_gate, 0, &env_c, &st);

    /* Send Vitals telemetry BEFORE SNTP */
    vitals_packet_t v = { .hr_ecg = 72, .hr_ppg = 71, .spo2 = 98, .temp_x10 = 365 };
    uart_v2_envelope_t env_t1 = { .bridge_epoch = 500ULL, .session_id = 202, .seq = 2, .central_rx_mono_ms = 1020 };
    uart_rx_frame_t frame_v1 = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_VITALS_V2,
        .len = sizeof(env_t1) + sizeof(v),
    };
    memcpy(frame_v1.payload, &env_t1, sizeof(env_t1));
    memcpy(frame_v1.payload + sizeof(env_t1), &v, sizeof(v));

    gateway_ingest_on_frame(&ingest, &frame_v1);

    /* Assert: Tuple is in queue with TIME_QUALITY_UNKNOWN and timestamp 0 */
    frame_snapshot_tuple_t popped1;
    bool pop_ok = frame_snapshot_queue_pop(&queue, &popped1, 0);
    assert(pop_ok == true);
    assert(popped1.time_quality == TIME_QUALITY_UNKNOWN);
    assert(popped1.timestamp_utc_ms == 0);

    /* Now post SNTP observation from Core 0 to mailbox */
    gateway_time_mailbox_post(&mb, 1700000000000LL, 1000ULL);

    /* Send Vitals telemetry AFTER SNTP */
    uart_v2_envelope_t env_t2 = { .bridge_epoch = 500ULL, .session_id = 202, .seq = 3, .central_rx_mono_ms = 1040 };
    uart_rx_frame_t frame_v2 = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_VITALS_V2,
        .len = sizeof(env_t2) + sizeof(v),
    };
    memcpy(frame_v2.payload, &env_t2, sizeof(env_t2));
    memcpy(frame_v2.payload + sizeof(env_t2), &v, sizeof(v));

    gateway_ingest_on_frame(&ingest, &frame_v2);

    frame_snapshot_tuple_t popped2;
    pop_ok = frame_snapshot_queue_pop(&queue, &popped2, 0);
    assert(pop_ok == true);
    assert(popped2.node_id == 0);
    assert(popped2.session_id == 202);
    assert(popped2.stream_type == STREAM_TYPE_VITALS);
    assert(popped2.time_quality == TIME_QUALITY_SYNCED);
    assert(popped2.timestamp_utc_ms > 0);
    assert(popped2.mac[0] == 0x11 && popped2.mac[5] == 0x66);

    printf("      -> PASS: Telemetry validated on Core 1 and pushed into snapshot queue (pre-SNTP and post-SNTP)\n");
}

static void test_core1_bridge_retry(void) {
    printf("[RUN] Test 3: Core 1 periodic bridge retry progress...\n");
    gateway_ingest_t ingest;
    frame_snapshot_queue_t queue;
    revocation_table_t rev;
    gateway_time_t gt;
    gateway_time_mailbox_t mb;

    frame_snapshot_queue_init(&queue);
    revocation_table_init(&rev);
    gateway_time_init(&gt, 1000);
    gateway_time_mailbox_init(&mb);
    gateway_ingest_init(&ingest, &queue, &rev, &gt, &mb);

    s_tx_count = 0;

    /* When unready and interval < 2000ms, no send */
    gateway_ingest_step_retry(&ingest, 1000);
    assert(s_tx_count == 1); /* First triggers because last_sync_req_mono = -100000 */

    s_tx_count = 0;
    gateway_ingest_step_retry(&ingest, 2500);
    assert(s_tx_count == 0); /* 1500ms < 2000ms */

    gateway_ingest_step_retry(&ingest, 3001);
    assert(s_tx_count == 1); /* 2001ms >= 2000ms */

    /* If link is READY, retry does nothing */
    ingest.bridge_fsm.state = BRIDGE_LINK_READY;
    s_tx_count = 0;
    gateway_ingest_step_retry(&ingest, 6000);
    assert(s_tx_count == 0);

    printf("      -> PASS: Core 1 retry timer progresses independently\n");
}

static void test_control_queue_overflow_triggers_resync(void) {
    printf("[RUN] Test 4: Control queue overflow triggers snapshot resync request...\n");
    gateway_ingest_t ingest;
    frame_snapshot_queue_t queue;
    revocation_table_t rev;
    gateway_time_t gt;
    gateway_time_mailbox_t mb;

    frame_snapshot_queue_init(&queue);
    revocation_table_init(&rev);
    gateway_time_init(&gt, 1000);
    gateway_time_mailbox_init(&mb);
    gateway_ingest_init(&ingest, &queue, &rev, &gt, &mb);

    /* Fill all priority control slots in the queue directly */
    frame_snapshot_tuple_t dummy = { .node_id = 0, .stream_type = STREAM_TYPE_STATUS };
    for (int i = 0; i < FRAME_SNAPSHOT_PRIORITY_CAPACITY; i++) {
        assert(frame_snapshot_queue_push_priority(&queue, &dummy) == true);
    }

    s_tx_count = 0;
    s_last_tx_type = 0;

    /* Now send a status frame into ingest */
    uart_v2_envelope_t env = { .bridge_epoch = 1000ULL, .session_id = 55, .seq = 1, .central_rx_mono_ms = 1000 };
    uart_node_status_pkt_t status = { .status = 1, .mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF } };
    uart_rx_frame_t frame_slot = {
        .node_id = 0,
        .pkt_type = UART_PKT_TYPE_NODE_STATUS_V2,
        .len = sizeof(env) + sizeof(status),
    };
    memcpy(frame_slot.payload, &env, sizeof(env));
    memcpy(frame_slot.payload + sizeof(env), &status, sizeof(status));

    gateway_ingest_on_frame(&ingest, &frame_slot);

    /* Assert: Because queue is full, SNAPSHOT_REQ must have been transmitted */
    assert(s_tx_count == 1);
    assert(s_last_tx_type == UART_PKT_TYPE_SNAPSHOT_REQ);
    assert(frame_snapshot_queue_prio_dropped(&queue) == 1);

    printf("      -> PASS: Control queue overflow cleanly dropped frame and triggered snapshot resync\n");
}

int main(void) {
    printf("====================================================\n");
    printf("    RUNNING GATEWAY INGEST UNIT TESTS (TDD)         \n");
    printf("====================================================\n\n");

    test_hello_and_snapshot_flow();
    test_telemetry_ingest_to_queue();
    test_core1_bridge_retry();
    test_control_queue_overflow_triggers_resync();

    printf("\n🎉 ALL GATEWAY INGEST TESTS PASSED!\n");
    return 0;
}
