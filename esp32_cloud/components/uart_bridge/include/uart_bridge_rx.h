#pragma once

/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef HOST_UNIT_TEST
#include <driver/uart.h>
#include <esp_err.h>
#endif

#include "uart_frame_def.h"

/* Cấu hình phần cứng UART Bridge */
#define UART_BRIDGE_PORT        1  /* UART_NUM_1 */
#define UART_BRIDGE_RX_PIN      18
#define UART_BRIDGE_TX_PIN      17
#define UART_BRIDGE_BUF_SIZE    2048U

typedef enum {
    BRIDGE_LINK_UNSYNCED = 0,
    BRIDGE_LINK_SYNCED   = 1,
    BRIDGE_LINK_READY    = 2,  /* Snapshot committed, ingest open */
} bridge_link_state_t;

typedef struct {
    bridge_link_state_t state;
    uint64_t            epoch;
    uint32_t            central_boot_nonce;
    uint32_t            snapshot_tx_id;
    uint16_t            snapshot_slots_mask; /* Bitmask of slots 0..9 received */
    uint32_t            sync_count;
    uint32_t            stale_epoch_drops;
    uint32_t            partial_snapshot_drops;
} bridge_fsm_t;

/* Lifecycle and Control APIs */
void uart_bridge_fsm_init(bridge_fsm_t *fsm, uint64_t initial_epoch);
void uart_bridge_fsm_reset(bridge_fsm_t *fsm);

/* FSM Handlers */
int uart_bridge_handle_hello(bridge_fsm_t *fsm,
                            const uart_ctrl_hello_pkt_t *hello,
                            uart_ctrl_sync_pkt_t *sync_out);

int uart_bridge_request_snapshot(bridge_fsm_t *fsm,
                                 uart_snapshot_req_pkt_t *req_out);

int uart_bridge_handle_snapshot_slot(bridge_fsm_t *fsm,
                                     uint8_t slot_id,
                                     uint64_t frame_epoch);

int uart_bridge_handle_snapshot_end(bridge_fsm_t *fsm,
                                    const uart_snapshot_end_pkt_t *end);

bool uart_bridge_is_ingest_allowed(const bridge_fsm_t *fsm, uint64_t frame_epoch);

#ifndef HOST_UNIT_TEST
esp_err_t uart_bridge_rx_init(void);
int uart_bridge_rx_read(uint8_t *dest, size_t max_len, uint32_t timeout_ms);
#endif
int uart_bridge_tx_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len);
