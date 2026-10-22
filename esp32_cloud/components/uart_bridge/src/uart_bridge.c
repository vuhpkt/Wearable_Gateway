#include "uart_bridge.h"
#include "gateway_ingest.h"

/* ══════════════════════════════════════════════════════════════
 *  Subsystem Static Instances (Owned exclusively by uart_bridge)
 * ════════════════════════════════════════════════════════════ */
static gateway_ingest_t         s_bridge_ingest;
static gateway_time_t           s_bridge_time;
static gateway_time_mailbox_t   s_bridge_mailbox;
static frame_snapshot_queue_t   s_bridge_snapshot_queue;
static revocation_table_t       s_bridge_rev_table;

esp_err_t uart_bridge_subsystem_init(void) {
    gateway_time_init(&s_bridge_time, 1000);
    gateway_time_mailbox_init(&s_bridge_mailbox);
    frame_snapshot_queue_init(&s_bridge_snapshot_queue);
    revocation_table_init(&s_bridge_rev_table);
    gateway_ingest_init(&s_bridge_ingest,
                        &s_bridge_snapshot_queue,
                        &s_bridge_rev_table,
                        &s_bridge_time,
                        &s_bridge_mailbox);
    return ESP_OK;
}

void uart_bridge_post_sntp(int64_t now_utc_ms, uint64_t esp_mono_ms) {
    gateway_time_mailbox_post(&s_bridge_mailbox, now_utc_ms, esp_mono_ms);
}

bool uart_bridge_pop_snapshot(frame_snapshot_tuple_t *tuple, uint32_t timeout_ms) {
    return frame_snapshot_queue_pop(&s_bridge_snapshot_queue, tuple, timeout_ms);
}

bool uart_bridge_is_session_valid(uint8_t node_id, uint64_t epoch, uint32_t session_id) {
    return revocation_table_is_session_valid(&s_bridge_rev_table, node_id, epoch, session_id);
}

const frame_snapshot_queue_t* uart_bridge_get_snapshot_queue(void) {
    return &s_bridge_snapshot_queue;
}

gateway_ingest_t* uart_bridge_get_ingest_ctx(void) {
    return &s_bridge_ingest;
}

#ifndef HOST_UNIT_TEST
#include "esp_log.h"

static const char *TAG = "UART_BRIDGE";

int uart_bridge_send_sync_req(void) {
    uint8_t dummy = 0;
    return uart_bridge_tx_frame(0, UART_PKT_TYPE_CONTROL_SYNC_REQ, &dummy, 1);
}

esp_err_t uart_bridge_init(void) {
    esp_err_t err = uart_bridge_rx_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UART hardware: %s", esp_err_to_name(err));
        return err;
    }

    /* Send initial SYNC_REQ to nRF Central to trigger handshake */
    uart_bridge_send_sync_req();

    ESP_LOGI(TAG, "UART bridge initialized successfully (460800 baud, 64-item ring buffer)");
    return ESP_OK;
}
#else
int uart_bridge_send_sync_req(void) {
    return 0;
}
#endif
