/**
 * @file test_core1_gate.c
 * @brief Host-based Unit Tests for ESP32 Core 1 Ingest Gate & Session Guard (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Acceptance of valid telemetry matching bound session and monotonic seq.
 * 2. Stale epoch frame barrier (drop + counter).
 * 3. Unknown / unmapped session rejection when connect status was lost:
 *    telemetry is DROPPED, snapshot is requested, NEVER inferred from slot or old MAC.
 * 4. Duplicate and out-of-order sequence number rejection.
 * 5. Slot reuse isolation (Watch A -> Watch B):
 *    - Delayed telemetry from A is dropped, never tagged as B.
 *    - Delayed disconnect from A does not revoke B.
 * 6. Downlink guard (DOWNLINK_V2):
 *    - Commands targeting revoked session or mismatched MAC are rejected.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "uart_frame_def.h"
#include "core1_gate.h"

static void test_normal_telemetry_flow(void) {
    printf("[RUN] Test 1: Normal telemetry flow for bound session...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 100ULL);

    uint8_t mac_a[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };

    /* Connect Node 0 with session 101 */
    uart_v2_envelope_t env_conn = {
        .bridge_epoch = 100ULL,
        .session_id = 101,
        .seq = 1,
        .central_rx_mono_ms = 1000ULL
    };
    uart_node_status_pkt_t status_conn = {
        .status = 1,
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 }
    };
    int ret = core1_gate_handle_status(&gate, 0, &env_conn, &status_conn);
    assert(ret == 0);
    assert(core1_gate_is_slot_connected(&gate, 0) == true);
    assert(core1_gate_get_slot_session(&gate, 0) == 101);

    /* Telemetry packet with seq 2 */
    uart_v2_envelope_t env_telem1 = {
        .bridge_epoch = 100ULL,
        .session_id = 101,
        .seq = 2,
        .central_rx_mono_ms = 1020ULL
    };
    uint8_t resolved_mac[6];
    ret = core1_gate_validate_telemetry(&gate, 0, &env_telem1, resolved_mac);
    assert(ret == 0);
    assert(memcmp(resolved_mac, mac_a, 6) == 0);

    /* Telemetry packet with seq 3 */
    uart_v2_envelope_t env_telem2 = {
        .bridge_epoch = 100ULL,
        .session_id = 101,
        .seq = 3,
        .central_rx_mono_ms = 1040ULL
    };
    ret = core1_gate_validate_telemetry(&gate, 0, &env_telem2, resolved_mac);
    assert(ret == 0);
    printf("      -> PASS: Telemetry validated with bound MAC E4:5F:01:23:45:67 and monotonic seq\n");
}

