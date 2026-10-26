/**
 * @file test_bridge_handshake.c
 * @brief Host-based Unit Tests for Central-ESP32 Handshake & Snapshot Control (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Version negotiation and HELLO -> SYNC handshake.
 * 2. Idempotent duplicate HELLO / SYNC retry handling.
 * 3. Central reboot nonce detection and stale epoch barrier.
 * 4. Snapshot transaction (REQUEST -> 10 slots -> END -> COMMIT -> READY).
 * 5. Stale SNAPSHOT_END rejection (mismatched epoch or transaction_id).
 * 6. Partial snapshot rejection (missing slots -> uncommitted).
 * 7. End-to-end byte-by-byte wire streaming across framing codec.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1
#define HOST_TEST 1

#include "uart_frame_def.h"
#include "uart_bridge_rx.h"
#include "nrf_central/src/uart_tx_bridge.h"
#include "uart_rx_parser.h"

static void test_version_negotiation(void) {
    printf("[RUN] Test 1: Version negotiation & initial handshake...\n");
    bridge_fsm_t esp_fsm;
    uart_bridge_fsm_init(&esp_fsm, 1000ULL);

    central_bridge_fsm_t central_fsm;
    central_bridge_fsm_init(&central_fsm, 0x12345678U);

    /* 1.1 Incompatible version from Central must be rejected by ESP32 */
    uart_ctrl_hello_pkt_t bad_hello = {
        .version = 0x0001, /* Old v1 */
        .boot_nonce = 0x12345678U,
        .reserved = 0
    };
    uart_ctrl_sync_pkt_t sync_out;
    int ret = uart_bridge_handle_hello(&esp_fsm, &bad_hello, &sync_out);
    assert(ret < 0);
    assert(esp_fsm.state == BRIDGE_LINK_UNSYNCED);

    /* 1.2 Compatible version v2 */
    uart_ctrl_hello_pkt_t hello;
    central_bridge_create_hello(&central_fsm, &hello);
    assert(hello.version == UART_BRIDGE_PROTOCOL_VERSION);
    assert(hello.boot_nonce == 0x12345678U);

    ret = uart_bridge_handle_hello(&esp_fsm, &hello, &sync_out);
    assert(ret == 0);
    assert(esp_fsm.state == BRIDGE_LINK_SYNCED);
    assert(sync_out.version == UART_BRIDGE_PROTOCOL_VERSION);
    assert(sync_out.boot_nonce == 0x12345678U);
    assert(sync_out.bridge_epoch == 1001ULL);
    assert(sync_out.status == 1);

    /* Central receives SYNC */
    ret = central_bridge_handle_sync(&central_fsm, &sync_out);
    assert(ret == 0);
    assert(central_fsm.state == CENTRAL_LINK_SYNCED);
    assert(central_fsm.active_epoch == 1001ULL);
    printf("      -> PASS: Version negotiated, epoch 1001 established\n");
}

static void test_idempotent_duplicate_hello(void) {
    printf("[RUN] Test 2: Idempotent duplicate HELLO handling...\n");
    bridge_fsm_t esp_fsm;
    uart_bridge_fsm_init(&esp_fsm, 2000ULL);

    central_bridge_fsm_t central_fsm;
    central_bridge_fsm_init(&central_fsm, 0xABCDE001U);

    uart_ctrl_hello_pkt_t hello;
    uart_ctrl_sync_pkt_t sync_out_1;
    uart_ctrl_sync_pkt_t sync_out_2;

    central_bridge_create_hello(&central_fsm, &hello);

    /* First HELLO */
    int ret = uart_bridge_handle_hello(&esp_fsm, &hello, &sync_out_1);
    assert(ret == 0);
    uint64_t established_epoch = sync_out_1.bridge_epoch;
    assert(established_epoch == 2001ULL);

    /* Duplicate HELLO with same boot nonce */
    ret = uart_bridge_handle_hello(&esp_fsm, &hello, &sync_out_2);
    assert(ret == 0);
    /* Epoch must remain unchanged (idempotent) */
    assert(sync_out_2.bridge_epoch == established_epoch);
    assert(esp_fsm.epoch == established_epoch);
    printf("      -> PASS: Duplicate HELLO replied with same epoch (idempotent)\n");
}

