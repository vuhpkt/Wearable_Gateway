#ifndef UART_FRAME_DEF_H
#define UART_FRAME_DEF_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════
 *  Inter-Chip UART Bridge Protocol Constants
 *  Physical: 460800 baud (safe for 3-wire without RTS/CTS),
 *            8 data bits, no parity, 1 stop bit (8-N-1)
 * ════════════════════════════════════════════════════════════ */
#define UART_BRIDGE_BAUDRATE           460800U /* 460.8 kbps: ~46 kB/s (10x bandwidth overhead, zero FIFO overflow) */
#define UART_FRAME_SOF                 0xAAU   /* Start of Frame sync byte */
#define UART_MAX_PAYLOAD_LEN           128U
#define UART_FRAME_HEADER_LEN          4U      /* [SOF][NODE_ID][PKT_TYPE][LEN] */
#define UART_FRAME_OVERHEAD            5U      /* HEADER (4B) + CRC8 (1B) */
#define UART_MAX_FRAME_LEN             (UART_MAX_PAYLOAD_LEN + UART_FRAME_OVERHEAD)

#define MAX_NODES_SUPPORTED            10U     /* Node ID: 0 .. 9 */

/* ══════════════════════════════════════════════════════════════
 *  Packet Types
 * ════════════════════════════════════════════════════════════ */
typedef enum {
    UART_PKT_TYPE_ECG_BATCH   = 0x01, /* ECG waveform batch (100 bytes = 50 int16 samples) */
    UART_PKT_TYPE_VITALS      = 0x02, /* Vitals (5 bytes: hr_ecg, hr_ppg, spo2, temp_x10) */
    UART_PKT_TYPE_NODE_STATUS = 0x03, /* Connection state report (1 byte: 0=DISC, 1=CONN) */
    UART_PKT_TYPE_DOWNLINK    = 0x80, /* Downlink command forwarded to node (CMD_*) */
} uart_pkt_type_t;

/* ══════════════════════════════════════════════════════════════
 *  UART Frame Structure
 * ════════════════════════════════════════════════════════════ */
#pragma pack(push, 1)
typedef struct {
    uint8_t sof;        /* UART_FRAME_SOF (0xAA) */
    uint8_t node_id;    /* Target or Source Node index (0 .. 9) */
    uint8_t pkt_type;   /* uart_pkt_type_t */
    uint8_t length;     /* Payload byte count (1 .. 128) */
    uint8_t payload[UART_MAX_PAYLOAD_LEN];
    uint8_t crc8;       /* CRC-8 calculated over [node_id .. payload[length-1]] */
} uart_frame_t;
#pragma pack(pop)

/* ══════════════════════════════════════════════════════════════
 *  Fast CRC-8 Implementation (Polynomial 0x07: x^8 + x^2 + x + 1)
 *  Calculated over bytes from node_id to payload[length - 1]
 * ════════════════════════════════════════════════════════════ */
static inline uint8_t uart_crc8_calc(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief Pack raw payload into a standard UART bridge frame with SOF and CRC-8.
 *
 * @param dest Output buffer (must have capacity >= len + 5)
 * @param max_dest_len Size of dest buffer
 * @param node_id Source/Target node index (0 .. 9)
 * @param pkt_type Packet type enum
 * @param payload Pointer to payload buffer
 * @param len Payload byte length (1 .. 128)
 * @return Total frame length (len + 5) on success, or negative error code (< 0)
 */
static inline int uart_frame_pack(uint8_t *dest, size_t max_dest_len,
                                  uint8_t node_id, uint8_t pkt_type,
                                  const void *payload, uint8_t len) {
    if (dest == NULL || payload == NULL) {
        return -1;
    }
    if (len == 0 || len > UART_MAX_PAYLOAD_LEN) {
        return -2;
    }
    if (node_id >= MAX_NODES_SUPPORTED) {
        return -3;
    }
    if (max_dest_len < (size_t)(len + UART_FRAME_OVERHEAD)) {
        return -4;
    }

    dest[0] = UART_FRAME_SOF;
    dest[1] = node_id;
    dest[2] = pkt_type;
    dest[3] = len;
    memcpy(&dest[4], payload, len);
    dest[4 + len] = uart_crc8_calc(&dest[1], 3 + len);

    return (int)(len + UART_FRAME_OVERHEAD);
}

#ifdef __cplusplus
}
#endif

#endif /* UART_FRAME_DEF_H */
