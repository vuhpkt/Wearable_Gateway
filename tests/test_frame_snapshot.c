/**
 * @file test_frame_snapshot.c
 * @brief Host-based Unit Tests for Core 1 -> Core 0 Immutable Frame Snapshot Queue (TDD)
 *
 * Verifies Architecture v2 Contract §4.4:
 * 1. Immutable copy-by-value: mutating source parser buffer does NOT affect queued tuple.
 * 2. Static bounded capacity (64 items): Core 1 non-blocking drop when full, drop counter tracked.
 * 3. Core 0 never queries slot for MAC: canonical MAC is carried in the tuple itself.
 * 4. Revocation guard: if session is revoked between enqueue and dequeue, tuple is rejected before publish.
 * 5. Control priority / mailbox: control frames are not starved by 50Hz ECG telemetry.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "frame_snapshot.h"

static void test_immutable_copy_by_value(void) {
    printf("[RUN] Test 1: Immutable copy-by-value isolation...\n");
    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    uint8_t volatile_buffer[100];
    memset(volatile_buffer, 0x55, sizeof(volatile_buffer));

    frame_snapshot_tuple_t item = {
        .bridge_epoch = 100ULL,
        .session_id = 1,
        .seq = 10,
        .node_id = 0,
        .stream_type = STREAM_TYPE_VITALS,
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 },
        .timestamp_utc_ms = 1718000000000ULL,
        .central_rx_mono_ms = 50000ULL,
        .esp_rx_mono_ms = 50002ULL,
        .time_quality = TIME_QUAL_SYNCED,
        .payload_len = 5
    };
    memcpy(item.payload, volatile_buffer, 5);

    bool push_ok = frame_snapshot_queue_push(&queue, &item);
    assert(push_ok == true);

    /* Mutate the original buffer completely */
    memset(volatile_buffer, 0xFF, sizeof(volatile_buffer));
    memset(&item, 0x00, sizeof(item));

    /* Pop from queue */
    frame_snapshot_tuple_t popped;
    bool pop_ok = frame_snapshot_queue_pop(&queue, &popped, 0);
    assert(pop_ok == true);
    assert(popped.session_id == 1);
    assert(popped.seq == 10);
    assert(popped.node_id == 0);
    assert(popped.mac[0] == 0xE4 && popped.mac[5] == 0x67);
    assert(popped.payload[0] == 0x55 && popped.payload[4] == 0x55);
    printf("      -> PASS: Buffer mutation after push did not corrupt queued tuple\n");
}

static void test_bounded_queue_non_blocking_drop(void) {
    printf("[RUN] Test 2: Bounded queue capacity (64) and non-blocking drop...\n");
    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    frame_snapshot_tuple_t item = {
        .session_id = 1,
        .payload_len = 5
    };

    /* Fill all 64 slots */
    for (uint32_t i = 0; i < FRAME_SNAPSHOT_QUEUE_CAPACITY; i++) {
        item.seq = (uint32_t)i;
        bool ok = frame_snapshot_queue_push(&queue, &item);
        assert(ok == true);
    }
    assert(frame_snapshot_queue_count(&queue) == FRAME_SNAPSHOT_QUEUE_CAPACITY);
    assert(frame_snapshot_queue_dropped(&queue) == 0);

    /* 65th push MUST fail non-blocking without stalling Core 1 */
    item.seq = 999;
    bool overflow_push = frame_snapshot_queue_push(&queue, &item);
    assert(overflow_push == false);
    assert(frame_snapshot_queue_dropped(&queue) == 1);

    /* Dequeue 1 item, then next push succeeds */
    frame_snapshot_tuple_t popped;
    bool pop_ok = frame_snapshot_queue_pop(&queue, &popped, 0);
    assert(pop_ok == true);
    assert(popped.seq == 0);

    bool retry_push = frame_snapshot_queue_push(&queue, &item);
    assert(retry_push == true);
    printf("      -> PASS: 64-item limit strictly bounded, overflow incremented drop counter\n");
}

