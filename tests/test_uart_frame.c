#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "common/uart_frame_def.h"

int main(void) {
    printf("==================================================\n");
    printf("   RUNNING UART FRAME PROTOCOL UNIT TESTS (TDD)   \n");
    printf("==================================================\n\n");

    uint8_t buffer[256];

    /* ─── TEST 1: Basic Node Status Frame (1-byte payload) ─── */
    printf("[RUN] Test 1: Pack Node Status frame (Node 0, Conn=1)...\n");
    uint8_t status_payload = 1;
    int frame_len = uart_frame_pack(buffer, sizeof(buffer), 0, UART_PKT_TYPE_NODE_STATUS, &status_payload, sizeof(status_payload));
    
    assert(frame_len == 6); /* 1 + 5 = 6 bytes */
    assert(buffer[0] == UART_FRAME_SOF); /* 0xAA */
    assert(buffer[1] == 0); /* Node ID */
    assert(buffer[2] == UART_PKT_TYPE_NODE_STATUS); /* 0x03 */
    assert(buffer[3] == 1); /* Length */
    assert(buffer[4] == 1); /* Payload */
    uint8_t expected_crc = uart_crc8_calc(&buffer[1], 4);
    assert(buffer[5] == expected_crc);
    printf("      -> PASS: Frame [0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, CRC: 0x%02X] length=%d\n",
           buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], frame_len);

    /* ─── TEST 1b: Node Status Frame with 6-byte MAC (7-byte payload, Golden Vector) ─── */
    printf("[RUN] Test 1b: Pack Node Status frame with MAC (Node 0, Conn=1, E4:5F:01:23:45:67)...\n");
    uart_node_status_pkt_t status_mac_conn = {
        .status = 1,
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 }
    };
    frame_len = uart_frame_pack(buffer, sizeof(buffer), 0, UART_PKT_TYPE_NODE_STATUS, &status_mac_conn, sizeof(status_mac_conn));
    assert(frame_len == 12); /* 7 + 5 = 12 bytes */
    assert(buffer[0] == UART_FRAME_SOF);
    assert(buffer[1] == 0);
    assert(buffer[2] == UART_PKT_TYPE_NODE_STATUS);
    assert(buffer[3] == 7);
    assert(buffer[4] == 1); /* status */
    assert(memcmp(&buffer[5], status_mac_conn.mac, 6) == 0);
    assert(buffer[11] == 0x22); /* Golden vector CRC-8 = 0x22 (34) */
    printf("      -> PASS: 7-byte connect status matches Golden Vector CRC-8 = 0x%02X\n", buffer[11]);

    /* Disconnect golden vector check */
    uart_node_status_pkt_t status_mac_disc = {
        .status = 0,
        .mac = { 0xE4, 0x5F, 0x01, 0x23, 0x45, 0x67 }
    };
    frame_len = uart_frame_pack(buffer, sizeof(buffer), 0, UART_PKT_TYPE_NODE_STATUS, &status_mac_disc, sizeof(status_mac_disc));
    assert(frame_len == 12);
    assert(buffer[3] == 7);
    assert(buffer[4] == 0);
    assert(buffer[11] == 0xFD); /* Golden vector CRC-8 = 0xFD (253) */
    printf("      -> PASS: 7-byte disconnect status matches Golden Vector CRC-8 = 0x%02X\n", buffer[11]);

    /* ─── TEST 2: Vitals Frame (5-byte payload) ─── */
    printf("[RUN] Test 2: Pack Vitals frame (Node 2, HR=75, SpO2=98%%, Temp=36.5C)...\n");
    uint8_t vitals_raw[5] = { 75, 74, 98, 0x6D, 0x01 }; /* 365 = 0x016D (LE) */
    frame_len = uart_frame_pack(buffer, sizeof(buffer), 2, UART_PKT_TYPE_VITALS, vitals_raw, sizeof(vitals_raw));

    assert(frame_len == 10); /* 5 + 5 = 10 bytes */
    assert(buffer[0] == UART_FRAME_SOF);
    assert(buffer[1] == 2);
    assert(buffer[2] == UART_PKT_TYPE_VITALS);
    assert(buffer[3] == 5);
    assert(memcmp(&buffer[4], vitals_raw, 5) == 0);
    assert(buffer[9] == uart_crc8_calc(&buffer[1], 3 + 5));
    printf("      -> PASS: Vitals frame correctly packed with CRC8=0x%02X\n", buffer[9]);

    /* ─── TEST 3: ECG Waveform Batch (100 bytes payload) ─── */
    printf("[RUN] Test 3: Pack ECG Batch frame (Node 9, 50 samples = 100 bytes)...\n");
    uint8_t ecg_mock[100];
    for (int i = 0; i < 100; i++) {
        ecg_mock[i] = (uint8_t)(i * 3);
    }
    frame_len = uart_frame_pack(buffer, sizeof(buffer), 9, UART_PKT_TYPE_ECG_BATCH, ecg_mock, sizeof(ecg_mock));

    assert(frame_len == 105); /* 100 + 5 = 105 bytes */
    assert(buffer[0] == UART_FRAME_SOF);
    assert(buffer[1] == 9);
    assert(buffer[2] == UART_PKT_TYPE_ECG_BATCH);
    assert(buffer[3] == 100);
    assert(memcmp(&buffer[4], ecg_mock, 100) == 0);
    assert(buffer[104] == uart_crc8_calc(&buffer[1], 3 + 100));
    printf("      -> PASS: 100-byte ECG batch packed with CRC8=0x%02X\n", buffer[104]);

    /* ─── TEST 4: CRC-8 Bit-Error Detection ─── */
    printf("[RUN] Test 4: Verify CRC-8 Bit-Error detection sensitivity...\n");
    uint8_t valid_crc = buffer[104];
    /* Corrupt 1 bit in payload */
    buffer[50] ^= 0x01;
    uint8_t recalculated_crc = uart_crc8_calc(&buffer[1], 3 + 100);
    assert(recalculated_crc != valid_crc);
    /* Corrupt node_id */
    buffer[50] ^= 0x01; /* Restore payload */
    buffer[1] = 0;      /* Change Node ID */
    recalculated_crc = uart_crc8_calc(&buffer[1], 3 + 100);
    assert(recalculated_crc != valid_crc);
    printf("      -> PASS: Single-bit and Node ID corruptions 100%% caught by CRC-8\n");

    /* ─── TEST 5: Boundary & Error Handling ─── */
    printf("[RUN] Test 5: Verify input boundary checks...\n");
    /* Zero length */
    assert(uart_frame_pack(buffer, sizeof(buffer), 0, UART_PKT_TYPE_VITALS, vitals_raw, 0) < 0);
    /* Length exceeding 128 */
    assert(uart_frame_pack(buffer, sizeof(buffer), 0, UART_PKT_TYPE_VITALS, vitals_raw, 129) < 0);
    /* NULL payload */
    assert(uart_frame_pack(buffer, sizeof(buffer), 0, UART_PKT_TYPE_VITALS, NULL, 5) < 0);
    /* NULL destination */
    assert(uart_frame_pack(NULL, sizeof(buffer), 0, UART_PKT_TYPE_VITALS, vitals_raw, 5) < 0);
    /* Invalid Node ID (>= 10) */
    assert(uart_frame_pack(buffer, sizeof(buffer), 10, UART_PKT_TYPE_VITALS, vitals_raw, 5) < 0);
    assert(uart_frame_pack(buffer, sizeof(buffer), 255, UART_PKT_TYPE_VITALS, vitals_raw, 5) < 0);
    /* Destination buffer too small */
    assert(uart_frame_pack(buffer, 8, 0, UART_PKT_TYPE_VITALS, vitals_raw, 5) < 0); /* needs 10 */
    printf("      -> PASS: All invalid boundaries safely rejected (no buffer overrun)\n");

    printf("\n==================================================\n");
    printf("  🎉 ALL UART FRAME PROTOCOL UNIT TESTS PASSED!   \n");
    printf("==================================================\n");
    return 0;
}
