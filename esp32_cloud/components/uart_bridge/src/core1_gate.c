#include <string.h>
#include "core1_gate.h"

void core1_gate_init(core1_gate_t *gate, uint64_t initial_epoch) {
    if (!gate) return;
    memset(gate, 0, sizeof(*gate));
    gate->current_epoch = initial_epoch;
    for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
        gate->slots[i].slot_id = i;
        gate->slots[i].connected = false;
        gate->slots[i].session_id = 0;
        gate->slots[i].last_seq = 0;
    }
}

void core1_gate_set_epoch(core1_gate_t *gate, uint64_t epoch) {
    if (!gate) return;
    if (gate->current_epoch != epoch) {
        gate->current_epoch = epoch;
        for (uint8_t i = 0; i < MAX_NODES_SUPPORTED; i++) {
            gate->slots[i].connected = false;
            gate->slots[i].session_id = 0;
            gate->slots[i].last_seq = 0;
            memset(gate->slots[i].mac, 0, sizeof(gate->slots[i].mac));
        }
    }
}

int core1_gate_handle_status(core1_gate_t *gate,
                             uint8_t node_id,
                             const uart_v2_envelope_t *env,
                             const uart_node_status_pkt_t *status) {
    if (!gate || !env || !status) {
        return CORE1_GATE_DROP_SLOT_INVALID;
    }
    if (node_id >= MAX_NODES_SUPPORTED) {
        gate->stats.invalid_slot_drops++;
        return CORE1_GATE_DROP_SLOT_INVALID;
    }

    if (env->bridge_epoch != gate->current_epoch) {
        gate->stats.epoch_mismatch_drops++;
        return CORE1_GATE_DROP_EPOCH_MISMATCH;
    }

    if (status->status == 1) {
        /* Connect frame: bind session, MAC, and seq */
        gate->slots[node_id].connected = true;
        gate->slots[node_id].session_id = env->session_id;
        gate->slots[node_id].last_seq = env->seq;
        memcpy(gate->slots[node_id].mac, status->mac, 6);
        return CORE1_GATE_OK;
    } else {
        /* Disconnect frame / snapshot empty slot */
        if (!gate->slots[node_id].connected && env->session_id == 0) {
            return CORE1_GATE_OK;
        }
        if (gate->slots[node_id].session_id == env->session_id) {
            gate->slots[node_id].connected = false;
            gate->slots[node_id].session_id = 0;
            gate->slots[node_id].last_seq = 0;
            return CORE1_GATE_OK;
        } else {
            gate->stats.session_mismatch_drops++;
            return CORE1_GATE_DROP_SESSION_MISMATCH;
        }
    }
}

int core1_gate_validate_telemetry(core1_gate_t *gate,
                                  uint8_t node_id,
                                  const uart_v2_envelope_t *env,
                                  uint8_t *mac_out) {
    if (!gate || !env) {
        return CORE1_GATE_DROP_SLOT_INVALID;
    }
    if (node_id >= MAX_NODES_SUPPORTED) {
        gate->stats.invalid_slot_drops++;
        return CORE1_GATE_DROP_SLOT_INVALID;
    }

    if (env->bridge_epoch != gate->current_epoch) {
        gate->stats.epoch_mismatch_drops++;
        return CORE1_GATE_DROP_EPOCH_MISMATCH;
    }

    if (!gate->slots[node_id].connected || gate->slots[node_id].session_id != env->session_id) {
        /* Session mismatch: either status frame was lost or delayed frame from previous connection */
        gate->stats.session_mismatch_drops++;
        gate->snapshot_requested = true;
        gate->stats.snapshot_requests_triggered++;
        return CORE1_GATE_DROP_SESSION_MISMATCH;
    }

    if (env->seq <= gate->slots[node_id].last_seq) {
        gate->stats.seq_order_drops++;
        return CORE1_GATE_DROP_SEQ_ORDER;
    }

    /* Valid packet: update last_seq and return canonical MAC */
    gate->slots[node_id].last_seq = env->seq;
    if (mac_out) {
        memcpy(mac_out, gate->slots[node_id].mac, 6);
    }

    return CORE1_GATE_OK;
}

int core1_gate_validate_downlink(const core1_gate_t *gate,
                                 uint8_t node_id,
                                 uint64_t epoch,
                                 uint32_t session_id,
                                 const uint8_t *target_mac) {
    if (!gate) return CORE1_GATE_DOWNLINK_REJECT_SESSION;
    if (node_id >= MAX_NODES_SUPPORTED) return CORE1_GATE_DOWNLINK_REJECT_SESSION;

    if (epoch != gate->current_epoch) {
        return CORE1_GATE_DOWNLINK_REJECT_EPOCH;
    }

    if (!gate->slots[node_id].connected || gate->slots[node_id].session_id != session_id) {
        return CORE1_GATE_DOWNLINK_REJECT_SESSION;
    }

    if (target_mac && memcmp(gate->slots[node_id].mac, target_mac, 6) != 0) {
        return CORE1_GATE_DOWNLINK_REJECT_MAC;
    }

    return CORE1_GATE_DOWNLINK_OK;
}

bool core1_gate_is_slot_connected(const core1_gate_t *gate, uint8_t node_id) {
    if (!gate || node_id >= MAX_NODES_SUPPORTED) return false;
    return gate->slots[node_id].connected;
}

uint32_t core1_gate_get_slot_session(const core1_gate_t *gate, uint8_t node_id) {
    if (!gate || node_id >= MAX_NODES_SUPPORTED) return 0;
    return gate->slots[node_id].session_id;
}

bool core1_gate_is_snapshot_requested(core1_gate_t *gate) {
    if (!gate) return false;
    return gate->snapshot_requested;
}

void core1_gate_clear_snapshot_request(core1_gate_t *gate) {
    if (!gate) return;
    gate->snapshot_requested = false;
}
