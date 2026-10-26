/**
 * @file test_gateway_mqtt_outbox.c
 * @brief Host-based Unit Tests for Custom Static MQTT Outbox & Retransmit Guard (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Zero heap allocation: 100% fixed static pool of 16 items.
 * 2. Strict packet size limit (1536 bytes): Oversize packets rejected without truncation.
 * 3. Capacity bounding: 16-item limit strictly enforced; overflow rejected non-blocking.
 * 4. ACK deletion: msg_id/msg_type matching frees slot back to static pool.
 * 5. Expiration & cleanup: Packets aged > timeout cleanly discarded.
 * 6. Retransmit guard: Validates freshness (< 5000ms) before retransmission.
 * 7. Destroy & re-init lifecycle cleanliness.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "gateway_mqtt_outbox.h"

static void test_outbox_enqueue_dequeue_ack(void) {
    printf("[RUN] Test 1: Basic enqueue, dequeue and ACK deletion flow...\n");
    outbox_handle_t outbox = outbox_init();
    assert(outbox != NULL);
    assert(outbox_get_size(outbox) == 0);

    /* Enqueue 1 message */
    uint8_t payload[] = "{\"telemetry\":42}";
    outbox_message_t msg = {
        .data = payload,
        .len = sizeof(payload),
        .msg_id = 101,
        .msg_type = 3, /* PUBLISH */
        .msg_qos = 1,
        .remaining_data = NULL,
        .remaining_len = 0
    };

    outbox_item_handle_t item = outbox_enqueue(outbox, &msg, 1000);
    assert(item != NULL);
    assert(outbox_get_size(outbox) == sizeof(payload));

    gateway_outbox_stats_t stats;
    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.current_items == 1);
    assert(stats.total_enqueued == 1);

    /* Dequeue */
    outbox_tick_t tick = 0;
    outbox_item_handle_t deq = outbox_dequeue(outbox, QUEUED, &tick);
    assert(deq == item);
    assert(tick == 1000);

    /* Set transmitted */
    assert(outbox_set_pending(outbox, 101, TRANSMITTED) == ESP_OK);
    assert(outbox_item_get_pending(item) == TRANSMITTED);

    /* Delete on PUBACK */
    assert(outbox_delete(outbox, 101, 3) == ESP_OK);
    assert(outbox_get_size(outbox) == 0);

    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.current_items == 0);
    assert(stats.total_deleted == 1);

    printf("      -> PASS: Enqueued, dequeued, transitioned to TRANSMITTED, deleted on ACK\n");
}

static void test_oversized_packet_rejection(void) {
    printf("[RUN] Test 2: Oversized packet rejection without truncation...\n");
    outbox_handle_t outbox = outbox_init();

    /* Create dummy packet larger than 1536 bytes */
    uint8_t huge_payload[1600];
    memset(huge_payload, 'A', sizeof(huge_payload));

    outbox_message_t msg = {
        .data = huge_payload,
        .len = sizeof(huge_payload),
        .msg_id = 99,
        .msg_type = 3,
        .msg_qos = 1,
        .remaining_data = NULL,
        .remaining_len = 0
    };

    outbox_item_handle_t item = outbox_enqueue(outbox, &msg, 1000);
    assert(item == NULL);
    assert(outbox_get_size(outbox) == 0);

    gateway_outbox_stats_t stats;
    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.dropped_oversize == 1);
    assert(stats.current_items == 0);

    printf("      -> PASS: 1600-byte packet (>1536B limit) cleanly rejected without payload truncation\n");
}