static void test_revocation_guard_before_publish(void) {
    printf("[RUN] Test 3: Revocation guard between enqueue and dequeue (Session + Epoch)...\n");
    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    revocation_table_t rev_table;
    revocation_table_init(&rev_table);
    revocation_table_set_active_session(&rev_table, 0, 1000ULL, 201); /* Node 0 bound to epoch 1000, session 201 */

    /* Enqueue telemetry for session 201 */
    frame_snapshot_tuple_t item1 = {
        .node_id = 0,
        .bridge_epoch = 1000ULL,
        .session_id = 201,
        .seq = 1,
        .payload_len = 5
    };
    frame_snapshot_queue_push(&queue, &item1);

    /* Disconnection / slot reuse occurs: active session changed to 202! */
    revocation_table_set_active_session(&rev_table, 0, 1000ULL, 202);

    /* Core 0 dequeues item1 */
    frame_snapshot_tuple_t popped;
    bool pop_ok = frame_snapshot_queue_pop(&queue, &popped, 0);
    assert(pop_ok == true);

    /* Revocation check MUST detect stale session 201 != 202 */
    bool valid = revocation_table_is_session_valid(&rev_table, popped.node_id, popped.bridge_epoch, popped.session_id);
    assert(valid == false);

    /* Valid item for current session 202 */
    frame_snapshot_tuple_t item2 = {
        .node_id = 0,
        .bridge_epoch = 1000ULL,
        .session_id = 202,
        .seq = 1,
        .payload_len = 5
    };
    frame_snapshot_queue_push(&queue, &item2);
    frame_snapshot_queue_pop(&queue, &popped, 0);
    valid = revocation_table_is_session_valid(&rev_table, popped.node_id, popped.bridge_epoch, popped.session_id);
    assert(valid == true);

    /* Central rebooted: epoch changed to 1001ULL, and session counter reset to 202! */
    revocation_table_set_active_session(&rev_table, 0, 1001ULL, 202);
    /* Late frame from old epoch 1000ULL with session 202 MUST be rejected! */
    valid = revocation_table_is_session_valid(&rev_table, popped.node_id, popped.bridge_epoch, popped.session_id);
    assert(valid == false);

    /* Stale disconnect from old epoch 1000ULL MUST NOT clear active binding of epoch 1001ULL! */
    revocation_table_revoke_slot(&rev_table, 0, 1000ULL, 202);
    valid = revocation_table_is_session_valid(&rev_table, 0, 1001ULL, 202);
    assert(valid == true);

    /* Matching disconnect clears binding */
    revocation_table_revoke_slot(&rev_table, 0, 1001ULL, 202);
    valid = revocation_table_is_session_valid(&rev_table, 0, 1001ULL, 202);
    assert(valid == false);

    printf("      -> PASS: Tuples from revoked session or old epoch dropped before publish\n");
}

static void test_control_mailbox_priority(void) {
    printf("[RUN] Test 4: Control mailbox priority over telemetry queue...\n");
    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    /* Push 5 telemetry items */
    for (int i = 0; i < 5; i++) {
        frame_snapshot_tuple_t telem = {
            .node_id = (uint8_t)i,
            .stream_type = STREAM_TYPE_ECG,
            .seq = (uint32_t)i
        };
        frame_snapshot_queue_push(&queue, &telem);
    }

    /* Send high-priority control item */
    frame_snapshot_tuple_t ctrl = {
        .node_id = 0,
        .stream_type = STREAM_TYPE_STATUS,
        .seq = 999
    };
    bool push_ctrl = frame_snapshot_queue_push_priority(&queue, &ctrl);
    assert(push_ctrl == true);

    /* First pop MUST be the priority control item! */
    frame_snapshot_tuple_t popped;
    bool pop_ok = frame_snapshot_queue_pop(&queue, &popped, 0);
    assert(pop_ok == true);
    assert(popped.stream_type == STREAM_TYPE_STATUS);
    assert(popped.seq == 999);
    printf("      -> PASS: Control frame prioritized and serviced before regular telemetry\n");
}

