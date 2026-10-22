#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "uart_frame_def.h"

#define CORE1_GATE_OK                     0
#define CORE1_GATE_DROP_EPOCH_MISMATCH   -1
#define CORE1_GATE_DROP_SESSION_MISMATCH  -2
#define CORE1_GATE_DROP_SEQ_ORDER        -3
#define CORE1_GATE_DROP_SLOT_INVALID     -4

#define CORE1_GATE_DOWNLINK_OK            0
#define CORE1_GATE_DOWNLINK_REJECT_EPOCH -10
#define CORE1_GATE_DOWNLINK_REJECT_SESSION -11
#define CORE1_GATE_DOWNLINK_REJECT_MAC   -12

typedef struct {
    uint8_t  slot_id;
    bool     connected;
    uint32_t session_id;       /* 0 if disconnected */
    uint32_t last_seq;
    uint8_t  mac[6];           /* Big-Endian canonical display format */
} core1_slot_binding_t;

typedef struct {
    uint32_t epoch_mismatch_drops;
    uint32_t session_mismatch_drops;
    uint32_t seq_order_drops;
    uint32_t invalid_slot_drops;
    uint32_t snapshot_requests_triggered;
} core1_gate_stats_t;

typedef struct {
    uint64_t             current_epoch;
    core1_slot_binding_t slots[MAX_NODES_SUPPORTED];
    core1_gate_stats_t   stats;
    bool                 snapshot_requested;
} core1_gate_t;

void core1_gate_init(core1_gate_t *gate, uint64_t initial_epoch);
void core1_gate_set_epoch(core1_gate_t *gate, uint64_t epoch);

int core1_gate_handle_status(core1_gate_t *gate,
                             uint8_t node_id,
                             const uart_v2_envelope_t *env,
                             const uart_node_status_pkt_t *status);

int core1_gate_validate_telemetry(core1_gate_t *gate,
                                  uint8_t node_id,
                                  const uart_v2_envelope_t *env,
                                  uint8_t *mac_out);

int core1_gate_validate_downlink(const core1_gate_t *gate,
                                 uint8_t node_id,
                                 uint64_t epoch,
                                 uint32_t session_id,
                                 const uint8_t *target_mac);

bool core1_gate_is_slot_connected(const core1_gate_t *gate, uint8_t node_id);
uint32_t core1_gate_get_slot_session(const core1_gate_t *gate, uint8_t node_id);
bool core1_gate_is_snapshot_requested(core1_gate_t *gate);
void core1_gate_clear_snapshot_request(core1_gate_t *gate);
