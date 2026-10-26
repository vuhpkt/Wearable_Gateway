/**
 * @file test_ble_node_slots.c
 * @brief Host-based Unit Tests for BLE Central Node Slot Manager (TDD)
 *
 * Verifies:
 * 1. Clean initialization of all 10 multi-link slots.
 * 2. Sequential slot allocation for unique peripheral MAC addresses.
 * 3. Static MAC Affinity: Reconnecting peripheral reclaims its previous slot ID
 *    (Crucial for maintaining consistent Node1..Node10 IDs on ThingsBoard Gateway).
 * 4. Saturation Protection: Attempting to allocate an 11th node safely returns NULL.
 * 5. Dynamic slot free and query by address / connection pointer.
 * 6. Active and subscribed node count tracking.
 */

/* Standard C Libraries */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Zephyr Mock Headers & System Under Test */
#include "ble_node_slots.h"

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING BLE NODE SLOTS MANAGER UNIT TESTS (TDD)  \n");
    printf("====================================================\n\n");

    /* ─── TEST 1: Initialization ─── */
    printf("[RUN] Test 1: Verify clean initialization...\n");
    ble_node_slots_init();
    assert(ble_node_slots_get_active_count() == 0);
    assert(ble_node_slots_get_subscribed_count() == 0);
    printf("      -> PASS: All slots empty and reset\n");

    /* ─── TEST 2: Sequential Allocation (Slots 0 to 9) ─── */
    printf("[RUN] Test 2: Verify sequential allocation of 10 nodes...\n");
    bt_addr_le_t addrs[10];
    ble_node_slot_t *slots[10];

    for (uint8_t i = 0; i < 10; i++) {
        addrs[i].type = 0;
        memset(addrs[i].a.val, 0x10 + i, 6);
        slots[i] = ble_node_slot_alloc(&addrs[i]);
        assert(slots[i] != NULL);
        assert(slots[i]->slot_id == i);
        assert(slots[i]->is_used == true);
        assert(slots[i]->state == NODE_STATE_CONNECTING);
    }
    assert(ble_node_slots_get_active_count() == 10);
    printf("      -> PASS: 10 distinct slots allocated with IDs 0..9\n");

    /* ─── TEST 3: Saturation Protection (Slot 11) ─── */
    printf("[RUN] Test 3: Verify saturation protection when slots are full...\n");
    bt_addr_le_t addr_overflow = { .type = 0, .a = { .val = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF } } };
    ble_node_slot_t *overflow_slot = ble_node_slot_alloc(&addr_overflow);
    assert(overflow_slot == NULL);
    assert(ble_node_slots_get_active_count() == 10);
    printf("      -> PASS: 11th node safely rejected without buffer overrun\n");

    /* ─── TEST 4: Query by Address ─── */
    printf("[RUN] Test 4: Verify lookup by MAC address...\n");
    ble_node_slot_t *found = ble_node_slot_find_by_addr(&addrs[4]);
    assert(found != NULL);
    assert(found->slot_id == 4);

    bt_addr_le_t unknown_addr = { .type = 0, .a = { .val = { 0x99, 0x99, 0x99, 0x99, 0x99, 0x99 } } };
    assert(ble_node_slot_find_by_addr(&unknown_addr) == NULL);
    printf("      -> PASS: Correct slot retrieved by MAC\n");

    /* ─── TEST 5: Static MAC Affinity (Reclaim Disconnected Slot) ─── */
    printf("[RUN] Test 5: Verify Static MAC Affinity upon node reconnect...\n");
    /* Disconnect Node 3 */
    ble_node_slot_free(slots[3]);
    assert(ble_node_slots_get_active_count() == 9);
    assert(slots[3]->is_used == false);

    /* Disconnect Node 7 */
    ble_node_slot_free(slots[7]);
    assert(ble_node_slots_get_active_count() == 8);

    /* Node 7 reconnects first -> Must get Slot 7, NOT Slot 3! */
    ble_node_slot_t *reconn_node7 = ble_node_slot_alloc(&addrs[7]);
    assert(reconn_node7 != NULL);
    assert(reconn_node7->slot_id == 7);
    printf("      -> PASS: Node 7 reconnected and reclaimed Slot 7 (Static MAC Affinity confirmed)\n");

    /* Node 3 reconnects -> Must get Slot 3 */
    ble_node_slot_t *reconn_node3 = ble_node_slot_alloc(&addrs[3]);
    assert(reconn_node3 != NULL);
    assert(reconn_node3->slot_id == 3);
    printf("      -> PASS: Node 3 reconnected and reclaimed Slot 3\n");
    assert(ble_node_slots_get_active_count() == 10);

    /* ─── TEST 6: Subscribed Node Counting ─── */
    printf("[RUN] Test 6: Verify subscribed node counting...\n");
    assert(ble_node_slots_get_subscribed_count() == 0);
    slots[0]->state = NODE_STATE_SUBSCRIBED;
    slots[1]->state = NODE_STATE_SUBSCRIBED;
    slots[5]->state = NODE_STATE_SUBSCRIBED;
    assert(ble_node_slots_get_subscribed_count() == 3);
    printf("      -> PASS: Subscribed count = 3\n");

    /* ─── TEST 7: Dynamic Session ID Generation ─── */
    printf("[RUN] Test 7: Verify unique session_id allocation & change on reconnect...\n");
    ble_node_slots_init();
    bt_addr_le_t dev_addr = { .type = 0, .a = { .val = { 0x67, 0x45, 0x23, 0x01, 0x5F, 0xE4 } } };
    ble_node_slot_t *slot_a = ble_node_slot_alloc(&dev_addr);
    assert(slot_a != NULL);
    uint32_t session_1 = slot_a->session_id;
    assert(session_1 > 0);

    /* Disconnect */
    ble_node_slot_free(slot_a);
    assert(slot_a->session_id == 0);

    /* Reconnect with same device MAC */
    ble_node_slot_t *slot_b = ble_node_slot_alloc(&dev_addr);
    assert(slot_b != NULL);
    assert(slot_b->slot_id == slot_a->slot_id); /* Static MAC affinity preserves slot index */
    uint32_t session_2 = slot_b->session_id;
    assert(session_2 > 0);
    assert(session_2 != session_1); /* MUST allocate a new session_id! */
    printf("      -> PASS: Reconnection allocated distinct session_id (%u -> %u)\n", session_1, session_2);

    /* ─── TEST 8: Canonical Big-Endian MAC Order (Matching Watch LCD) ─── */
    printf("[RUN] Test 8: Verify MAC Big-Endian conversion...\n");
    /* Native Zephyr val: LSB -> MSB: [0x67, 0x45, 0x23, 0x01, 0x5F, 0xE4] */
    /* Canonical Big-Endian: MSB -> LSB: [0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67] */
    uint8_t expected_mac_be[6] = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 };
    assert(memcmp(slot_b->mac_be, expected_mac_be, 6) == 0);
    printf("      -> PASS: MAC converted to canonical Big-Endian E4:5F:01:23:45:67\n");

    /* ─── TEST 9: Context Capture & Monotonic Seq Increment ─── */
    printf("[RUN] Test 9: Verify context capture and seq increments...\n");
    ble_conn_context_t ctx;
    bool ok = ble_node_slot_capture_context(slot_b, &ctx, 50000ULL);
    assert(ok == true);
    assert(ctx.slot_id == slot_b->slot_id);
    assert(ctx.session_id == session_2);
    assert(ctx.seq == 1);
    assert(ctx.rx_mono_ms == 50000ULL);
    assert(memcmp(ctx.mac, expected_mac_be, 6) == 0);

    /* Second capture */
    ok = ble_node_slot_capture_context(slot_b, &ctx, 50020ULL);
    assert(ok == true);
    assert(ctx.seq == 2);
    assert(ctx.rx_mono_ms == 50020ULL);

    /* Third capture */
    ok = ble_node_slot_capture_context(slot_b, &ctx, 50040ULL);
    assert(ok == true);
    assert(ctx.seq == 3);
    printf("      -> PASS: Context captured with seq=1,2,3 monotonically\n");

    /* ─── TEST 10: Slot Reuse & Stale Connection Isolation ─── */
    printf("[RUN] Test 10: Verify slot reuse and stale connection isolation...\n");
    struct bt_conn *mock_conn_a = (struct bt_conn *)0x1000;
    struct bt_conn *mock_conn_b = (struct bt_conn *)0x2000;

    /* Slot 0 binds to conn_a */
    slot_b->conn = mock_conn_a;
    assert(ble_node_slot_find_by_conn(mock_conn_a) == slot_b);

    /* conn_a disconnects */
    ble_node_slot_free(slot_b);
    assert(ble_node_slot_find_by_conn(mock_conn_a) == NULL);

    /* Slot 0 reallocated for conn_b */
    bt_addr_le_t dev_addr_2 = { .type = 0, .a = { .val = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } } };
    ble_node_slot_t *slot_c = ble_node_slot_alloc(&dev_addr_2);
    assert(slot_c != NULL);
    slot_c->conn = mock_conn_b;

    /* Late notification / disconnect callback referencing stale conn_a arrives: */
    /* Must NOT match slot_c! */
    ble_node_slot_t *stale_match = ble_node_slot_find_by_conn(mock_conn_a);
    assert(stale_match == NULL);

    /* Valid callback for conn_b matches properly */
    ble_node_slot_t *active_match = ble_node_slot_find_by_conn(mock_conn_b);
    assert(active_match == slot_c);
    assert(active_match->session_id != session_2);
    printf("      -> PASS: Stale conn_a safely rejected, conn_b context strictly isolated\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL BLE NODE SLOTS UNIT TESTS PASSED (100%%)!   \n");
    printf("====================================================\n");
    return 0;
}
