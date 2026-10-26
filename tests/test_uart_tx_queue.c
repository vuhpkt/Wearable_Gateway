/**
 * @file test_uart_tx_queue.c
 * @brief Host-based Unit Tests for UART TX Message Queue (TDD)
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uart_tx_queue.h"

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING UART TX QUEUE UNIT TESTS (TDD)          \n");
    printf("====================================================\n\n");

    /* ─── TEST 1: Clean Initialization ─── */
    printf("[RUN] Test 1: Verify clean initialization...\n");
    uart_tx_queue_init();
    uart_tx_queue_stats_t stats;
    uint32_t count = uart_tx_queue_get_stats(&stats);
    assert(count == 0);
    assert(stats.queued_count == 0);
    assert(stats.sent_count == 0);
    assert(stats.dropped_count == 0);
    printf("      -> PASS: Queue empty, stats zeroed\n");

    /* ─── TEST 2: Basic Enqueue and Dequeue (FIFO Order) ─── */
    printf("[RUN] Test 2: Enqueue Node Status and Vitals frames...\n");
    uint8_t status_val = 1;
    int ret = uart_tx_queue_send(0, UART_PKT_TYPE_NODE_STATUS, &status_val, sizeof(status_val));
    assert(ret == 0);

    uint8_t vitals_payload[5] = {72, 70, 98, 0x6D, 0x01};
    ret = uart_tx_queue_send(1, UART_PKT_TYPE_VITALS, vitals_payload, sizeof(vitals_payload));
    assert(ret == 0);

    count = uart_tx_queue_get_stats(&stats);
    assert(count == 2);
    assert(stats.queued_count == 2);
    assert(stats.dropped_count == 0);

    /* Dequeue first item (Node Status) */
    uart_tx_item_t item1;
    ret = uart_tx_queue_get(&item1, 0);
    assert(ret == 0);
    assert(item1.node_id == 0);
    assert(item1.pkt_type == UART_PKT_TYPE_NODE_STATUS);
    assert(item1.len == 1);
    assert(item1.payload[0] == 1);

    /* Dequeue second item (Vitals) */
    uart_tx_item_t item2;
    ret = uart_tx_queue_get(&item2, 0);
    assert(ret == 0);
    assert(item2.node_id == 1);
    assert(item2.pkt_type == UART_PKT_TYPE_VITALS);
    assert(item2.len == 5);
    assert(memcmp(item2.payload, vitals_payload, 5) == 0);

    /* Queue should now be empty */
    count = uart_tx_queue_get_stats(&stats);
    assert(count == 0);
    printf("      -> PASS: FIFO order preserved, payloads intact\n");

    /* ─── TEST 3: Saturation Protection (Queue Full / Drop Counter) ─── */
    printf("[RUN] Test 3: Fill queue to capacity (32 items) and test overflow drop...\n");
    uart_tx_queue_init();

    for (int i = 0; i < UART_TX_QUEUE_DEPTH; i++) {
        uint8_t dummy[4] = {(uint8_t)i, 0, 0, 0};
        ret = uart_tx_queue_send((uint8_t)(i % 10), UART_PKT_TYPE_VITALS, dummy, sizeof(dummy));
        assert(ret == 0);
    }

    count = uart_tx_queue_get_stats(&stats);
    assert(count == UART_TX_QUEUE_DEPTH);
    assert(stats.queued_count == UART_TX_QUEUE_DEPTH);
    assert(stats.dropped_count == 0);

    /* 33rd item must be dropped with non-zero error and increment dropped_count */
    uint8_t overflow_val = 0xFF;
    ret = uart_tx_queue_send(9, UART_PKT_TYPE_NODE_STATUS, &overflow_val, sizeof(overflow_val));
    assert(ret != 0); /* Must fail */

    count = uart_tx_queue_get_stats(&stats);
    assert(count == UART_TX_QUEUE_DEPTH); /* Queue count stays at max */
    assert(stats.dropped_count == 1);     /* Drop count incremented */
    printf("      -> PASS: 33rd packet safely dropped without crash (dropped_count = 1)\n");

    /* ─── TEST 4: Invalid Parameter Boundary Checks ─── */
    printf("[RUN] Test 4: Verify boundary validation (oversized payload, NULL ptr)...\n");
    /* Oversized payload > 128 bytes */
    uint8_t huge_buf[129];
    ret = uart_tx_queue_send(0, UART_PKT_TYPE_ECG_BATCH, huge_buf, 129);
    assert(ret == -EINVAL);

    /* NULL payload with non-zero len */
    ret = uart_tx_queue_send(0, UART_PKT_TYPE_VITALS, NULL, 5);
    assert(ret == -EINVAL);

    /* Zero length payload */
    ret = uart_tx_queue_send(0, UART_PKT_TYPE_VITALS, vitals_payload, 0);
    assert(ret == -EINVAL);
    printf("      -> PASS: Invalid parameters safely rejected\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL UART TX QUEUE UNIT TESTS PASSED (100%)!    \n");
    printf("====================================================\n");
    return 0;
}
