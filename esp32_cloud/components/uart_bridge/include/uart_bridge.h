#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef HOST_UNIT_TEST
#include <esp_err.h>
#else
#ifndef ESP_ERR_H
#ifndef _ESP_ERR_T_DEFINED
#define _ESP_ERR_T_DEFINED
typedef int esp_err_t;
#endif
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#endif
#endif

#include "uart_frame_def.h"
#include "bridge_time.h"
#include "uart_bridge_rx.h"
#include "uart_rx_parser.h"
#include "core1_gate.h"
#include "frame_snapshot.h"
#include "gateway_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gateway_ingest_s gateway_ingest_t;

#ifndef HOST_UNIT_TEST
/**
 * @brief Initialize UART hardware bridge (460800 baud, XTAL clock source)
 *        and snapshot ring buffer queue.
 */
esp_err_t uart_bridge_init(void);
#endif

/**
 * @brief Send SYNC_REQ to nRF Central to trigger HELLO & snapshot recovery
 */
int uart_bridge_send_sync_req(void);

/* ══════════════════════════════════════════════════════════════
 *  Hybrid Facade APIs (Inter-Core Bridge & Ingest Subsystem)
 * ════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize all Ingest & Bridge subsystem data structures
 *        (Time Engine, SNTP Mailbox, Snapshot Queue, Revocation Table, Ingest FSM).
 *        Note: Does NOT install UART hardware driver (Core 1 sensor_task installs hardware).
 */
esp_err_t uart_bridge_subsystem_init(void);

/**
 * @brief Forward SNTP synchronized UTC timestamp from Core 0 to Core 1 via Mailbox
 */
void uart_bridge_post_sntp(int64_t now_utc_ms, uint64_t esp_mono_ms);

/**
 * @brief Pop immutable frame snapshot from inter-core queue
 * @param tuple Output pointer for snapshot tuple
 * @param timeout_ms Timeout in milliseconds (e.g. 100)
 * @return true if popped, false on timeout/empty
 */
bool uart_bridge_pop_snapshot(frame_snapshot_tuple_t *tuple, uint32_t timeout_ms);

/**
 * @brief Thread-safe check if a session ID is valid in the revocation table
 */
bool uart_bridge_is_session_valid(uint8_t node_id, uint64_t epoch, uint32_t session_id);

/**
 * @brief Get pointer to the default snapshot queue (e.g. for system_health monitor)
 */
const frame_snapshot_queue_t* uart_bridge_get_snapshot_queue(void);

/**
 * @brief Get pointer to default Ingest context (passed to gateway_ingest_task on Core 1)
 */
gateway_ingest_t* uart_bridge_get_ingest_ctx(void);

#ifdef __cplusplus
}
#endif