static void test_stale_epoch_rejection(void) {
    printf("[RUN] Test 2: Stale epoch frame barrier...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 200ULL);

    uart_v2_envelope_t env_stale = {
        .bridge_epoch = 199ULL, /* Stale epoch */
        .session_id = 101,
        .seq = 1,
        .central_rx_mono_ms = 1000ULL
    };
    uint8_t resolved_mac[6];
    int ret = core1_gate_validate_telemetry(&gate, 0, &env_stale, resolved_mac);
    assert(ret == CORE1_GATE_DROP_EPOCH_MISMATCH);
    assert(gate.stats.epoch_mismatch_drops == 1);
    printf("      -> PASS: Stale epoch frame rejected immediately\n");
}

static void test_unknown_session_rejection(void) {
    printf("[RUN] Test 3: Unknown / lost status session rejection...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 300ULL);

    /* Telemetry arrives for Node 0, but no connect status frame was received! */
    uart_v2_envelope_t env_unknown = {
        .bridge_epoch = 300ULL,
        .session_id = 999, /* Unmapped session */
        .seq = 1,
        .central_rx_mono_ms = 2000ULL
    };
    uint8_t resolved_mac[6];
    int ret = core1_gate_validate_telemetry(&gate, 0, &env_unknown, resolved_mac);
    assert(ret == CORE1_GATE_DROP_SESSION_MISMATCH);
    assert(gate.stats.session_mismatch_drops == 1);
    assert(core1_gate_is_snapshot_requested(&gate) == true);
    printf("      -> PASS: Unknown session dropped and snapshot recovery requested\n");
}

static void test_duplicate_and_out_of_order_seq(void) {
    printf("[RUN] Test 4: Duplicate & out-of-order sequence number rejection...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 400ULL);

    /* Connect Node 1 with session 201 */
    uart_v2_envelope_t env_conn = {
        .bridge_epoch = 400ULL,
        .session_id = 201,
        .seq = 10,
        .central_rx_mono_ms = 1000ULL
    };
    uart_node_status_pkt_t status_conn = { .status = 1, .mac = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
    core1_gate_handle_status(&gate, 1, &env_conn, &status_conn);

    /* Valid seq 11 */
    uart_v2_envelope_t env = { .bridge_epoch = 400ULL, .session_id = 201, .seq = 11, .central_rx_mono_ms = 1020ULL };
    uint8_t mac[6];
    int ret = core1_gate_validate_telemetry(&gate, 1, &env, mac);
    assert(ret == 0);

    /* Duplicate seq 11 */
    ret = core1_gate_validate_telemetry(&gate, 1, &env, mac);
    assert(ret == CORE1_GATE_DROP_SEQ_ORDER);
    assert(gate.stats.seq_order_drops == 1);

    /* Regressive seq 9 */
    env.seq = 9;
    ret = core1_gate_validate_telemetry(&gate, 1, &env, mac);
    assert(ret == CORE1_GATE_DROP_SEQ_ORDER);
    assert(gate.stats.seq_order_drops == 2);
    printf("      -> PASS: Duplicate and regressive sequence numbers safely dropped\n");
}

static void test_slot_reuse_isolation(void) {
    printf("[RUN] Test 5: Slot reuse Watch A -> Watch B isolation...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 500ULL);

    uint8_t mac_b[6] = { 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB };

    /* 1. Slot 0 connects Watch A (session 301) */
    uart_v2_envelope_t env_conn_a = { .bridge_epoch = 500ULL, .session_id = 301, .seq = 1, .central_rx_mono_ms = 1000ULL };
    uart_node_status_pkt_t status_a = { .status = 1, .mac = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA } };
    core1_gate_handle_status(&gate, 0, &env_conn_a, &status_a);

    /* 2. Slot 0 disconnects Watch A */
    uart_v2_envelope_t env_disc_a = { .bridge_epoch = 500ULL, .session_id = 301, .seq = 2, .central_rx_mono_ms = 2000ULL };
    uart_node_status_pkt_t status_disc_a = { .status = 0, .mac = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA } };
    core1_gate_handle_status(&gate, 0, &env_disc_a, &status_disc_a);
    assert(core1_gate_is_slot_connected(&gate, 0) == false);

    /* 3. Slot 0 connects Watch B (session 302) */
    uart_v2_envelope_t env_conn_b = { .bridge_epoch = 500ULL, .session_id = 302, .seq = 1, .central_rx_mono_ms = 3000ULL };
    uart_node_status_pkt_t status_b = { .status = 1, .mac = { 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB } };
    core1_gate_handle_status(&gate, 0, &env_conn_b, &status_b);
    assert(core1_gate_is_slot_connected(&gate, 0) == true);
    assert(core1_gate_get_slot_session(&gate, 0) == 302);

    /* 4. Late telemetry from Watch A (session 301) arrives for Slot 0 */
    uart_v2_envelope_t env_late_a = { .bridge_epoch = 500ULL, .session_id = 301, .seq = 5, .central_rx_mono_ms = 3050ULL };
    uint8_t resolved_mac[6];
    int ret = core1_gate_validate_telemetry(&gate, 0, &env_late_a, resolved_mac);
    /* MUST be dropped! Cannot be accepted under Watch B's identity */
    assert(ret == CORE1_GATE_DROP_SESSION_MISMATCH);
    assert(gate.stats.session_mismatch_drops >= 1);

    /* 5. Late disconnect from Watch A (session 301) arrives for Slot 0 */
    /* Must NOT revoke Watch B (session 302)! */
    core1_gate_handle_status(&gate, 0, &env_disc_a, &status_disc_a);
    assert(core1_gate_is_slot_connected(&gate, 0) == true);
    assert(core1_gate_get_slot_session(&gate, 0) == 302);

    /* 6. Telemetry for Watch B (session 302) passes cleanly */
    uart_v2_envelope_t env_telem_b = { .bridge_epoch = 500ULL, .session_id = 302, .seq = 2, .central_rx_mono_ms = 3100ULL };
    ret = core1_gate_validate_telemetry(&gate, 0, &env_telem_b, resolved_mac);
    assert(ret == 0);
    assert(memcmp(resolved_mac, mac_b, 6) == 0);
    printf("      -> PASS: Delayed telemetry/disconnect from Watch A completely isolated from Watch B\n");
}

static void test_downlink_guard(void) {
    printf("[RUN] Test 6: Downlink command session guard...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 600ULL);

    uint8_t mac_target[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
    uint8_t mac_wrong[6]  = { 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA };

    /* Connect Node 2 */
    uart_v2_envelope_t env_conn = { .bridge_epoch = 600ULL, .session_id = 401, .seq = 1, .central_rx_mono_ms = 1000ULL };
    uart_node_status_pkt_t status = { .status = 1, .mac = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC } };
    core1_gate_handle_status(&gate, 2, &env_conn, &status);

    /* 6.1 Valid downlink matching epoch, session, and MAC */
    int ret = core1_gate_validate_downlink(&gate, 2, 600ULL, 401, mac_target);
    assert(ret == 0);

    /* 6.2 Wrong epoch */
    ret = core1_gate_validate_downlink(&gate, 2, 599ULL, 401, mac_target);
    assert(ret == CORE1_GATE_DOWNLINK_REJECT_EPOCH);

    /* 6.3 Wrong session */
    ret = core1_gate_validate_downlink(&gate, 2, 600ULL, 399, mac_target);
    assert(ret == CORE1_GATE_DOWNLINK_REJECT_SESSION);

    /* 6.4 Wrong MAC */
    ret = core1_gate_validate_downlink(&gate, 2, 600ULL, 401, mac_wrong);
    assert(ret == CORE1_GATE_DOWNLINK_REJECT_MAC);
    printf("      -> PASS: Downlink commands guarded against wrong epoch, session, and MAC\n");
}

static void test_central_reboot_epoch_reset_and_empty_snapshot(void) {
    printf("[RUN] Test 7: Central reboot epoch reset and empty snapshot handling...\n");
    core1_gate_t gate;
    core1_gate_init(&gate, 100ULL);

    /* 1. Connect Slot 0 with session 101 in epoch 100 */
    uart_v2_envelope_t env_conn = { .bridge_epoch = 100ULL, .session_id = 101, .seq = 1, .central_rx_mono_ms = 1000ULL };
    uart_node_status_pkt_t status_conn = { .status = 1, .mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 } };
    assert(core1_gate_handle_status(&gate, 0, &env_conn, &status_conn) == 0);
    assert(core1_gate_is_slot_connected(&gate, 0) == true);
    assert(core1_gate_get_slot_session(&gate, 0) == 101);

    /* 2. Central reboots -> new epoch 200 */
    core1_gate_set_epoch(&gate, 200ULL);

    /* Slot 0 bindings should be cleared on epoch change */
    assert(core1_gate_is_slot_connected(&gate, 0) == false);
    assert(core1_gate_get_slot_session(&gate, 0) == 0);

    /* 3. Central sends snapshot for Slot 0 as empty (status=0, session_id=0) in epoch 200 */
    uart_v2_envelope_t env_snap_empty = { .bridge_epoch = 200ULL, .session_id = 0, .seq = 1, .central_rx_mono_ms = 2000ULL };
    uart_node_status_pkt_t status_empty = { .status = 0, .mac = { 0 } };
    int ret = core1_gate_handle_status(&gate, 0, &env_snap_empty, &status_empty);
    assert(ret == CORE1_GATE_OK);

    /* 4. Old telemetry from session 101 (even if using new epoch 200) MUST be rejected */
    uart_v2_envelope_t env_stale = { .bridge_epoch = 200ULL, .session_id = 101, .seq = 2, .central_rx_mono_ms = 2010ULL };
    uint8_t resolved_mac[6];
    ret = core1_gate_validate_telemetry(&gate, 0, &env_stale, resolved_mac);
    assert(ret == CORE1_GATE_DROP_SESSION_MISMATCH);
    assert(core1_gate_is_snapshot_requested(&gate) == true);

    printf("      -> PASS: Epoch reset clears old bindings and accepts empty snapshot slots\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING CORE 1 INGEST GATE UNIT TESTS (TDD)      \n");
    printf("====================================================\n\n");

    test_normal_telemetry_flow();
    test_stale_epoch_rejection();
    test_unknown_session_rejection();
    test_duplicate_and_out_of_order_seq();
    test_slot_reuse_isolation();
    test_downlink_guard();
    test_central_reboot_epoch_reset_and_empty_snapshot();

    printf("\n🎉 ALL CORE 1 INGEST GATE TESTS PASSED!\n");
    return 0;
}
