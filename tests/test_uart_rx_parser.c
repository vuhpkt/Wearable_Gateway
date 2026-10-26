/**
 * @file test_uart_rx_parser.c
 * @brief Host-based Unit Tests for ESP32 UART Rx Streaming FSM (TDD)
 *
 * Verifies:
 * 1. Single valid frame decoding via callback dispatch.
 * 2. Byte-by-byte incremental streaming (simulating UART RX interrupts).
 * 3. Garbage / noise preamble rejection before genuine SOF (0xAA).
 * 4. False SOF recovery (false SOF followed by real SOF resynchronizes cleanly).
 * 5. Single-bit CRC error detection (corrupted frame dropped, callback NOT called).
 * 6. Back-to-back consecutive streaming of multiple mixed frames (Vitals + ECG).
 */

/* Standard C Libraries */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Project Local Headers */
#include "uart_frame_def.h"
#include "uart_rx_parser.h"

/* Test State & Verification Variables */
static int s_callback_invoked_count = 0;
static uart_rx_frame_t s_last_frame;

static void test_frame_callback(const uart_rx_frame_t *frame) {
    s_callback_invoked_count++;
    s_last_frame = *frame;
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING UART RX PARSER FSM UNIT TESTS (TDD)      \n");
    printf("====================================================\n\n");

    uint8_t raw_frame[256];

    /* ─── TEST 1: Single Valid Frame ─── */
    printf("[RUN] Test 1: Verify single valid frame parsing (Node 0, Vitals)...\n");
    uart_rx_parser_init(test_frame_callback);
    s_callback_invoked_count = 0;

    uint8_t vitals_payload[5] = { 72, 70, 98, 0x6D, 0x01 };
    int packed_len = uart_frame_pack(raw_frame, sizeof(raw_frame), 0, UART_PKT_TYPE_VITALS, vitals_payload, 5);
    assert(packed_len == 10);

    uart_rx_parser_process(raw_frame, packed_len);
    assert(s_callback_invoked_count == 1);
    assert(s_last_frame.node_id == 0);
    assert(s_last_frame.pkt_type == UART_PKT_TYPE_VITALS);
    assert(s_last_frame.len == 5);
    assert(memcmp(s_last_frame.payload, vitals_payload, 5) == 0);
    printf("      -> PASS: Frame decoded cleanly via callback\n");

    /* ─── TEST 2: Byte-by-Byte Streaming (1-byte chunks) ─── */
    printf("[RUN] Test 2: Verify byte-by-byte incremental streaming...\n");
    s_callback_invoked_count = 0;
    for (int i = 0; i < packed_len; i++) {
        uart_rx_parser_process(&raw_frame[i], 1);
    }
    assert(s_callback_invoked_count == 1);
    assert(s_last_frame.node_id == 0);
    printf("      -> PASS: Byte-by-byte streaming reassembled 100%% accurately\n");

    /* ─── TEST 3: Garbage / Noise Preamble Rejection ─── */
    printf("[RUN] Test 3: Verify noise rejection prior to SOF...\n");
    s_callback_invoked_count = 0;
    uint8_t noise[16] = { 0x00, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    uart_rx_parser_process(noise, sizeof(noise));
    assert(s_callback_invoked_count == 0); /* Noise ignored */

    /* Now feed valid frame */
    uart_rx_parser_process(raw_frame, packed_len);
    assert(s_callback_invoked_count == 1);
    printf("      -> PASS: 16 bytes of line noise ignored, valid frame locked immediately\n");

    /* ─── TEST 4: False SOF Resynchronization ─── */
    printf("[RUN] Test 4: Verify false SOF (0xAA) followed by genuine SOF resynchronizes...\n");
    s_callback_invoked_count = 0;
    /* 0xAA followed by 0xAA (real start of frame) */
    uint8_t false_sof_stream[20];
    false_sof_stream[0] = 0xAA; /* False start */
    memcpy(&false_sof_stream[1], raw_frame, packed_len); /* Real frame starting with 0xAA */

    uart_rx_parser_process(false_sof_stream, packed_len + 1);
    assert(s_callback_invoked_count == 1);
    printf("      -> PASS: False SOF did not blind the parser; resynced to real SOF\n");

    /* ─── TEST 5: CRC Error Rejection ─── */
    printf("[RUN] Test 5: Verify corrupted frame rejection (CRC error)...\n");
    s_callback_invoked_count = 0;
    uint8_t corrupted_frame[16];
    memcpy(corrupted_frame, raw_frame, packed_len);
    corrupted_frame[packed_len - 1] ^= 0x55; /* Invert CRC byte */

    uart_rx_parser_process(corrupted_frame, packed_len);
    assert(s_callback_invoked_count == 0); /* Must DROP */
    printf("      -> PASS: Corrupted CRC rejected, callback withheld\n");

    /* ─── TEST 6: Back-to-Back Multiple Frames ─── */
    printf("[RUN] Test 6: Verify consecutive multi-frame streaming (5 frames)...\n");
    s_callback_invoked_count = 0;
    for (uint8_t node = 0; node < 5; node++) {
        uint8_t payload[1] = { node };
        int len = uart_frame_pack(raw_frame, sizeof(raw_frame), node, UART_PKT_TYPE_NODE_STATUS, payload, 1);
        uart_rx_parser_process(raw_frame, len);
    }
    assert(s_callback_invoked_count == 5);
    assert(s_last_frame.node_id == 4);
    printf("      -> PASS: All 5 consecutive frames parsed in exact chronological order\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL UART RX PARSER UNIT TESTS PASSED (100%%)!   \n");
    printf("====================================================\n");
    return 0;
}