static void test_reboot_nonce_barrier(void) {
    printf("[RUN] Test 3: Central reboot nonce detection & stale epoch barrier...\n");
    bridge_fsm_t esp_fsm;
    uart_bridge_fsm_init(&esp_fsm, 3000ULL);

    /* Central Run 1 */
    central_bridge_fsm_t central_1;
    central_bridge_fsm_init(&central_1, 0x11111111U);
    uart_ctrl_hello_pkt_t hello_1;
    uart_ctrl_sync_pkt_t sync_1;
    central_bridge_create_hello(&central_1, &hello_1);
    uart_bridge_handle_hello(&esp_fsm, &hello_1, &sync_1);
    central_bridge_handle_sync(&central_1, &sync_1);
    uint64_t epoch_1 = sync_1.bridge_epoch;
    assert(epoch_1 == 3001ULL);

    /* Central reboots with new nonce */
    central_bridge_fsm_t central_2;
    central_bridge_fsm_init(&central_2, 0x22222222U);
    uart_ctrl_hello_pkt_t hello_2;
    uart_ctrl_sync_pkt_t sync_2;
    central_bridge_create_hello(&central_2, &hello_2);

    /* ESP detects new nonce -> increments epoch */
    int ret = uart_bridge_handle_hello(&esp_fsm, &hello_2, &sync_2);
    assert(ret == 0);
    uint64_t epoch_2 = sync_2.bridge_epoch;
    assert(epoch_2 == 3002ULL);
    assert(epoch_2 != epoch_1);

    /* Frame stamped with old epoch_1 must be dropped by the barrier */
    assert(uart_bridge_is_ingest_allowed(&esp_fsm, epoch_1) == false);
    ret = uart_bridge_handle_snapshot_slot(&esp_fsm, 0, epoch_1);
    assert(ret < 0);
    assert(esp_fsm.stale_epoch_drops == 1);
    printf("      -> PASS: New boot nonce allocated epoch 3002; stale epoch 3001 blocked\n");
}

static void test_snapshot_transaction_success(void) {
    printf("[RUN] Test 4: Full snapshot transaction commit & ingest opening...\n");
    bridge_fsm_t esp_fsm;
    uart_bridge_fsm_init(&esp_fsm, 4000ULL);

    central_bridge_fsm_t central_fsm;
    central_bridge_fsm_init(&central_fsm, 0x33333333U);

    uart_ctrl_hello_pkt_t hello;
    uart_ctrl_sync_pkt_t sync;
    central_bridge_create_hello(&central_fsm, &hello);
    uart_bridge_handle_hello(&esp_fsm, &hello, &sync);
    central_bridge_handle_sync(&central_fsm, &sync);

    /* Ingest must NOT be allowed before snapshot is committed */
    assert(uart_bridge_is_ingest_allowed(&esp_fsm, esp_fsm.epoch) == false);

    /* ESP requests snapshot */
    uart_snapshot_req_pkt_t req;
    int ret = uart_bridge_request_snapshot(&esp_fsm, &req);
    assert(ret == 0);
    assert(req.bridge_epoch == esp_fsm.epoch);
    assert(req.transaction_id == 1);

    /* Central handles snapshot request */
    ret = central_bridge_handle_snapshot_req(&central_fsm, &req);
    assert(ret == 0);

    /* Central streams 10 slots (0 .. 9) */
    for (uint8_t slot = 0; slot < MAX_NODES_SUPPORTED; slot++) {
        ret = uart_bridge_handle_snapshot_slot(&esp_fsm, slot, esp_fsm.epoch);
        assert(ret == 0);
    }
    assert(esp_fsm.snapshot_slots_mask == 0x03FF); /* All 10 slots set */

    /* Central sends SNAPSHOT_END */
    uart_snapshot_end_pkt_t end;
    ret = central_bridge_create_snapshot_end(&central_fsm, &end);
    assert(ret == 0);
    assert(end.bridge_epoch == esp_fsm.epoch);
    assert(end.transaction_id == 1);
    assert(end.slot_count == MAX_NODES_SUPPORTED);

    /* ESP commits snapshot */
    ret = uart_bridge_handle_snapshot_end(&esp_fsm, &end);
    assert(ret == 0);
    assert(esp_fsm.state == BRIDGE_LINK_READY);

    /* Now ingest IS allowed */
    assert(uart_bridge_is_ingest_allowed(&esp_fsm, esp_fsm.epoch) == true);
    printf("      -> PASS: Snapshot committed 10 slots; link transitioned to READY\n");
}

