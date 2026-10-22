#pragma once

/* Standard C Libraries */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "uart_frame_def.h"

typedef enum {
    CENTRAL_LINK_UNSYNCED = 0,
    CENTRAL_LINK_SYNCED   = 1,
} central_link_state_t;

typedef struct {
    central_link_state_t state;
    uint32_t             boot_nonce;
    uint64_t             active_epoch;
    uint32_t             snapshot_tx_id;
    uint32_t             hello_sent_count;
    uint32_t             sync_recv_count;
    uint32_t             stale_sync_drops;
} central_bridge_fsm_t;

void central_bridge_fsm_init(central_bridge_fsm_t *fsm, uint32_t boot_nonce);
int central_bridge_create_hello(central_bridge_fsm_t *fsm, uart_ctrl_hello_pkt_t *hello_out);
int central_bridge_handle_sync(central_bridge_fsm_t *fsm, const uart_ctrl_sync_pkt_t *sync);
int central_bridge_handle_snapshot_req(central_bridge_fsm_t *fsm, const uart_snapshot_req_pkt_t *req);
int central_bridge_create_snapshot_end(central_bridge_fsm_t *fsm, uart_snapshot_end_pkt_t *end_out);
void central_bridge_barrier_reset(central_bridge_fsm_t *fsm);
int central_bridge_dispatch_downlink(uint8_t node_id, const uint8_t *payload, uint8_t len);

#ifndef HOST_TEST
int uart_tx_bridge_init(void);
int uart_tx_bridge_send(const uint8_t *data, size_t len);
int uart_tx_bridge_send_frame(uint8_t node_id, uint8_t pkt_type, const void *payload, uint8_t len);
uint64_t uart_tx_bridge_get_active_epoch(void);
void uart_tx_bridge_poll_handshake(void);
#endif