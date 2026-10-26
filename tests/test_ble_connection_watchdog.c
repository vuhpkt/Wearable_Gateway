/**
 * @file test_ble_connection_watchdog.c
 * @brief Host Unit Tests for BLE Central Connection & Handshake Watchdog (Task 4)
 *
 * Verifies against PRODUCTION ble_central_mgr.c via host test seam:
 * 1. Standard Lifecycle: Connect -> MTU -> Discovery -> Subscribe succeeds -> Handshake timer cancelled.
 * 2. Genuine Timeout: Stuck connection exceeds 10000ms deadline -> Handshake aborts and disconnects stuck node.
 * 3. Critical P2 Race Elimination: Delayed work item submitted for Node A expires after Node A succeeds and
 *    Node B starts connecting. The delayed work item MUST NOT disconnect Node B or abort its handshake!
 * 4. Unrelated Node Disconnect: Disconnect of an already subscribed or unrelated node does not cancel active handshake.
 * 5. Pointer Reuse Safety: When memory address of a closed connection is recycled for a new connection,
 *    stale work items are rejected due to generation counter mismatch.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include "ble_protocol_def.h"
#include "ble_node_slots.h"

/* Monotonic host simulated clock and mock tracking */
int64_t g_host_uptime_ms = 0;
uint32_t g_k_work_submit_count = 0;
uint32_t g_k_work_cancel_count = 0;
uint32_t g_host_bt_disconnect_count = 0;
struct bt_conn *g_host_last_disconnected_conn = NULL;