static void test_stale_snapshot_end_rejection(void) {
    printf("[RUN] Test 5: Stale SNAPSHOT_END rejection...\n");
    bridge_fsm_t esp_fsm;
    uart_bridge_fsm_init(&esp_fsm, 5000ULL);

    central_bridge_fsm_t central_fsm;
    central_bridge_fsm_init(&central_fsm, 0x44444444U);

    uart_ctrl_hello_pkt_t hello;
    uart_ctrl_sync_pkt_t sync;
    central_bridge_create_hello(&central_fsm, &hello);
    uart_bridge_handle_hello(&esp_fsm, &hello, &sync);
    central_bridge_handle_sync(&central_fsm, &sync);

    uart_snapshot_req_pkt_t req;
    uart_bridge_request_snapshot(&esp_fsm, &req);

    for (uint8_t slot = 0; slot < MAX_NODES_SUPPORTED; slot++) {
        uart_bridge_handle_snapshot_slot(&esp_fsm, slot, esp_fsm.epoch);
    }

    /* Stale transaction_id (e.g. 99 != 1) */
    uart_snapshot_end_pkt_t bad_end = {
        .bridge_epoch = esp_fsm.epoch,
        .transaction_id = 99,
        .slot_count = MAX_NODES_SUPPORTED,
        .reserved = 0
    };
    int ret = uart_bridge_handle_snapshot_end(&esp_fsm, &bad_end);
    assert(ret < 0);
    assert(esp_fsm.state != BRIDGE_LINK_READY);

    /* Stale epoch */
    bad_end.transaction_id = req.transaction_id;
    bad_end.bridge_epoch = 12345ULL; /* Wrong epoch */
    ret = uart_bridge_handle_snapshot_end(&esp_fsm, &bad_end);
    assert(ret < 0);
    assert(esp_fsm.state != BRIDGE_LINK_READY);
    printf("      -> PASS: Stale transaction ID and epoch rejected; state remains uncommitted\n");
}

static void test_partial_snapshot_rejection(void) {
    printf("[RUN] Test 6: Partial snapshot rejection (missing slots)...\n");
    bridge_fsm_t esp_fsm;
    uart_bridge_fsm_init(&esp_fsm, 6000ULL);

    central_bridge_fsm_t central_fsm;
    central_bridge_fsm_init(&central_fsm, 0x55555555U);

    uart_ctrl_hello_pkt_t hello;
    uart_ctrl_sync_pkt_t sync;
    central_bridge_create_hello(&central_fsm, &hello);
    uart_bridge_handle_hello(&esp_fsm, &hello, &sync);
    central_bridge_handle_sync(&central_fsm, &sync);

    uart_snapshot_req_pkt_t req;
    uart_bridge_request_snapshot(&esp_fsm, &req);

    /* Only slots 0..8 sent (slot 9 dropped on line) */
    for (uint8_t slot = 0; slot < 9; slot++) {
        uart_bridge_handle_snapshot_slot(&esp_fsm, slot, esp_fsm.epoch);
    }
    assert(esp_fsm.snapshot_slots_mask != 0x03FF);

    uart_snapshot_end_pkt_t end;
    central_bridge_create_snapshot_end(&central_fsm, &end);
    end.transaction_id = req.transaction_id;

    int ret = uart_bridge_handle_snapshot_end(&esp_fsm, &end);
    assert(ret < 0);
    assert(esp_fsm.state != BRIDGE_LINK_READY);
    assert(esp_fsm.partial_snapshot_drops == 1);
    printf("      -> PASS: Incomplete snapshot (9/10 slots) rejected; ingest blocked\n");
}

