/**
 * @file test_ble_central_downlink.c
 * @brief Host Unit Tests for nRF Central BLE Downlink & Write Concurrency Guard
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1
#define HOST_TEST 1
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include "ble_protocol_def.h"
#include "ble_node_slots.h"
#include "ble_central_mgr.h"

/* Host mock tracking variables required by zephyr/kernel.h */
int64_t g_host_uptime_ms = 0;
uint32_t g_k_work_submit_count = 0;
uint32_t g_k_work_cancel_count = 0;
uint32_t g_host_bt_disconnect_count = 0;
struct bt_conn *g_host_last_disconnected_conn = NULL;

int main(void) {
    printf("====================================================\n");
    printf("  RUNNING BLE CENTRAL DOWNLINK WRITE TESTS (TDD)     \n");
    printf("====================================================\n\n");

    ble_node_slots_init();
    bt_addr_le_t addr = { .type = 0, .a = { .val = { 0x67, 0x45, 0x23, 0x01, 0x5F, 0xE4 } } };
    ble_node_slot_t *slot = ble_node_slot_alloc(&addr);
    assert(slot != NULL);
    struct bt_conn dummy_conn = { .id = 1 };
    slot->conn = &dummy_conn;
    slot->state = NODE_STATE_CONNECTED;

    uint8_t cmd_payload[] = { CMD_VITAL_CFG, 0xE8, 0x03 }; // 1000 ms

    /* ─── TEST 1: Từ chối khi chưa hoàn tất Stage 4 GATT Discovery ─── */
    printf("[RUN] Test 1: Reject write when rx_char_val_handle == 0...\n");
    slot->rx_char_val_handle = 0; // Chưa tìm thấy 0x1402
    int ret = ble_central_mgr_write_cmd(slot->slot_id, cmd_payload, sizeof(cmd_payload));
    assert(ret == -ENOTCONN);
    assert(g_mock_bt_gatt_write_call_count == 0);
    printf("      -> PASS: Safely rejected write to uninitialized characteristic\n");

    /* ─── TEST 2: Ghi thành công lần đầu ─── */
    printf("[RUN] Test 2: Successful GATT write execution...\n");
    slot->rx_char_val_handle = 0x0024;
    slot->state = NODE_STATE_SUBSCRIBED;
    ret = ble_central_mgr_write_cmd(slot->slot_id, cmd_payload, sizeof(cmd_payload));
    assert(ret == 0);
    assert(slot->write_in_progress == true);
    assert(g_mock_bt_gatt_write_call_count == 1);
    assert(g_mock_last_write_params != NULL);
    assert(g_mock_last_write_params->handle == 0x0024);
    assert(g_mock_last_write_params->length == 3);
    printf("      -> PASS: Command queued and write_in_progress flag asserted\n");

    /* ─── TEST 3: Chống race condition & -EBUSY khi ghi đè ─── */
    printf("[RUN] Test 3: Overlapping write protection returns -EBUSY...\n");
    ret = ble_central_mgr_write_cmd(slot->slot_id, cmd_payload, sizeof(cmd_payload));
    assert(ret == -EBUSY);
    assert(g_mock_bt_gatt_write_call_count == 1); // Không gọi thêm bt_gatt_write
    printf("      -> PASS: -EBUSY guard prevented corrupting in-flight write params\n");

    /* ─── TEST 4: Callback hoàn tất giải phóng cờ bận ─── */
    printf("[RUN] Test 4: ATT Write Response callback clears write_in_progress...\n");
    // Giả lập Zephyr gọi write callback
    assert(g_mock_last_write_params->func != NULL);
    g_mock_last_write_params->func(slot->conn, 0, g_mock_last_write_params);
    assert(slot->write_in_progress == false);

    // Bây giờ có thể ghi lệnh tiếp theo bình thường
    ret = ble_central_mgr_write_cmd(slot->slot_id, cmd_payload, sizeof(cmd_payload));
    assert(ret == 0);
    assert(slot->write_in_progress == true);
    assert(g_mock_bt_gatt_write_call_count == 2);
    printf("      -> PASS: write_in_progress cleanly cleared upon response\n");

    /* ─── TEST 5: Slot Free dọn sạch cờ bận (tránh kẹt vĩnh viễn) ─── */
    printf("[RUN] Test 5: Slot free clears in-flight state...\n");
    ble_node_slot_free(slot);
    assert(slot->write_in_progress == false);
    assert(slot->rx_char_val_handle == 0);
    printf("      -> PASS: Disconnect cleanup prevents deadlocking reconnected slots\n");

    printf("\n🎉 ALL BLE CENTRAL DOWNLINK TESTS PASSED!\n");
    return 0;
}