/* Host test seam functions implemented in ble_central_mgr.c */
extern uint32_t ble_central_mgr_get_handshake_gen(void);
extern bool ble_central_mgr_is_connecting(void);
extern struct bt_conn *ble_central_mgr_get_connecting_conn(void);
extern void ble_central_mgr_test_arm_timeout(struct bt_conn *conn);
extern void ble_central_mgr_test_timer_expiry(void);
extern void ble_central_mgr_test_work_handler(void);
extern void ble_central_mgr_test_session_cleanup(struct bt_conn *conn);
extern void ble_central_mgr_test_subscribe_cb(struct bt_conn *conn, uint8_t err);
extern void ble_central_mgr_test_disconnected_cb(struct bt_conn *conn, uint8_t reason);
extern void ble_central_mgr_test_init(void);
extern void ble_central_mgr_test_set_ticket_for_race_test(uint32_t gen, struct bt_conn *conn, int64_t deadline);
extern struct k_timer *ble_central_mgr_test_get_timer(void);
extern struct k_work *ble_central_mgr_test_get_work(void);

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING BLE CONNECTION WATCHDOG UNIT TESTS (TDD) \n");
    printf("   (Testing PRODUCTION ble_central_mgr.c via Seam)  \n");
    printf("====================================================\n\n");

    ble_node_slots_init();
    ble_central_mgr_test_init();

    struct bt_conn conn_a = { .id = 1, .ref_count = 1, .disconnected = false };
    struct bt_conn conn_b = { .id = 2, .ref_count = 1, .disconnected = false };
    struct bt_conn conn_c = { .id = 3, .ref_count = 1, .disconnected = false };

    bt_addr_le_t addr_a = { .type = 0, .a = { .val = { 1, 2, 3, 4, 5, 1 } } };
    ble_node_slot_t *slot_a = ble_node_slot_alloc(&addr_a);
    assert(slot_a != NULL);
    slot_a->conn = &conn_a;

    bt_addr_le_t addr_b = { .type = 0, .a = { .val = { 1, 2, 3, 4, 5, 2 } } };
    ble_node_slot_t *slot_b = ble_node_slot_alloc(&addr_b);
    assert(slot_b != NULL);
    slot_b->conn = &conn_b;

    bt_addr_le_t addr_c = { .type = 0, .a = { .val = { 1, 2, 3, 4, 5, 3 } } };
    ble_node_slot_t *slot_c = ble_node_slot_alloc(&addr_c);
    assert(slot_c != NULL);
    slot_c->conn = &conn_c;

    struct k_timer *timer = ble_central_mgr_test_get_timer();
    struct k_work *work = ble_central_mgr_test_get_work();

    /* ─── TEST 1: Standard Lifecycle (Success Path) ─── */
    printf("[RUN] Test 1: Verify normal handshake connect -> subscribe -> cleanup...\n");
    g_host_uptime_ms = 1000;
    ble_central_mgr_test_arm_timeout(&conn_a);
    assert(ble_central_mgr_is_connecting() == true);
    assert(ble_central_mgr_get_connecting_conn() == &conn_a);
    assert(ble_central_mgr_get_handshake_gen() == 1);
    assert(timer->is_active == true);

    /* Handshake completes successfully at t=1500ms via production subscribe_cb */
    g_host_uptime_ms = 1500;
    ble_central_mgr_test_subscribe_cb(&conn_a, 0);
    assert(ble_central_mgr_is_connecting() == false);
    assert(ble_central_mgr_get_connecting_conn() == NULL);
    assert(ble_central_mgr_get_handshake_gen() == 2);
    assert(timer->is_active == false);
    assert(conn_a.disconnected == false);
    printf("      -> PASS: Handshake completed cleanly, timer stopped, lock released\n");

    /* ─── TEST 2: Genuine Timeout on Stuck Connection ─── */
    printf("[RUN] Test 2: Verify stuck handshake is aborted after 10s deadline...\n");
    conn_a.disconnected = false;
    g_host_bt_disconnect_count = 0;
    g_host_uptime_ms = 2000;
    ble_central_mgr_test_arm_timeout(&conn_a);
    assert(ble_central_mgr_get_handshake_gen() == 3);

    /* Timer expires at t=12000ms (10,000ms deadline) */
    g_host_uptime_ms = 12000;
    ble_central_mgr_test_timer_expiry();
    assert(work->is_pending == true);

    /* Workqueue executes timeout handler */
    ble_central_mgr_test_work_handler();
    assert(conn_a.disconnected == true);
    assert(g_host_bt_disconnect_count == 1);
    assert(g_host_last_disconnected_conn == &conn_a);
    printf("      -> PASS: Stuck connection aborted cleanly upon genuine timeout\n");

    /* ─── TEST 3: Critical P2 Race (Work A Delayed, Node B Connecting) ─── */
    printf("[RUN] Test 3: Verify delayed work item from Node A DOES NOT abort Node B...\n");
    conn_a.disconnected = false;
    conn_b.disconnected = false;
    g_host_bt_disconnect_count = 0;
    g_k_work_cancel_count = 0;

    /* 1. Node A starts handshake at t=10000ms */
    g_host_uptime_ms = 10000;
    uint32_t gen_before_a = ble_central_mgr_get_handshake_gen();
    ble_central_mgr_test_arm_timeout(&conn_a);
    uint32_t gen_a = ble_central_mgr_get_handshake_gen();
    assert(gen_a == gen_before_a + 1);

    /* 2. Timer fires at t=20000ms, submitting work item to workqueue */
    g_host_uptime_ms = 20000;
    ble_central_mgr_test_timer_expiry();
    assert(work->is_pending == true);

    /* 3. Before workqueue executes, Node A completes subscribe at t=20001ms */
    g_host_uptime_ms = 20001;
    ble_central_mgr_test_subscribe_cb(&conn_a, 0);
    assert(ble_central_mgr_is_connecting() == false);
    assert(ble_central_mgr_get_handshake_gen() == gen_a + 1); /* Generation invalidated! */
    assert(g_k_work_cancel_count == 1);

    /* 4. Scanner finds Node B and initiates connection at t=20010ms */
    g_host_uptime_ms = 20010;
    ble_central_mgr_test_arm_timeout(&conn_b);
    assert(ble_central_mgr_is_connecting() == true);
    assert(ble_central_mgr_get_connecting_conn() == &conn_b);
    uint32_t gen_b = ble_central_mgr_get_handshake_gen();
    assert(gen_b == gen_a + 2);

    /* 5. System workqueue finally executes delayed work item (simulating race) */
    /* In vulnerable code, this would have aborted s_connecting_conn and killed Node B! */
    ble_central_mgr_test_work_handler();

    /* Verification: Node B MUST NOT be aborted! */
    assert(conn_b.disconnected == false);
    assert(g_host_bt_disconnect_count == 0);
    assert(ble_central_mgr_is_connecting() == true);
    assert(ble_central_mgr_get_connecting_conn() == &conn_b);
    printf("      -> PASS: Delayed work from Node A rejected; Node B protected and active!\n");

    /* ─── TEST 4: Early Disconnect of Unrelated Node ─── */
    printf("[RUN] Test 4: Verify disconnect of unrelated node does not cancel active handshake...\n");
    /* Node B is currently handshaking */
    conn_c.disconnected = false;
    ble_central_mgr_test_disconnected_cb(&conn_c, BT_HCI_ERR_REMOTE_USER_TERM_CONN); /* Node C disconnects */

    /* Node B must remain untouched */
    assert(ble_central_mgr_is_connecting() == true);
    assert(ble_central_mgr_get_connecting_conn() == &conn_b);
    assert(timer->is_active == true);
    printf("      -> PASS: Unrelated node disconnect ignored by active handshake\n");

    /* ─── TEST 5: Pointer Address Reuse Safety ─── */
    printf("[RUN] Test 5: Verify pointer address reuse with distinct generation counter...\n");
    /* Complete Node B */
    ble_central_mgr_test_subscribe_cb(&conn_b, 0);
    assert(ble_central_mgr_is_connecting() == false);

    /* Node D reuses the exact same memory address as conn_a */
    struct bt_conn *conn_d = &conn_a;
    conn_d->disconnected = false;

    ble_central_mgr_test_arm_timeout(conn_d);
    uint32_t gen_d = ble_central_mgr_get_handshake_gen();

    /* Stale ticket simulation with gen_a (old conn_a generation) */
    ble_central_mgr_test_set_ticket_for_race_test(gen_a, conn_d, g_host_uptime_ms - 100);

    /* Execute stale work: generation mismatch must reject abort! */
    ble_central_mgr_test_work_handler();
    assert(conn_d->disconnected == false);
    assert(ble_central_mgr_is_connecting() == true);
    assert(ble_central_mgr_get_handshake_gen() == gen_d);
    printf("      -> PASS: Pointer reuse safely discriminated via generation counter\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL BLE CONNECTION WATCHDOG TESTS PASSED (100%%)!\n");
    printf("====================================================\n");
    return 0;
}