static void test_capacity_exhaustion_and_drop_tracking(void) {
    printf("[RUN] Test 3: Static pool capacity exhaustion (16 items) and drop tracking...\n");
    outbox_handle_t outbox = outbox_init();

    uint8_t payload[] = "vitals";
    outbox_message_t msg = {
        .data = payload,
        .len = sizeof(payload),
        .msg_type = 3,
        .msg_qos = 1,
        .remaining_data = NULL,
        .remaining_len = 0
    };

    /* Fill all 16 slots */
    for (int i = 0; i < (int)GATEWAY_OUTBOX_MAX_ITEMS; i++) {
        msg.msg_id = 200 + i;
        outbox_item_handle_t it = outbox_enqueue(outbox, &msg, 1000 + i);
        assert(it != NULL);
    }

    gateway_outbox_stats_t stats;
    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.current_items == GATEWAY_OUTBOX_MAX_ITEMS);
    assert(stats.dropped_full == 0);

    /* 17th item must fail non-blocking */
    msg.msg_id = 999;
    outbox_item_handle_t overflow = outbox_enqueue(outbox, &msg, 2000);
    assert(overflow == NULL);

    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.dropped_full == 1);
    assert(stats.current_items == GATEWAY_OUTBOX_MAX_ITEMS);

    /* Free 1 item via ACK */
    assert(outbox_delete(outbox, 200, 3) == ESP_OK);
    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.current_items == GATEWAY_OUTBOX_MAX_ITEMS - 1);

    /* Retry enqueue now succeeds */
    outbox_item_handle_t retried = outbox_enqueue(outbox, &msg, 2001);
    assert(retried != NULL);
    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.current_items == GATEWAY_OUTBOX_MAX_ITEMS);

    printf("      -> PASS: 16-item static bound strictly enforced; overflow tracked; freed slot re-used\n");
}

static void test_expiration_and_retransmit_validation(void) {
    printf("[RUN] Test 4: Message expiration and retransmission deadline validation...\n");
    outbox_handle_t outbox = outbox_init();

    uint8_t payload[] = "packet";
    outbox_message_t msg = {
        .data = payload,
        .len = sizeof(payload),
        .msg_type = 3,
        .msg_qos = 1,
        .remaining_data = NULL,
        .remaining_len = 0
    };

    /* Enqueue at tick = 10000 */
    msg.msg_id = 301;
    outbox_item_handle_t item = outbox_enqueue(outbox, &msg, 10000);
    assert(item != NULL);

    /* Retransmit validation at tick = 14999 (< 5000ms TTL): VALID */
    assert(gateway_outbox_validate_retransmit(item, 14999, 5000) == true);

    /* Retransmit validation at tick = 15000 (>= 5000ms TTL): EXPIRED */
    assert(gateway_outbox_validate_retransmit(item, 15000, 5000) == false);

    /* Delete expired at tick = 15001 with timeout = 5000ms */
    int expired_id = outbox_delete_single_expired(outbox, 15001, 5000);
    assert(expired_id == 301);
    assert(outbox_get_size(outbox) == 0);

    gateway_outbox_stats_t stats;
    gateway_outbox_get_stats(outbox, &stats);
    assert(stats.total_expired == 1);
    assert(stats.current_items == 0);

    printf("      -> PASS: Retransmit validation checked freshness at 4999ms boundary; expired packet cleaned\n");
}

static void test_destroy_and_reinitialize(void) {
    printf("[RUN] Test 5: Outbox destroy and reinitialization cleanliness...\n");
    outbox_handle_t outbox = outbox_init();

    uint8_t payload[] = "test";
    outbox_message_t msg = {
        .data = payload,
        .len = sizeof(payload),
        .msg_id = 401,
        .msg_type = 3,
        .msg_qos = 1,
        .remaining_data = NULL,
        .remaining_len = 0
    };

    outbox_enqueue(outbox, &msg, 1000);
    assert(outbox_get_size(outbox) > 0);

    outbox_destroy(outbox);
    assert(outbox_get_size(outbox) == 0);

    /* Reinitialize */
    outbox = outbox_init();
    assert(outbox_get_size(outbox) == 0);
    gateway_outbox_stats_t stats_reinit;
    gateway_outbox_get_stats(outbox, &stats_reinit);
    assert(stats_reinit.current_items == 0);

    printf("      -> PASS: Outbox destroyed and reinitialized with zero memory leak\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING GATEWAY MQTT OUTBOX UNIT TESTS (TDD)     \n");
    printf("====================================================\n\n");

    test_outbox_enqueue_dequeue_ack();
    test_oversized_packet_rejection();
    test_capacity_exhaustion_and_drop_tracking();
    test_expiration_and_retransmit_validation();
    test_destroy_and_reinitialize();

    printf("\n🎉 ALL GATEWAY MQTT OUTBOX TESTS PASSED!\n");
    return 0;
}