static int s_rx_frame_count = 0;
static uart_rx_frame_t s_last_frame;

static void rx_parser_callback(const uart_rx_frame_t *frame) {
    s_rx_frame_count++;
    s_last_frame = *frame;
}

static void test_byte_by_byte_wire_codec(void) {
    printf("[RUN] Test 7: Incremental byte-by-byte wire streaming...\n");
    uart_rx_parser_init(rx_parser_callback);
    s_rx_frame_count = 0;

    /* Pack CONTROL_HELLO */
    uart_ctrl_hello_pkt_t hello = {
        .version = UART_BRIDGE_PROTOCOL_VERSION,
        .boot_nonce = 0x12345678U,
        .reserved = 0
    };
    uint8_t frame_buf[32];
    int frame_len = uart_frame_pack(frame_buf, sizeof(frame_buf), 0,
                                   UART_PKT_TYPE_CONTROL_HELLO, &hello, sizeof(hello));
    assert(frame_len == (int)(sizeof(hello) + UART_FRAME_OVERHEAD));

    /* Stream byte-by-byte into parser */
    for (int i = 0; i < frame_len; i++) {
        uart_rx_parser_process(&frame_buf[i], 1);
    }
    assert(s_rx_frame_count == 1);
    assert(s_last_frame.node_id == 0);
    assert(s_last_frame.pkt_type == UART_PKT_TYPE_CONTROL_HELLO);
    assert(s_last_frame.len == sizeof(hello));
    assert(memcmp(s_last_frame.payload, &hello, sizeof(hello)) == 0);

    /* Pack CONTROL_SYNC */
    uart_ctrl_sync_pkt_t sync = {
        .version = UART_BRIDGE_PROTOCOL_VERSION,
        .boot_nonce = 0x12345678U,
        .bridge_epoch = 0x0102030405060708ULL,
        .status = 1,
        .reserved = 0
    };
    frame_len = uart_frame_pack(frame_buf, sizeof(frame_buf), 0,
                               UART_PKT_TYPE_CONTROL_SYNC, &sync, sizeof(sync));
    assert(frame_len == (int)(sizeof(sync) + UART_FRAME_OVERHEAD));

    for (int i = 0; i < frame_len; i++) {
        uart_rx_parser_process(&frame_buf[i], 1);
    }
    assert(s_rx_frame_count == 2);
    assert(s_last_frame.pkt_type == UART_PKT_TYPE_CONTROL_SYNC);
    assert(s_last_frame.len == sizeof(sync));
    assert(memcmp(s_last_frame.payload, &sync, sizeof(sync)) == 0);

    printf("      -> PASS: Control frames streamed byte-by-byte decoded 100%% accurately\n");
}

static void test_sync_req_frame(void) {
    printf("[RUN] Test 8: SYNC_REQ packet decoding...\n");
    uint8_t frame_buf[UART_MAX_FRAME_LEN];
    uint8_t dummy = 0;
    int frame_len = uart_frame_pack(frame_buf, sizeof(frame_buf), 0,
                                   UART_PKT_TYPE_CONTROL_SYNC_REQ, &dummy, 1);
    assert(frame_len > 0);

    uart_rx_parser_init(rx_parser_callback);
    s_rx_frame_count = 0;

    uart_rx_parser_process(frame_buf, frame_len);
    assert(s_rx_frame_count == 1);
    assert(s_last_frame.pkt_type == UART_PKT_TYPE_CONTROL_SYNC_REQ);

    printf("      -> PASS: SYNC_REQ successfully parsed\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING BRIDGE HANDSHAKE & SNAPSHOT TESTS (TDD)  \n");
    printf("====================================================\n\n");

    test_version_negotiation();
    test_idempotent_duplicate_hello();
    test_reboot_nonce_barrier();
    test_snapshot_transaction_success();
    test_stale_snapshot_end_rejection();
    test_partial_snapshot_rejection();
    test_byte_by_byte_wire_codec();
    test_sync_req_frame();

    printf("\n🎉 ALL BRIDGE HANDSHAKE & SNAPSHOT TESTS PASSED!\n");
    return 0;
}
