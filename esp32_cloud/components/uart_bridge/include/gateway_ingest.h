#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "uart_frame_def.h"
#include "uart_bridge_rx.h"
#include "uart_rx_parser.h"
#include "core1_gate.h"
#include "frame_snapshot.h"
#include "gateway_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gateway_ingest_s gateway_ingest_t;

struct gateway_ingest_s {
    bridge_fsm_t            bridge_fsm;
    core1_gate_t            core1_gate;
    frame_snapshot_queue_t *snapshot_queue;
    revocation_table_t     *rev_table;
    gateway_time_t         *gateway_time;
    gateway_time_mailbox_t *time_mailbox;
    int64_t                 last_sync_req_mono;
    int64_t                 last_snapshot_req_mono;
};

#include "uart_bridge.h"

void gateway_ingest_init(gateway_ingest_t *ingest,
                         frame_snapshot_queue_t *queue,
                         revocation_table_t *rev,
                         gateway_time_t *gt,
                         gateway_time_mailbox_t *time_mb);

void gateway_ingest_on_frame(gateway_ingest_t *ingest, const uart_rx_frame_t *frame);

void gateway_ingest_step_retry(gateway_ingest_t *ingest, int64_t now_mono_ms);

bool gateway_ingest_is_link_ready(const gateway_ingest_t *ingest);

#ifndef HOST_UNIT_TEST
/**
 * @brief Core 1 Sensor Producer Task: Initializes UART bridge and processes RX stream
 */
void gateway_ingest_task(void *pvParameters);
#endif

#ifdef __cplusplus
}
#endif
