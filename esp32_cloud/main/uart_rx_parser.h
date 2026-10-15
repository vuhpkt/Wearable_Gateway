#pragma once

/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>

/* Project Local Headers */
#include "uart_frame_def.h"


typedef enum {
    PARSER_STATE_WAIT_SOF,
    PARSER_STATE_READ_NODE_ID,
    PARSER_STATE_READ_PKT_TYPE,
    PARSER_STATE_READ_LEN,
    PARSER_STATE_READ_PAYLOAD,
    PARSER_STATE_CHECK_CRC8
} uart_parser_state_t;

typedef struct {
    uint8_t node_id;
    uint8_t pkt_type;
    uint8_t len;
    uint8_t payload[UART_MAX_PAYLOAD_LEN]; /* Tối đa 128 bytes */
} uart_rx_frame_t;

typedef void (*uart_frame_cb_t)(const uart_rx_frame_t *frame);

void uart_rx_parser_init(uart_frame_cb_t callback);

void uart_rx_parser_process(const uint8_t *data, size_t len);