static void test_priority_capacity_16_and_drop_tracking(void) {
    printf("[RUN] Test 5: Priority mailbox burst of 10 nodes snapshot & capacity 16...\n");
    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    /* Burst of 10 status items from Central snapshot */
    for (uint32_t i = 0; i < 10; i++) {
        frame_snapshot_tuple_t status = {
            .node_id = (uint8_t)i,
            .stream_type = STREAM_TYPE_STATUS,
            .seq = i
        };
        bool ok = frame_snapshot_queue_push_priority(&queue, &status);
        assert(ok == true);
    }
    assert(queue.prio_count == 10);
    assert(frame_snapshot_queue_prio_dropped(&queue) == 0);

    /* Fill remaining slots up to FRAME_SNAPSHOT_PRIORITY_CAPACITY */
    for (uint32_t i = 10; i < FRAME_SNAPSHOT_PRIORITY_CAPACITY; i++) {
        frame_snapshot_tuple_t status = {
            .node_id = (uint8_t)(i % 10),
            .stream_type = STREAM_TYPE_STATUS,
            .seq = i
        };
        bool ok = frame_snapshot_queue_push_priority(&queue, &status);
        assert(ok == true);
    }
    assert(queue.prio_count == FRAME_SNAPSHOT_PRIORITY_CAPACITY);

    /* (FRAME_SNAPSHOT_PRIORITY_CAPACITY + 1)-th priority item MUST drop non-blocking and increment prio_dropped_count */
    frame_snapshot_tuple_t overflow = {
        .node_id = 0,
        .stream_type = STREAM_TYPE_STATUS,
        .seq = 999
    };
    bool ok_overflow = frame_snapshot_queue_push_priority(&queue, &overflow);
    assert(ok_overflow == false);
    assert(frame_snapshot_queue_prio_dropped(&queue) == 1);
    printf("      -> PASS: Snapshot bursts absorbed without drop, %u-item priority bound enforced\n",
           FRAME_SNAPSHOT_PRIORITY_CAPACITY);
}

static void test_revocation_reset_epoch_atomicity(void) {
    printf("[RUN] Test 6: Revocation table atomic epoch reset across all slots...\n");
    revocation_table_t rev_table;
    revocation_table_init(&rev_table);

    /* Bind 5 slots in epoch 100 */
    for (uint8_t i = 0; i < 5; i++) {
        revocation_table_set_active_session(&rev_table, i, 100ULL, 500 + i);
        assert(revocation_table_is_session_valid(&rev_table, i, 100ULL, 500 + i) == true);
    }

    /* Central reboots -> epoch jumps to 101 */
    revocation_table_reset_epoch(&rev_table, 101ULL);

    /* All 5 old sessions MUST be invalidated immediately */
    for (uint8_t i = 0; i < 5; i++) {
        assert(revocation_table_is_session_valid(&rev_table, i, 100ULL, 500 + i) == false);
        assert(revocation_table_is_session_valid(&rev_table, i, 101ULL, 500 + i) == false);
    }

    /* Disconnect from old epoch 100 arrives late: must NOT revoke a new session */
    revocation_table_set_active_session(&rev_table, 0, 101ULL, 999);
    revocation_table_revoke_slot(&rev_table, 0, 100ULL, 500);
    assert(revocation_table_is_session_valid(&rev_table, 0, 101ULL, 999) == true);

    printf("      -> PASS: Epoch reset atomically cleared all slots; late disconnect did not wipe new session\n");
}

static void test_synchronized_queue_stats(void) {
    printf("[RUN] Test 7: Synchronized queue stats snapshot...\n");
    frame_snapshot_queue_t queue;
    frame_snapshot_queue_init(&queue);

    frame_snapshot_tuple_t item = { .session_id = 1, .seq = 1 };
    frame_snapshot_queue_push(&queue, &item);
    frame_snapshot_queue_push_priority(&queue, &item);

    frame_snapshot_queue_stats_t stats;
    frame_snapshot_queue_get_stats(&queue, &stats);
    assert(stats.count == 1);
    assert(stats.prio_count == 1);
    assert(stats.dropped_count == 0);
    assert(stats.prio_dropped_count == 0);

    printf("      -> PASS: Queue stats snapshot cleanly retrieved\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING FRAME SNAPSHOT QUEUE UNIT TESTS (TDD)    \n");
    printf("====================================================\n\n");

    test_immutable_copy_by_value();
    test_bounded_queue_non_blocking_drop();
    test_revocation_guard_before_publish();
    test_control_mailbox_priority();
    test_priority_capacity_16_and_drop_tracking();
    test_revocation_reset_epoch_atomicity();
    test_synchronized_queue_stats();

    printf("\n🎉 ALL FRAME SNAPSHOT QUEUE TESTS PASSED!\n");
    return 0;
}
