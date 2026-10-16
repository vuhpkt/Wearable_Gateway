/* Standard C Libraries */
#include <string.h>

/* ESP-IDF System Headers */
#include <esp_log.h>

/* Project Local Headers */
#include "uart_rx_parser.h"


static const char *TAG __attribute__((unused)) = "uart_parser";

static uart_parser_state_t s_state = PARSER_STATE_WAIT_SOF;
static uart_rx_frame_t s_current_frame;
static uint8_t s_payload_idx = 0;
static uint8_t s_crc_buffer[3 + UART_MAX_PAYLOAD_LEN];
static uart_frame_cb_t s_frame_cb = NULL;
static uint32_t s_valid_frames = 0;
static uint32_t s_crc_errors = 0;

static void parse_byte(uint8_t byte) {
    switch (s_state) {
        case PARSER_STATE_WAIT_SOF:
            if (byte == UART_FRAME_SOF) {
                s_state = PARSER_STATE_READ_NODE_ID;
            }
            break;

        case PARSER_STATE_READ_NODE_ID:
            if (byte < MAX_NODES_SUPPORTED) {
                s_current_frame.node_id = byte;
                s_crc_buffer[0] = byte;
                s_state = PARSER_STATE_READ_PKT_TYPE;
            } else if (byte == UART_FRAME_SOF) {
                /* Vẫn có thể là SOF thật nếu byte trước là rác */
                s_state = PARSER_STATE_READ_NODE_ID;
            } else {
                s_state = PARSER_STATE_WAIT_SOF;
            }
            break;

        case PARSER_STATE_READ_PKT_TYPE:
            if (byte == UART_PKT_TYPE_ECG_BATCH || 
                byte == UART_PKT_TYPE_VITALS || 
                byte == UART_PKT_TYPE_NODE_STATUS || 
                byte == UART_PKT_TYPE_DOWNLINK) {
                s_current_frame.pkt_type = byte;
                s_crc_buffer[1] = byte;
                s_state = PARSER_STATE_READ_LEN;
            } else if (byte == UART_FRAME_SOF) {
                s_state = PARSER_STATE_READ_NODE_ID;
            } else {
                s_state = PARSER_STATE_WAIT_SOF;
            }
            break;  

        case PARSER_STATE_READ_LEN:
            if (byte > 0 && byte <= UART_MAX_PAYLOAD_LEN) {
                bool valid_len = true;
                if (s_current_frame.pkt_type == UART_PKT_TYPE_ECG_BATCH) {
                    /* ECG batch payload must be an even byte length (multiple of sizeof(int16_t), 1..64 samples) */
                    if ((byte % sizeof(int16_t) != 0) || (byte < sizeof(int16_t))) {
                        valid_len = false;
                    }
                } else if (s_current_frame.pkt_type == UART_PKT_TYPE_VITALS && byte != 5) {
                    valid_len = false;
                } else if (s_current_frame.pkt_type == UART_PKT_TYPE_NODE_STATUS && byte != 1) {
                    valid_len = false;
                }

                if (valid_len) {
                    s_current_frame.len = byte;
                    s_crc_buffer[2] = byte;
                    s_payload_idx = 0;
                    s_state = PARSER_STATE_READ_PAYLOAD;
                } else if (byte == UART_FRAME_SOF) {
                    s_state = PARSER_STATE_READ_NODE_ID;
                } else {
                    s_state = PARSER_STATE_WAIT_SOF;
                }
            } else if (byte == UART_FRAME_SOF) {
                s_state = PARSER_STATE_READ_NODE_ID;
            } else {
                s_state = PARSER_STATE_WAIT_SOF;
            }
            break;

        case PARSER_STATE_READ_PAYLOAD:
            s_current_frame.payload[s_payload_idx] = byte;
            s_crc_buffer[3 + s_payload_idx] = byte;
            s_payload_idx++;
            if (s_payload_idx == s_current_frame.len) {
                s_state = PARSER_STATE_CHECK_CRC8;
            }
            break;

        case PARSER_STATE_CHECK_CRC8: {
            uint8_t expected_crc = uart_crc8_calc(s_crc_buffer, 3 + s_current_frame.len);
            if (byte == expected_crc) {
                s_valid_frames++;
                if (s_frame_cb != NULL) {
                    s_frame_cb(&s_current_frame);
                }
            } else {
                s_crc_errors++;
                ESP_LOGW(TAG, "CRC Drop! Recv=0x%02X != Calc=0x%02X (Node=%u, Type=0x%02X, Total Errors=%lu)",
                         byte, expected_crc, s_current_frame.node_id, s_current_frame.pkt_type, s_crc_errors);
            }
            /* Luôn reset về WAIT_SOF để đón khung tiếp theo */
            s_state = PARSER_STATE_WAIT_SOF;
            break;
        }

        default:
            s_state = PARSER_STATE_WAIT_SOF;
            break;
    }
}

void uart_rx_parser_init(uart_frame_cb_t callback) {
    s_frame_cb = callback;
    s_state = PARSER_STATE_WAIT_SOF;
    s_payload_idx = 0;
    s_valid_frames = 0;
    s_crc_errors = 0;
}

void uart_rx_parser_process(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        parse_byte(data[i]);
    }
}