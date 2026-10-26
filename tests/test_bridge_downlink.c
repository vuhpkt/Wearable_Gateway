/**
 * @file test_bridge_downlink.c
 * @brief Host Unit Tests for Central UART Bridge Downlink Frame Codec & CRC Sensitivity
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1
#define HOST_TEST 1

#include "uart_frame_def.h"
#include "nrf_central/src/uart_tx_bridge.h"

/* Mock tracking for ble_central_mgr_write_cmd */
static uint8_t s_mock_write_slot = 0xFF;
static uint8_t s_mock_write_payload[128];
static uint16_t s_mock_write_len = 0;
static uint32_t s_mock_write_count = 0;

int ble_central_mgr_write_cmd(uint8_t slot_id, const uint8_t *data, uint16_t len) {
    s_mock_write_slot = slot_id;
    s_mock_write_len = len;
    if (data && len > 0) {
        memcpy(s_mock_write_payload, data, len < sizeof(s_mock_write_payload) ? len : sizeof(s_mock_write_payload));
    }
    s_mock_write_count++;
    return 0;
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING UART BRIDGE DOWNLINK (0x80) TESTS        \n");
    printf("====================================================\n\n");

    uint8_t frame_buf[256];
    uint8_t cmd_payload[] = { 0xCB, 0x01, 0x1E, 0x00, 0x0A, 0x00, 0x01 }; // Mode CFG 7 bytes

    /* ─── TEST 1: Đóng gói chuẩn Frame 0x80 ─── */
    printf("[RUN] Test 1: Pack standard Downlink Frame (Slot 2, Type 0x80)...\n");
    int frame_len = uart_frame_pack(frame_buf, sizeof(frame_buf), 2, UART_PKT_TYPE_DOWNLINK, cmd_payload, sizeof(cmd_payload));
    assert(frame_len == (int)(sizeof(cmd_payload) + UART_FRAME_OVERHEAD)); // 7 + 5 = 12
    assert(frame_buf[0] == UART_FRAME_SOF);       // 0xAA
    assert(frame_buf[1] == 2);                     // Slot ID
    assert(frame_buf[2] == UART_PKT_TYPE_DOWNLINK);// 0x80
    assert(frame_buf[3] == 7);                     // Len
    assert(memcmp(&frame_buf[4], cmd_payload, 7) == 0);
    uint8_t expected_crc = uart_crc8_calc(&frame_buf[1], 3 + 7);
    assert(frame_buf[11] == expected_crc);
    printf("      -> PASS: Frame 0x80 packed correctly with CRC-8 = 0x%02X\n", expected_crc);

    /* ─── TEST 2: Thử nghiệm Bit-Flip CRC-8 toàn diện ─── */
    printf("[RUN] Test 2: CRC-8 Bit-Flip Sensitivity Test across all payload bytes...\n");
    for (int byte_idx = 1; byte_idx < frame_len - 1; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t corrupted[256];
            memcpy(corrupted, frame_buf, frame_len);
            corrupted[byte_idx] ^= (uint8_t)(1 << bit); // Đảo 1 bit

            uint8_t calc_crc = uart_crc8_calc(&corrupted[1], frame_len - 2);
            // CRC tính lại phải khác CRC lưu ở đuôi
            assert(calc_crc != corrupted[frame_len - 1]);
        }
    }
    printf("      -> PASS: 100%% of single-bit corruptions detected by CRC-8\n");

    /* ─── TEST 3: Kiểm tra biên Slot ID (Slot >= 10 phải bị reject) ─── */
    printf("[RUN] Test 3: Reject slot_id >= MAX_NODES_SUPPORTED (10)...\n");
    int err = uart_frame_pack(frame_buf, sizeof(frame_buf), 10, UART_PKT_TYPE_DOWNLINK, cmd_payload, sizeof(cmd_payload));
    assert(err < 0);
    err = uart_frame_pack(frame_buf, sizeof(frame_buf), 255, UART_PKT_TYPE_DOWNLINK, cmd_payload, sizeof(cmd_payload));
    assert(err < 0);
    printf("      -> PASS: Out-of-bounds slot IDs rejected\n");

    /* ─── TEST 4: Kiểm tra biên Payload Length ─── */
    printf("[RUN] Test 4: Payload length boundaries (0 and >128)...\n");
    err = uart_frame_pack(frame_buf, sizeof(frame_buf), 0, UART_PKT_TYPE_DOWNLINK, cmd_payload, 0);
    assert(err < 0); // Len 0 bị từ chối
    uint8_t huge_buf[129];
    err = uart_frame_pack(frame_buf, sizeof(frame_buf), 0, UART_PKT_TYPE_DOWNLINK, huge_buf, 129);
    assert(err < 0); // Len 129 bị từ chối
    printf("      -> PASS: Length boundaries 0 and 129 safely rejected\n");

    /* ─── TEST 5: Dispatch qua central_bridge_dispatch_downlink ─── */
    printf("[RUN] Test 5: central_bridge_dispatch_downlink validation...\n");
    assert(central_bridge_dispatch_downlink(10, cmd_payload, sizeof(cmd_payload)) == -1); // Invalid slot
    assert(central_bridge_dispatch_downlink(0, NULL, sizeof(cmd_payload)) == -1);          // NULL payload
    assert(central_bridge_dispatch_downlink(0, cmd_payload, 0) == -1);                     // Zero len
    assert(central_bridge_dispatch_downlink(3, cmd_payload, sizeof(cmd_payload)) == 0);    // Success
    assert(s_mock_write_count == 1);
    assert(s_mock_write_slot == 3);
    assert(s_mock_write_len == sizeof(cmd_payload));
    assert(memcmp(s_mock_write_payload, cmd_payload, sizeof(cmd_payload)) == 0);
    printf("      -> PASS: central_bridge_dispatch_downlink successfully forwards to BLE mgr\n");

    printf("\n🎉 ALL UART BRIDGE DOWNLINK TESTS PASSED!\n");
    return 0;
}
